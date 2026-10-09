#include "debug/input_script.h"
#include "shared/memory.h"
#include "shared/file.h"
#include "shared/logger.h"

#if MONO_DEBUG_INPUT

#include <stdarg.h> // va_list / va_start（诊断文本）
#include <stdio.h>  // vsnprintf
#include <stdlib.h> // strtoul / strtof
#include <string.h> // memcpy / strcmp / strncmp / strlen

// ============================================================================
// 输入脚本：文本 ⇄ 磁带
//
// 两条方向的实现都在这个 TU —— 改格式只改一个文件。执行（喂输入、求值断言）
// 在 src/debug/replay.cc，所以这里不出现任何「逻辑步」概念之外的运行时状态。
// ============================================================================

struct NameValue
{
    const char *name;
    u32 value;
};

global_variable const NameValue SCRIPT_STATE_NAMES[] = {
    { "IDLE", PSTATE_IDLE },
    { "RUN", PSTATE_RUN },
    { "JUMP", PSTATE_JUMP },
    { "FALL", PSTATE_FALL },
    { "DASH", PSTATE_DASH },
};

// 世界名不另建表：反过来问 game_world_name，于是「名字」这件事只有一个真源
internal bool script_lookup_world(const char *name, u32 *value)
{
    for (u32 w = 0; w < WORLD_COUNT; ++w) {
        if (strcmp(game_world_name((WorldId)w), name) == 0) {
            *value = w;
            return true;
        }
    }
    return false;
}

internal bool script_lookup(const NameValue *table, u32 count, const char *name, u32 *value)
{
    for (u32 i = 0; i < count; ++i) {
        if (strcmp(table[i].name, name) == 0) {
            *value = table[i].value;
            return true;
        }
    }
    return false;
}

// 动作名表来自 input.h（与录制导出共用同一份）
internal bool script_lookup_action(const char *name, GameAction *action)
{
    for (const auto &entry : ACTION_NAMES) {
        if (strcmp(entry.name, name) == 0) {
            *action = entry.action;
            return true;
        }
    }
    return false;
}

// 鼠标按钮名表同样来自 input.h（`mouse_press left` 的 `left` / `middle` / `right`）
internal bool script_lookup_mouse_button(const char *name, MouseButton *button)
{
    for (const auto &entry : MOUSE_BUTTON_NAMES) {
        if (strcmp(entry.name, name) == 0) {
            *button = entry.button;
            return true;
        }
    }
    return false;
}

// ============================================================================
// 解析
// ============================================================================

struct ScriptParse
{
    InputTape *tape;
    u32 line_number;

    bool saw_statement; // 是否已经见过第一条语句（用来强制 `save` 排第一）
    bool has_save_line; // 见过 `save` 行
    bool failed;        // 已经报过错：不再继续解析（第一处错误通常就是根因）

    bool has_input_frame; // 是否已经有过输入行（用于非递减校验）
    u32 last_input_frame;
    u32 last_frame; // 所有帧号的最大值（决定磁带长度）

    TapeInputState input; // 累积的输入状态：press / release 都在它上面改
};

// 取下一个空白分隔的参数，没有则返回 false
internal bool script_next_arg(char **cursor, char *out, u32 out_size)
{
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0' || *p == '\r' || *p == '#') {
        return false;
    }

    u32 len = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && len + 1 < out_size) {
        out[len++] = *p++;
    }
    out[len] = '\0';
    *cursor = p;
    return true;
}

// 取下一个参数解析成 f32。参数缺失（行尾或遇到 '#'）时用 fallback —— 这是可选参数的正常情况
internal void script_next_f32(char **cursor, f32 *value, f32 fallback)
{
    char token[64];
    if (!script_next_arg(cursor, token, sizeof(token))) {
        *value = fallback;
        return;
    }

    char *number_end = nullptr;
    f32 parsed = strtof(token, &number_end);
    if (number_end == token || *number_end != '\0') {
        LOG_WARN("input script: '%s' is not a number, using %.1f", token, fallback);
        parsed = fallback;
    }
    *value = parsed;
}

