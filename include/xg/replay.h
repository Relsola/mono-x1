#pragma once

#include "core.h"

// ============================================================================
// 录制回放调试
// ============================================================================

// 录制文件魔数 "RPLY" 与版本号
inline constexpr wchar_t REPLAY_FILE_NAME[] = L"build/replay.bin";
inline constexpr u32 REPLAY_MAGIC = 0x52504C59;
inline constexpr u32 REPLAY_VERSION = 1;

// 录制开始游戏状态快照
struct ReplayState
{
    f32 player_x;
    f32 player_y;
    v2 velocity;
    Camera2D camera;
};

// 每个固定逻辑步记录的输入快照
struct ReplayInputFrame
{
    bool current[GA_COUNT]; // 本帧各动作的按下状态
    f32 left_stick_x;
    f32 left_stick_y;
    f32 right_stick_x;
    f32 right_stick_y;
    f32 left_trigger;
    f32 right_trigger;
};

// 录制文件头
struct ReplayHeader
{
    u32 magic;       // REPLAY_MAGIC
    u32 version;     // REPLAY_VERSION
    u32 state_size;  // sizeof(ReplayState)，用于结构变化时的兼容校验
    u32 frame_count; // 录制的输入帧数
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

    // 首次分配 4096 帧
    recorder->frames = init<ReplayInputFrame>(4096);
}

// 记录一帧输入（在 input_update 之后、game_update 之前调用）
internal void replay_record_input(ReplayRecorder *recorder, const GameControllerInput *controller)
{
    assert(recorder->is_recording);

    ReplayInputFrame *frame = array_push_slot(&recorder->frames);
    for (u8 i = 0; i < GA_COUNT; ++i) {
        frame->current[i] = controller->current[i];
    }
    frame->left_stick_x = controller->left_stick_x;
    frame->left_stick_y = controller->left_stick_y;
    frame->right_stick_x = controller->right_stick_x;
    frame->right_stick_y = controller->right_stick_y;
    frame->left_trigger = controller->left_trigger;
    frame->right_trigger = controller->right_trigger;
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
    header.version = REPLAY_VERSION;
    header.state_size = sizeof(ReplayState);
    header.frame_count = recorder->frames.size;

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

    // 校验魔数、版本与状态结构大小
    if (header.magic != REPLAY_MAGIC || header.version != REPLAY_VERSION || header.state_size != sizeof(ReplayState)) {
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

// 回放一帧：用录制的输入驱动控制器；到达末尾时自动循环（重置状态到初始）
internal void replay_tick(ReplayRecorder *recorder, GameState *game_state, GameControllerInput *controller)
{
    assert(recorder->replay_frame_count > 0);

    // 循环重放：播到末尾后回到开头，并把游戏状态重置为初始状态
    if (recorder->replay_index >= recorder->replay_frame_count) {
        recorder->replay_index = 0;
        replay_state_to_game(&recorder->replay_initial_state, game_state);

        // 清空控制器的上一帧/边沿状态，避免与上一圈结尾混淆
        for (u32 i = 0; i < GA_COUNT; ++i) {
            controller->previous[i] = false;
            controller->current[i] = false;
            controller->pressed[i] = false;
            controller->released[i] = false;
        }
    }

    // 上一帧存档，再恢复本帧输入
    for (u32 i = 0; i < GA_COUNT; ++i) {
        controller->previous[i] = controller->current[i];
    }

    ReplayInputFrame *frame = &recorder->replay_frames[recorder->replay_index];
    for (u32 i = 0; i < GA_COUNT; ++i) {
        controller->current[i] = frame->current[i];
    }
    controller->left_stick_x = frame->left_stick_x;
    controller->left_stick_y = frame->left_stick_y;
    controller->right_stick_x = frame->right_stick_x;
    controller->right_stick_y = frame->right_stick_y;
    controller->left_trigger = frame->left_trigger;
    controller->right_trigger = frame->right_trigger;

    // 计算边沿状态
    for (u32 i = 0; i < GA_COUNT; ++i) {
        controller->pressed[i] = controller->current[i] && !controller->previous[i];
        controller->released[i] = !controller->current[i] && controller->previous[i];
    }

    recorder->replay_index++;
}
