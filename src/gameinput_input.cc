#include "input.h"
#include "shared/logger.h"

#include "win32_prefix.h"
#include <gameinput.h>

#pragma comment(lib, "GameInput.lib")

// ============================================================================
// GameInput 后端（首选）：键盘 + 手柄 + 鼠标一次轮询拿到
//
// 注意：GameInput 的键盘读数来自 Raw Input，外部 PostMessage 注入的按键会被过滤，
// 因此自动化验证依赖 Win32 后端或 input_script 的注入层。
// ============================================================================

// 一次最多处理的按键数。GameInput 的键盘 reading 是「当前按下的全部键」快照，
// 正常远小于这个值；超过就会丢掉一部分（被丢掉的键会被当成松开），因此给足余量并告警。
constexpr u32 MAX_GAME_KEY_COUNT = 64;

// GameInput 掩码 → 物理按钮。按钮到动作的映射在 input.h 的 PAD_ACTION_MAP（两个手柄后端共用），
// 所以加动作不用动这里；只有加**按钮**才要动 —— 漏了会被下面的 static_assert 拦住。
struct GameInputButtonMap
{
    GameInputGamepadButtons mask;
    GamepadButton button;
};

global_variable constexpr GameInputButtonMap GAMEINPUT_BUTTON_MAP[] = {
    { GameInputGamepadDPadLeft,      PAD_BTN_DPAD_LEFT      },
    { GameInputGamepadDPadRight,     PAD_BTN_DPAD_RIGHT     },
    { GameInputGamepadDPadUp,        PAD_BTN_DPAD_UP        },
    { GameInputGamepadDPadDown,      PAD_BTN_DPAD_DOWN      },
    { GameInputGamepadA,             PAD_BTN_A              },
    { GameInputGamepadX,             PAD_BTN_X              },
    { GameInputGamepadLeftShoulder,  PAD_BTN_LEFT_SHOULDER  },
    { GameInputGamepadRightShoulder, PAD_BTN_RIGHT_SHOULDER },
    { GameInputGamepadMenu,          PAD_BTN_START          },
};

static_assert(array_size(GAMEINPUT_BUTTON_MAP) == PAD_BTN_COUNT,
              "GAMEINPUT_BUTTON_MAP must cover every GamepadButton");

struct GameInputBackendState
{
    IGameInput *input;
    HWND window; // 鼠标坐标要从屏幕空间转成客户区空间，需要窗口句柄
    bool prev_down[GA_COUNT];
    bool prev_mouse_down[MOUSE_BUTTON_COUNT];
    f32 prev_wheel; // 上一帧滚轮累计位置，用于算增量
};

global_variable GameInputBackendState global_gi = {};

internal bool gameinput_init(void *native_window)
{
    global_gi.window = (HWND)native_window;

    HRESULT result = GameInputCreate(&global_gi.input);
    if (FAILED(result) || !global_gi.input) {
        global_gi.input = nullptr;
        return false;
    }
    return true;
}

internal void gameinput_shutdown()
{
    if (global_gi.input) {
        global_gi.input->Release();
        global_gi.input = nullptr;
    }
}

