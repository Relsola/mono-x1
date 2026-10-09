#include "debug/replay.h"

#if MONO_DEBUG_INPUT

#include "core.h"
#include "shared/memory.h"
#include "shared/file.h"
#include "shared/logger.h"
#include "shared/mono_math.h"
#include "game.h"
#include "save.h"
#include "input.h"
// 文本 ⇄ 磁带 在 input_script.cc；这里只用它的「写」那一半（导出）与「读」那一半（F6 回退载入）。
// 头文件的方向是单向的：input_script.h -> replay.h，所以这个包含不会形成环。
#include "debug/input_script.h"

#include <stdio.h> // snprintf（断言失败文本与导出脚本）

// ============================================================================
// 磁带：构造 + 录制 + 执行 + 断言求值（全部状态与实现都在这个 TU）
//
// 公共头（include/debug/replay.h）只暴露入口与磁带结构，装配层不认识 TapePlayer。
// ============================================================================

// 磁带的两份产物（与 build/ 下其它调试产物同处）：
//   存档点：磁带的第一条，录制开始时写；导出脚本时被引用成 `save <路径>` 那一行
//   脚本：导出（F4 / 录制结束）写出的文本形式，也是 F6 跨会话回放时的载入源
internal constexpr wchar_t TAPE_SAVE_FILE_NAME[] = L"build/replay_state.sav";
internal constexpr wchar_t TAPE_SCRIPT_FILE_NAME[] = L"build/replay_script.txt";

// F8 标记写出的断言容差（与脚本文本的默认容差一致：2 像素）
internal constexpr f32 TAPE_MARK_POS_TOLERANCE = 2.0f;

// 失败清单的容量。跑完统一打一遍（行号 + 帧号 + 断言名），
// 省得让人/AI 去 game.log 里把 45 条 FAIL 逐行找出来
internal constexpr u32 TAPE_FAILURE_LIST_MAX = 16;

// 操作名表：失败清单与日志里的可读名字（也是 /W4 之外的一层「新增枚举值别忘了同步」保险）
global_variable const char *const TAPE_OP_NAMES[] = {
    "assert_pos",
    "assert_state",
    "assert_grounded",
    "assert_world",
    "assert_transition",
    "assert_time_stop",
    "assert_monster",
    "assert_monster_time_slowed",
    "probe_monster_respawns",
    "assert_monster_respawns",
    "assert_monster_respawns_total",
    "assert_projectiles",
    "assert_air_jumps",
    "probe_reset",
    "assert_rise",
    "assert_run_x",
    "save_state",
    "load_state",
    "log_state",
};
static_assert(array_size(TAPE_OP_NAMES) == TAPE_LOG_STATE + 1, "TAPE_OP_NAMES 与 TapeOpKind 不同步");

internal const char *tape_op_name(TapeOpKind kind) { return TAPE_OP_NAMES[(u32)kind]; }

// 失败清单与上下文行的缓冲。一行上下文约 220 字符，日志单行上限是 1KB
internal constexpr u32 TAPE_CONTEXT_SIZE = 320;

struct TapeFailure
{
    u32 source_line;
    u32 frame;
    TapeOpKind kind;
};

struct TapePlayer
{
    InputTape tape;

    bool recording;
    bool playing;

    u32 frame;        // 当前磁带帧号（从 0 开始）
    u32 frame_cursor; // 下一个要应用的输入变化点
    u32 op_cursor;    // 下一个要求值的操作
    u32 cycle;        // 回放圈数（从 1 开始，只用于日志）

    u32 assert_count;
    u32 assert_failed;

    // 失败断言的索引（只记前 TAPE_FAILURE_LIST_MAX 条，其余只计数）
    TapeFailure failures[TAPE_FAILURE_LIST_MAX];
    u32 failure_count;
    u32 failure_dropped;

    // 存档点在内存里的那份：循环回放时不必反复读盘
    bool has_save_point;
    GameStateSnapshot save_point;

    // 位移探针与怪物刷新基准：只属于当前这盘磁带，换磁带（含循环重启）时清零
    bool probe_active;
    f32 probe_base_x;
    f32 probe_base_y;
    f32 probe_max_x;
    f32 probe_max_y;
    u32 probe_monster_respawns;
};

global_variable TapePlayer global_player = {};

// ============================================================================
// 磁带构造
// ============================================================================

// 解析器当前的来源行号：tape_add_op 把它抄进每个新操作（见 replay.h 的说明）
global_variable u32 global_op_source_line = 0;

void tape_set_op_source_line(u32 source_line) { global_op_source_line = source_line; }

InputTape tape_create(bool loop)
{
    // frames / ops 不预分配：它们只在真的用上时才由 array_push_slot 按需增长（初值见 Array<T>）。
    // 一盘小脚本（几十个变化点）因此只占几 KB，而不是一上来就几百帧的容量
    InputTape tape = {};
    tape.loop = loop;
    return tape;
}