internal void script_error(ScriptParse *parse, const char *fmt, ...)
{
    parse->failed = true;

    // 行号统一在这里带上，调用点只写「这一行哪里不对」
    char detail[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    LOG_ERROR("input script: line %u: %s", parse->line_number, detail);
}

internal void script_touch_frame(ScriptParse *parse, u32 frame)
{
    if (frame > parse->last_frame) {
        parse->last_frame = frame;
    }
}

// 必填的数值参数：缺失或不是数字都是错误。
// 旧写法缺参数时静默用 fallback（0），于是 `assert_rise` 变成「上升 >= 0 - 0」
// = 永远通过的假断言 —— 空断言比失败更难发现，所以它必须是错误
internal bool script_require_f32(ScriptParse *parse, char **cursor, const char *command, const char *arg, f32 *value)
{
    char token[64];
    if (!script_next_arg(cursor, token, sizeof(token))) {
        script_error(parse, "`%s` needs a value (`%s`)", command, arg);
        return false;
    }

    char *number_end = nullptr;
    f32 parsed = strtof(token, &number_end);
    if (number_end == token || *number_end != '\0') {
        script_error(parse, "`%s %s`: '%s' is not a number", command, arg, token);
        return false;
    }
    *value = parsed;
    return true;
}

// 行尾还有没有别的参数（用来报「多余的参数被忽略」）
internal bool script_arg_is_empty(char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    return *cursor == '\0' || *cursor == '\r' || *cursor == '#';
}

// 存档文件是不是现在就存在（拿读一次来判：.sav 只有几百字节）
internal bool script_file_exists(const char *utf8_path)
{
    wchar_t wide[SAVE_PATH_SIZE];
    if (!utf8_to_wide(utf8_path, wide, SAVE_PATH_SIZE)) {
        return false;
    }
    ReadFileRes probe = read_file(wide);
    if (!probe.contents) {
        return false;
    }
    return true;
}

// 这个脚本里前面写过同名存档吗（`load_state` 的拼写检查用）
internal bool script_writes_path(const ScriptParse *parse, const char *path)
{
    for (u32 i = 0; i < parse->tape->ops.size; ++i) {
        const TapeOp *op = &parse->tape->ops.data[i];
        if (op->kind == TAPE_SAVE_STATE && op->path && strcmp(op->path, path) == 0) {
            return true;
        }
    }
    return false;
}

// `save` 行：磁带的第一条。spawn 表示不读档，从关卡出生点开始
internal void script_parse_save(ScriptParse *parse, char *p)
{
    char value[SAVE_PATH_SIZE];
    if (!script_next_arg(&p, value, sizeof(value))) {
        script_error(parse, "`save` needs a path (or `spawn`)");
        return;
    }

    if (strcmp(value, "spawn") == 0) {
        tape_set_save_spawn(parse->tape);
    } else {
        // 存档点是在第一个逻辑步之前读的，所以它必须现在就在盘上。
        // 否则错误要等到游戏起来、读档失败才出现（那时报的是「世界不对」，不像缺文件）
        if (!script_file_exists(value)) {
            script_error(parse, "the save point `%s` does not exist — a tape starts from an existing .sav", value);
            return;
        }
        tape_set_save_path(parse->tape, value);
    }
    parse->has_save_line = true;
}

// 输入类命令（press / release / mouse_*）共用的一段：帧号非递减校验 + 「状态真的变了才落变化点」。
// 变化点是**按顺序追加**的，所以这里必须自己把关；操作那一侧不同（tape_add_op 会插到正确位置）。
// 状态没变就不落点（也就不会产生边沿）：重复声明同一个值不等于「又按了一次」。
internal void script_push_input(ScriptParse *parse, u32 frame, bool changed)
{
    if (parse->has_input_frame && frame < parse->last_input_frame) {
        script_error(parse, "frame %u goes backwards (the last input frame is %u)", frame, parse->last_input_frame);
        return;
    }

    if (changed || !parse->has_input_frame) {
        tape_push_frame(parse->tape, frame, &parse->input);
    }

    parse->has_input_frame = true;
    parse->last_input_frame = frame;
    script_touch_frame(parse, frame);
}

internal void script_parse_input(ScriptParse *parse, u32 frame, bool down, char **p)
{
    char name[32];
    GameAction action = GA_COUNT;
    if (!script_next_arg(p, name, sizeof(name)) || !script_lookup_action(name, &action)) {
        script_error(parse, "unknown action");
        return;
    }

    bool changed = (parse->input.current[action] != down);
    parse->input.current[action] = down;
    script_push_input(parse, frame, changed);
}

// `mouse_move <x> <y>`：鼠标位置（客户区像素）。UI 靠它做悬停/命中，
// 所以它是鼠标交互能被脚本驱动的前提（否则鼠标只能手动测）
internal void script_parse_mouse_move(ScriptParse *parse, u32 frame, char **p)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (!script_require_f32(parse, p, "mouse_move", "x", &x) ||
        !script_require_f32(parse, p, "mouse_move", "y", &y)) {
        return;
    }

    bool changed = (parse->input.mouse_x != x || parse->input.mouse_y != y);
    parse->input.mouse_x = x;
    parse->input.mouse_y = y;
    script_push_input(parse, frame, changed);
}

