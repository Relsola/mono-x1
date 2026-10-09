# 输入系统实现说明

> 本文件讲**输入层怎么工作**；用脚本注入按键、写断言做回归的用法见 `docs/input-script.md`；录制回放见 §8。

## 1. 这一层解决什么问题

三件事：

1. **游戏逻辑只问「这个动作按下了吗」**，不问是哪个键、哪个设备（`GA_JUMP` 而不是 `VK_SPACE`）
2. **两个可替换后端**（GameInput 优先，Win32 消息 + XInput 降级），选一次就不再判断来源
3. **对齐两个时间尺度**：后端每渲染帧采样一次，逻辑却按固定 60Hz 步进 —— 靠"粘滞边沿"保证不漏按

## 2. 数据流

```
窗口消息 ──┐
XInput  ───┼─→ 后端 poll() ─→ RawInput ─→ input.cc 累积粘滞边沿 ─→ input_step() ─→ GameInput ─→ 游戏逻辑
GameInput ─┘                                      ▲
脚本注入 ────────────────────────────────────────┘（绕过后端，直接写动作状态）
```

## 3. 术语矩阵

| 概念 | 本项目的名字 | 说明 |
| --- | --- | --- |
| 动作语义 | `GameAction`（11 个） | LEFT / RIGHT / UP / DOWN / JUMP / DASH / SHOOT / TIME_STOP / COIN / CAMERA_ZOOM / MAP |
| 键位映射 | `KEY_ACTION_MAP` | 虚拟键码 → 动作，**两个后端共用同一张键盘表**（手柄按钮映射不在该表里） |
| 手柄按钮映射 | `PAD_ACTION_MAP` | 物理按钮（`GamepadButton`）→ 动作，**两个手柄后端共用同一张表**；后端只负责把原生掩码翻成 `GamepadButton` |
| 动作名表 | `ACTION_NAMES` | 枚举 ↔ 名字，**脚本语法 / 回放导出共用**（`game_action_name`）；漏填会静默返回 `"?"` |
| 后端 | `InputBackend` | `name` 字符串 + 4 个函数指针：init / shutdown / poll / on_message |
| 后端输出契约 | `RawInput` | down 快照 + "自上次 poll 以来"的边沿 + 摇杆/扳机/鼠标 |
| 粘滞边沿 | `global_edge_down[]` / `global_edge_up[]` | 累积到被某个逻辑步消费为止 |
| 游戏看到的输入 | `GameInput` | `{ PlayerInput player; MouseInput mouse; }` |
| 磁带（录制回放 / 输入脚本） | `InputTape`（`include/debug/replay.h`） | 整帧写 `GameInput`（含边沿），**不经过 `input_step`** |

## 4. 关键设计决策

| 决定 | 备选 | 为什么 | 代价 |
| --- | --- | --- | --- |
| **函数指针表**选后端 | 虚函数 / 编译期宏 | SKILL 禁虚函数；且后端只在 `input_init` 里选一次，之后每帧只有一次间接调用、运行期零分支 | 不能运行期切换后端 |
| **粘滞边沿**这一层 | 只用 `down` 快照 | 渲染帧与逻辑步数量不匹配：一帧可能跑 0 步或 2 步。只有快照的话，"一帧内按下又松开"会被完全丢掉（手感上就是漏按） | 边沿最多挂几帧才被消费（"不漏"优先于"不迟"） |
| 默认后端是 **`INPUT_NONE_BACKEND`**（4 个空函数） | 空指针 | `input_init` 之前窗口就已经会收到消息 —— 这里空指针崩过一次 | 一小段样板代码 |
| **GameInput 优先** | 只用 Win32 消息 | 键盘 + 手柄 + 鼠标一次轮询拿全，Raw Input 精度更高 | **外部注入的按键会被过滤**（自动化得走脚本层，或显式 `--input win32`） |
| **Win32 消息做降级** | `GetAsyncKeyState` 轮询 | 消息是事件语义，不会因为采样时机丢边沿；而且 `PostMessageW` 能注入，便于自动化 | 只在窗口有焦点时有效，失焦要清空所有键 |
| ~~**注入与后端解耦**~~ | 直接改后端状态 | ~~任何后端下自动化都能用（含 GameInput）~~ | 2026-09-22 已删除：磁带（`include/debug/replay.h`）整帧接管 `GameInput`，两套注入路径合并成一条 |
| **非 BACKEND 输入源不 poll 后端** | 一直 poll | 否则回放/脚本运行期间，攒下的粘滞边沿会在**切回真实设备的第一帧一次性灌进游戏**（凭空跳一下） | 无 |
| 边沿用 **OR 累积**、消费即清 | 计数器 | 简单，且不会漏 | 极短时间内的"按下-松开-按下"会合并成一次 |