void tape_set_save_path(InputTape *tape, const char *utf8_path)
{
    u32 len = 0;
    while (utf8_path[len] != '\0' && len + 1 < SAVE_PATH_SIZE) {
        tape->save_path[len] = utf8_path[len];
        ++len;
    }
    tape->save_path[len] = '\0';
    tape->has_save_path = true;

    // 截断是能被发现的：路径不完整就必然读档失败，但那时已经看不出是为什么了
    if (utf8_path[len] != '\0') {
        LOG_WARN("tape: save path is too long and was truncated: %s", utf8_path);
    }
}

void tape_set_save_spawn(InputTape *tape)
{
    tape->has_save_path = false;
    tape->save_path[0] = '\0';
}

void tape_push_frame(InputTape *tape, u32 frame, const TapeInputState *state)
{
    TapeFrame *slot = array_push_slot(&tape->frames);
    slot->frame = frame;
    slot->state = *state;
}

void tape_add_op(InputTape *tape, u32 frame, TapeOpKind kind, f32 a, f32 b, f32 c, const char *path)
{
    // 按帧号插到该在的位置：录制与解析都是追加，但「回放中途按 F8」可能补出比已有操作更小的
    // 帧号，而求值是单向扫游标的 —— 乱序会让操作在错误的帧上求值（旧实现踩过：标记被静默丢掉）
    (void)array_push_slot(&tape->ops);
    u32 at = tape->ops.size - 1;
    while (at > 0 && tape->ops.data[at - 1].frame > frame) {
        tape->ops.data[at] = tape->ops.data[at - 1];
        --at;
    }

    TapeOp *op = &tape->ops.data[at];
    op->frame = frame;
    // 来源行号由解析器设置（脚本）或保持 0（录制 / F8 标记）：
    // 插入到中间也照标，所以脚本文本里的语句顺序不影响它
    op->source_line = global_op_source_line;
    op->kind = kind;
    op->a = a;
    op->b = b;
    op->c = c;
    op->path = path;
}

void tape_set_total_frames(InputTape *tape, u32 total_frames) { tape->total_frames = total_frames; }

// ============================================================================
// 输入：抓取 / 比较 / 喂给游戏
// ============================================================================

internal TapeInputState tape_capture_input(const GameInput *input)
{
    const PlayerInput *player = &input->player;
    const MouseInput *mouse = &input->mouse;

    TapeInputState state = {};
    state.is_pad = player->is_pad;
    for (u32 i = 0; i < GA_COUNT; ++i) {
        state.current[i] = player->current[i];
    }
    state.left_stick_x = player->left_stick_x;
    state.left_stick_y = player->left_stick_y;
    state.right_stick_x = player->right_stick_x;
    state.right_stick_y = player->right_stick_y;
    state.left_trigger = player->left_trigger;
    state.right_trigger = player->right_trigger;

    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        state.mouse_buttons[i] = mouse->current[i];
    }
    state.mouse_x = mouse->x;
    state.mouse_y = mouse->y;
    state.mouse_wheel_delta = mouse->wheel_delta;
    return state;
}

bool tape_input_equal(const TapeInputState *a, const TapeInputState *b)
{
    if (a->is_pad != b->is_pad ||
        a->left_stick_x != b->left_stick_x || a->left_stick_y != b->left_stick_y ||
        a->right_stick_x != b->right_stick_x || a->right_stick_y != b->right_stick_y ||
        a->left_trigger != b->left_trigger || a->right_trigger != b->right_trigger ||
        a->mouse_x != b->mouse_x || a->mouse_y != b->mouse_y ||
        a->mouse_wheel_delta != b->mouse_wheel_delta) {
        return false;
    }

    for (u32 i = 0; i < GA_COUNT; ++i) {
        if (a->current[i] != b->current[i]) {
            return false;
        }
    }
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        if (a->mouse_buttons[i] != b->mouse_buttons[i]) {
            return false;
        }
    }
    return true;
}

// 清空整份输入（含粘滞边沿）：开始一盘磁带前必须做，否则真实设备留下的按着状态会被当成
// 「第 0 帧之前就按着」，磁带的第一帧边沿就错了。
// 整体清零而不是逐字段抄：这两个结构是纯 POD，以后加输入字段这里也不会漏（同「字段表」的思路）
internal void tape_reset_input(GameInput *input)
{
    input->player = {};
    input->mouse = {};
}

