#pragma once

#include "core.h"

// TODO 最多四个手柄支持连接，但合并为单个输入
// TODO 正式上线前随游戏安装程序或steam补充GameInputRedist

// ============================================================================
// 输入：动作语义 + 后端抽象
//
// 分层：
//   后端（gameinput_input.cc / win32_input.cc）
//     只负责把「当前按下状态 + 本次新增的边沿」写进 RawInput，不关心动作含义之外的东西。
//   input.cc
//     初始化时选定一个后端（函数指针），运行期不再判断来源；
//     把后端状态的边沿做成「粘滞边沿」，由固定逻辑步消费，保证不漏按、不重复触发。
// ============================================================================

enum GameAction : u8
{
    GA_LEFT,        // 左移动（键盘 ← / A）
    GA_RIGHT,       // 右移动（键盘 → / D）
    GA_UP,          // 上（预留：攀爬 / 向上瞄准）
    GA_DOWN,        // 下（下蹲 / 下穿单向平台）
    GA_JUMP,        // 跳跃（键盘 Space / Z + 手柄 A）
    GA_DASH,        // 冲刺（键盘 F / Shift + 手柄右扳机 RT）
    GA_SHOOT,       // 发射能量波（键盘 R + 手柄右肩 RB）
    GA_TIME_STOP,   // 伪时停（键盘 C + 手柄左肩 LB）
    GA_COIN,        // 拾取 / 交互（键盘 E + 手柄 X）
    GA_CAMERA_ZOOM, // 相机放大（键盘 Q，调试用）
    GA_MAP,         // 大地图 UI（键盘 ESC + 手柄 Start）：同一个键开关（双层语义）
    GA_COUNT        // 动作总数
};

enum MouseButton : u8
{
    MOUSE_LEFT,   // 左键
    MOUSE_MIDDLE, // 中键
    MOUSE_RIGHT,  // 右键
    MOUSE_BUTTON_COUNT
};

// 动作名表：脚本语法、回放导出、日志共用同一份，避免两处名字对不上
struct ActionName
{
    const char *name;
    GameAction action;
};

inline constexpr ActionName ACTION_NAMES[] = {
    { "LEFT",      GA_LEFT        },
    { "RIGHT",     GA_RIGHT       },
    { "UP",        GA_UP          },
    { "DOWN",      GA_DOWN        },
    { "JUMP",      GA_JUMP        },
    { "DASH",      GA_DASH        },
    { "SHOOT",     GA_SHOOT       },
    { "TIME_STOP", GA_TIME_STOP   },
    { "COIN",      GA_COIN        },
    { "ZOOM",      GA_CAMERA_ZOOM },
    { "MAP",       GA_MAP         },
};

// 动作名表少一行**不会报错**（那个动作在脚本 / 日志里只会显示 "?"），所以用数组长度把它钉在枚举上
static_assert(array_size(ACTION_NAMES) == (int)GA_COUNT, "ACTION_NAMES 必须覆盖每一个 GameAction");

inline const char *game_action_name(GameAction action)
{
    for (const auto &entry : ACTION_NAMES) {
        if (entry.action == action) {
            return entry.name;
        }
    }
    return "?";
}

// 鼠标按钮名（小写）：脚本的 `mouse_press left` 与日志共用一份，避免两处名字对不上
struct MouseButtonName
{
    const char *name;
    MouseButton button;
};

inline constexpr MouseButtonName MOUSE_BUTTON_NAMES[] = {
    { "left",   MOUSE_LEFT   },
    { "middle", MOUSE_MIDDLE },
    { "right",  MOUSE_RIGHT  },
};

static_assert(array_size(MOUSE_BUTTON_NAMES) == (int)MOUSE_BUTTON_COUNT, "MOUSE_BUTTON_NAMES 必须覆盖每一个 MouseButton");

inline const char *mouse_button_name(MouseButton button)
{
    for (const auto &entry : MOUSE_BUTTON_NAMES) {
        if (entry.button == button) {
            return entry.name;
        }
    }
    return "?";
}