## 5. 每帧 / 每步的调用链

```
每渲染帧（仅当输入源 = 真实设备时）：
  input_begin_frame()
    ├─ global_backend->poll(&raw)              一次间接调用
    ├─ global_backend_down[i] = raw.down[i]    当前状态直接覆盖
    └─ global_edge_down[i] |= raw.pressed[i]   边沿只累积、不清零
       global_edge_up[i]   |= raw.released[i]

每固定逻辑步：
  input_step(&game_input)
    ├─ down = global_backend_down[i]   ← 只有真实设备；磁带整帧接管时根本不走这里
    ├─ player->previous = current;  player->current = down
    ├─ pressed  = (down && !previous) || global_edge_down[i]
    ├─ released = (!down && previous) || global_edge_up[i]
    └─ 清掉这一对粘滞边沿（消费完成）

窗口消息（消息泵内）：
  input_on_message(msg, wparam, lparam) → global_backend->on_message(...)
```

**输入源二选一（互斥）**，在 `main.cc` 的主循环里决定：

| 输入源 | 每步做什么 |
| --- | --- |
| `INPUT_SOURCE_BACKEND` | `input_begin_frame()` + `input_step()`（录制就在这条路上抓变化点） |
| `INPUT_SOURCE_TAPE` | 都不用：磁带自己把整帧输入（含边沿）写进 `GameInput`，所以回放能带手柄与鼠标 |

磁带无论来自「回放（F6）」还是「脚本（`input_script`）」都是同一个源 —— 跑完循环还是跑完退出是磁带自己的属性（`InputTape::loop`）。

`--input <auto|gameinput|win32>` 与 `input_script <路径>` 可以同时指定：前者仍选择并初始化真实设备后端；
后者生效时磁带取得输入驱动权，因此期间不会轮询该后端。

## 6. 两个后端对照

| | `gameinput_input.cc`（首选） | `win32_input.cc`（降级） |
| --- | --- | --- |
| 键盘 | `GetCurrentReading(GameInputKindKeyboard)` → 虚拟键码查 `KEY_ACTION_MAP` | `WM_KEYDOWN` / `WM_KEYUP` + 自动重复过滤（`lParam & (1<<30)`） |
| 手柄 | GameInput 的 `GAMEINPUT_BUTTON_MAP` → 共用 `PAD_ACTION_MAP`，另有摇杆 + 扳机 | XInput 槽位 0，`XINPUT_BUTTON_MAP` → 共用 `PAD_ACTION_MAP` |
| 鼠标 | `GetMouseState`（**屏幕坐标** → `ScreenToClient`） | `WM_MOUSEMOVE` 的 `LOWORD/HIWORD`（**客户区坐标**） |
| 边沿怎么来 | 每次 poll 与 `prev_down[]` 比较 | 消息产生；一个动作绑多个键时，释放要先查"还有没有别的键按着" |
| 失焦 | — | `WM_KILLFOCUS` 释放全部按键与鼠标键 |
| 能接 `PostMessageW` 注入 | 否（Raw Input 会过滤） | 是 |
| 轮询成本 | 每次 `GetCurrentReading` 三个设备 | XInput 一次 + 纯查表 |

**键盘**共用 `KEY_ACTION_MAP`，所以键盘键位天然一致。**手柄**因为两个 SDK 的按钮常量值不同
（A 分别是 `0x1000` / `0x0004`），没法直接共用「原生值 → 动作」表，于是隔了一层：

