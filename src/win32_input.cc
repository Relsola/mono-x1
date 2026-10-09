#include "input.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

#include "win32_prefix.h"
#include <xinput.h>

#pragma comment(lib, "xinput")

// ============================================================================
// Win32 降级输入后端：窗口消息（键盘/鼠标）+ XInput（手柄）
//
// 键盘与鼠标走窗口消息而不是 GetAsyncKeyState / Raw Input，好处是：
//   1. 消息是「事件」语义，按下与松开不会因为采样时机而丢失；
//   2. 可以用 PostMessageW 从外部注入按键做自动化验证（Raw Input 会过滤注入消息）。
// 代价是只在窗口有焦点时有效，失焦时会清空所有按键状态。
// ============================================================================

// XInput 掩码 → 物理按钮。按钮到动作的映射在 input.h 的 PAD_ACTION_MAP（两个手柄后端共用），
// 所以加动作不用动这里；只有加**按钮**才要动 —— 漏了会被下面的 static_assert 拦住。
struct XInputButtonMap
{
    u16 mask;
    GamepadButton button;
};

global_variable constexpr XInputButtonMap XINPUT_BUTTON_MAP[] = {
    { XINPUT_GAMEPAD_DPAD_LEFT, PAD_BTN_DPAD_LEFT },
    { XINPUT_GAMEPAD_DPAD_RIGHT, PAD_BTN_DPAD_RIGHT },
    { XINPUT_GAMEPAD_DPAD_UP, PAD_BTN_DPAD_UP },
    { XINPUT_GAMEPAD_DPAD_DOWN, PAD_BTN_DPAD_DOWN },
    { XINPUT_GAMEPAD_A, PAD_BTN_A },
    { XINPUT_GAMEPAD_X, PAD_BTN_X },
    { XINPUT_GAMEPAD_LEFT_SHOULDER, PAD_BTN_LEFT_SHOULDER },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_BTN_RIGHT_SHOULDER },
    { XINPUT_GAMEPAD_START, PAD_BTN_START },
};

static_assert(array_size(XINPUT_BUTTON_MAP) == PAD_BTN_COUNT,
              "XINPUT_BUTTON_MAP must cover every GamepadButton");

struct Win32InputState
{
    bool key_down[256];

    bool action_down[GA_COUNT];    // 键盘侧的动作状态
    bool action_pressed[GA_COUNT]; // 自上次 poll 以来新增的边沿
    bool action_released[GA_COUNT];

    bool pad_down[GA_COUNT]; // 上一次 poll 时手柄的动作状态
    bool pad_pressed[GA_COUNT];
    bool pad_released[GA_COUNT];

    bool mouse_down[MOUSE_BUTTON_COUNT];
    bool mouse_pressed[MOUSE_BUTTON_COUNT];
    bool mouse_released[MOUSE_BUTTON_COUNT];
    f32 mouse_x;
    f32 mouse_y;
    f32 wheel_delta;
};

global_variable Win32InputState global_win32_input = {};

internal void win32_set_action(GameAction action, bool down)
{
    if (global_win32_input.action_down[action] == down) {
        return;
    }

    global_win32_input.action_down[action] = down;
    if (down) {
        global_win32_input.action_pressed[action] = true;
    } else {
        global_win32_input.action_released[action] = true;
    }
}

// 某个动作是否还有别的键按着（一个动作可以绑多个键）
internal bool win32_any_key_down_for(GameAction action)
{
    for (const auto &mapping : KEY_ACTION_MAP) {
        if (mapping.action == action && global_win32_input.key_down[mapping.vk]) {
            return true;
        }
    }
    return false;
}

internal void win32_release_all_keys()
{
    for (u32 vk = 0; vk < 256; ++vk) {
        global_win32_input.key_down[vk] = false;
    }
    for (u32 i = 0; i < GA_COUNT; ++i) {
        win32_set_action((GameAction)i, false);
    }
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        if (global_win32_input.mouse_down[i]) {
            global_win32_input.mouse_down[i] = false;
            global_win32_input.mouse_released[i] = true;
        }
    }
}