// `mouse_press [left|middle|right]` / `mouse_release [...]`：缺省是左键。
// 按钮同样用变化点表达，边沿由引擎算（与键盘动作一致）
internal void script_parse_mouse_button(ScriptParse *parse, u32 frame, bool down, char **p)
{
    char name[32];
    MouseButton button = MOUSE_LEFT;
    if (script_next_arg(p, name, sizeof(name)) && !script_lookup_mouse_button(name, &button)) {
        script_error(parse, "unknown mouse button `%s` (left / middle / right)", name);
        return;
    }

    bool changed = (parse->input.mouse_buttons[button] != down);
    parse->input.mouse_buttons[button] = down;
    script_push_input(parse, frame, changed);
}

// `save_state` / `load_state`：两者的形状一样，只差一个 kind。
// 路径要活到操作被求值那一刻，而文件内容解析完就被释放 → 拷进 arena 常驻
internal void script_parse_path_op(ScriptParse *parse, u32 frame, char **p, TapeOpKind kind, const char *command)
{
    char path[SAVE_PATH_SIZE];
    if (!script_next_arg(p, path, sizeof(path))) {
        script_error(parse, "`%s` needs a path", command);
        return;
    }

    u64 length = strlen(path) + 1;
    char *copy = (char *)arena_push(length);
    memcpy(copy, path, length);

    // `load_state` 引用的存档要么盘上有、要么这个脚本前面写过 —— 两者都没有就是拼错了。
    // 不查的话错误要等跑到那一帧才报，而那时可能已经跑了几十秒，看上去还像物理坏了
    if (kind == TAPE_LOAD_STATE && !script_writes_path(parse, path) && !script_file_exists(path)) {
        script_error(parse, "`load_state %s`: nothing writes that file (add a `save_state` first, or fix the path)",
                     path);
        return;
    }

    tape_add_op(parse->tape, frame, kind, 0.0f, 0.0f, 0.0f, copy);
    script_touch_frame(parse, frame);
}