internal void gameinput_poll(RawInput *raw)
{
    bool down[GA_COUNT] = {};
    f32 left_stick_x = 0.0f;
    f32 left_stick_y = 0.0f;
    f32 right_stick_x = 0.0f;
    f32 right_stick_y = 0.0f;
    f32 left_trigger = 0.0f;
    f32 right_trigger = 0.0f;
    bool is_pad = false;
    f32 mouse_x = 0.0f;
    f32 mouse_y = 0.0f;
    f32 wheel_delta = 0.0f;
    bool mouse_down[MOUSE_BUTTON_COUNT] = {};

    // --------------------------------------------------------------
    // 键盘：虚拟键码映射为游戏动作
    IGameInputReading *reading = nullptr;
    if (SUCCEEDED(global_gi.input->GetCurrentReading(GameInputKindKeyboard, nullptr, &reading)) && reading) {
        u32 key_count = reading->GetKeyCount();
        if (key_count > MAX_GAME_KEY_COUNT) {
            // 只告警一次：这里每渲染帧都走，不能刷日志
            local_persist bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARN("GameInput: %u keys pressed, only %u handled", key_count, MAX_GAME_KEY_COUNT);
            }
            key_count = MAX_GAME_KEY_COUNT;
        }

        GameInputKeyState key_states[MAX_GAME_KEY_COUNT];
        if (key_count > 0 && SUCCEEDED(reading->GetKeyState(key_count, key_states))) {
            for (u32 i = 0; i < key_count; ++i) {
                for (const auto &mapping : KEY_ACTION_MAP) {
                    if (key_states[i].virtualKey == mapping.vk) {
                        down[mapping.action] = true;
                    }
                }
            }
        }
        reading->Release();
    }

    // --------------------------------------------------------------
    // 手柄：按钮 + 摇杆 + 扳机（死区与速度映射在游戏逻辑层处理）
    reading = nullptr;
    if (SUCCEEDED(global_gi.input->GetCurrentReading(GameInputKindGamepad, nullptr, &reading)) && reading) {
        GameInputGamepadState gamepad = {};
        if (SUCCEEDED(reading->GetGamepadState(&gamepad))) {
            bool pad_button_down[PAD_BTN_COUNT] = {};
            for (const auto &mapping : GAMEINPUT_BUTTON_MAP) {
                if (gamepad.buttons & mapping.mask) {
                    pad_button_down[mapping.button] = true;
                }
            }
            pad_buttons_apply(pad_button_down, down);

            left_stick_x = gamepad.leftThumbstickX;
            left_stick_y = gamepad.leftThumbstickY;
            right_stick_x = gamepad.rightThumbstickX;
            right_stick_y = gamepad.rightThumbstickY;
            left_trigger = gamepad.leftTrigger;
            right_trigger = gamepad.rightTrigger;
            is_pad = true;
        }
        reading->Release();
    }

    // --------------------------------------------------------------
    // 鼠标：按键 + 客户区位置 + 滚轮
    reading = nullptr;
    if (SUCCEEDED(global_gi.input->GetCurrentReading(GameInputKindMouse, nullptr, &reading)) && reading) {
        GameInputMouseState mouse_state = {};
        if (SUCCEEDED(reading->GetMouseState(&mouse_state))) {
            if (mouse_state.buttons & GameInputMouseLeftButton) {
                mouse_down[MOUSE_LEFT] = true;
            }
            if (mouse_state.buttons & GameInputMouseMiddleButton) {
                mouse_down[MOUSE_MIDDLE] = true;
            }
            if (mouse_state.buttons & GameInputMouseRightButton) {
                mouse_down[MOUSE_RIGHT] = true;
            }

            // GameInput 给的是屏幕坐标，统一转成客户区坐标（与 Win32 后端一致），
            // 否则两个后端下同一个 mouse_x/mouse_y 含义不同
            POINT mouse_point = { (LONG)mouse_state.positionX, (LONG)mouse_state.positionY };
            if (global_gi.window) {
                ScreenToClient(global_gi.window, &mouse_point);
            }
            mouse_x = (f32)mouse_point.x;
            mouse_y = (f32)mouse_point.y;

            // wheelY 是累计位置，与上一帧做差才是增量
            f32 wheel = (f32)mouse_state.wheelY;
            wheel_delta = wheel - global_gi.prev_wheel;
            global_gi.prev_wheel = wheel;
        }
        reading->Release();
    }

    // --------------------------------------------------------------
    // 与上一次轮询比较得出边沿，并写入原始状态
    for (u32 i = 0; i < GA_COUNT; ++i) {
        raw->down[i] = down[i];
        raw->pressed[i] = down[i] && !global_gi.prev_down[i];
        raw->released[i] = !down[i] && global_gi.prev_down[i];
        global_gi.prev_down[i] = down[i];
    }
    raw->is_pad = is_pad;
    raw->left_stick_x = left_stick_x;
    raw->left_stick_y = left_stick_y;
    raw->right_stick_x = right_stick_x;
    raw->right_stick_y = right_stick_y;
    raw->left_trigger = left_trigger;
    raw->right_trigger = right_trigger;

    for (u32 i = 0; i < MOUSE_BUTTON_COUNT; ++i) {
        raw->mouse_down[i] = mouse_down[i];
        raw->mouse_pressed[i] = mouse_down[i] && !global_gi.prev_mouse_down[i];
        raw->mouse_released[i] = !mouse_down[i] && global_gi.prev_mouse_down[i];
        global_gi.prev_mouse_down[i] = mouse_down[i];
    }
    raw->mouse_x = mouse_x;
    raw->mouse_y = mouse_y;
    raw->mouse_wheel_delta = wheel_delta;
}

internal void gameinput_on_message(u32, u64, u64)
{
    // 轮询式后端，不处理窗口消息
}

const InputBackend *input_backend_gameInput()
{
    local_persist const InputBackend backend = {
        "gameinput",
        gameinput_init,
        gameinput_shutdown,
        gameinput_poll,
        gameinput_on_message,
    };
    return &backend;
}