internal void win32_input_on_message(u32 msg, u64 wparam, u64 lparam)
{
    Win32InputState *state = &global_win32_input;

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        u8 vk = (u8)wparam;
        if (lparam & (1 << 30)) {
            break; // 自动重复，忽略
        }
        if (state->key_down[vk]) {
            break;
        }

        state->key_down[vk] = true;
        for (const auto &mapping : KEY_ACTION_MAP) {
            if (mapping.vk == vk) {
                win32_set_action(mapping.action, true);
            }
        }
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        u8 vk = (u8)wparam;
        state->key_down[vk] = false;
        for (const auto &mapping : KEY_ACTION_MAP) {
            if (mapping.vk == vk && !win32_any_key_down_for(mapping.action)) {
                win32_set_action(mapping.action, false);
            }
        }
        break;
    }
    case WM_KILLFOCUS:
        win32_release_all_keys();
        break;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        u32 button = (msg == WM_LBUTTONDOWN) ? MOUSE_LEFT : (msg == WM_MBUTTONDOWN ? MOUSE_MIDDLE : MOUSE_RIGHT);
        if (!state->mouse_down[button]) {
            state->mouse_down[button] = true;
            state->mouse_pressed[button] = true;
        }
        break;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        u32 button = (msg == WM_LBUTTONUP) ? MOUSE_LEFT : (msg == WM_MBUTTONUP ? MOUSE_MIDDLE : MOUSE_RIGHT);
        if (state->mouse_down[button]) {
            state->mouse_down[button] = false;
            state->mouse_released[button] = true;
        }
        break;
    }
    case WM_MOUSEMOVE:
        state->mouse_x = (f32)(i16)LOWORD(lparam);
        state->mouse_y = (f32)(i16)HIWORD(lparam);
        break;
    case WM_MOUSEWHEEL:
        // 一格滚轮是 120
        state->wheel_delta += (f32)(i16)HIWORD(wparam) / 120.0f;
        break;
    default:
        break;
    }
}

internal f32 win32_thumb_to_axis(SHORT value)
{
    // -32768 ~ 32767 归一化到 [-1, 1]
    return clamp((f32)value / 32767.0f, -1.0f, 1.0f);
}

internal void win32_input_poll(RawInput *raw)
{
    Win32InputState *state = &global_win32_input;

    // --------------------------------------------------------------
    // 手柄（XInput 轮询）
    bool pad_down[GA_COUNT] = {};
    f32 left_stick_x = 0.0f;
    f32 left_stick_y = 0.0f;
    f32 right_stick_x = 0.0f;
    f32 right_stick_y = 0.0f;
    f32 left_trigger = 0.0f;
    f32 right_trigger = 0.0f;

    XINPUT_STATE pad_state = {};
    bool pad_connected = (XInputGetState(0, &pad_state) == ERROR_SUCCESS);
    if (pad_connected) {
        bool pad_button_down[PAD_BTN_COUNT] = {};
        for (const auto &mapping : XINPUT_BUTTON_MAP) {
            if (pad_state.Gamepad.wButtons & mapping.mask) {
                pad_button_down[mapping.button] = true;
            }
        }
        pad_buttons_apply(pad_button_down, pad_down);

        left_stick_x = win32_thumb_to_axis(pad_state.Gamepad.sThumbLX);
        left_stick_y = win32_thumb_to_axis(pad_state.Gamepad.sThumbLY);
        right_stick_x = win32_thumb_to_axis(pad_state.Gamepad.sThumbRX);
        right_stick_y = win32_thumb_to_axis(pad_state.Gamepad.sThumbRY);
        left_trigger = (f32)pad_state.Gamepad.bLeftTrigger / 255.0f;
        right_trigger = (f32)pad_state.Gamepad.bRightTrigger / 255.0f;
    }

    // --------------------------------------------------------------
    // 键盘 + 手柄合并（按钮边沿以「键盘事件」和「手柄状态变化」的并集为准）
    for (u32 i = 0; i < GA_COUNT; ++i) {
        raw->down[i] = state->action_down[i] || pad_down[i];
        raw->pressed[i] = state->action_pressed[i] || (pad_down[i] && !state->pad_down[i]);
        raw->released[i] = state->action_released[i] || (!pad_down[i] && state->pad_down[i]);

        // 已转交给输入层，清空本地边沿
        state->action_pressed[i] = false;
        state->action_released[i] = false;
        state->pad_down[i] = pad_down[i];
    }

    raw->is_pad = pad_connected;
    raw->left_stick_x = left_stick_x;
    raw->left_stick_y = left_stick_y;
    raw->right_stick_x = right_stick_x;
    raw->right_stick_y = right_stick_y;
    raw->left_trigger = left_trigger;
    raw->right_trigger = right_trigger;

    // --------------------------------------------------------------
    // 鼠标
    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        raw->mouse_down[i] = state->mouse_down[i];
        raw->mouse_pressed[i] = state->mouse_pressed[i];
        raw->mouse_released[i] = state->mouse_released[i];
        state->mouse_pressed[i] = false;
        state->mouse_released[i] = false;
    }
    raw->mouse_x = state->mouse_x;
    raw->mouse_y = state->mouse_y;
    raw->mouse_wheel_delta = state->wheel_delta;
    state->wheel_delta = 0.0f;
}

internal bool win32_input_init(void *)
{
    // XInput 手柄缺失、窗口失焦都不算失败，键盘鼠标仍然可用
    return true;
}

internal void win32_input_shutdown()
{
    global_win32_input = {};
}

const InputBackend *input_backend_win32()
{
    local_persist const InputBackend backend = {
        "win32-msg+xinput",
        win32_input_init,
        win32_input_shutdown,
        win32_input_poll,
        win32_input_on_message,
    };
    return &backend;
}
