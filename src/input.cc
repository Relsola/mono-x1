#include "input.h"
#include "shared/logger.h"

// ============================================================================
// 输入层：后端选择 + 原始状态累积 + 边沿分发
//
// 关键点：
//   1. 后端在初始化时选定一次（函数指针），运行期只做一次间接调用，没有来源判断分支；
//   2. 后端的边沿是「自上次 poll 以来新增的变化」，这里把它做成粘滞边沿：
//      累积到被某个固定逻辑步消费为止。这样高频渲染 + 多逻辑步不会漏按或重复触发；
//   3. 磁带（录制回放 / 输入脚本）不走这一层：它整帧接管 GameInput（含边沿），
//      见 include/debug/replay.h —— 所以这里没有「注入」这条并存的路径。
// ============================================================================

// 默认后端：在 input_init 之前窗口就可能已经收到消息，用一个空实现兼容启动顺序
internal bool input_none_init(void *) { return false; }
internal void input_none_shutdown() {}
internal void input_none_poll(RawInput *) {}
internal void input_none_on_message(u32, u64, u64) {}

global_variable const InputBackend INPUT_NONE_BACKEND = {
    "none",
    input_none_init,
    input_none_shutdown,
    input_none_poll,
    input_none_on_message,
};

global_variable const InputBackend *global_backend = &INPUT_NONE_BACKEND;

// 后端最近一次上报的原始状态
global_variable bool global_backend_down[GA_COUNT];
global_variable bool global_backend_mouse_down[MOUSE_BUTTON_COUNT];
global_variable f32 global_left_stick_x;
global_variable f32 global_left_stick_y;
global_variable f32 global_right_stick_x;
global_variable f32 global_right_stick_y;
global_variable f32 global_left_trigger;
global_variable f32 global_right_trigger;
global_variable f32 global_mouse_x;
global_variable f32 global_mouse_y;
global_variable f32 global_mouse_wheel_delta;
global_variable bool global_is_pad;

// 粘滞边沿（被固定逻辑步消费后清零）
global_variable bool global_edge_down[GA_COUNT];
global_variable bool global_edge_up[GA_COUNT];
global_variable bool global_mouse_edge_down[MOUSE_BUTTON_COUNT];
global_variable bool global_mouse_edge_up[MOUSE_BUTTON_COUNT];

void input_init(void *native_window, InputBackendKind preferred)
{
    // 数组顺序即优先级；preferred 非 AUTO 时只保留匹配的那一项。
    // 偏好与后端的对应关系就写在这张表里：新加后端时在这里补一行（而不是往 InputBackend 里加字段）
    struct BackendEntry
    {
        const InputBackend *backend;
        InputBackendKind kind;
    };

    const BackendEntry entries[] = {
        { input_backend_gameInput(), INPUT_BACKEND_GAMEINPUT },
        { input_backend_win32(),     INPUT_BACKEND_WIN32     },
    };

    for (const auto &entry : entries) {
        const InputBackend *backend = entry.backend;
        if (preferred != INPUT_BACKEND_AUTO && entry.kind != preferred) {
            continue;
        }

        if (backend->init(native_window)) {
            global_backend = backend;
            LOG_INFO("Input backend: %s", backend->name);
            return;
        }

        if (preferred == INPUT_BACKEND_AUTO) {
            LOG_WARN("Input backend '%s' unavailable, falling back", backend->name);
        } else {
            LOG_ERROR("Input backend '%s' unavailable (requested by --input), running without input", backend->name);
        }
    }

    // 保留空实现：游戏仍能运行，只是收不到输入
    LOG_ERROR("No input backend available");
}

void input_shutdown()
{
    if (global_backend != &INPUT_NONE_BACKEND) {
        global_backend->shutdown();
    }
    global_backend = &INPUT_NONE_BACKEND;
}

void input_begin_frame()
{
    RawInput raw = {};
    global_backend->poll(&raw);

    for (u32 i = 0; i < GA_COUNT; ++i) {
        global_backend_down[i] = raw.down[i];
        // 粘滞：上一帧没被消费掉的边沿继续保留
        global_edge_down[i] = global_edge_down[i] || raw.pressed[i];
        global_edge_up[i] = global_edge_up[i] || raw.released[i];
    }

    // RT 是连续轴：动作层只认一个确定的按下阈值。边沿由 input_step 的 current/previous
    // 自动算出，两个手柄后端因此保持同一份映射与手感。
    global_backend_down[GA_DASH] = global_backend_down[GA_DASH] ||
                                   raw.right_trigger >= PAD_RIGHT_TRIGGER_DASH_PRESS;

    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        global_backend_mouse_down[i] = raw.mouse_down[i];
        global_mouse_edge_down[i] = global_mouse_edge_down[i] || raw.mouse_pressed[i];
        global_mouse_edge_up[i] = global_mouse_edge_up[i] || raw.mouse_released[i];
    }

    global_left_stick_x = raw.left_stick_x;
    global_left_stick_y = raw.left_stick_y;
    global_right_stick_x = raw.right_stick_x;
    global_right_stick_y = raw.right_stick_y;
    global_left_trigger = raw.left_trigger;
    global_right_trigger = raw.right_trigger;

    global_mouse_x = raw.mouse_x;
    global_mouse_y = raw.mouse_y;
    global_mouse_wheel_delta += raw.mouse_wheel_delta;
    global_is_pad = raw.is_pad;
}

void input_step(GameInput *input)
{
    PlayerInput *player = &input->player;
    for (u32 i = 0; i < GA_COUNT; ++i) {
        bool down = global_backend_down[i];

        player->previous[i] = player->current[i];
        player->current[i] = down;
        player->pressed[i] = (down && !player->previous[i]) || global_edge_down[i];
        player->released[i] = (!down && player->previous[i]) || global_edge_up[i];

        global_edge_down[i] = false;
        global_edge_up[i] = false;
    }
    player->is_pad = global_is_pad;

    player->left_stick_x = global_left_stick_x;
    player->left_stick_y = global_left_stick_y;
    player->right_stick_x = global_right_stick_x;
    player->right_stick_y = global_right_stick_y;
    player->left_trigger = global_left_trigger;
    player->right_trigger = global_right_trigger;

    MouseInput *mouse = &input->mouse;
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        bool down = global_backend_mouse_down[i];

        mouse->previous[i] = mouse->current[i];
        mouse->current[i] = down;
        mouse->pressed[i] = (down && !mouse->previous[i]) || global_mouse_edge_down[i];
        mouse->released[i] = (!down && mouse->previous[i]) || global_mouse_edge_up[i];

        global_mouse_edge_down[i] = false;
        global_mouse_edge_up[i] = false;
    }

    mouse->x = global_mouse_x;
    mouse->y = global_mouse_y;
    mouse->wheel_delta = global_mouse_wheel_delta;
    global_mouse_wheel_delta = 0.0f;
}

void input_on_message(u32 msg, u64 wparam, u64 lparam)
{
    // 后端在初始化时选定，这里只有一次间接调用；非消息后端为空实现
    global_backend->on_message(msg, wparam, lparam);
}
