#pragma once

#include "core.h"

// ============================================================================
// 录制回放调试
// ============================================================================

// 录制文件魔数 "RPLY"
inline constexpr wchar_t REPLAY_FILE_NAME[] = L"build/replay.bin";
inline constexpr u32 REPLAY_MAGIC = 0x52504C59;

// 录制开始游戏状态快照
struct ReplayState
{
    f32 player_x;
    f32 player_y;
    v2 velocity;
    Camera2D camera;

    // 冲刺状态机
    PlayerFacing facing;
    PlayerState state;
    f32 dash_timer;
    f32 dash_cooldown;
    f32 dash_dir_x;
};

// 每个固定逻辑步记录的输入快照
struct ReplayInputFrame
{
    bool is_pad;            // 本帧输入来源是否为手柄
    bool current[GA_COUNT]; // 本帧各动作的按下状态
    f32 left_stick_x;
    f32 left_stick_y;
    f32 right_stick_x;
    f32 right_stick_y;
    f32 left_trigger;
    f32 right_trigger;

    bool mouse_buttons[MOUSE_BUTTON_COUNT]; // 本帧鼠标按键状态
    f32 mouse_x;                            // 屏幕像素坐标
    f32 mouse_y;
    f32 mouse_wheel_delta;                  // 本帧滚轮增量
};

// 录制文件头
struct ReplayHeader
{
    u32 magic;       // REPLAY_MAGIC
    u32 version;     // REPLAY_VERSION
    u32 state_size;  // sizeof(ReplayState)，用于结构变化时的兼容校验
    u32 frame_count; // 录制的输入帧数
    u32 frame_size;  // sizeof(ReplayInputFrame)，用于帧结构变化时的兼容校验
};

// 录制回放运行时状态
struct ReplayRecorder
{
    // 录制
    bool is_recording;
    ReplayState initial_state;
    Array<ReplayInputFrame> frames;

    // 回放
    ReplayState replay_initial_state;
    ReplayInputFrame *replay_frames;
    u32 replay_frame_count;
    u32 replay_index;
    bool is_replaying;
};

// 开始录制
internal void replay_start_recording(ReplayRecorder *recorder, const GameState *game_state)
{
    recorder->is_recording = true;

    recorder->initial_state.player_x = game_state->player_x;
    recorder->initial_state.player_y = game_state->player_y;
    recorder->initial_state.velocity = game_state->velocity;
    recorder->initial_state.camera = game_state->camera;
    recorder->initial_state.facing = game_state->facing;
    recorder->initial_state.state = game_state->state;
    recorder->initial_state.dash_timer = game_state->dash_timer;
    recorder->initial_state.dash_cooldown = game_state->dash_cooldown;
    recorder->initial_state.dash_dir_x = game_state->dash_dir_x;

    // 首次分配 4096 帧
    recorder->frames = init<ReplayInputFrame>(4096);
}

// 记录一帧输入（在 input_update 之后、game_update 之前调用）
internal void replay_record_input(ReplayRecorder *recorder, const GameInput *input)
{
    assert(recorder->is_recording);

    const PlayerInput *controller = &input->player;
    const MouseInput *mouse = &input->mouse;

    ReplayInputFrame *frame = array_push_slot(&recorder->frames);
    frame->is_pad = controller->is_pad;
    for (u8 i = 0; i < GA_COUNT; ++i) {
        frame->current[i] = controller->current[i];
    }
    frame->left_stick_x = controller->left_stick_x;
    frame->left_stick_y = controller->left_stick_y;
    frame->right_stick_x = controller->right_stick_x;
    frame->right_stick_y = controller->right_stick_y;
    frame->left_trigger = controller->left_trigger;
    frame->right_trigger = controller->right_trigger;
    for (u8 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        frame->mouse_buttons[i] = mouse->current[i];
    }
    frame->mouse_x = mouse->x;
    frame->mouse_y = mouse->y;
    frame->mouse_wheel_delta = mouse->wheel_delta;
}

// 结束录制并持久化
internal void replay_stop_recording(ReplayRecorder *recorder, const wchar_t *filename)
{
    recorder->is_recording = false;

    u64 frame_bytes = (u64)recorder->frames.size * sizeof(ReplayInputFrame);
    u64 total_size = sizeof(ReplayHeader) + sizeof(ReplayState) + frame_bytes;

    u8 *buffer = (u8 *)arena_push(total_size);
    u8 *cursor = buffer;

    ReplayHeader header = {};
    header.magic = REPLAY_MAGIC;
    header.state_size = sizeof(ReplayState);
    header.frame_count = recorder->frames.size;
    header.frame_size = sizeof(ReplayInputFrame);

    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    memcpy(cursor, &recorder->initial_state, sizeof(ReplayState));
    cursor += sizeof(ReplayState);

    memcpy(cursor, recorder->frames.data, frame_bytes);

    write_file(filename, safe_cast_u64(total_size), buffer);
}