// 解析一行（会被就地改：行尾已经换成 '\0'）
internal void script_parse_line(ScriptParse *parse, char *line)
{
    constexpr char SAVE[] = "save";

    char *p = line;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0' || *p == '\r' || *p == '#') {
        return;
    }

    // 第一条非注释语句必须是 `save`：磁带先要回答「世界从哪来」
    if (!parse->saw_statement) {
        parse->saw_statement = true;
        if (strncmp(p, SAVE, array_size(SAVE) - 1) != 0 || (p[4] != ' ' && p[4] != '\t')) {
            script_error(parse, "the first statement must be `save <path.sav>` (or `save spawn`)");
            return;
        }
        script_parse_save(parse, p + array_size(SAVE) - 1);
        return;
    }

    if (strncmp(p, SAVE, array_size(SAVE) - 1) == 0 && (p[4] == ' ' || p[4] == '\t')) {
        script_error(parse, "`save` must be the first statement");
        return;
    }

    // 帧号是第一个 token。先拦负号：strtoull 会把 "-5" 吃下去并回绕成一个巨大的数，
    // 报出来的错就成了「帧号太大」，看不出真正的问题
    if (*p == '-') {
        script_error(parse, "the frame number must not be negative");
        return;
    }

    char *number_end = nullptr;
    u64 frame = strtoull(p, &number_end, 10);
    if (number_end == p) {
        script_error(parse, "no frame number");
        return;
    }
    p = number_end;

    char command[32];
    if (!script_next_arg(&p, command, sizeof(command))) {
        script_error(parse, "no command");
        return;
    }

    // 帧号范围：磁带长度是 u32，超出就说明这行多半写错了（而不是「跑到天荒地老」）
    if (frame > 0xFFFFFFFFull) {
        script_error(parse, "frame number is too large");
        return;
    }

    // 这一行产生的操作要标注来源行号（写进 TapeOp.source_line，FAIL 行靠它指回文本）
    tape_set_op_source_line(parse->line_number);

    if (strcmp(command, "press") == 0) {
        script_parse_input(parse, (u32)frame, true, &p);
    } else if (strcmp(command, "release") == 0) {
        script_parse_input(parse, (u32)frame, false, &p);
    } else if (strcmp(command, "mouse_move") == 0) {
        script_parse_mouse_move(parse, (u32)frame, &p);
    } else if (strcmp(command, "mouse_press") == 0) {
        script_parse_mouse_button(parse, (u32)frame, true, &p);
    } else if (strcmp(command, "mouse_release") == 0) {
        script_parse_mouse_button(parse, (u32)frame, false, &p);
    } else if (strcmp(command, "assert_pos") == 0) {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 tolerance = 0.0f;
        // x / y 必填：缺了就变成「断言在 (0, 0)」——一个看上去很正常、却永远失败的断言
        if (!script_require_f32(parse, &p, command, "x", &x) ||
            !script_require_f32(parse, &p, command, "y", &y)) {
            return;
        }
        script_next_f32(&p, &tolerance, 2.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_POS, x, y, tolerance, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_state") == 0) {
        char name[32];
        u32 state = 0;
        if (!script_next_arg(&p, name, sizeof(name)) ||
            !script_lookup(SCRIPT_STATE_NAMES, array_size(SCRIPT_STATE_NAMES), name, &state)) {
            script_error(parse, "unknown state");
            return;
        }
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_STATE, (f32)state, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_grounded") == 0) {
        f32 expected = 1.0f;
        script_next_f32(&p, &expected, 1.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_GROUNDED, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_world") == 0) {
        char name[32];
        u32 world = 0;
        if (!script_next_arg(&p, name, sizeof(name)) ||
            !script_lookup_world(name, &world)) {
            script_error(parse, "unknown world");
            return;
        }
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_WORLD, (f32)world, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_transition") == 0) {
        f32 expected = 1.0f;
        script_next_f32(&p, &expected, 1.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_TRANSITION, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_time_stop") == 0) {
        f32 expected = 1.0f;
        script_next_f32(&p, &expected, 1.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_TIME_STOP, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_monster") == 0) {
        f32 expected = 1.0f;
        script_next_f32(&p, &expected, 1.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_MONSTER, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_monster_time_slowed") == 0) {
        f32 expected = 1.0f;
        script_next_f32(&p, &expected, 1.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_MONSTER_TIME_SLOWED, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "probe_monster_respawns") == 0) {
        tape_add_op(parse->tape, (u32)frame, TAPE_PROBE_MONSTER_RESPAWNS, 0.0f, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_monster_respawns") == 0) {
        f32 expected = 0.0f;
        script_next_f32(&p, &expected, 0.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_MONSTER_RESPAWNS, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_monster_respawns_total") == 0) {
        f32 expected = 0.0f;
        script_next_f32(&p, &expected, 0.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_MONSTER_RESPAWNS_TOTAL, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_projectiles") == 0) {
        f32 expected = 0.0f;
        script_next_f32(&p, &expected, 0.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_PROJECTILES, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_air_jumps") == 0) {
        f32 expected = 0.0f;
        script_next_f32(&p, &expected, 0.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_AIR_JUMPS, expected, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "probe_reset") == 0) {
        tape_add_op(parse->tape, (u32)frame, TAPE_PROBE_RESET, 0.0f, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_rise") == 0) {
        f32 min_rise = 0.0f;
        f32 tolerance = 0.0f;
        // 必填：缺了就是「上升 >= 0 - 0」，永远通过
        if (!script_require_f32(parse, &p, command, "min_rise", &min_rise)) {
            return;
        }
        script_next_f32(&p, &tolerance, 2.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_RISE, min_rise, 0.0f, tolerance, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "assert_run_x") == 0) {
        f32 min_run = 0.0f;
        f32 tolerance = 0.0f;
        if (!script_require_f32(parse, &p, command, "min_run", &min_run)) {
            return;
        }
        script_next_f32(&p, &tolerance, 2.0f);
        tape_add_op(parse->tape, (u32)frame, TAPE_ASSERT_RUN_X, min_run, 0.0f, tolerance, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else if (strcmp(command, "save_state") == 0) {
        script_parse_path_op(parse, (u32)frame, &p, TAPE_SAVE_STATE, command);
    } else if (strcmp(command, "load_state") == 0) {
        script_parse_path_op(parse, (u32)frame, &p, TAPE_LOAD_STATE, command);
    } else if (strcmp(command, "log_state") == 0) {
        tape_add_op(parse->tape, (u32)frame, TAPE_LOG_STATE, 0.0f, 0.0f, 0.0f, nullptr);
        script_touch_frame(parse, (u32)frame);
    } else {
        script_error(parse, "unknown command '%s'", command);
        return;
    }

    // 多余的参数通常是打字写错（`assert_grounded 1 0`），静默忽略会让人以为那一行有别的意思
    if (!script_arg_is_empty(p)) {
        LOG_WARN("input script: line %u: extra arguments after `%s` are ignored", parse->line_number, command);
    }
}

bool input_script_load_tape(const wchar_t *path, InputTape *out)
{
    ReadFileRes file = read_file(path);
    if (!file.contents) {
        LOG_WARN("input script: cannot open %ls", path);
        return false;
    }

    // 整份文件拷进 arena 后**就地解析**（行尾换成 '\0'、行内注释由取参数的工具处理）。
    // 这样就没有「行太长就整行丢掉」这个上限了 —— 旧实现有一行 256 字节的缓冲，
    // 而当时的 initial_state 行有 762 字符，于是整行被丢弃、脚本静默地从出生点跑
    u32 file_size = file.file_size;
    char *text = (char *)arena_push((u64)file_size + 1);
    memcpy(text, file.contents, file_size);
    text[file_size] = '\0';

    *out = tape_create(false);

    ScriptParse parse = {};
    parse.tape = out;
    parse.line_number = 1;

    char *cursor = text;
    // 跳过 UTF-8 BOM（编辑器另存为带 BOM 时很常见）
    if (file_size >= 3 && (u8)cursor[0] == 0xEF && (u8)cursor[1] == 0xBB && (u8)cursor[2] == 0xBF) {
        cursor += 3;
    }

    while (*cursor != '\0') {
        char *line_end = cursor;
        while (*line_end != '\0' && *line_end != '\n' && *line_end != '\r') {
            ++line_end;
        }

        char terminator = *line_end;
        *line_end = '\0';
        script_parse_line(&parse, cursor);

        if (parse.failed) {
            return false;
        }

        if (terminator == '\0') {
            break;
        }

        cursor = line_end + 1;
        if (terminator == '\r' && *cursor == '\n') {
            ++cursor; // 吃掉 \r\n 的 \n
        }
        ++parse.line_number;
    }

    if (!parse.has_save_line) {
        LOG_ERROR("input script: %ls has no `save` line — a tape must say where the world comes from", path);
        return false;
    }

    tape_set_total_frames(out, parse.last_frame + 1);

    // 解析完了就清掉来源行号：后面 F8 标记补进来的操作不该继承最后一行脚本的行号
    tape_set_op_source_line(0);
    LOG_DEBUG("input script: %ls → %u frames (%u change points, %u ops), save %s",
              path, out->total_frames, out->frames.size, out->ops.size,
              out->has_save_path ? out->save_path : "spawn");
    return true;
}

bool input_script_run(const wchar_t *path, GameState *game_state, GameInput *input)
{
    InputTape tape = {};
    if (!input_script_load_tape(path, &tape)) {
        return false;
    }

    if (!replay_start_tape(&tape, game_state, input)) {
        return false;
    }

    LOG_DEBUG("input script: running %ls", path);
    return true;
}

// ============================================================================
// 导出（磁带 → 文本）
// ============================================================================

internal void text_append(char *buffer, u64 capacity, u32 *used, bool *truncated, const char *fmt, ...)
{
    if ((u64)*used + 1 >= capacity) {
        if (!*truncated) {
            *truncated = true;
            LOG_WARN("script export: the output buffer is full, the rest was dropped");
        }
        return;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer + *used, capacity - *used, fmt, args);
    va_end(args);

    if (written <= 0) {
        return;
    }

    u64 room = capacity - *used - 1; // 给结尾 '\0' 留一位
    if ((u64)written > room) {
        *used += (u32)room;
        if (!*truncated) {
            *truncated = true;
            LOG_WARN("script export: the output buffer is full, the rest was dropped");
        }
        return;
    }

    *used += (u32)written;
}

bool input_script_write(const InputTape *tape, const wchar_t *path)
{
    // 容量估算：每个变化点最多 GA_COUNT 行、每个操作一行，每行几十字节，再留出表头
    u64 capacity = KB(2) + (u64)tape->frames.size * (GA_COUNT * 24) + (u64)tape->ops.size * 128;
    char *buffer = (char *)arena_push(capacity);
    u32 used = 0;
    bool truncated = false;

    text_append(buffer, capacity, &used, &truncated,
                "# 由磁带导出（F4 或录制结束）：存档点 + 操作序列 + 断言\n"
                "# 帧号 = 磁带逻辑步（60Hz，从 0 开始，必须非递减）；第一行给存档点，跑之前先读档\n"
                "# 运行：main.exe input_script build/replay_script.txt；不想要的断言行删掉即可\n");

    if (tape->has_save_path) {
        text_append(buffer, capacity, &used, &truncated, "save %s\n\n", tape->save_path);
    } else {
        text_append(buffer, capacity, &used, &truncated, "save spawn\n\n");
    }

    // 输入：把变化点写成「与上一帧相比改了哪些动作 / 鼠标走了没 / 按钮变了没」。
    // 摇杆与扳机仍然只能留在磁带里（回放能带、导不成文本），所以出现时吵一声，
    // 别让人以为导出的脚本是完整的
    TapeInputState previous = {};
    bool warned_analog = false;
    for (u32 i = 0; i < tape->frames.size; ++i) {
        const TapeFrame *frame = &tape->frames.data[i];
        const TapeInputState *state = &frame->state;

        if (!warned_analog &&
            (state->left_stick_x != 0.0f || state->left_stick_y != 0.0f ||
             state->right_stick_x != 0.0f || state->right_stick_y != 0.0f ||
             state->left_trigger != 0.0f || state->right_trigger != 0.0f)) {
            LOG_WARN("script export: analog stick / trigger input cannot be written as text and was dropped");
            warned_analog = true;
        }

        for (u32 action = 0; action < GA_COUNT; ++action) {
            if (state->current[action] == previous.current[action]) {
                continue;
            }
            text_append(buffer, capacity, &used, &truncated, "%-5u %-7s %s\n", frame->frame,
                        state->current[action] ? "press" : "release", game_action_name((GameAction)action));
        }

        // 鼠标：先写位置再写按钮 —— 同一帧的两行按文本顺序变成两个变化点，
        // 顺序反了就会“先点后移”（解析器要求帧号非递减，同帧的顺序就是执行顺序）
        if (state->mouse_x != previous.mouse_x || state->mouse_y != previous.mouse_y) {
            text_append(buffer, capacity, &used, &truncated, "%-5u %-7s %.2f %.2f\n", frame->frame,
                        "mouse_move", state->mouse_x, state->mouse_y);
        }
        for (u32 button = 0; button < MOUSE_BUTTON_COUNT; ++button) {
            if (state->mouse_buttons[button] == previous.mouse_buttons[button]) {
                continue;
            }
            text_append(buffer, capacity, &used, &truncated, "%-5u %-12s %s\n", frame->frame,
                        state->mouse_buttons[button] ? "mouse_press" : "mouse_release",
                        mouse_button_name((MouseButton)button));
        }
        previous = *state;
    }

    if (tape->frames.size > 0 && tape->ops.size > 0) {
        text_append(buffer, capacity, &used, &truncated, "\n");
    }

    // 操作：与解析端的命令逐条对应（这个 switch 刻意不写 default —— 新增 TapeOpKind 时
    // 漏掉导出会得到一条 /W4 警告，而不是悄悄少写一行断言）
    for (u32 i = 0; i < tape->ops.size; ++i) {
        const TapeOp *op = &tape->ops.data[i];
        switch (op->kind) {
        case TAPE_ASSERT_POS:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_pos      %.1f  %.1f  %.1f\n",
                        op->frame, op->a, op->b, op->c);
            break;
        case TAPE_ASSERT_STATE:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_state    %s\n",
                        op->frame, player_state_name((PlayerState)(u32)op->a));
            break;
        case TAPE_ASSERT_GROUNDED:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_grounded %d\n",
                        op->frame, (op->a != 0.0f) ? 1 : 0);
            break;
        case TAPE_ASSERT_WORLD:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_world    %s\n",
                        op->frame, ((u32)op->a == WORLD_FIRST) ? "FIRST" : "SECOND");
            break;
        case TAPE_ASSERT_TRANSITION:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_transition %d\n",
                        op->frame, (op->a != 0.0f) ? 1 : 0);
            break;
        case TAPE_ASSERT_TIME_STOP:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_time_stop %d\n",
                        op->frame, (op->a != 0.0f) ? 1 : 0);
            break;
        case TAPE_ASSERT_MONSTER:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_monster  %d\n",
                        op->frame, (op->a != 0.0f) ? 1 : 0);
            break;
        case TAPE_ASSERT_MONSTER_TIME_SLOWED:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_monster_time_slowed %d\n",
                        op->frame, (op->a != 0.0f) ? 1 : 0);
            break;
        case TAPE_PROBE_MONSTER_RESPAWNS:
            text_append(buffer, capacity, &used, &truncated, "%-5u probe_monster_respawns\n", op->frame);
            break;
        case TAPE_ASSERT_MONSTER_RESPAWNS:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_monster_respawns %u\n",
                        op->frame, (u32)op->a);
            break;
        case TAPE_ASSERT_MONSTER_RESPAWNS_TOTAL:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_monster_respawns_total %u\n",
                        op->frame, (u32)op->a);
            break;
        case TAPE_ASSERT_PROJECTILES:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_projectiles %u\n",
                        op->frame, (u32)op->a);
            break;
        case TAPE_ASSERT_AIR_JUMPS:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_air_jumps %u\n",
                        op->frame, (u32)op->a);
            break;
        case TAPE_PROBE_RESET:
            text_append(buffer, capacity, &used, &truncated, "%-5u probe_reset\n", op->frame);
            break;
        case TAPE_ASSERT_RISE:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_rise     %.1f  %.1f\n",
                        op->frame, op->a, op->c);
            break;
        case TAPE_ASSERT_RUN_X:
            text_append(buffer, capacity, &used, &truncated, "%-5u assert_run_x    %.1f  %.1f\n",
                        op->frame, op->a, op->c);
            break;
        case TAPE_SAVE_STATE:
            text_append(buffer, capacity, &used, &truncated, "%-5u save_state     %s\n",
                        op->frame, op->path ? op->path : "");
            break;
        case TAPE_LOAD_STATE:
            text_append(buffer, capacity, &used, &truncated, "%-5u load_state     %s\n",
                        op->frame, op->path ? op->path : "");
            break;
        case TAPE_LOG_STATE:
            text_append(buffer, capacity, &used, &truncated, "%-5u log_state\n", op->frame);
            break;
        }
    }

    bool ok = write_file(path, safe_cast_u64(used), buffer);
    if (ok) {
        LOG_DEBUG("script: exported %ls (%u bytes, %u change points, %u ops)",
                  path, used, tape->frames.size, tape->ops.size);
    }
    return ok;
}

#endif // MONO_DEBUG_INPUT