```
XInput 掩码   ──XINPUT_BUTTON_MAP（win32_input.cc）──┐
                                                     ├─→ GamepadButton → PAD_ACTION_MAP（input.h）→ GameAction
GameInput 掩码 ──GAMEINPUT_BUTTON_MAP（gameinput .cc）┘
```

「哪个按钮做什么」只存在于 `PAD_ACTION_MAP` 一处，改手柄键位不会出现两个后端不一致；
两张原生小表只做「原生掩码 → 物理按钮」，并且带 `static_assert(array_size(...) == PAD_BTN_COUNT)` —— 
新增按钮时漏改某个后端会是编译错误，而不是那个入口静默按不出来。差异只在"事件 vs 轮询"和"能不能被注入"。

**摇杆轴向也是后端的契约**（和鼠标坐标必须统一到客户区是同一类问题）：`x` 的 +1 = 右，`y` 的 +1 = 上（与世界 `+y` 一致），
约定写在 `include/input.h` 的 `PlayerInput` 上。XInput 的 `sThumbLY` 官方文档写明"+ 是上"；
GameInput 官方文档页 404、头文件里也没注释，但**手柄实测确认与 XInput 一致**（推摇杆向下会触发下穿、向上是正常跳跃）
→ 两个后端都直接透传，不需要取负。
消费方式：水平轴由 `game.cc` 的 `stick_to_dir` 驱动移动；竖直轴用于判定"下"（左摇杆向下与十字键下同义，
可触发下穿单向平台）—— 死区与响应曲线都在游戏侧，输入层只原样透传。
注意"下"用的阀值**不是死区**：死区（0.2）是抗漂移的下限，而"下"是一个确定意图的输入，
所以另有一个 `game.cc` 的 `STICK_DOWN_PRESS = 0.7`（避免左右移动时拇指带出的向下偏移误触发）。

## 7. API 参考

| 函数 | 调用时机 | 说明 |
| --- | --- | --- |
| `input_init(void *native_window, InputBackendKind)` | 窗口创建后一次 | `AUTO` 时依次试 gameInput → win32，第一个成功的留下；`--input` 显式指定时**只试那一个**（起不来就报错并保留空实现，不静默换后端） |
| `input_shutdown()` | 退出前 | 只关当前后端 |
| `input_begin_frame()` | 每渲染帧 | 轮询 + 累积粘滞边沿。**只在真实设备驱动时调用** |
| `input_step(GameInput *)` | 每固定逻辑步 | 把后端状态滚成 `previous/current`、算出边沿并清粘滞（**磁带在跑时不会调它**） |
| `input_on_message(msg, wp, lp)` | 消息泵内每条消息 | 转给后端的 `on_message`（轮询式后端是空实现） |

## 8. 与调试基建的关系

| 能力 | 走哪条路 | 特点 |
| --- | --- | --- |
| 磁带：录制（F5） | `INPUT_SOURCE_BACKEND` + 每步抓变化点 | 能录手柄/鼠标与键盘；存档点为磁带第一条 |
| 磁带：回放（F6）/ 脚本（`input_script`） | `INPUT_SOURCE_TAPE`，磁带直接写 `GameInput` | 同一条执行路径；断言在磁带里求值，语法见 `docs/input-script.md` |
| 手动游玩 | 真实后端 | 两种后端都行 |

三者互斥（真实设备 / 磁带），避免隐式覆盖。

## 9. 坑与限制