// 把「本帧生效的输入状态」写进 GameInput：
//   1. 推进到最后一个 frame <= 当前帧 的变化点（磁带存状态，不存边沿）；
//   2. 边沿在这里算 —— 同一帧里先按后松（脚本可以这么写）必须留下 pressed 边沿，
//      否则「第 N 帧轻点一下」会变成什么都没发生（旧的注入路径也是粘滞边沿）。
internal void tape_apply_input(const InputTape *tape, u32 frame, u32 *cursor, GameInput *input)
{
    PlayerInput *player = &input->player;
    MouseInput *mouse = &input->mouse;

    for (u32 i = 0; i < GA_COUNT; ++i) {
        player->previous[i] = player->current[i];
    }
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        mouse->previous[i] = mouse->current[i];
    }

    bool pressed[GA_COUNT] = {};
    bool released[GA_COUNT] = {};
    bool mouse_pressed[MOUSE_BUTTON_COUNT] = {};
    bool mouse_released[MOUSE_BUTTON_COUNT] = {};

    while (*cursor < tape->frames.size && tape->frames.data[*cursor].frame <= frame) {
        const TapeInputState *state = &tape->frames.data[*cursor].state;

        player->is_pad = state->is_pad;
        for (u32 i = 0; i < GA_COUNT; ++i) {
            bool down = state->current[i];
            pressed[i] = pressed[i] || (down && !player->current[i]);
            released[i] = released[i] || (!down && player->current[i]);
            player->current[i] = down;
        }
        player->left_stick_x = state->left_stick_x;
        player->left_stick_y = state->left_stick_y;
        player->right_stick_x = state->right_stick_x;
        player->right_stick_y = state->right_stick_y;
        player->left_trigger = state->left_trigger;
        player->right_trigger = state->right_trigger;

        for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
            bool down = state->mouse_buttons[i];
            mouse_pressed[i] = mouse_pressed[i] || (down && !mouse->current[i]);
            mouse_released[i] = mouse_released[i] || (!down && mouse->current[i]);
            mouse->current[i] = down;
        }
        mouse->x = state->mouse_x;
        mouse->y = state->mouse_y;
        mouse->wheel_delta = state->mouse_wheel_delta;

        ++*cursor;
    }

    for (u32 i = 0; i < GA_COUNT; ++i) {
        player->pressed[i] = (player->current[i] && !player->previous[i]) || pressed[i];
        player->released[i] = (!player->current[i] && player->previous[i]) || released[i];
    }
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        mouse->pressed[i] = (mouse->current[i] && !mouse->previous[i]) || mouse_pressed[i];
        mouse->released[i] = (!mouse->current[i] && mouse->previous[i]) || mouse_released[i];
    }
}

// ============================================================================
// 操作求值（断言与控制指令）
// ============================================================================

// 求值期间的「当前操作 / 当前状态」。tape_run_op 是断言求值的唯一入口，它进来就先设好
// 这两个指针 —— 于是十几个断言分支都只调 tape_assert_result(ok, frame, message)，
// 不必每个都多传两个参数（漏传一个数组/写成另一个指针都会很难查）
global_variable const TapeOp *global_current_op = nullptr;
global_variable const GameState *global_current_state = nullptr;

// 所有断言看得到的量排成一行：「为什么失败」通常一眼就能看出来
// （速度是不是 0、世界对不对、怪物还在不在），不必再回头看 --trace
internal void tape_fail_context(const GameState *state, char *out, u32 out_size)
{
    u32 projectiles = 0;
    for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
        if (state->projectiles.items[i].active) {
            ++projectiles;
        }
    }

    snprintf(out, out_size,
             "context: pos (%.1f, %.1f)  vel (%.1f, %.1f)  state %s  grounded %d  world %s  "
             "air_jumps %u  coyote %.2f  dash %.2f/%.2f  transition %d  time_stop %d  "
             "monster {active %d time_slowed %d respawns %u}  projectiles %u",
             state->player_x, state->player_y, state->velocity.x, state->velocity.y,
             player_state_name(state->state), state->grounded ? 1 : 0,
             (state->world == WORLD_FIRST) ? "FIRST" : "SECOND",
             state->air_jumps_left, state->coyote_timer, state->dash_timer, state->dash_cooldown,
             state->world_transition_active ? 1 : 0, state->time_stop.active ? 1 : 0,
             state->monster.active ? 1 : 0, state->monster.time_slowed ? 1 : 0,
             state->monster.respawn_count, projectiles);
}

// 这条断言之前的最后一个输入变化点：回答「我明明按了跳」这类疑问
// （脚本把帧号写错时，这里会显示最后一次变化其实发生在很久以前）
internal void tape_fail_input_note(char *out, u32 out_size)
{
    const InputTape *tape = &global_player.tape;

    u32 cursor = global_player.frame_cursor;
    if (cursor == 0 || tape->frames.size == 0) {
        snprintf(out, out_size,
                 "input: no change point before this frame (the tape already holds this state at frame 0)");
        return;
    }

    u32 index = MIN(cursor, tape->frames.size) - 1;
    const TapeInputState *now = &tape->frames.data[index].state;
    const TapeInputState *before = (index > 0) ? &tape->frames.data[index - 1].state : nullptr;

    u32 used = (u32)snprintf(out, out_size, "input: last change at frame %u:", tape->frames.data[index].frame);
    u32 shown = 0;
    for (const auto &entry : ACTION_NAMES) {
        if (shown >= 4) {
            break;
        }
        u32 action = (u32)entry.action;
        if (action >= GA_COUNT) {
            continue;
        }
        bool was = before ? before->current[action] : false;
        bool down = now->current[action];
        if (was == down) {
            continue;
        }
        if (used + 1 < out_size) {
            int written = snprintf(out + used, out_size - used, " %s %s", entry.name, down ? "down" : "up");
            if (written > 0) {
                used += (u32)written;
            }
        }
        ++shown;
    }
    if (shown == 0 && used + 1 < out_size) {
        snprintf(out + used, out_size - used, " (no button changed: a stick or mouse value did)");
    }
}