struct MouseInput
{
    bool current[MOUSE_BUTTON_COUNT];  // 当前逻辑步状态
    bool previous[MOUSE_BUTTON_COUNT]; // 上一逻辑步状态
    bool pressed[MOUSE_BUTTON_COUNT];  // 本逻辑步的上升沿
    bool released[MOUSE_BUTTON_COUNT]; // 本逻辑步的下降沿

    f32 x;
    f32 y;
    f32 wheel_delta; // 本逻辑步的滚轮增量
};

struct PlayerInput
{
    bool is_pad; // 本逻辑步是否有手柄参与

    bool current[GA_COUNT];  // 当前逻辑步状态
    bool previous[GA_COUNT]; // 上一逻辑步状态
    bool pressed[GA_COUNT];  // 本逻辑步的上升沿
    bool released[GA_COUNT]; // 本逻辑步的下降沿

    // 左右摇杆在水平和垂直方向上的位置 [-1.0, 1.0]
    // 轴向约定（两个后端必须一致，否则同一个手势在不同后端下含义不同）：
    //   x：+1 = 右
    //   y：+1 = 上（与世界的 +y 一致）→ 所以“摇杆向下”是负值
    f32 left_stick_x;
    f32 left_stick_y;
    f32 right_stick_x;
    f32 right_stick_y;
    // 扳机键 [0.0, 1.0]
    f32 left_trigger;
    f32 right_trigger;
};

struct GameInput
{
    PlayerInput player; // 玩家输入（键盘 + 手柄）
    MouseInput mouse;   // 全局鼠标输入
};

// ----------------------------------------------------------------------------
// 键盘虚拟键码 → 动作：两个键盘后端共用同一张表，保证手感一致
// ----------------------------------------------------------------------------
struct KeyActionMap
{
    u8 vk;
    GameAction action;
};

inline constexpr KeyActionMap KEY_ACTION_MAP[] = {
    { 0x25, GA_LEFT        }, // ←
    { 0x41, GA_LEFT        }, // A
    { 0x27, GA_RIGHT       }, // →
    { 0x44, GA_RIGHT       }, // D
    { 0x26, GA_UP          }, // ↑
    { 0x57, GA_UP          }, // W
    { 0x28, GA_DOWN        }, // ↓
    { 0x53, GA_DOWN        }, // S
    { 0x20, GA_JUMP        }, // Space
    { 0x5A, GA_JUMP        }, // Z
    { 0x46, GA_DASH        }, // F
    { 0x10, GA_DASH        }, // Shift
    { 0x52, GA_SHOOT       }, // R
    { 0x43, GA_TIME_STOP   }, // C
    { 0x45, GA_COIN        }, // E
    { 0x51, GA_CAMERA_ZOOM }, // Q
    { 0x1B, GA_MAP         }, // Esc（不再是“退出游戏”：退出走 Alt+F4 / 关闭窗口）
};

// ----------------------------------------------------------------------------
// 手柄按钮 → 动作：两个手柄后端共用同一张表
//
// 键盘能直接共用「原生值 → 动作」是因为两个后端拿到的都是虚拟键码；手柄不行 ——
// XInput 与 GameInput 的按钮常量值完全不同（A 分别是 0x1000 / 0x0004），
// 所以中间隔一层「物理按钮」枚举：
//   后端 .cc：原生掩码 → GamepadButton（各自一张小表，一个按钮一行）
//   下面：     GamepadButton → GameAction（唯一一张表，改手柄键位只改这里）
// 两张原生表用 static_assert 强制覆盖全部 PAD_BTN_xxx：
// 「加了按钮却漏改某个后端」因此是编译错误，而不是那个入口静默按不出来。
// ----------------------------------------------------------------------------
enum GamepadButton : u8
{
    PAD_BTN_DPAD_LEFT,      // 十字键 ←
    PAD_BTN_DPAD_RIGHT,     // 十字键 →
    PAD_BTN_DPAD_UP,        // 十字键 ↑
    PAD_BTN_DPAD_DOWN,      // 十字键 ↓
    PAD_BTN_A,              // 下键 A
    PAD_BTN_X,              // 左键 X
    PAD_BTN_LEFT_SHOULDER,  // 左肩 LB
    PAD_BTN_RIGHT_SHOULDER, // 右肩 RB（已采集，当前未绑定动作）
    PAD_BTN_START,          // Start / Menu（大地图）
    PAD_BTN_COUNT
};