- **GameInput 走 Raw Input，`PostMessageW` 注入的按键会被它过滤** —— 所以"用外部工具发消息做自动化"只在 Win32 后端下可行（用 `--input win32` 显式指定；日志里的 `Input backend:` 行会告诉你实际是谁）；跨后端一律用脚本层
- **Win32 后端只在窗口有焦点时有效**（消息驱动）。失焦时 `WM_KILLFOCUS` 会清空所有键，避免"切出去时按着 → 切回来还按着"
- **GameInput 的键盘 reading 是"当前按下的全部键"快照**，代码里按 64 个截断并加一次性告警（超出的键会被当成松开）
- **两个后端的鼠标坐标单位必须一致**：GameInput 给的是屏幕坐标，已用 `ScreenToClient` 统一到客户区（否则同一个 `mouse_x` 在两后端下含义不同）
- **摇杆死区与重映射不在输入层**：`STICK_DEADZONE` 与 `stick_to_dir` 在 `game.cc` —— 那是手感，不是输入语义
- **一个动作可以绑多个键**（LEFT = `←` + `A`），所以释放路径要检查其它键（`win32_any_key_down_for`）
- **注入的状态是"保持型"**：脚本里 `press` 必须配对 `release`，否则那个动作会一直按住
- **`pressed == true` 而 `current == false` 是正常状态**（一帧内的极短点按），不要用 `current` 去判断"刚按下"
- **左摇杆已经被消费**：`game.cc` 的 `stick_to_dir` 用它驱动水平移动（死区也在那里，不在输入层）
- **鼠标 / 右摇杆 / 扳机 / `UP` 目前还没有消费者**：采集、录制、透传都是完整的，是给后续矄准/UI/攀爬预留的

## 10. 排查表

| 症状 | 可能原因 | 排查位置 |
| --- | --- | --- |
| 按键完全没反应 | 窗口没焦点（Win32 后端）；或 GameInput 下按键被过滤（外部注入的键就是这种）；或 `--input` 指定的后端起不来；或后端初始化失败 | 日志里的 `Input backend:` 行、`input_init` |
| 某个键"粘住"不放 | 脚本/磁带里 `press` 没有配对 `release`（磁带的状态会一直保持到下一个变化点） | 脚本的 `press` / `release` 行 |
| 按一次触发两次 | 自动重复没过滤 | `win32_input_on_message` 的 `lParam & (1<<30)` 判断 |
| 偶尔漏按 | 那个渲染帧没有调 `input_begin_frame()`（输入源不是真实设备），边沿没被采集 | 主循环里 `input_source == INPUT_SOURCE_BACKEND` 的分支 |
| 切回手动操作后凭空跳一下 | （2026-09-19 已修）非真实设备时不再 poll 后端；若复现先看该分支是不是又失效了 | 同上 |
| 手柄和键盘同时按会互相干扰 | 两个后端的边沿在同一个逻辑步合并（`\|\|`） | `input_step` |
| 两个后端下鼠标位置对不上 | 坐标单位不一致 | `ScreenToClient` 是否生效 |

## 11. 扩展指南

| 想做的事 | 落点 |
| --- | --- |
| 加一个动作 | **四处**：`GameAction` 枚举 + `ACTION_NAMES` + `KEY_ACTION_MAP` + `PAD_ACTION_MAP`（均在 `input.h`）。**漏任何一处都不会报错**：名字缺失只会显示 `"?"`，键位/按钮缺失就是那个入口按不出来 |
| 改键盘键位 | `KEY_ACTION_MAP`（两个后端共用键盘表，改一处即可） |
| 改手柄键位 | `PAD_ACTION_MAP`（两个手柄后端共用，**改一处即可**） |
| 加一个手柄按钮 | 三处：`GamepadButton` 加一项 → `PAD_ACTION_MAP` 加映射 → 两个后端各加一行原生掩码。后者漏了会被 `static_assert` 拦住（编译错误） |
| 加一个后端 | 在 `input.h` 声明入口函数 → 实现 `init / shutdown / poll / on_message` 四个函数 → 填 `InputBackend.name`（日志与排查表都靠它）→ 在 `input_init` 的数组里加一行（数组顺序即优先级），并在同一处的偏好表里写上它对应的 `InputBackendKind`（否则只能靠 `auto` 才能选中它） |
| 按键重映射 UI（README 代办） | `KEY_ACTION_MAP` 改成可写：现在是 `inline constexpr`，需要变成运行期可修改的表 |
| 手柄振动 | Win32 后端走 `XInputSetState`，GameInput 走 haptics —— 都需要先有"当前后端是谁"的概念，现在是刻意没有的 |

### 2026-09-22：能量波动作