// 跑完把失败清单打一遍：这是给人/AI 的「下一步看哪一行」，
// 每条的详细内容在上面各自的 FAIL 行里（含状态上下文与最后一个输入变化点）
internal void tape_report_failures()
{
    if (global_player.assert_failed == 0) {
        return;
    }

    LOG_DEBUG("TAPE: %u failed assertion(s), first %u listed:", global_player.assert_failed,
              global_player.failure_count);
    if (global_player.failure_count == 0) {
        // 典型情况：磁带停在了一个控制指令上（写档/读档失败）—— 那不是断言失败
        LOG_DEBUG("TAPE:   (no assertion failed: the run stopped at a control op, see the line above)");
    }
    for (u32 i = 0; i < global_player.failure_count; ++i) {
        const TapeFailure *failure = &global_player.failures[i];
        if (failure->source_line != 0) {
            LOG_DEBUG("TAPE:   line %u  frame %u  %s", failure->source_line, failure->frame,
                      tape_op_name(failure->kind));
        } else {
            LOG_DEBUG("TAPE:   frame %u  %s", failure->frame, tape_op_name(failure->kind));
        }
    }
    if (global_player.failure_dropped > 0) {
        LOG_DEBUG("TAPE:   ... and %u more (only the first %u are listed)", global_player.failure_dropped,
                  TAPE_FAILURE_LIST_MAX);
    }
}

internal void tape_assert_result(bool ok, u32 frame, const char *message)
{
    ++global_player.assert_count;

    // 有来源行号就带上：FAIL 行因此能直接指回脚本文本的那一行，不用靠帧号去数
    u32 line = global_current_op ? global_current_op->source_line : 0;
    if (line != 0) {
        if (ok) {
            LOG_DEBUG("[tape] frame %u  OK   %s  [script line %u]", frame, message, line);
        } else {
            LOG_ERROR("[tape] frame %u  FAIL %s  [script line %u]", frame, message, line);
        }
    } else if (ok) {
        LOG_DEBUG("[tape] frame %u  OK   %s", frame, message);
    } else {
        LOG_ERROR("[tape] frame %u  FAIL %s", frame, message);
    }

    if (ok) {
        return;
    }

    ++global_player.assert_failed;

    // 失败时多给两行诊断：一行「所有断言看得到的量」，一行「最后一个输入变化点」。
    // 这两行回答的是「为什么」，而 FAIL 行只回答「差多少」
    char context[TAPE_CONTEXT_SIZE];
    if (global_current_state) {
        tape_fail_context(global_current_state, context, sizeof(context));
        LOG_ERROR("[tape]        %s", context);
    }
    tape_fail_input_note(context, sizeof(context));
    LOG_ERROR("[tape]        %s", context);

    // 记进清单：跑完统一打一遍，不必从几十行 FAIL 里逐行找
    if (global_current_op) {
        if (global_player.failure_count < TAPE_FAILURE_LIST_MAX) {
            TapeFailure *failure = &global_player.failures[global_player.failure_count++];
            failure->source_line = line;
            failure->frame = frame;
            failure->kind = global_current_op->kind;
        } else {
            ++global_player.failure_dropped;
        }
    }
}

internal void tape_log_state_line(u32 frame, const GameState *state)
{
    LOG_DEBUG("[tape] frame %u  pos (%.1f, %.1f)  vel (%.1f, %.1f)  state %s  grounded %d  air_jumps %u  dash_cd %.2f  coyote %.2f",
              frame, state->player_x, state->player_y, state->velocity.x, state->velocity.y,
              player_state_name(state->state), state->grounded ? 1 : 0, state->air_jumps_left,
              state->dash_cooldown, state->coyote_timer);
}