struct PadActionMap
{
    GamepadButton button;
    GameAction action;
};

inline constexpr PadActionMap PAD_ACTION_MAP[] = {
    { PAD_BTN_DPAD_LEFT,      GA_LEFT      },
    { PAD_BTN_DPAD_RIGHT,     GA_RIGHT     },
    { PAD_BTN_DPAD_UP,        GA_UP        },
    { PAD_BTN_DPAD_DOWN,      GA_DOWN      },
    { PAD_BTN_A,              GA_JUMP      },
    { PAD_BTN_LEFT_SHOULDER,  GA_TIME_STOP },
    { PAD_BTN_RIGHT_SHOULDER, GA_SHOOT     },
    { PAD_BTN_X,              GA_COIN      },
    { PAD_BTN_START,          GA_MAP       },
};

// RT 是模拟量，不能放进“物理按钮 → 动作”的按键表；输入汇聚层以这个阈值把它转换成 GA_DASH。
inline constexpr f32 PAD_RIGHT_TRIGGER_DASH_PRESS = 0.5f;

// 把手柄按钮快照按 PAD_ACTION_MAP 合并成动作状态（两个后端共用这一处）
inline void pad_buttons_apply(const bool *button_down, bool *action_down)
{
    for (const auto &mapping : PAD_ACTION_MAP) {
        if (button_down[mapping.button]) {
            action_down[mapping.action] = true;
        }
    }
}

// ----------------------------------------------------------------------------
// 后端接口
// ----------------------------------------------------------------------------

// 后端每渲染帧输出一次：down 为「当前状态」，pressed/released 为「自上次 poll 以来新增的边沿」
struct RawInput
{
    bool down[GA_COUNT];
    bool pressed[GA_COUNT];
    bool released[GA_COUNT];

    bool is_pad;

    f32 left_stick_x;
    f32 left_stick_y;
    f32 right_stick_x;
    f32 right_stick_y;
    f32 left_trigger;
    f32 right_trigger;

    bool mouse_down[MOUSE_BUTTON_COUNT];
    bool mouse_pressed[MOUSE_BUTTON_COUNT];
    bool mouse_released[MOUSE_BUTTON_COUNT];
    f32 mouse_x;
    f32 mouse_y;
    f32 mouse_wheel_delta;
};

struct InputBackend
{
    const char *name;
    bool (*init)(void *native_window);                                // Win32: HWND
    void (*shutdown)();
    void (*poll)(RawInput *raw);                                      // 每渲染帧一次
    void (*on_message)(u32 msg, u64 wparam, u64 lparam);              // 消息泵转发，非消息后端为空实现
};

// 各后端入口（由 input.cc 在初始化时按优先级尝试）
const InputBackend *input_backend_gameInput();
const InputBackend *input_backend_win32();

// ----------------------------------------------------------------------------
// 对外接口
// ----------------------------------------------------------------------------

// 输入后端偏好（命令行 `--input <auto|gameinput|win32>`）
enum InputBackendKind : u8
{
    INPUT_BACKEND_AUTO,      // 自动：按优先级依次尝试（GameInput → Win32）
    INPUT_BACKEND_GAMEINPUT, // 只用 GameInput（Raw Input：注入式按键会被过滤）
    INPUT_BACKEND_WIN32,     // 只用「窗口消息 + XInput」：WM_KEYDOWN 可被 PostMessage 注入，便于自动化
};

// 选择并初始化后端。
// preferred 为 AUTO 时按优先级回落；**显式指定时只用那一个** ——
// “为什么注入的按键没反应”因此有一个明确答案（日志会说清楚它没起来），
// 而不是静默换成另一个后端。
void input_init(void *native_window, InputBackendKind preferred);
void input_shutdown();

// 每渲染帧一次：轮询后端，累积原始状态与粘滞边沿
void input_begin_frame();

// 每固定逻辑步一次：把原始状态写入 GameInput 并计算边沿
void input_step(GameInput *input);

// 窗口消息转发（消息泵调用）
void input_on_message(u32 msg, u64 wparam, u64 lparam);