// 从文件读取录制内容
internal bool replay_load(ReplayRecorder *recorder, const wchar_t *filename)
{
    ReadFileRes file = read_file(filename);
    if (!file.contents) {
        return false;
    }

    u8 *cursor = (u8 *)file.contents;

    ReplayHeader header = {};
    memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);

    // 校验魔数、状态结构与帧结构大小
    if (header.magic != REPLAY_MAGIC ||
        header.state_size != sizeof(ReplayState) ||
        header.frame_size != sizeof(ReplayInputFrame)) {
        free_file_memory(file.contents);
        return false;
    }

    u64 expected_size = sizeof(ReplayHeader) + sizeof(ReplayState) + (u64)header.frame_count * sizeof(ReplayInputFrame);
    if ((u64)file.file_size < expected_size) {
        free_file_memory(file.contents);
        return false;
    }

    memcpy(&recorder->replay_initial_state, cursor, sizeof(ReplayState));
    cursor += sizeof(ReplayState);

    u64 frame_bytes = (u64)header.frame_count * sizeof(ReplayInputFrame);
    recorder->replay_frames = (ReplayInputFrame *)arena_push(frame_bytes);
    memcpy(recorder->replay_frames, cursor, frame_bytes);
    recorder->replay_frame_count = header.frame_count;

    free_file_memory(file.contents);
    return true;
}

internal void replay_state_to_game(ReplayState *state, GameState *game_state)
{
    game_state->player_x = state->player_x;
    game_state->player_y = state->player_y;
    game_state->velocity = state->velocity;
    game_state->camera = state->camera;
    game_state->facing = state->facing;
    game_state->state = state->state;
    game_state->dash_timer = state->dash_timer;
    game_state->dash_cooldown = state->dash_cooldown;
    game_state->dash_dir_x = state->dash_dir_x;
}

// 开始回放
internal void replay_begin(ReplayRecorder *recorder, GameState *game_state)
{
    recorder->is_replaying = true;
    recorder->replay_index = 0;

    replay_state_to_game(&recorder->replay_initial_state, game_state);
}

// 结束回放
internal void replay_end(ReplayRecorder *recorder) { recorder->is_replaying = false; }

// 回放一帧：用录制的输入驱动玩家与鼠标；到达末尾时自动循环（重置状态到初始）
internal void replay_tick(ReplayRecorder *recorder, GameState *game_state, GameInput *input)
{
    assert(recorder->replay_frame_count > 0);

    PlayerInput *controller = &input->player;
    MouseInput *mouse = &input->mouse;

    // 循环重放：播到末尾后回到开头，并把游戏状态重置为初始状态
    if (recorder->replay_index >= recorder->replay_frame_count) {
        recorder->replay_index = 0;
        replay_state_to_game(&recorder->replay_initial_state, game_state);

        // 清空控制器的上一帧/边沿状态与模拟量，避免与上一圈结尾混淆
        for (u32 i = 0; i < GA_COUNT; ++i) {
            controller->previous[i] = false;
            controller->current[i] = false;
            controller->pressed[i] = false;
            controller->released[i] = false;
        }
        controller->is_pad = false;
        controller->left_stick_x = 0.0f;
        controller->left_stick_y = 0.0f;
        controller->right_stick_x = 0.0f;
        controller->right_stick_y = 0.0f;
        controller->left_trigger = 0.0f;
        controller->right_trigger = 0.0f;
        mouse->wheel_delta = 0.0f;
        for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
            mouse->previous[i] = false;
            mouse->current[i] = false;
            mouse->pressed[i] = false;
            mouse->released[i] = false;
        }
    }

    // 上一帧存档，再恢复本帧输入
    for (u32 i = 0; i < GA_COUNT; ++i) {
        controller->previous[i] = controller->current[i];
    }

    ReplayInputFrame *frame = &recorder->replay_frames[recorder->replay_index];
    controller->is_pad = frame->is_pad;
    for (u32 i = 0; i < GA_COUNT; ++i) {
        controller->current[i] = frame->current[i];
    }
    controller->left_stick_x = frame->left_stick_x;
    controller->left_stick_y = frame->left_stick_y;
    controller->right_stick_x = frame->right_stick_x;
    controller->right_stick_y = frame->right_stick_y;
    controller->left_trigger = frame->left_trigger;
    controller->right_trigger = frame->right_trigger;

    for (u32 i = 0; i < GA_COUNT; ++i) {
        controller->pressed[i] = controller->current[i] && !controller->previous[i];
        controller->released[i] = !controller->current[i] && controller->previous[i];
    }

    // 鼠标
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        mouse->previous[i] = mouse->current[i];
        mouse->current[i] = frame->mouse_buttons[i];
    }
    mouse->x = frame->mouse_x;
    mouse->y = frame->mouse_y;
    mouse->wheel_delta = frame->mouse_wheel_delta;
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        mouse->pressed[i] = mouse->current[i] && !mouse->previous[i];
        mouse->released[i] = !mouse->current[i] && mouse->previous[i];
    }

    recorder->replay_index++;
}