// 求值一个操作。返回 false = 这盘磁带已经不可信（写档/读档失败），调用方应当停掉它。
// 这里刻意不写 default 分支：新增 TapeOpKind 而漏掉处理时，/W4 会报「枚举值未被 switch 处理」
// ——静默漏掉一个断言的代价比一条警告高得多。
internal bool tape_run_op(const TapeOp *op, GameState *game_state)
{
    char message[192];

    // 断言求值期间的当前操作/状态（见上面那两个全局指针的说明）
    global_current_op = op;
    global_current_state = game_state;

    switch (op->kind) {
    case TAPE_ASSERT_POS: {
        bool ok = fabsf(game_state->player_x - op->a) <= op->c && fabsf(game_state->player_y - op->b) <= op->c;
        snprintf(message, sizeof(message), "assert_pos (%.1f, %.1f) tol %.1f, actual (%.1f, %.1f)",
                 op->a, op->b, op->c, game_state->player_x, game_state->player_y);
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_STATE: {
        bool ok = (u32)game_state->state == (u32)op->a;
        snprintf(message, sizeof(message), "assert_state %s, actual %s",
                 player_state_name((PlayerState)(u32)op->a), player_state_name(game_state->state));
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_GROUNDED: {
        bool expected = op->a != 0.0f;
        bool ok = game_state->grounded == expected;
        snprintf(message, sizeof(message), "assert_grounded %d, actual %d",
                 expected ? 1 : 0, game_state->grounded ? 1 : 0);
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_WORLD: {
        WorldId expected = (WorldId)(u32)op->a;
        bool ok = game_state->world == expected;
        snprintf(message, sizeof(message), "assert_world %s, actual %s",
                 (expected == WORLD_FIRST) ? "FIRST" : "SECOND",
                 (game_state->world == WORLD_FIRST) ? "FIRST" : "SECOND");
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_TRANSITION: {
        bool expected = op->a != 0.0f;
        bool ok = game_state->world_transition_active == expected;
        snprintf(message, sizeof(message), "assert_transition %d, actual %d",
                 expected ? 1 : 0, game_state->world_transition_active ? 1 : 0);
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_TIME_STOP: {
        bool expected = op->a != 0.0f;
        bool ok = game_state->time_stop.active == expected;
        snprintf(message, sizeof(message), "assert_time_stop %d, actual %d",
                 expected ? 1 : 0, game_state->time_stop.active ? 1 : 0);
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_MONSTER: {
        bool expected = op->a != 0.0f;
        bool ok = game_state->monster.active == expected;
        snprintf(message, sizeof(message), "assert_monster %d, actual %d",
                 expected ? 1 : 0, game_state->monster.active ? 1 : 0);
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_ASSERT_MONSTER_TIME_SLOWED: {
        bool expected = op->a != 0.0f;
        bool ok = game_state->monster.time_slowed == expected;
        snprintf(message, sizeof(message), "assert_monster_time_slowed %d, actual %d",
                 expected ? 1 : 0, game_state->monster.time_slowed ? 1 : 0);
        tape_assert_result(ok, op->frame, message);
        break;
    }
    case TAPE_PROBE_MONSTER_RESPAWNS:
        global_player.probe_monster_respawns = game_state->monster.respawn_count;
        LOG_DEBUG("[tape] frame %u  probe_monster_respawns %u", op->frame, game_state->monster.respawn_count);
        break;
    case TAPE_ASSERT_MONSTER_RESPAWNS: {
        u32 expected = (u32)op->a;
        u32 actual = game_state->monster.respawn_count - global_player.probe_monster_respawns;
        snprintf(message, sizeof(message), "assert_monster_respawns %u, actual %u", expected, actual);
        tape_assert_result(actual == expected, op->frame, message);
        break;
    }
    case TAPE_ASSERT_MONSTER_RESPAWNS_TOTAL: {
        u32 expected = (u32)op->a;
        snprintf(message, sizeof(message), "assert_monster_respawns_total %u, actual %u",
                 expected, game_state->monster.respawn_count);
        tape_assert_result(game_state->monster.respawn_count == expected, op->frame, message);
        break;
    }
    case TAPE_ASSERT_PROJECTILES: {
        u32 actual = 0;
        for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
            if (game_state->projectiles.items[i].active) {
                ++actual;
            }
        }
        u32 expected = (u32)op->a;
        snprintf(message, sizeof(message), "assert_projectiles %u, actual %u", expected, actual);
        tape_assert_result(actual == expected, op->frame, message);
        break;
    }
    case TAPE_ASSERT_AIR_JUMPS: {
        u32 expected = (u32)op->a;
        snprintf(message, sizeof(message), "assert_air_jumps %u, actual %u", expected, game_state->air_jumps_left);
        tape_assert_result(game_state->air_jumps_left == expected, op->frame, message);
        break;
    }
    case TAPE_PROBE_RESET:
        global_player.probe_active = true;
        global_player.probe_base_x = game_state->player_x;
        global_player.probe_base_y = game_state->player_y;
        global_player.probe_max_x = game_state->player_x;
        global_player.probe_max_y = game_state->player_y;
        LOG_DEBUG("[tape] frame %u  probe_reset at (%.1f, %.1f)",
                  op->frame, game_state->player_x, game_state->player_y);
        break;
    case TAPE_ASSERT_RISE: {
        // 没 arm 过的探针会让这条断言变成「拿 0 去比」—— 那是个永远通过的假断言，
        // 比失败更难发现，所以当成失败报出来
        if (!global_player.probe_active) {
            tape_assert_result(false, op->frame, "assert_rise without a prior probe_reset (the probe never armed)");
            break;
        }
        f32 rise = global_player.probe_max_y - global_player.probe_base_y;
        snprintf(message, sizeof(message), "assert_rise >= %.1f, actual %.1f", op->a, rise);
        tape_assert_result(rise >= op->a - op->c, op->frame, message);
        break;
    }
    case TAPE_ASSERT_RUN_X: {
        if (!global_player.probe_active) {
            tape_assert_result(false, op->frame, "assert_run_x without a prior probe_reset (the probe never armed)");
            break;
        }
        f32 run = global_player.probe_max_x - global_player.probe_base_x;
        snprintf(message, sizeof(message), "assert_run_x >= %.1f, actual %.1f", op->a, run);
        tape_assert_result(run >= op->a - op->c, op->frame, message);
        break;
    }
    case TAPE_SAVE_STATE: {
        wchar_t path[SAVE_PATH_SIZE];
        if (!op->path || !utf8_to_wide(op->path, path, SAVE_PATH_SIZE)) {
            LOG_ERROR("[tape] frame %u  save_state: bad path", op->frame);
            return false;
        }
        // 写档失败就是磁带坏了：后面的断言会跟着一起失真，不如当场停掉
        if (!save_write(path, game_state)) {
            return false;
        }
        LOG_DEBUG("[tape] frame %u  save_state %s", op->frame, op->path);
        break;
    }
    case TAPE_LOAD_STATE: {
        wchar_t path[SAVE_PATH_SIZE];
        if (!op->path || !utf8_to_wide(op->path, path, SAVE_PATH_SIZE)) {
            LOG_ERROR("[tape] frame %u  load_state: bad path", op->frame);
            return false;
        }
        // 读档失败（文件不在 / 校验不过）同样当场停：不然会拿一份「没变过」的状态继续跑，
        // 后面几十条断言逐个 FAIL，看上去像是物理坏了
        if (!save_restore(path, game_state)) {
            LOG_ERROR("[tape] frame %u  load_state: cannot restore %s", op->frame, op->path);
            return false;
        }
        LOG_DEBUG("[tape] frame %u  load_state %s (x=%.1f y=%.1f state=%s)",
                  op->frame, op->path, game_state->player_x, game_state->player_y,
                  player_state_name(game_state->state));
        break;
    }
    case TAPE_LOG_STATE:
        tape_log_state_line(op->frame, game_state);
        break;
    }

    return true;
}

// ============================================================================
// 引擎
// ============================================================================

bool replay_is_recording() { return global_player.recording; }
bool replay_is_playing() { return global_player.playing; }

// 按磁带的存档点读档。`save spawn` 是不读档：启动时（game_init_asset 之后）的世界
// 就是出生点状态，磁带要的正是那一刻。
internal bool tape_restore_save_point(GameState *game_state)
{
    if (!global_player.tape.has_save_path) {
        return true;
    }

    wchar_t path[SAVE_PATH_SIZE];
    if (!utf8_to_wide(global_player.tape.save_path, path, SAVE_PATH_SIZE)) {
        LOG_ERROR("tape: save path is not valid UTF-8: %s", global_player.tape.save_path);
        return false;
    }

    // 读盘一次就留在内存里（save_point）：循环回放时不必反复读盘，也不会因为
    // 播放途中文件被改写而中途变化 —— 一盘磁带的世界自始至终是同一份
    if (!global_player.has_save_point) {
        if (!save_read(path, game_state, &global_player.save_point)) {
            return false;
        }
        global_player.has_save_point = true;
    }

    if (!game_state_load(game_state, &global_player.save_point)) {
        LOG_ERROR("tape: the save point %s holds an invalid state for this build", global_player.tape.save_path);
        return false;
    }
    return true;
}

internal void tape_reset_cursors()
{
    global_player.frame = 0;
    global_player.frame_cursor = 0;
    global_player.op_cursor = 0;
    global_player.assert_count = 0;
    global_player.assert_failed = 0;
    global_player.failure_count = 0;
    global_player.failure_dropped = 0;
    global_player.probe_active = false;
    global_player.probe_base_x = 0.0f;
    global_player.probe_base_y = 0.0f;
    global_player.probe_max_x = 0.0f;
    global_player.probe_max_y = 0.0f;
    global_player.probe_monster_respawns = 0;
}

bool replay_start_tape(const InputTape *tape, GameState *game_state, GameInput *input)
{
    if (tape->total_frames == 0) {
        LOG_ERROR("tape: nothing to play (the tape has no input and no op)");
        return false;
    }

    // 先保住手里原来那盘磁带：读档失败时不该把「刚录的那盘」一起丢掉
    InputTape previous = global_player.tape;
    global_player.tape = *tape;
    global_player.has_save_point = false;
    tape_reset_cursors();
    global_player.cycle = 1;
    global_player.playing = false;

    if (!tape_restore_save_point(game_state)) {
        global_player.tape = previous;
        return false;
    }

    tape_reset_input(input);
    global_player.playing = true;

    LOG_DEBUG("tape: playing %u frames (%u change points, %u ops)%s, save %s",
              tape->total_frames, tape->frames.size, tape->ops.size,
              tape->loop ? ", looping" : "",
              tape->has_save_path ? tape->save_path : "spawn");
    return true;
}

bool replay_start_last(GameState *game_state, GameInput *input)
{
    // 优先本次会话录的那盘（最新的那份）；没有才去读最近导出的脚本
    if (global_player.tape.total_frames > 0) {
        InputTape tape = global_player.tape;
        tape.loop = true; // F6 是「循环看一段」的语义，与脚本「跑完退出」不同
        return replay_start_tape(&tape, game_state, input);
    }

    InputTape tape = {};
    if (!input_script_load_tape(TAPE_SCRIPT_FILE_NAME, &tape)) {
        LOG_WARN("replay: nothing to replay (record with F5, or run a script first)");
        return false;
    }
    tape.loop = true;
    return replay_start_tape(&tape, game_state, input);
}

void replay_before_step(GameInput *input)
{
    if (!global_player.playing) {
        return;
    }

    tape_apply_input(&global_player.tape, global_player.frame, &global_player.frame_cursor, input);
}

bool replay_after_step(GameState *game_state, GameInput *input)
{
    if (!global_player.playing) {
        return false;
    }

    // 探针与断言看的是同一时刻：本逻辑步的结果
    if (global_player.probe_active) {
        global_player.probe_max_x = MAX(global_player.probe_max_x, game_state->player_x);
        global_player.probe_max_y = MAX(global_player.probe_max_y, game_state->player_y);
    }

    while (global_player.op_cursor < global_player.tape.ops.size &&
           global_player.tape.ops.data[global_player.op_cursor].frame <= global_player.frame) {
        const TapeOp *op = &global_player.tape.ops.data[global_player.op_cursor];
        if (!tape_run_op(op, game_state)) {
            // 磁带上的操作失败（写档/读档）＝这盘磁带不再可信：停下来、计入一次失败。
            // 脚本（不循环）→ 返回 true 让主循环退出，退出码自然是 1；
            // 回放（循环）→ 只停回放，游戏继续跑（别把人正在看的窗口关掉）
            ++global_player.assert_failed;
            global_player.playing = false;
            // 脚本这一侧要给「和断言失败同一形状」的结果：只有退出码 1 的话，
            // 自动化看不出「是操作没做成」而不是「某个断言不准」，也不知道停在哪一条
            if (!global_player.tape.loop) {
                LOG_DEBUG("TAPE: FAIL  (stopped at frame %u: `%s` could not be performed; %u op(s) not evaluated, including this one)",
                          op->frame, tape_op_name(op->kind),
                          (u32)(global_player.tape.ops.size - global_player.op_cursor));
                tape_report_failures();
            }
            log_flush();
            return !global_player.tape.loop;
        }
        ++global_player.op_cursor;
    }

    ++global_player.frame;
    if (global_player.frame < global_player.tape.total_frames) {
        return false;
    }

    u32 passed = global_player.assert_count - global_player.assert_failed;
    const char *result = (global_player.assert_failed == 0) ? "PASS" : "FAIL";

    if (global_player.tape.loop) {
        LOG_DEBUG("TAPE: cycle %u %s  (%u/%u asserts passed, %u logic frames)",
                  global_player.cycle, result, passed, global_player.assert_count, global_player.frame);

        // 循环：回到存档点再来一遍。存档点已在内存里，所以这里不读盘，只重新装载
        if (!tape_restore_save_point(game_state)) {
            // 开始时校验过的存档点在播放途中失效（文件被删改）——停下来比继续更诚实
            LOG_ERROR("tape: cannot restore the save point, replay stopped");
            global_player.playing = false;
            return false;
        }

        ++global_player.cycle;
        tape_reset_cursors();
        tape_reset_input(input);
        return false;
    }

    // 非循环（脚本）：跑完就把结果落盘 —— 自动化靠的是这行与退出码
    LOG_DEBUG("TAPE: %s  (%u/%u asserts passed, %u logic frames)",
              result, passed, global_player.assert_count, global_player.frame);
    tape_report_failures();
    if (global_player.assert_count == 0) {
        // 「没有任何断言」的脚本也会 PASS：说清楚 PASS 的含义，别让人以为它验了什么
        LOG_WARN("tape: this tape has no assertions at all — PASS only means it ran to the end");
    }
    log_flush();
    global_player.playing = false;
    return true;
}

void replay_end()
{
    if (!global_player.playing) {
        return;
    }

    global_player.playing = false;
    LOG_DEBUG("replay: stopped at tape frame %u", global_player.frame);
}

// ============================================================================
// 录制（F5 / F8）
// ============================================================================

void replay_record_start(const GameState *game_state, const GameInput *input)
{
    if (global_player.playing) {
        LOG_WARN("replay: cannot record while a tape is playing");
        return;
    }

    // 存档点先落盘：磁带的第一条就是它，丢了它这盘磁带就不可复现
    if (!save_write(TAPE_SAVE_FILE_NAME, game_state)) {
        LOG_ERROR("replay: cannot write the save point, recording not started");
        return;
    }

    global_player.tape = tape_create(true);
    char utf8_path[SAVE_PATH_SIZE];
    if (wide_to_utf8(TAPE_SAVE_FILE_NAME, utf8_path, SAVE_PATH_SIZE)) {
        tape_set_save_path(&global_player.tape, utf8_path);
    } else {
        LOG_WARN("replay: the save path cannot be encoded as UTF-8, the exported script will start from spawn");
    }

    global_player.recording = true;
    global_player.has_save_point = false;
    tape_reset_cursors();

    // 录制开始时按着的键：文本形式只能表达「第 0 帧按下」，于是那一下跳跃/冲刺会再触发一次。
    // 脚本语法没有「带着按下状态开始」这种写法，所以只能提前说清楚
    for (u32 i = 0; i < GA_COUNT; ++i) {
        if (input->player.current[i]) {
            LOG_WARN("replay: '%s' is held at record start; the exported script will press it at frame 0",
                     game_action_name((GameAction)i));
        }
    }

    LOG_DEBUG("replay: recording (save point written to %ls)", TAPE_SAVE_FILE_NAME);
}

void replay_record_input(const GameInput *input)
{
    if (!global_player.recording) {
        return;
    }

    // 只记变化点：输入没变就不落新帧（回放时状态一路保持，语义不变、内存与文本都省一半）
    TapeInputState state = tape_capture_input(input);
    const Array<TapeFrame> *frames = &global_player.tape.frames;
    if (frames->size == 0 || !tape_input_equal(&frames->data[frames->size - 1].state, &state)) {
        tape_push_frame(&global_player.tape, global_player.frame, &state);
    }

    ++global_player.frame;
}

void replay_record_stop()
{
    if (!global_player.recording) {
        return;
    }

    global_player.recording = false;
    tape_set_total_frames(&global_player.tape, global_player.frame);
    LOG_DEBUG("replay: recorded %u frames (%u change points, %u ops)",
              global_player.frame, global_player.tape.frames.size, global_player.tape.ops.size);

    // 结束即导出：录制与导出本就是一个动作的两半，不必再让用户按一次 F4
    replay_export_script();
}

void replay_mark_assert(const GameState *game_state)
{
    if (!global_player.recording && !global_player.playing) {
        LOG_WARN("replay: assert mark ignored (record with F5, or replay a tape first)");
        return;
    }
    if (global_player.frame == 0) {
        LOG_WARN("replay: assert mark ignored (no logic step has run yet)");
        return;
    }

    // frame 取「最后已完成的那一步」：按 F8 时屏幕上看到的就是那一步之后的状态，
    // 而磁带里的操作也在该帧的 game_update 之后求值 —— 两边对齐，导出的脚本才能原样重跑
    u32 frame = global_player.frame - 1;

    // 只写「玩家状态 + 世界状态」这几条：便宜、稳定，且足够卡住绝大多数回归。
    // 其余（怪物 / 伪时停 / 投射物 / 二段跳余量）需要时手写进脚本，
    // 不必让每个标记都长成 11 行 —— 标记越啰嗦，人越倾向于整段删掉
    tape_add_op(&global_player.tape, frame, TAPE_ASSERT_POS,
                game_state->player_x, game_state->player_y, TAPE_MARK_POS_TOLERANCE, nullptr);
    tape_add_op(&global_player.tape, frame, TAPE_ASSERT_STATE, (f32)game_state->state, 0.0f, 0.0f, nullptr);
    tape_add_op(&global_player.tape, frame, TAPE_ASSERT_GROUNDED, game_state->grounded ? 1.0f : 0.0f, 0.0f, 0.0f, nullptr);
    tape_add_op(&global_player.tape, frame, TAPE_ASSERT_WORLD, (f32)game_state->world, 0.0f, 0.0f, nullptr);

    LOG_DEBUG("replay: assert mark at frame %u (x=%.1f y=%.1f state=%s grounded=%d world=%u)",
              frame, game_state->player_x, game_state->player_y, player_state_name(game_state->state),
              game_state->grounded ? 1 : 0, (u32)game_state->world);
}

// ============================================================================
// 导出与收尾
// ============================================================================

bool replay_export_script()
{
    if (global_player.tape.total_frames == 0) {
        LOG_WARN("replay: nothing to export (record with F5, or run a script first)");
        return false;
    }

    return input_script_write(&global_player.tape, TAPE_SCRIPT_FILE_NAME);
}

int replay_exit_code() { return (global_player.assert_failed > 0) ? 1 : 0; }

void replay_log_state(u32 frame, const GameState *state)
{
    tape_log_state_line(frame, state);
    log_flush();
}

#endif // MONO_DEBUG_INPUT