- 新增 `GA_SHOOT`：键盘 `R`、手柄右肩 `RB`。它是上升沿动作，游戏层在冷却结束后生成一枚能量波。
- 新增 `GA_MAP`：键盘 `ESC`、手柄 `Start`（`PAD_BTN_START` = XInput 的 `XINPUT_GAMEPAD_START` /
  GameInput 的 `GameInputGamepadMenu`）。
  它同时也是**行为变更**：`ESC` 以前在 `main.cc` 的 `WindowProc` 里硬编码 `DestroyWindow`，
  那样它永远最先被窗口过程吃掉、UI 层拿不到；现在它走正常动作链路，
  退出游戏改用 Alt+F4 / 关闭窗口（`WM_CLOSE` 仍然照旧）。
  新增手柄按钮要改三处：`GamepadButton` 枚举、两张原生小表 —— 漏改会被 `static_assert` 拦住。
- `RB` 不再是“已采集、当前未绑定”的预留入口；`COIN` 仍由手柄 `X` 触发。
| 死区 / 响应曲线 | `game.cc` 的 `stick_to_dir`（输入层只负责原样透传摇杆值） |

## 12. 变更记录

### 2026-09-19：输入层解耦与修正

- **粘滞边沿**：明确"后端每渲染帧 poll、逻辑每固定步消费"，边沿 OR 累积到被消费（修掉了"一帧内点按丢失"）
- **输入源互斥落到实处**：非真实设备时**不再 poll 后端**，修掉"回放/脚本期间堆边沿、切回设备时爆发"
- **鼠标坐标统一**：GameInput 后端补 `ScreenToClient`，与 Win32 后端对齐到客户区坐标
- **GameInput 键盘截断**：上限 16 → 64，并在超限时打一次性告警（原来是静默丢键）
- **删除死代码**：`input_debug_press`（定时注入，已被脚本层取代）、`input_backend_name`（无调用者）
- **本文件建立**

### 2026-09-22：磁带接管输入，删除并存的注入路径

- **输入源从三选一变二选一**：`INPUT_SOURCE_BACKEND` / `INPUT_SOURCE_TAPE`（回放与脚本共用一条执行路径）
- **删除 `input_debug_set` 与 `global_debug_down`**：它已无使用者 —— 磁带整帧写 `GameInput`（含边沿），
  于是「注入」不再是输入层的第三条路（本次重构的目的就是把两套注入合并成一套）

### 2026-09-20：文档校正

- 修正“鼠标/摇杆都没有消费者”的旧结论：**左摇杆已由 `game.cc` 的 `stick_to_dir` 消费**，
  未消费的只剩鼠标 / 右摇杆 / 扳机 / `UP`
- 澄清后端共性边界：共用的是**键盘**表；**手柄按钮映射是两套**（`PAD_BUTTON_MAP` 与 GameInput 的 `if` 链）
- 排查表里“切回真实设备后边沿爆发”改为已修状态，并补上真正的 `input_source` 分支位置
- 扩展指南“加一个动作”由“三处、会报错”改为**五处、且全部静默失败**
- 补上 `InputBackend.name` 字段与“加一个后端”实际需要做的四步

### 2026-09-21：手柄按钮映射统一

- **`PAD_ACTION_MAP` 成为两个手柄后端唯一的一张「按钮 → 动作」表**（`input.h`）：后端只把各自 SDK
  的原生掩码翻成 `GamepadButton`（`XINPUT_BUTTON_MAP` / `GAMEINPUT_BUTTON_MAP`，各在自家 `.cc`），
  GameInput 侧原来那串硬编码 `if` 随之删除
- 两张原生表加 `static_assert(array_size(...) == PAD_BTN_COUNT)`：新增手柄按钮时漏改某个后端，
  由“那个入口静默按不出来”变成**编译错误**
- 因此本文档里“手柄按钮映射是两套独立实现”、“改手柄键位必须改两处”、“加一个动作要改五处”
  的旧说法作废（§3 / §6 / §11 已相应改写）。按钮到动作的映射本身没有变化，键位与之前完全一致
