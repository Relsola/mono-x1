#include "debug/trace.h"
#include "shared/file.h"
#include "shared/logger.h"

#if MONO_DEBUG_BUILD

#include <stdio.h> // snprintf
#include <wchar.h> // wcsncpy_s

// 行缓冲：攒够一批再落盘，避免每步一次系统调用
constexpr u32 TRACE_BUFFER_SIZE = KB(8);
constexpr u32 TRACE_LINE_SIZE = 256;

global_variable bool global_trace_open = false;
global_variable wchar_t global_trace_path[260] = {};
global_variable char global_trace_buffer[TRACE_BUFFER_SIZE] = {};
global_variable u32 global_trace_used = 0;

internal void trace_flush()
{
    if (global_trace_used == 0) {
        return;
    }
    write_file(global_trace_path, global_trace_used, global_trace_buffer, true);
    global_trace_used = 0;
}

bool trace_open(const wchar_t *path)
{
    trace_close();

    wcsncpy_s(global_trace_path, array_size(global_trace_path), path, _TRUNCATE);

    // 先截断成一个空文件，再追加表头，后续都用追加模式写入
    if (!write_file(global_trace_path, 0, nullptr, false)) {
        LOG_ERROR("trace: cannot create file");
        return false;
    }

    const char *header =
        "frame,x,y,vx,vy,state,grounded,facing,"
        "coyote,jump_buffer,air_jumps,drop_through,dash_timer,dash_cooldown,"
        "cam_x,cam_y,zoom,"
        "held,pressed,released\n";
    global_trace_open = true;
    u32 header_size = 0;
    while (header[header_size]) {
        ++header_size;
    }
    for (u32 i = 0; i < header_size; ++i) {
        global_trace_buffer[i] = header[i];
    }
    global_trace_used = header_size;
    trace_flush();

    LOG_DEBUG("trace: writing %ls", global_trace_path);
    return true;
}

internal u32 held_mask(const bool *states)
{
    u32 mask = 0;
    for (u32 i = 0; i < GA_COUNT; ++i) {
        if (states[i]) {
            mask |= (1u << i);
        }
    }
    return mask;
}

void trace_step(u32 frame, const GameInput *input, const GameState *state)
{
    if (!global_trace_open) {
        return;
    }

    if (global_trace_used + TRACE_LINE_SIZE > TRACE_BUFFER_SIZE) {
        trace_flush();
    }

    const PlayerInput *player = &input->player;
    int written = snprintf(global_trace_buffer + global_trace_used, TRACE_BUFFER_SIZE - global_trace_used,
                           "%u,%.2f,%.2f,%.2f,%.2f,%s,%d,%d,%.3f,%.3f,%u,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%02X,%02X,%02X\n",
                           frame,
                           state->player_x, state->player_y,
                           state->velocity.x, state->velocity.y,
                           player_state_name(state->state),
                           state->grounded ? 1 : 0,
                           (state->facing == FACE_RIGHT) ? 1 : 0,
                           state->coyote_timer, state->jump_buffer_timer, state->air_jumps_left,
                           state->drop_through_timer, state->dash_timer, state->dash_cooldown,
                           state->camera.pos_x, state->camera.pos_y, state->camera.zoom,
                           held_mask(player->current), held_mask(player->pressed), held_mask(player->released));

    if (written > 0) {
        global_trace_used += (u32)written;
    }
}

void trace_close()
{
    if (!global_trace_open) {
        return;
    }
    trace_flush();
    global_trace_open = false;
    global_trace_used = 0;
}

#endif // MONO_DEBUG_BUILD
