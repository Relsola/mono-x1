# AGENTS.md — 协作须知与已知坑

> 这个文件会被自动加载到每次新对话里。目的是让新会话**不必重新踩一遍已经踩过的坑**。
> 下面每一条都是**实际发生过**的问题（多数还留了崩溃或错误结果），不是预防性建议。
> 人工阅读同样有价值：这些就是当前代码里最脆弱的几处约定。

## 实现编码前基本要求

1. 在实现前先分析，拟定实现方案，保持良好的基本架构和可读性，但不要超前设计，必要时依然会重构。
2. 有重要的不确定信息要先向我确定你所需要的信息而不是靠推理猜测。
3. 有多种可能的方案先解释下各种利弊取舍和实现思路然后由我做取舍。

## 1. 项目速览

- Win32 Unicode + D3D12（Flip Model）的 2D 平台探索游戏工程，纯 C 风格 C++20（渲染层 2026-10-01 从 D3D11 换代，见 §4.7）。
- 当前关卡资产是 `data/map/first.bin` 与 `data/map/second.bin`；`src/shared/level_asset.cc` 读取编辑语义，
  `src/level.cc` 编译 tile 为运行时碰撞体；角色是 11 帧循环动画。
- `include/shared/` = **游戏与编辑器共用**的那一层（判据：编辑器是否也要用它）：`level.h` = 关卡层
  （`Level` / `WorldId` / `PlayerFacing` / `WorldConnection` / `MAX_VANISH_BLOCKS` /
  `level_build_from_asset`）、`collision.h`（AABB + 两个求解函数）、`memory.h`（arena + `Array`）、
  `file.h`（文件 IO + UTF-8 路径）、`level_asset.h`（格式 + 实体注册表 + 语义校验）、`logger.h`、
  `mono_math.h`（`v2` + `MIN` / `MAX` / `clamp` / `lerp`，**全项目唯一包含 `<math.h>` 的地方**）。
  编辑器**只**包含这一层与 `core.h`（2026-09-25 之前 editor_doc.h 包含整个 game.h，于是
  input / camera / sprite / ui 被一起拖进编辑器）。
- `include/scene.h` + `src/scene.cc` = 场景装配（程序化占位贴图 + 每帧绘制提交）。这两件事以前住在
  `main.cc` 的 `wWinMain`（那个函数原本 600 行，其中 250 行是场景图遍历）：现在 `main.cc` 只管窗口、
  初始化与主循环骨架，搬走之后 `SceneTextures` 把「有哪几张贴图」变成一处可见的数据。
- **关卡编辑器（2026-09-22 起）**：独立程序 `editor\build.bat` → `build\editor.exe`（Dear ImGui + Win32 + D3D11，
  自建设备/交换链，**不链**游戏渲染层/GameState）。它只编辑 `LevelAsset` 那一层，使用说明见 `editor/README.md`。
  **两个程序现在用的是两代 API**（游戏 D3D12 / 编辑器 D3D11，2026-10-01 定）；各建各的设备、各用各的调试层，见 §4.7。
  三个新入口：① 无界面自检 `build\editor.exe --check [<map.bin>] [--smoke] [--out <报告>]`
  （不建窗口，报告默认 `build\editor_check.txt`，退出码 0 = 每张图 0 error；给自动化与 AI 用）；
  ② 编辑逻辑自检 `build\editor.exe --selftest`（跑 `editor_edit` 的交互用例 + `editor_doc` 的撤销用例，
  同样不建窗口，退出码 0 = 全过）；
  ③ 编辑器自己的日志 `editor.log`（与游戏共用 `logger.cc`，但各写自己的文件）。
  ④ 一条命令跑完前两件（自检 + 校验/冒烟）：`editor\test\run_tests.bat`（无参数、从任何目录都能跑、退出码给自动化）。
  **两个回归入口别搞混**：游戏的用例与 runner 在 `test\`（`run_tests.bat` + `*.txt` 用例），
  编辑器的 runner 在 `editor\test\`，而编辑器的用例**不在 `.txt` 里**（它们编进 `build\editor.exe --selftest`）。
  编辑器的编辑逻辑（命中测试 / 拖动判定与解算 / 吸附）住在 `editor/src/editor_edit.h/.cc`，是**纯函数**
  （不碰 ImGui、不碰窗口、不读全局）：UI 层只做「屏幕坐标 → 世界坐标」的换算与绘制。
  所以改了交互先跑 `--selftest`（规则层），再开窗手动点一次并核对存盘字段（换算层）—— 后者验不了前者，反之亦然。
  撤销栈（快照环，128 步，含重做）挂在 `editor_doc_touch` **这一个出口**上：改文档的函数最后都会走到它。
  两条配套约定：①**加新的连续交互（按住拖动 / 按住输入框）必须用 `editor_doc_action_begin/end` 包住**，
  否则一次拖动会灌进几百格历史；②界面拿着结构体指针直接改字段后要调 `editor_doc_mark_changed`
  （它内部落格 + 重算 dirty）。`dirty` 是拿 `level_asset_equal` 与「上次打开/保存的那份」比出来的，不是自己置位的标志。
  两条要记住的新约定：①保存 = 写 `.tmp` → 读回来**逐字段比对** → 原子替换，且写前先跑语义校验；
  ②语义校验 `level_asset_validate()` 在**共享层**（规则是游戏侧 assert 的镜像），数字只在 `include/shared/level_asset.h`。
  **加一种实体 = 注册表加一行**：`LEVEL_ASSET_ENTITY_TABLE`（`include/shared/level_asset.h`）收着实体种类的
  名字/数组位置/元素大小/矩形偏移/数量上限，游戏与编辑器共用；校验里的数量与矩形检查、编辑器的
  命中测试、列表、增删数组都是表驱动。新加实体时不要再去加一份并行枚举或手写 switch。
  另外它是**类型化**的：游戏的运行时（`Level`/`GameState`）仍按类型写字段，不消费这张表。
- 已有八个文档：七个子系统文档 `docs/audio-system.md`、`docs/input-system.md`、`docs/input-script.md`、`docs/d3d12-renderer.md`、
  `docs/camera.md`（相机手感、四条不变量，以及亚像素抖动 / pixel shimmer 的根因与解法）、`docs/level-assets.md`（二进制格式与编辑资产/运行态分界）、
  `docs/save-system.md`（存档格式、三重校验、关卡指纹、会丢什么）；
  另有一份学习向的**专题资料整合** `docs/pixel-jitter.md`（这个渲染问题的历史脉络、业界各种解法与逐条资料来源标注）。

## 2. 动手之前

- **代码风格硬约束在 `SKILL.md`**：禁 STL / 异常 / RTTI / 虚函数 / 智能指针 / 模板元编程；
  `internal` = `static`、`global_variable` = `static`；`using` 别名代替 `typedef`；注释用中文；Win32 用 Unicode API。
  其中的"禁用虚函数"有两个已知例外（`src/audio.cc` 里两个 XAudio2 回调结构体继承 SDK 接口），改音频时要记住。
- **无语义的量用 `int`**（不是 `i32`，也不是 `u32` / `size_t`）：循环下标、格号、光标、CRT/Win32/ImGui
  直通的返回值都是这一类。有语义的宽度（句柄、格式字段、容量）继续用 `u8` / `u32` 这些别名。
  **注意 `for (int i = 0; i < u32边界; ++i)` 在 /W4 下报 C4018**（实测）—— 边界是 u32 的循环就保持 u32，
  不要为了统一去硬转（那会引入 ~200 处 `(int)` 转换，可读性反而变差）。
- **系统头与数学头「谁用谁包含」**（2026-10-01 起）：`core.h` 只包含**它自己的内联定义**要用到的两个
  （`<stdint.h>` 给类型别名、`<assert.h>` 给 `safe_cast_u64`）。**不要把新的系统头加进 `core.h`** ——
  往任意一个 `.cc` 里加一个轻量标准库头，都会顺着头文件传染给全部编译单元（包括编辑器）。
  **与数学有关的只从 `shared/mono_math.h` 来**：要 `sqrtf` / `fabsf` / `expf` 就直接用（它已经包含
  `<math.h>`），不要再自己包含 `<math.h>`，也不要往 `core.h` 加数学头。
- **内部接口不检查空指针**（自己项目，调用方约定不传空）：`write_file` / `file_move_replace` /
  `file_remove` / `utf8_to_wide` / `wide_to_utf8` / `log_init` / `renderer_destroy` / `game_audio_init` /
  `editor_message` / `editor_parse_command_line` 这些入口现在都没有 `if (!x) return;`。
  加回去之前先想清楚：**调用方真的会传空吗**？真会传空的地方要保留检查 —— 那里空指针是「正常值」
  而不是违规（`read_file` 失败时 `contents == nullptr`、`level_asset_entity_at` 越界返回空、
  `editor_check_run` 的 `report_path` 与 `editor_run_game` 的 `arguments` 是可以传 `nullptr` 的）。
  这条的价值是：**约定被违反时当场崩，而不是静默走一条「看起来安全」的岔路** —— 踩过：
  `editor_check_run` 曾用 `if (!buffer)` 把「arena 被撑爆」伪装成一句 out of memory，
  而 `arena_push` 本来会在 Debug 下直接 assert（那里才是真正的原因）。把它删掉之后
  「arena 不够」就只剩一个说法：断言。
- **新增 / 删除 / 改名 `.cc` 时要同步三处文件清单**（顺序以 `build.bat` 为准）：
  1. `build.bat` 的 `source_files` —— **顺序的唯一来源**；漏了要到链接期才报错；
  2. `compile_commands.json` —— 每个源文件一条精简条目（`directory` / `file` / `output` / `command`），照抄现有格式；
  3. `.clangd` —— **只放公共编译参数**（`/I` `/D` `/std` 等），它不列文件。
  **编辑器另有自己的一小份清单**：`editor/build.bat` 的 `editor_sources`（4 条）＋ `compile_commands.json` 里
  对应的 `editor/src/*.cc` 四条。只增删编辑器源文件时只动这两处（游戏那三个清单不动）。
  **不要在 `compile_commands.json` 的 `command` 里重复 `/I` `/D` 这类公共参数**（那些由 `.clangd` 统一提供），
  也不要给这两个文件加无关字段 —— 保持用户既有的精简格式。
- **写日志要选对级别**（`include/shared/logger.h`）：
  - `LOG_DEBUG` —— **排查/观察用**。会带上 `文件:行号` 前缀；门控是 `core.h` 的 `MONO_DEBUG_ANY`
    （= 四个调试宏里任意一个开着），全关时是空宏。
    **不要**用单个模块的宏去门控它 —— 那会在“只开另一个模块”的配置下把日志静默关掉。
  - `LOG_INFO` —— **正式发布版也要收集的少量必要信息**，它不会被任何宏关掉。现在的用途只有启动时的环境信息
    （`main.cc` 的 OS / CPU / RAM / 时间）和“选中了哪个输入后端”。**不要拿它当调试输出**，否则发布版日志会被噪声淹掉。- **日志文件由调用方指定**：`log_init(L"game.log")` / `log_init(L"editor.log")`（`logger.cc` 是两边共用的实现，
  `editor\build.bat` 把它列在 shared_sources 里）。**一个程序一个文件**：游戏每次启动都会清空日志，
  而编辑器随时会起游戏（冒烟验证）—— 共用一个文件时子进程会把编辑器刚写的记录抹掉（实测踩到过）。
  `LOG_INFO` 走缓冲、正常退出才落盘，所以强杀进程会丢掉最后几行（同「空日志 ≠ 崩在早期」）。  - `LOG_WARN` / `LOG_ERROR` —— 真实的异常路径（这两个级别会立即落盘）。
  - 没有运行期级别过滤：只要被编译进去，每一行都会写盘。
- **四个调试宏各自管什么**（互相独立，可任意组合开关；实测任意组合都能编过且零警告）：
  | 宏 | 管什么 | 门控点在 |
  | --- | --- | --- |
  | `MONO_DEBUG_TMP` | 临时验证（**当前无使用者**，留作约定） | — |
  | `MONO_DEBUG_VIS` | 碰撞盒线框（`debug_vis.h/.cc` + `scene.cc` 的线框绘制） | `game.cc` 4 处、`scene.h` 1 处 + `scene.cc` 3 处 |
  | `MONO_DEBUG_BUILD` | `--trace` 轨迹文件（`trace.h/.cc`）+ XAudio2 内部告警（`audio.cc`） | |
  | `MONO_DEBUG_INPUT` | 输入脚本（`input_script.h/.cc`）+ F7 取样 + 录制回放（`replay.h/.cc`、F4/F5/F6/F8） | |
  | `MONO_DEBUG_ANY`（派生） | `LOG_DEBUG` 与窗口置顶这两个“跟着调试走”的开关 | 只在 `core.h` 里定义 |
  **新增一个调试宏时，必须把它加进 `core.h` 的 `MONO_DEBUG_ANY` 那一行**，否则它在全开时也算“全关”。
  `MONO_DEBUG_BUILD` 这个名字**保留**（用户 2026-09-20 定：这个语义是存在的，只是目前用得少）；
  不要因为它现在只管 `--trace` 就建议改名或把它并进别的宏。
- 构建：当前用户已经是在 MSVC 环境下启动的，直接跑 `build.bat`。三种模式：
  `build.bat`（默认 debug，= 回归跑的那一档）、`build.bat release`（`/MT /O2 /DNDEBUG /GL /LTCG` + 四个调试宏全关）、
  `build.bat shaders`（只重编着色器）。**三种模式都会重编着色器**，因为运行期读的是 `fxc` 预编译好的
  `build/shaders/*.cso`（`vs_5_1` / `ps_5_1`；`d3d12_renderer.cc` 不包含 `<d3dcompiler.h>`、也不在启动时编译 HLSL）。
  产物是 `build\main.exe`，**在仓库根启动**（`build/shaders/*.cso` 与 `data/` 都按进程工作目录解析）。
- **`release` 只用于出包与真实性能测量**（用户 2026-10-01 定）：它带 `/DNDEBUG`，所以
  「约定被违反时当场崩，而不是静默走一条看起来安全的岔路」那条铁律在 release 里**不成立** ——
  全项目那些 `assert` 都是 debug 档专属的护栏，release 里违规就是未定义行为。
  推论：**「改完必须跑一次验证」永远用 debug 档**（回归也只在 debug 档跑，`test\run_tests.bat` 里那组参数就是照它定的）。
  `static_assert` 是编译期的，两个档都在。
- **四个调试宏的「全关」组合第一次被真正编译是 2026-10-01 的 `release` 档**（那之前只有 debug 档被构建过）：
  当场拓出 `CommandLine::fast` 的字段在 `#if MONO_DEBUG_INPUT` 里、三处使用在 `#if` 外 —— 也就是说
  「任意组合都能编过且零警告」这句话此前并不成立。**给结构体字段加 `#if` 门控时，顺手 grep 一遍它的所有使用点。**
  （同类：`/DNDEBUG` 会让「只被 assert 使用」的变量/内部函数变成 C4189 / C4505。
  实测在 2026-10-01 的 release 档一次过 —— 说明没有这种残留；真出现了就删残留，不要把 NDEBUG 挪走。）
- 改完**必须跑一次验证**（§5），不要只凭"代码看起来对"下结论。
- **关卡资产边界**：`data/map/*.bin` 只保存 tile、地刺、门与动态实体出生描述；运行期位置、速度、计时器、相机和能力状态只在 `GameState`。格式细节见 `docs/level-assets.md`，改格式必须提升版本或明确拒绝旧版本。

## 3. 分层铁律

- `<windows.h>` **只能通过 `include/win32_prefix.h` 包含**（它统一定义 `WIN32_LEAN_AND_MEAN` / `NOMINMAX`），且只允许在 `.cc` 里包含。
  - 不要往 `win32_prefix.h` 里加 `NOGDI` / `NOUSER` / `NOSOUND`：`logger.h` 等要用到被它们排除的类型。
  - 目前包含平台头的编译单元共 6 个：装配层 `main.cc` + `d3d12_renderer.cc` / `audio.cc` / `platform_win32.cc` / `win32_input.cc` / `gameinput_input.cc`。
  （编辑器是另一个程序：它自己的 `editor_main.cc` / `editor_doc.cc` 也含平台头，那是它的宿主层；
  编辑器自己的公共头（`editor_doc.h` / `editor_ui.h`）同样不准包含平台头。）
- **`include/` 下的公共头一律不许包含平台头**。需要窗口句柄就用 `void *`（`input.h` / `renderer.h` 的做法）；
  需要不透明实例就前向声明 `struct X;` 并让调用方只持指针（`audio.h` 的做法）。
- 数据流方向：`debug_vis` 只上报纯数据，**翻译成绘制项在装配层 `src/scene.cc`**；`d3d12_renderer.cc` 是唯一出现 D3D12/DXGI 类型的地方。
- **头文件的目录约定（2026-09-22 整理后）**：
  - `include/` 根 —— **只有游戏用**的头（`game.h` / `scene.h` / `input.h` / `renderer.h` / `save.h` /
    `camera.h` / `sprite.h` / `ui.h` / `font.h` / `audio.h` / `game_audio.h`）。
  - `include/shared/` —— **游戏与编辑器共用**的那一层，判据是「编辑器也要用它」：`level.h`（关卡层）、
    `collision.h`（AABB + 求解）、`memory.h`（arena + `Array`）、`file.h`（文件 IO + UTF-8 路径）、
    `level_asset.h`（格式 + 实体注册表 + 语义校验）、`logger.h`（日志实现是共享的 `src/shared/logger.cc`，
    两边各给一个文件名）。**归这里的头，它的 `.cc` 也必须列在 `editor/build.bat` 的 `shared_sources` 里**
    （现在是五条：level_asset / level / collision / platform_win32 / logger），否则编辑器链接不过；
    反过来，编辑器**只**能包含这一层与 `core.h`。
  - `include/debug/` —— 四个调试模块的头：`debug_vis.h` / `trace.h` / `replay.h` / `input_script.h`。
    实现也按 `src/shared/`、`src/debug/` 分开（与头文件目录一一对应），文件清单只管 `.cc`，所以移动头**不动任何清单**。
  - 引用时把目录写进 include 路径（`#include "debug/replay.h"`），**不要**往 `.clangd` 里加
    `-Iinclude/debug` —— 目录名要能一眼看出文件在哪，和 `include/lib/stb_image.h` 的做法一致。
  - `editor/include/` —— 编辑器自己的头（`editor_doc.h` / `editor_edit.h` / `editor_ui.h`），
    `editor/src/` 只放 `.cc`。头**不进任何清单**，只需 include 路径里有它：
    `/Ieditor/include` 加在 `editor/build.bat` 与 `.clangd`（公共参数）两处。
- **网格 ↔ 世界坐标的换算只走 `include/shared/level_asset.h` 的五个 helper**（`level_cell_center_x` /
  `level_cell_center_y` / `level_cell_bottom_y` / `level_row_span_center_x` / `level_grid_bounds`）：
  row 0 = **最上面**一行、格心 = `-(row+0.5)*格`、**格底边** = `-(row+1)*格`（出生点 / 门落点 / 传送点都是
  「脚底」语义）。这条公式以前在游戏侧与编辑器里各抄了十几处 —— 「改一处忘一处」的经典来源。
- **给实心项描边要显式传 order**：`renderer_push_rect_outline` 现在有 `order` 参数（与 `renderer_push_sprite`
  同义）。以前它硬写 `order = 0`，于是「边框压在门板之上」只靠纹理句柄大小（white 比 portal 后创建）
  —— 那是巧合不是约定。
- **调试模块（`debug_vis` / `trace` / `replay` / `input_script`）的五条约定**：
  1. 调试**头**内部自带 `#if MONO_DEBUG_X / #else`，`#else` 把**每一个入口**换成同名空宏（有返回值的给 `(false)` 之类的常量）。
     **`#include` 与调用点因此都不写 `#if` 也能安全生效** —— 名字与函数同名，不需要大写别名宏。
     **唯一必须守的不变量是 `#else` 分支必须齐全**：新加一个入口时别忘了同步补上，
     否则某个没写 `#if` 的调用点在关掉宏时会变成链接错误（这是它唯一的失败方式，不会静默出错）。
  2. 调试 `.cc` 用 `#if` 包住**整个文件**；`build.bat` 的 `source_files` 因此不做条件化、永远列全（关掉时那些 TU 是空文件）。
  3. **调用点怎么写，取决于关掉宏之后会不会留下零头**：
     - **单行通知、且实参不涉及局部变量 → 不加 `#if`**，直接写调用（关宏时由第 1 条展开成 `((void)0)`）。
       装配层 `main.cc` 里目前是这 6 处：`replay_before_step` / `replay_record_input` / `replay_after_step` /
       `debug_vis_begin_step` / `trace_step` / `trace_close`。
       （大写别名宏 `TRACE_STEP` / `INPUT_SCRIPT_BEFORE_STEP` 已经删了：四个调试头统一成“函数名 = 空宏名”）
       `game.cc` 里的 `debug_vis_static_box` / `debug_vis_box` **恰恰不属于**这一种：空宏会把**实参一起丢掉**，所以 `platform` / `tag` / `ground_probe` 就成了“已初始化但未引用”（C4189）—— 它们整块包了 `#if MONO_DEBUG_VIS`。
     - **多语句、带控制流或带日志的功能块 → 在调用点用 `#if` 包住整块**。
       关宏后"整块功能不存在"比"代码还在但全是空操作"更诚实。现在带 `#if` 的是：F4/F5/F6/F8/F7 热键块、
       `input_source` 的选择（`replay_is_playing`）、`input_script` 启动与退出码合并、`#if MONO_DEBUG_BUILD` 的 `trace_open`，
       以及 `game.cc` 的两处 `debug_vis` 上报。
     - 两种写法的**运行期成本都是零**，也都在编译器看到代码之前就消失；区别只是读代码时"这行为什么没了"的答案在哪一层。
     - **别留下“只被调试调用使用”的局部变量或包装函数（或把它们连同调用一起包进 `#if`）**：关掉宏后它们会变成“已初始化但未引用”（C4189）/
       “已删除未引用的内部链接函数”（C4505）警告 —— 产物没问题，但关宏的构建就不再是零警告。
       已踩过：
       `main.cc` 里的 `script_is_driving()` 包装（已删除）。
  4. 调试模块自己的状态放它自己的 `.cc` 里，装配层只调入口函数、不认识内部结构
     （例：`replay` 的录制缓冲住 `src/debug/replay.cc`，`main.cc` 只调 `replay_*()`）。
  5. **职责边界（2026-09-22 起）**：`replay` = **磁带**（存档点 + 输入变化点 + 断言）与执行引擎，
     录制 / 回放 / 脚本共用这一份；`input_script` 只剩「文本 ⇄ 磁带」两个方向。
     存档在 `save`（**正式系统、无门控**）—— 它不是调试模块，别给它套 `MONO_DEBUG_*`。

## 4. 踩过的坑

### 4.1 内存与对象生命周期

- **在 arena 上放含虚函数的对象必须 placement new**。`new (arena_push(sizeof(T))) T()` 才建立虚表；
  `*p = {}` 是**拷贝赋值**，而虚表指针不是数据成员、赋值不会写它 → arena 的全零内存留下空虚表指针。
  - 实际踩过：`AudioState` 这样写导致 XAudio2 在混音线程回调时跳空崩溃（确定性 AV，fault module = `xaudio2_9.dll`）。
  - `Renderer` 用 `*renderer = {}` 没事，只因为它没有虚函数。**给 `Renderer` 加虚成员时同样要改成 placement new。**
- `arena_*` 只能前进，没有 free；需要重复使用就自己做环形/复用（见音频的块缓冲只分配一次）。
- **成对资源用 `core.h` 的 `defer` 释放，不要手写「每条失败路径都清一遍」**（2026-10-01 起）：
  `defer { input_shutdown(); };` —— 就地把 `{}` 写成 lambda 体（展开成
  `Defer _defer_<行号> = [&] { ... };`），离开作用域时**逆序**执行。
  它**不能取消**（同 Go 的 defer）：需要「成功时不清理」就带一个标志位，不要把所有权交接藏进清理块：
  `bool ok = false; defer { if (!ok) { discard(x); } }; ... ok = true; return x;`。
  同一行只能写一个 `defer`（名字带行号）。已改造：`wWinMain` 的 6 个守卫（log / 窗口 / input /
  renderer / audio / scratch），它的 5 条退出路径现在都是干干净的 `return 0`。
  - **`ReadFileRes` 自己管自己的内存**（析构调 `free_file_memory`；拷贝被删，移动才转移所有权）：
    所以 `read_file` 的调用方**不要再写 `free_file_memory`**，失败路径直接 `return`。
    改造前 `level_asset_load` 一个函数里手写了 14 处释放、`save_read` 靠 `do { } while (false)` + `break`
    凑「单一出口」，现在都没了。另外 `read_file` 在 `GetFileSizeEx` 失败那条路上漏掉的 `CloseHandle` 也补上了。
  - **`renderer_create` 也改造完了（2026-10-01）**：用的是「`bool ok` 标志位 + 顶部一个 `defer`」，
    **没有**给 `Defer` 加 `cancel()` —— `defer` 宏把变量名藏成 `_defer_<行号>`，要 `cancel()` 就得手写行号
    （或者不用宏、退化成具名守卫），而这里 14 个失败点本来就都是 `return nullptr`，标志位版本更直白：
    失败路径退化成裸 `return nullptr;`，「半初始化的 renderer 怎么销毁」只剩一处
    （`*renderer = {}` 已经把指针清空，`renderer_destroy` 对空指针安全）。
    同一处还顺手把 `factory2` 收进 `defer { factory2->Release(); };` —— 原来「中途提前 Release +
    失败路径再 Release 一次」两处记账，现在只有一处（代价是它活到函数结束，而 swap chain 自己持了引用）。

### 4.2 窗口、输入

- **全屏窗口的创建矩形必须"贴显示器原点 + 铺满显示器"**（`MONITORINFO.rcMonitor`），
  不能复用窗口模式的居中坐标。两者共用一组坐标时，`WS_POPUP` 会从屏幕中间开始铺 3840x2160，
  表现是"跨屏 / 没铺满"。
- **输入源互斥要落到实处**：非 `INPUT_SOURCE_BACKEND` 时不要调 `input_begin_frame()`，
  否则回放/脚本期间攒下的粘滞边沿会在切回真实设备的第一帧一次性灌进游戏。
  - 已知**尚未完全闭合**的一处：`WindowProc` 里的 `input_on_message()` 是无条件转发的，
    而 Win32 后端内部的动作边沿只有 `poll` 会清。当前因为"脚本模式下不退出就一直在跑、只有 F6 结束回放才切回"
    而没有实际困扰，要彻底互斥就得把这一条也按 `input_source` 门控。
- 一个动作可以绑多个键，所以**释放路径要检查是否还有其它键按着**（`win32_any_key_down_for`）。
- **给输入结构加字段也要同步磁带**：`TapeInputState`（`include/debug/replay.h`）是 `PlayerInput` / `MouseInput`
  的一份子集，它 + `tape_capture_input` / `tape_input_equal` / `tape_apply_input`（`src/debug/replay.cc`）四处要一起改。
  漏了不会报错，表现是那个输入在录制/回放里永远是默认值 —— 而且**确定性守卫也拓不出来**（两遍漏得一模一样）。
- 两个后端的**鼠标坐标必须统一到客户区**（GameInput 要给屏幕坐标做 `ScreenToClient`），否则同一个 `mouse_x` 含义不同。
- **摇杆轴向也是后端契约**（同类问题）：约定写在 `include/input.h` 的 `PlayerInput` —— x 的 +1 = 右、
  y 的 +1 = 上（与世界 `+y` 一致）。XInput 的 `sThumbLY` 官方文档写明“+ = 上”；GameInput 官方文档页 404、
  头文件无注释，但 2026-09-21 手柄实测确认与 XInput 一致 → **两个后端都直接透传，不要取负**。
- 加一个动作要改**四处**（`GameAction` / `ACTION_NAMES` / `KEY_ACTION_MAP` / `PAD_ACTION_MAP`，全在 `input.h`），
  而且**漏任何一处都不会报错**：名字缺失只会显示 `"?"`（现在 `ACTION_NAMES` / `MOUSE_BUTTON_NAMES` 各有 `static_assert` 卡数量，
所以这一半已经是编译错误了），键位缺失就是那个入口按不出来。
- **手柄按钮映射已统一**：`input.h` 的 `PAD_ACTION_MAP` 是两个手柄后端**唯一**的一张「按钮 → 动作」表。
  后端只负责把各自 SDK 的原生掩码翻成 `GamepadButton`（`XINPUT_BUTTON_MAP` / `GAMEINPUT_BUTTON_MAP`，各在自家 `.cc`），
  所以**改手柄键位只改 `PAD_ACTION_MAP` 一处**。两张原生表带 `static_assert(array_size(...) == PAD_BTN_COUNT)`，
  新增按钮时漏改某个后端是**编译错误**（2026-09-21 之前是两套独立实现、漏了静默失效）。

### 4.3 音频

- **XAudio2 回调（混音线程）里只能"置标志 + SetEvent"**：不能调用任何 XAudio2 API、不能加锁、不能分配内存。
  在 `OnBufferEnd` 里 `DestroyVoice` 自己会死锁。
- `FlushSourceBuffers` 会**同步**触发 `OnBufferEnd`，所以**清 `retire_flag` 必须放在 flush 之后**，否则新起播的声音会被立刻回收。
- **submix 的 `ProcessingStage` 必须"发送方 < 接收方"**，否则 `CreateSubmixVoice` 返回 `0x88960001`。
  阶段 = 自底向上的深度（叶子 0，根最大）；`BUS_TABLE` 也要是拓扑序（父索引 < 子索引）。
- Win10 SDK 的 `IXAudio2VoiceCallback` **不继承 `IUnknown`**，别写 `QueryInterface/AddRef/Release`（会 `C3668`）。
- 该 SDK 头文件**不导出** `GetDeviceCount` / `GetDeviceDetails`，所以采样率固定 48000，不查询设备。
- 池的输入格式创建后固定 → **池表 = 允许的格式清单**；资产格式没有匹配的池时 `audio_play` 报错并返回无效句柄（不是 assert）。
- 本 SDK 没有 `XAUDIO2_VOICE_STATE_NONBLOCKING`，`GetState` 用 `XAUDIO2_VOICE_NOSAMPLESPLAYED`。
- **`log_write` 无锁，禁止在音频线程调用**。

### 4.4 游戏逻辑

- 单向平台的**侧向阻挡**（`platform_blocks_from_side`）只有当"移动前水平方向与平台完全不重叠"才算侧墙；
  否则跳穿途中按左右会被横推。它和 `platform_supports_from_above` 是**成对**的，改单向平台行为时必须一起考虑。
- 两块水平重叠的平台上下必须 **≥3 格（192px）**，否则角色站在下层会被上层卡住（碰撞盒高约 90px）。
- 音频/粒子等触发点必须放在**固定步长**里，不能放渲染循环，否则确定性回放无法重现。
- **给 `GameState` 加字段时，先判断它是不是「影响确定性回放」的那部分**：属于状态就加进
  `GAME_STATE_PERSISTENT_FIELDS`（`include/game.h`）—— **那份列表是唯一来源**，快照结构体、
  `game_state_save`、`game_state_load` 都由它生成（2026-09-22 改），所以「结构体加了、save/load 忘了抄」
  这种不对称已经不可能发生。剩下需要人判断的只有一件事：它到底算不算状态。
  漏了不会报错，表现是回放/脚本开头几帧对不上（很难查）—— 这一层只能靠 `test\run_tests.bat` 的
  确定性守卫（两遍 trace 对拍）拓出来。
  这份快照同时就是**存档系统的负载**（`include/save.h`），所以它也决定了「存档里有什么」；
  改了它等于改了存档格式 —— `save` 的 `payload_size` 校验会拒载旧存档（这是想要的行为，不是 bug）。
- **相机是有状态的东西**：平滑跟随（每帧向目标收敛，位置依赖上一帧相机位置），所以它同样得跟着快照存取。
  夹取发生在插值之前、且目标点先夹取 → “可见范围不越出关卡”是结构性保证，不靠调用方自觉；
  关卡比视野小/矮的那个轴直接对齐关卡中心。
- **传送（重生/将来传送门）的判据只能用「角色这一帧走了多远」**（`game.cc` 的 `CAMERA_TELEPORT_MARGIN`，
  正常上限 PLAYER_DASH_SPEED/60 ≈ 27px），**不能**用“相机离目标多远”—— 那个距离里混着跟随滞距
  （跑步稳态 ≈ 速度/rate ≈ 71px、满速下落 ≈ 178px），拿它当阈值会让相机在正常跑动/跳跃时**周期性瞬移**
  （实测：滞距涨到 64px 就跳 65px，一趟 29 次，画面锯齿状卡顿；数值上看就是“滞距永远长不到稳态”）。
  相机轨迹还与**视口尺寸**相关（夹取范围随 `--window` 变），拿 `--trace` 对拍相机列时两边窗口要一致。
- **相机手感数值只存在于一处**：`src/camera.cc` 的 `CAMERA_PROFILES` 表，按 `CameraProfile` 枚举（`camera.h`）索引（表是**不定长**的 + `static_assert`，所以
  「枚举加一档、表忘了加一行」是编译错误；以前写成 `[CAMERA_PROFILE_COUNT]` 时少一行只会补一档 rate = 0 的参数，
  相机永不收敛而且谁都不报错）。
  游戏侧只在 `game.cc` 的 `camera_profile_for()` 里**选档位名**，调用点不写数值 —— 想加一种手感就是
  「枚举加一行 + 表加一行 + `camera_profile_name` 加一个 case」。档位是**策略**（由场景决定、跨帧不变），
  所以不进 `GameStateSnapshot`；只有跨帧会变的东西（相机位置）才进。不要提前加没人用的空档位。
  现有两档：`DEFAULT`（水平/竖直同速平滑）与 `PRECISE`（竖直死区 ±128px，小跳不带动画面，实测竖直相机总行程 3044→1702px）；
- **相机在渲染前会被吸附到整数设备像素**（`main.cc` 的 `render_camera`）：相机是小数 + 精灵走 POINT 采样时，
  亚像素平移会让 1px 细节在相邻像素间跳（pixel shimmer），指数跟随的尾段又恰好全是亚像素位移。
  两条硬约束：吸附量必须记为 `round(cam * zoom) / zoom`（即 1 个**设备像素**，不是 1 个世界像素）；
  **绝不能吸附逻辑相机**（进快照/回放/trace 的那份要保持小数，否则确定性与对拍全断）。
  另外：**全屏 3840x2160 时关卡整幅在视野内**，相机会被永远夹在关卡中心 —— 想看相机行为必须用 `--window`。
  根因、公式与 A/B 数据见 `docs/camera.md`。
- 关卡 `bounds` 取的是**网格外沿**（x ∈ [0, 列数*tile]、y ∈ [-行数*tile, 0]）。旧写法取的是首/末格**中心**，
  两端各差半格 —— 相机夹取后画面上下各露 32 像素空白（已修，与 `src/scene.cc` 的背景平铺也一致了）。
- **可消失平台（tile `V`）的四条约定**：①它在 `level.cc` 里合并成**独立的一段** `Level::vanish_platforms`
  （不与实体/单向合并，否则它消失时会把邻居一起带走）；②相位/计时器在 `GameState::vanish` 里**按块下标**
  对齐几何（tile 没有身份，所以没有「实例」这层），因此**必须进快照**；③「不可碰撞」的实现是
  **不进 `build_collision_platforms()` 的结果数组**，而不是在 `collision.cc` 里加一种幽灵 collider；
  ④触发判据是「玩家盒下移 1px 后与块重叠」（与地面探针同一判据，所以“站住”才算），而且必须
  **连续踩住 `VANISH_HOLD_FRAMES`（10 帧 ≈ 0.17s）**才进预告期，离开立刻清零 —— 因为那个判据对
  “蹭一下 / 从边缘斜穿 / 冲刺横穿一格”都会命中，碰到就触发太敏感；
  恢复判据是「玩家盒不压在块里」—— 两条都用矩形关系表达，别去反查碰撞数组里的指针归属（那个数组每步都是新拼的副本）。
  ⑤**触发不可回退**：一旦进入预告期，倒计时一定走完（玩家离开、在块上又跳一下都不重置），
  所以“踩够 10 帧就走”也会让这块消失 —— 预告期是**纯倒计时**，不是“接触才计时”。
  **触发不可回退**：一旦进入预告期，倒计时一定走完（玩家离开/在块上跳跃都不重置），
  所以“踩一下就走”也会让这块消失 —— 预告期是纯倒计时，不是“接触才计时”。
  放一块**悬空**的可消失平台时要看两件事：正上方有没有天花板（跳上去会撞头，撞头点就在平面前一格），
  旁边有没有一根干净的柱子（否则只能靠撞头 + 侧移蹭上去，本工程实测花了四五轮才想清）。
- **传送门（实体，`LevelPortalAsset`）的五条约定**：①`pair_id` 既是配对键也是调色板下标（同色 = 一对），
  校验要求**每个色号恰好 2 扇**；②触发 = 「**角色中心**站在门矩形里」+「**按上键**（`GA_UP`）」两个条件同时满足 ——
  判据用**中心**而不是“碰撞盒相交”：实测踩过（半只脚压进门 3px 时按盒判会误触发）。
  按键是显式动作，所以**不需要**“刚用过就抑制”（那是接触触发时代的东西，`Teleport::suppress_mask` 已删）：
  在目标门里再按一次上就能传回去；③落点规则 =「**角色水平中心对齐门中心、脚底贴门矩形底边**」——
  所以门要贴地/贴平台摆，而且**不要**再加“向下探针找地面”，规则统一在几何上比猜意图稳；
  ④门被埋进墙体时（底边低于所在那一层的地面顶面）落点会**先顶上最近的那层地面顶面**：
  传送期间整步不走物理，不能等解算器挤 —— 实测过：门埋在第 13 行**单向**平台里 32px 时，
  当时只把 `COLLIDER_SOLID` 算作“墙体内”，角色被解算器顺着重力挤穿平台掉了 ~400px（修后该用例 9/9 过）。
  所以 `teleport_depenetrate_up()` 把**碰撞数组里的所有平台**（含单向、含移动组件、含未消失的可消失平台）
  都算进去，按“角色盒压到的最上面那块平台顶面”一次到位（顶完再检查一轮，最多 4 轮）；
  ⑤传送是**三段式相位机**（淡出 0.3 → 加载 0.4 → 淡入 0.3 秒），期间整步只推进相位：锁输入、不走物理；
  它**刻意不复用** `world_transition`（那套是“顶着输入锁自动走入另一个世界”，观感/时长/落点规则都不一样）；
  相位与目标门下标都**进快照**（脚本要能驱动它）。
- **移动组件（2026-09-25 改）的五条约定**：①它 = **形状 + 一条轴（两点）+ 沿轴速度**：
  轴可以任意方向（竖着 / 斜着都行），方向规则是**先朝 `point_b` 走**、到端点反向；`speed` 是**沿轴**速度；
  ②**本体按定义就在轴上**：资产里存的是轴上的参数 `t0`（没有绝对中心），本体矩形由
  `level_asset_mover_rect()` 派生 —— 运行时、编辑器、校验共用这一个函数，
  所以「本体飘在轴外」存不下来（v7 以前的图由“旧中心在轴上的投影”推出 t0，所以老图轨迹不变）；
  ③`t`/`direction`/`rect` 是跨帧会变的，所以**进快照**；端点 / t0 / 尺寸 / 速度 / 形状是资产，不进；
  ④**形状决定行为**：`SQUARE` 实心 + 可站 + **驮着走**（两个轴都驮），`damaging` 打开时「被挤住 / 压住」才受伤；
  `CIRCLE` 实心但**不驮人**，恒定伤害 —— 而且它的判据**不能复用 `test_rect_overlap`**：
  圆形是实心的，解算器会把角色推到**刚好贴住**的位置，严格相交会漏帧 →
  用 `mover_circle_touches_box()`（圆到盒的最近距离 <= 半径 + 1px 容差）；
  ⑤数量上限是 `LEVEL_ASSET_MAX_MOVERS`（8，不再是「每图一个」）：`GameState` 里是 `MoverSet`（数组 + count），
  快照一行 `X(MoverSet, movers)` 带全部；拖动/命中走 `level_asset_entity_rect_now()`（派生矩形的统一入口），
  **不要再直接读 `entity_rect()`** —— 移动组件在注册表里是 `NO_RECT`。
  ⑥**驮人的判定必须用「这一步移动之前」的几何**（`update_movers` 里先存一份 `old_rect`）：
  角色在帧初站的是旧位置，而平台在本步已经走开了 —— 拿新矩形判的话，平台**朝下走**的那一段
  （以及到端点反向的下一帧）会被判成「没接触」，角色被留在原地直接掉下去。
  这条只能靠「带竖直分量的轴 + 走完一个来回」拓出来：只测上升段看不见
  （上升时新顶面盖住了脚，探针依旧碰得到）。实测踩到：斜轴平台向上驮得很好，一到端点反向就掉。
  资产侧的迁移（v5/v6 → v7）要保证老图轨迹逐帧不变：
  证据就是 `test/baseline.txt`（它那段跨地刺的断言正好依赖平台位置）仍 53/53 过。

### 4.5 开发工具链

- **`.bat` 一律纯 ASCII**（`test\run_tests.bat` 与 `editor\build.bat` 都是）：cmd 按本机代码页（936）读 .bat，
  UTF-8 中文注释会变乱码，严重时其中一段会被当成命令去执行（实测踩到：报 `'自己' is not recognized`）。
  构建脚本里的说明一律用英文，中文说明放 .md。
- **源码是 UTF-8（无 BOM），两份 build.bat 与 `.clangd` 都带 `/utf-8`**：不加的话 MSVC 按本机代码页 936 读源码，
  窄字符串字面量只是**凑巧**正确（GBK 与 936 是同一个代码页，字节往返无损），而**宽字符串字面量会变成乱码**
  —— 实测例子：编辑器窗口标题 `L"关卡编辑器 — Mono"` 显示成乱码，而界面上的中文正常。别被「界面看着对」骗过。
- **崩溃定位第一步**：`Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 3`
  → 直接给出 faulting module / exception code / fault offset（`0xC0000005` 访问违例、`0xC000041D` 回调内未处理异常）。
  若 faulting module 是第三方 DLL，方向应转向"我们交给它的指针/回调/内存"，而不是它自己。
- **构建输出里有两条已知的第三方告警**：`include/lib/dr_wav.h` 与 `include/lib/stb_vorbis.c` 各有 1 条 C4701。
  `game_audio.cc` 里的 `#pragma warning(push, 0)` **压不住它们** —— C4701 由代码生成阶段发出、不吃 pragma 的
  warning state（实测：只有命令行 `/W0` 或 `/wd4701` 压得住）。所以「自研代码 0 警告」仍然成立，
  但看到这两行不要当成新引入的问题。
- **空日志 ≠ 崩在早期**：日志只在 `>=WARN`、显式 `log_flush()`、正常退出时落盘，`LOG_INFO` 全在内存缓冲里。
  进程一崩就一起丢，所以**不能用"日志为空"反推崩溃点**。
- **日志文件在"进程工作目录/game.log"**（不是 `build/log/`），UTF-8，用 `Get-Content -Encoding UTF8` 读。
- PowerShell 的 `Measure-Command` / `Stopwatch` 在本机会**提前返回**（曾量出 1.28s / 257s 两个荒谬值）。
  测时间用 cmd 自己的 `%TIME%`，或干脆不接管道。
- **临时写的 `.ps1` 驱动脚本里不要放中文**：PowerShell 5.1 按 ANSI（本机 936）读 UTF-8 文件，
  中文字符串字面量会直接解析失败（报 `The string is missing the terminator`）。全 ASCII，或写成带 BOM 的 UTF-8。
- 驱动「注入键 → 录制 → 导出」链路的最小做法（跑过、可用）：`Start-Process -PassThru` 启 `--input win32`，
  `FindWindowW("Mono","Mono")` 拿句柄，`PostMessageW(WM_KEYDOWN/WM_KEYUP)` 发键（键码：F4 0x73 / F5 0x74 / F6 0x75 / F8 0x77），
  最后 `PostMessageW(WM_CLOSE)` 退出并读 `.ExitCode`。注意 Win32 后端**失焦会清空所有按键** ——
  录制途中去动别的窗口会看到按键“自己松开了”，那不是录制的问题。
- **不要写 `... | Select-String ... | Select-Object -First N`**：匹配数达到 N 时管道提前关闭、**掐死上游构建**，
  之后跑的还是旧二进制（极易误判）。筛构建输出别用 `-First`。
- 本机是 150% DPI，PowerShell 是 DPI-unaware：它读到的窗口尺寸是 96DPI 虚拟化值（3840x2160 会读成 2560x1440），
  `CopyFromScreen` 也只能覆盖左上角那块物理区域。**想截全画面就让程序窗口化启动并放在 (0,0)**。
  - **窗口化时的客户区是「物理像素」**（游戏是 per-monitor-v2 感知的）：`--window 1280x720` 得到的是
    1280×720 **物理**客户区（不是 1920×1080 —— 只有 DPI-unaware 的进程才需要乘 1.5），
    窗口总宽约 1296 物理像素（另有标题栏）；所以屏幕中心 ≈ 客户区中心 + (8, 22) 物理像素。
    **别按 1.5 倍去算截图坐标**（本轮白跑了好几轮：按 1.5 倍找图标，扫错了区域，差点当成“没画出来”）——
    最稳的办法是先截一屏全屏图，在里面找到目标，再算偏移。
  - **按坐标取色能机械判定「自己画的、颜色独特」的图形**（前提是屏幕位置确定）：本轮验证 0.4s 的加载动画时，
    在图标中心 ±22px（轴上/对角）各取一点，看哪组是纯白 —— 两帧就是两组点，于是“两帧交替”变成了数字而不是目测。
    难点始终是“屏幕位置确定”：那次能成立是因为传送时相机是 `dt = 0` **直接贴合**的（角色在目标门里，
    图标必然在屏幕中心上方固定偏移处）；相机还在平滑跟随的时候就不能这么用。
- **验证界面按钮不要靠截图读像素**：150% 下 `CopyFromScreen` 得到的是左上角那块的 **1:1 物理像素**
  （所以 `SetCursorPos` 的虚拟坐标要乘 1.5），但文字会被查看工具缩得读不出来；而画布每帧重绘会把整片区域
  算成“变化”。可行做法是**只截按钮所在的那一栏**、比较「悬停某点」前后两帧的差分框：
  命中整行 `Selectable` 得到 ~390px 宽的色带，命中的是行尾按钮就得到 ~21×25px 的小方块 ——
  宽度差一个量级，不需要人眼。位置确认后再 `SetCursorPos` + `mouse_event` 真点一下，
  收尾仍然只看产物（Ctrl+S 后直接读 `.bin` 里的数量字段）。
- 无窗口焦点也能测按键：P/Invoke `FindWindowW("Mono","Mono")` + `PostMessageW(hwnd, 0x0100, VK, 0)`
  —— **只对 Win32 后端有效**，GameInput 走 Raw Input 会过滤注入消息。
  要用它就得先 `--input win32`（默认 `auto` 会选 GameInput，注入的键既不生效也不报错）。
  **但它只对 `WindowProc` 里直接处理的功能键有效**（F4~F8 那一批）：动作映射键（ESC/GA_MAP、方向键…）
  走的是 `input_on_message` → 动作边沿 → `input_step` 这条链，注入的消息实测**不生效**
  （2026-10-01 验证大地图画面时踩到：`PostMessageW` 与 `SetForegroundWindow` + `keybd_event`
  两种写法都试过，屏幕上的大地图始终没开）。
- **需要“把游戏停在一个特定画面”时，正确答案是输入脚本，不是注入键**：写个临时 tape 丢进 `%TEMP%`
  （纯 ASCII、路径无空格），`build\main.exe --window 1280x720 input_script <那个文件>`，
  末尾留几十秒的空帧就够截图了。例（开大地图后停住 ~15 秒）：
  ```
  save spawn
  40 press MAP
  44 release MAP
  1200 assert_world FIRST
  ```
  它比注入可靠得多：走项目自己受支持且确定性的驱动路径，不需要窗口焦点，也不受后端影响。
  截图仍然用两次调用之间的自然间隔代替 `Start-Sleep`（第一次 `Start-Process`，第二次截图 + 杀进程）。
- **命令行参数不要加引号**：`parse_command_line` 按空白切分且**不去引号**，所以 `input_script "test\x.txt"`
  会变成一个带引号字符的文件名（报 `cannot load`）。带空格的路径暂时传不进来（没做引号处理），用例名因此不能有空格。
- **计时必须与产物交叉验证**：`cmd /c "... for /L %i in (1,10) do @main.exe ..."` **不会等** GUI 程序 ——
  它会并发拉起 10 个进程、命令 0.02s 就返回（实测踩到，并因此得出过「`--fast` 只要 0.3 秒」的错误结论）。
  可靠做法：`Start-Process -PassThru -Wait`，或 `Start-Process cmd -ArgumentList '/c', xxx.bat -Wait`
  （**.bat 内部 cmd 确实会等**）；并且**再看一眼产物**（trace 行数 / 退出码 / `build\` 里的文件）确认真的跑完了。
- **确定性守卫只在“同一套参数”下才有意义**：`--window` 不同 → 相机夹取范围不同 → trace 必然不同
  （实测：同一用例换视口尺寸，`fc` 当场报差异）。所以 `run_tests.bat` 给所有用例用同一组参数。
- 终端输出中文乱码时先设 `[Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936)`。

### 4.6 UI 层（2026-09-25 起；自研，不把 ImGui 引进游戏运行时）

- **打开 = 世界完全暂停**：`game_update` 第一件事就是 `ui_update()`，它返回 true 就直接 return
  （不吃输入、不走物理、不动动画）。`ui_update` 在**刚开/刚关的那一步也返回 true** ——
  否则「按确认」的 `GA_JUMP` 会在同一步漏给角色，多跳一下（这个是写的时候就发现的，别改回去）。
- **UI 状态进快照**（`GAME_STATE_PERSISTENT_FIELDS` 里的 `X(UiState, ui)`）：它跳帧会变、又决定传送到哪，
  漏了它回放会从这里开始飘。`ui.selected` 也受 `game_state_snapshot_valid` 的下标检查保护。
- **绘制只上报语义矩形**（`ui_collect_rects` → `UiRect { x, y, w, h, kind }`，屏幕空间、左上角 + 尺寸）：
  贴图与颜色在装配层 `src/scene.cc` 按 kind 查表（`UI_RECT_COLORS` + `static_assert`，与 `DEBUG_BOX_COLORS` 同一套做法）。
  渲染层只多了一个**屏幕空间**入口（`renderer_push_ui_rect` / `_outline`，走 `LAYER_UI`）：
  与世界精灵共用绘制列表与着色器，只是 model 直接由屏幕坐标算出（不经过相机与缩放）。
  UI 所有矩形 order 相同 → 排序稳定 → 按提交顺序画，所以「压暗 → 底板 → 记号 → 选中边框」不会乱。
- **键位**：`GA_MAP`（ESC / 手柄 Start）开关、方向键选项（四个方向同义）、`GA_JUMP` 确认；
  鼠标**悬停即选项**（停在列表外不动选中项）、左键落在某项上 = 确认。
  代价：**ESC 不再是「退出游戏」** —— 退出改 Alt+F4 / 关闭窗口（`WindowProc` 里那段硬编码的
  `DestroyWindow` 已删；留着它的话 ESC 永远最先被窗口过程吃掉，UI 拿不到）。
- **布局只有一处**（`ui_item_box()`）：绘制与鼠标命中都调它 —— 两处各算一遍的话，
  「点到的」与「看到的」会在改布局时各自漂。鼠标坐标与 UI 矩形同为**客户区像素**（后端已统一），
  所以直接比；也因此**用例里的鼠标坐标与客户区尺寸绑定**（`test/ui_map.txt` 那几组是按默认全屏
  3840x2160、`ui_scale = 3` 算的，换显示器分辨率或加 `--window` 都要重算）。
- 脚本能表达鼠标了：`mouse_move <x> <y>` / `mouse_press|mouse_release [left|middle|right]`
  （磁带本来就存了鼠标状态，只是以前没有文本入口）；导出也会写出鼠标，**摇杆/扳机仍然只 WARN**。
- **候选项 = 所有世界的传送点**（资产层第五种实体，每图最多 `LEVEL_ASSET_MAX_WAYPOINTS = 4`，
  上限与 `UI_MAX_ITEMS` 由 game.h 的 static_assert 钉在一起）：顺序是「世界号 → 数组顺序」，确定性，
  脚本能指着它写断言；开图时 `selected` 对齐到**当前世界的第一个**传送点。
  传送点存的是**一个点**（脚底坐标，与出生点/门落点同一套语义）—— 它是「地图信息」不是场景物件，
  所以渲染层/碰撞层完全不认识它，只有大地图和编辑器认识。落点走 `place_player_at_feet()`，
  与传送门（矩形底边）共用一份实现，包括「埋进墙里顶上墙顶」那条保护。
- **字体是最小点阵系统**（`include/font.h` + `src/font.cc`）：字形表写成 **ASCII 图案**（每行 5 个字符），
  `static_assert` 卡住行宽 —— 抄错一行是编译错误，不用肉眼看；图集 16 列 × N 行、字形 5×7 + 右/下留白 1px，
  运行时由 `font_build_atlas` 生成（白色实心 + 透明底，颜色靠 tint）。
  三层分工：**码点 → 字形下标**（编译期建表，小写在大写那一行上多写一格，所以无运行期分支）→
  **字形 → uv 子矩形**（`font_glyph_uv` 直接给 SpriteStyle 的 offset/scale，**不要自己减**）→ **图集 → 贴图**。
  文字的尺度必须**整数倍**（`ui_glyph_scale` = `client_h / 720` 向下取整），否则点阵边缘忽胖忽瘦。
  字符串按**字节**处理：只支持 ASCII 显示子集，非 ASCII 字节当缺字形跳过（不崩、不乱码）。
  要中文 = 换/加一张含 CJK 的图集（UI 层只认「码点 → uv + 前进宽度」，不用改）—— 这是当初选「点阵 + 图集」而不是
  「矢量 + 系统字体光栅化」的理由：像素风要硬边，而且自制的每一台机器长得一样。
- **候选项的文字是运行时拼出来的**：世界名来自 `game_world_name`（脚本的 `assert_world` 也用这一份，
  不再另建一张表），序号来自数组下标 —— 资产里没有字符串。想给传送点起名字就得给格式加字段，那是另一件事。
- 验证：`test/ui_map.txt`（开图暂停 / 选项 / 传送 / 传回）；截图能直接看出「压暗 + 两个矩形 + 选中高亮」。

### 4.7 渲染层（2026-10-01 起：游戏 D3D12，编辑器仍留在 D3D11）

- **契约层一行没改**：`include/renderer.h` 本来就不含任何 D3D/DXGI 类型（窗口 `void *`、纹理句柄），
  所以这次换代只重写了 `src/d3d12_renderer.cc` 一个实现文件。
- **帧在途数 = 后台缓冲数（现在都是 2）**：每帧一套命令分配器 + 常量环里的一段。两者不等时 Present 会拿到
  一块“序号还没轮到”的缓冲，那时“等围栅”等的是错的帧。
- **上传堆资源只能停在 `GENERIC_READ`**（`CreateCommittedResource` 的 InitialState 填别的会被调试层拒），
  而 `GENERIC_READ` 的定义里**同时含 `VERTEX_AND_CONSTANT_BUFFER` 与两个 `SHADER_RESOURCE` 位**
  （实测 `d3d12.h`：`0x1|0x2|0x40|0x80|0x200|0x800`）—— 所以顶点缓冲与常量环都放上传堆、**一次状态转换都不需要**。
  纹理仍然走 DEFAULT 堆 + 暂存 + `COPY_DEST → PIXEL_SHADER_RESOURCE`（通用做法，收益不在省几行代码）。
- **常量缓冲视图的起点必须 256 字节对齐**：所以“每项一份 96 字节常量”必须按 256 切槽，
  一帧的槽上限是 `MAX_DRAW_ITEMS × 4` —— 因为**线框一项会展开成 4 次绘制、每条边一份独立的 model 矩阵**。
- **`CopyTextureRegion` 的源行距必须 256 字节对齐**：纹理上传不能一句 memcpy 铺平，
  要按 `GetCopyableFootprints` 给的 `RowPitch` 一行一行拷。
- **后台缓冲要显式转状态**：`frame_begin` 里 `PRESENT → RENDER_TARGET`、Present 之前转回 `PRESENT`。少一次就是画面全黑。
- **Resize 必须先等 GPU 排空**：D3D11 只要解绑 RTV，DX12 还要保证“没有任何后台缓冲的引用”（等所有在途帧的围栅），
  否则 `ResizeBuffers` 直接失败。
- **交换链挂在命令队列上**（D3D11 挂设备）；`GetCurrentBackBufferIndex` 在 `IDXGISwapChain3` 上，
  有它就不必自己推算“Present 之后轮到哪块缓冲”。
- **裁剪矩形没有默认值**：`RSSetScissorRects` 漏掉就整个画面不画（D3D11 有默认全屏）。
- **深度/模板状态不能留零**：`D3D12_DEPTH_STENCIL_DESC` 的 `DepthFunc` / `StencilOp` 里 0 不是合法枚举值，
  即使 `DepthEnable = FALSE` 也要填成 `ALWAYS` / `KEEP`。
- **描述符堆只建三个**：RTV（后台缓冲，不必 shader 可见）、SRV（纹理表）、SAMPLER（两个采样器）；
  后两个必须 shader 可见。常量不做描述符（用**根 CBV** 直接给 GPU 地址）。
- **采样器不能写成根签名里的静态采样器**（本项目实测踩过，是换代后唯一一处“看起来更优雅、其实错”的决定）：
  静态采样器按**寄存器**固定在根签名上，而“这一项要不要平铺”是**逐项**决定的（`uv_repeat_px > 0`）——
  写死一个就只能整帧用一种。症状：平台（平铺项）被 CLAMP 采到 texel(31,31)，而 `data/base/brick.png`
  的四边恰好是黑色勾缝，**整块平台渲染成纯黑**；而 `test\run_tests.bat all fast` 照样 7/7 全过
  —— 用例全是逻辑断言，黑屏也能过。正确做法是回到 D3D11 那套：两个采样器对象写进 SAMPLER 堆、逐项切 s0。
- **调试层本机已装**（`d3d12sdklayers.dll` 在位、`D3D12GetDebugInterface` 返回 S_OK；实测最高特性等级 12_2、
  默认适配器是 RTX 4080 SUPER）。它由 `MONO_DEBUG_ANY` **编译期**开启，没装“图形工具”时只记一条 WARN 继续跑。
  **实测它不影响 `all fast` 回归**（baseline 0.94~0.97s，与 D3D11 时代的 1.0s 同级）—— 所以不必再把它改成运行期开关。
- **回归与确定性守卫（两遍 trace 对拍）在 DX12 上照样过**（`test\run_tests.bat all fast` 7/7）：
  渲染层换代不参与逻辑，快照 / 磁带 / trace 都不受影响。

## 5. 怎么验证改动

改任何与物理/手感相关的东西之后，跑一遍回归：

**渲染层改动是例外：回归全过也不代表画面对。** 用例全是逻辑断言，黑屏或纯色块也照样 7/7
（实测踩过：平台整块渲染成纯黑，回归 7/7、编辑器 2/2 全过；见 §4.7）。
所以改完渲染层必须**窗口化启动看一眼**：`build\main.exe --window 1280x720`（程序会放在屏幕 (0,0)，
物理客户区就是 1280x720），在 PowerShell 里 `CopyFromScreen` 截一张，再用 `GetPixel` 取几个点**比数值**——
比肉眼看缩略图可靠得多（例：数一数某块区域里有多少个“砖面灰”像素、多少个“勾缝黑”像素）。
注意**别只扫一行**：平台砖块本来就带勾缝，扫到一条勾缝整行都是黑的，会得出相反的结论（踩过）。

**而且要动手前就先定好这套验证手段**：这次顺序反了（先写完实现、再临时拼验证），途中先信了“回归 7/7”、
又信了“黑的就是勾缝色”，两次都差点收工。最小套路：
① 从 `data/` 里挑一张**颜色独特**的素材、dump 出它的像素值当期望值（砖块 = 灰面 132,126,135 + 黑勾缝 0,0,0）；
② 窗口化启动 → 截图 → **统计一整块区域**里各类颜色的像素个数；
③ 期望值与实测对不上就是画错了，不需要人眼判断。UI / 屏幕空间的改动同理，
只是要先用输入脚本把画面停住（见 §4.5 的 tape 做法）。

1. **首选 `test\run_tests.bat`**（在任何目录都能跑，它自己 `cd` 到仓库根）。两个参数，其余情况给用例名：
   ```
   test\run_tests.bat <用例名>        一个用例，实时跑（人看着渲染）
   test\run_tests.bat <用例名> fast   一个用例，快速 + 确定性守卫
   test\run_tests.bat all             全部用例，实时
   test\run_tests.bat all fast        全部用例，快速 + 守卫   ← AI / 自动回归用这条
   ```
   - `fast` = 给游戏加 `--fast`（1961 帧 33 秒 → 1.0 秒）**且每条用例跑两遍、逐字节对拍两遍的 `--trace`**；
     后者就是**确定性守卫**（能拓出「没进快照却仍影响逻辑」的状态）。它**不是性能测试**，
     只是 `fast` 要跑两遍的原因（因此 `fast` 的成本是两倍）
   - 不带 `fast` 时只跑一遍、也不写 trace 文件（实时模式是给人看的）
   - 一次只跑一条；**不是 CI / 并行基础设施**（共用同一对 trace 路径是有意的）。并行用例、
     每条用例独立工件路径、无头模式都不在计划内（后期会上多线程，届时再议）
   - 参数**不能带引号**：`main.exe` 按空白切分命令行且不去引号，`"test\x.txt"` 会变成一个带引号字符的文件名
   - 用例失败时 runner 会给出：复现命令 + 前 12 行失败信息（带脚本文本行号与 `context:`/`input:`）+ `TAPE:` 清单；
     第一次就跑挂时直接跳过确定性守卫（不再拿它当「非确定性」报，也不会重复计数）。
     守卫对拍不一致时用 `fc /l /n` 写 `build\_trace_diff.txt` 并打印前 8 行 —— 行号旁边就是那一帧的 CSV，
     所以「两遍在第几帧开始不同」不需要自己数
   - VS Code 里也有任务：`.vscode/tasks.json` 的 `test`（= `all fast`，并且先跑 `build`）；
     构建还有 `build-release` / `build-shaders`（= `build.bat release` / `build.bat shaders`）；
     编辑器那对是 `editor-build` / `editor-test`（后者 = `editor\test\run_tests.bat`）
2. 不经 runner 直接跑：`build\main.exe input_script test\baseline.txt`（**脚本路径由命令行给定**；不传 `input_script` 就是手动游玩）。
   - 加 `--fast`：逻辑步不吃真实时间（每循环正好一个逻辑步）且每 64 帧才 Present 一次 →
     1961 帧从 **33 秒**降到 **1.0 秒**
   - 快跑的瓶颈是 **`Present`**（被 DXGI 翻转队列限速）：实测同一用例
     `每帧呈现 14.03s / 每 16 帧 2.02s / 每 64 帧 1.01s / 从不呈现 1.01s`
     → 所以 `--fast` 取「每 64 帧呈现一次」：窗口还有画面，Present 那条路也仍被执行
   - 实测 **`--fast` 与实时模式的逐帧轨迹逐字节一致**（含相机列），所以它只改节奏不改逻辑；
     但它**只能配磁带用**，手动游玩会被快进到不可玩
   - 脚本的**第一条非注释语句必须是 `save <路径.sav>`**（基线写 `save spawn` = 不读档、从关卡出生点开始）
3. 跑完自动退出：**退出码 0 = 全部断言通过**。
   - 失败信息的形状（2026-09-22 起，详见 `docs/input-script.md` 第 9 节）：
     `[tape] frame N  FAIL <期望 vs 实际>  [script line L]`，紧跟两行 `context:`（所有断言看得到的量）
     与 `input:`（最后一个输入变化点）；跑完还有 `TAPE:` 清单（行号 + 帧号 + 断言名）。
     解析阶段的问题一律带行号并以退出码 1 收尾（不会静默掉进手动游玩）。
     **`game.log` 每次进程启动被清空**，所以它只含本次运行；`LOG_ERROR`/`LOG_WARN` 立即落盘，
     `LOG_DEBUG`（含 `TAPE:` 清单）走缓冲、正常退出时写出。
   - **不能用 PowerShell 的 `&` 调用它**：这是一个 `/SUBSYSTEM:WINDOWS` 程序，调用运算符**不会等待**它结束
     （实测 `&` 在 0.00s 返回，而进程还活着），`$LASTEXITCODE` 因此是个无意义的假值 —— 哪怕断言全挂也会显示 0。
     要用 `Start-Process -PassThru -Wait` 读 `.ExitCode`，或 `cmd /c build\main.exe`，或干脆用 `run_tests.bat`。
4. 常驻用例（`test/`，都从出生点跑、不需要任何外部存档文件）：
   - `baseline.txt`：**53 条断言全部通过**（走路 / 跳跃顶点 / 上实体台 / 越坑 / 单向平台上穿下穿 / 土狼 / 冲刺位移 / 掉坑重生 / 空中冲刺 / 二段跳 / 左边界墙与攀爬塔逐层上到顶层 / 跨图后发射能量波击杀怪物）。
     帧号与 `assert_pos` 坐标与关卡强耦合，**改关卡或改运动数值后必须用 F7 重新取样**。
     它挂了**不能反过来推定“游戏坏了”**：先用 `--fast input_script test\baseline.txt` 拿到 FAIL 行，
     再把当前网格 dump 出来跟上一版**逐行对比**（`.bin` 头部固定 36 字节：magic/version/列/行/格边长/四个数量，
     tile 从偏移 32 开始（v2；v1 是 36，多一个地刺数量字段）、每行 `列数` 字节）—— 实测一次“只删了地刺”的恢复其实还少了 5×2 格实体墙，
     那 10 格正好在飞行段弹跳路径上，比按 FAIL 行猜快得多。
   - `save_roundtrip.txt`：存档往返（`save_state` → 接着跑 → `load_state` 回到那一刻，容差 0.05px 的逐位一致断言）
     与读档后的演化正确性。
   - `smoke.txt`：**关卡冒烟** —— 刻意不依赖具体坐标与几何（`assert_grounded 1` + 走两步 + 跳一下），
     所以改完关卡之后它仍应该通过。它回答「这张刚编辑过的图主程序能不能加载并跑起来」，
     编辑器菜单里的「保存并冒烟验证」跑的就是这一条。
   - `vanish_bridge.txt`：**可消失平台的四条语义** —— ①连续踩住 10 帧后变暗 → 消失 → 掉出关卡重生；
     ②再走一遍全桥（能走过去就证明它恢复成实体了）；③先只**蹭 4 帧**、等过“假如蹭触发了现在早已消失”
     的时刻再走一遍（卡住「触发门槛」：蹭一下不算）；④走开之后 2s 桥照样消失（卡住「触发不可回退」）。
     它是目前唯一同时覆盖「新增 tile + 运行态 + 快照」的用例。
   - `portal_pair.txt`：**传送门的一对同色门** —— 站在 B 门里（中心在门矩形内）**按上键** → 断言落在 A 门上
     （水平中心对齐门中心、脚底贴门底边）→ 走开半步让**碰撞盒压到门、中心在门外**再按上 → 断言不生效
     （卡住「判据是中心」）→ 中心回到门里再按上 → 断言能传回 B 门（卡住「不需要抑制」）。
     它的埋墙分支（门底边低于平台顶面 → 落点顶上墙顶）没有常驻用例：验证方法是把 `first.bin` 里 A 门的
     `center_y` 从 -768 临时改成 -800（`data\map\first.bin` 偏移 1596，float）再跑一遍本用例，
     改前 5/9（角色被挤穿平台掉 ~400px）、改后 9/9 —— 验完记得把字节改回去。
   - `ui_map.txt`：**大地图 UI** —— 开图（GA_MAP）后按住**冲刺**世界**一格不动**（「打开 = 世界暂停」的证据；
     用冲刺而不是方向键：方向键在图里有含义）；方向键选项（不确认就不传送）→ 跳跃键确认 →
     断言落到那个传送点的坐标 + `assert_world SECOND`（跨世界）→ 再开图传回第一世界 →
     鼠标那一段（悬停选项 / 左键确认 / 列表外点击不生效）。
   - 中途起点用例不必再维护“写/读”两个文件：**同一段脚本里 `save_state` + `load_state` 就够了**     （`load_state` 是 2026-09-22 加的操作）。要点：同一帧内先 `load_state` 再断言，看到的就是存档点那一刻的状态。
   - **移动组件的竖直轴、圆形接触伤害与方形 `damaging` 都没有常驻用例**（斜轴来回那一条已经进了 `test/mover_ride.txt`；
     临时手法：给 `first.bin` 插 1~2 个组件 —— 它是**中间插入**一段（v7 每项 40 字节 = 8 个 f32 + 2 个 u32，
     字段顺序：`a.x a.y b.x b.y t0 half_w half_h speed shape damaging`）到**连接段之后**
     （`first.bin` 只有 1 个连接 → 偏移 40 + 64×24 + 20 = **1596**），
     同时把 `version`（偏移 4）改成 7、`mover_count`（偏移 24）改成实际个数。
     五个变体各跑一段小脚本就能把整张矩阵验完（实测数据）：
     ①`a=(288,-1424) b=(288,-1124) t0=0 speed=100 shape=0`（竖直驮人）：角色 y 从 -1343.6 → -1143.6；
     ②再加一个圆形（`x=480` 半径 40）也不影响①→ 多组件循环没问题；
     ③方形 parked 在 `t0=1` + 圆形挡路 → 冲刺撞上去回出生点 `x=288`；
     ④把③的圆形换成 `shape=0 damaging=0` → 只被挡住 `x=404.8`（对照组）；
     ⑤方形从头顶压下来（`a=(288,-1290) b=(288,-1424) t0=0`）：`damaging=1` → 回出生点 `x=288`，`=0` → 被压在原地 `x=309.3`；
    ④**斜轴来回**（`a=(288,-1424) b=(512,-1200)`，45°、speed 320，`log_state` 逐帧看）：
     角色应该一路被驮着（每帧 x/y 各 3.77），到端点反向也不掉 —— 这是唯一能拓出「驮人判定用移动前几何」的形状，
     只测竖直上升段会漏（当时就是这么漏过去的）。
     **这条现在已经是常驻用例** `test/mover_ride.txt`（用第二世界那块斜轴平台、走完一个来回，两次反向都断言 `grounded 1`）；
     下面①~⑤仍然靠临时插件的手法验证。
     改完记得把 `first.bin` 还原（备份 → 覆盖 → 再跑一次 `all fast` 确认 7/7）。
5. 存档本身的拒载路径（改一个字节就应该看到明确的错误，而不是静默接受）：
   - 改指纹字段（头部偏移 12..15）→ `was written for another level layout`
   - 改负载里任意字节 → `is corrupted (checksum mismatch)`
   两者都用 `--load <那个文件>` 跑一次即可（窗口起来就关掉也可以，错误在启动时就落了盘）。

6. **编辑器**（它自己的回归入口是 `editor\test\run_tests.bat`：`--selftest` 的交互/撤销/资产往返用例 + `--check --smoke`；
   改共享层（`level_asset.cc` / `level.cc`）之后**游戏回归必须照跑** —— 两边共用那份 reader/writer）：
   `editor\build.bat` → `build\editor.exe`（从**仓库根**启动，`data/map/*.bin` 按进程工作目录解析）。
   带个路径可以指定关卡：`build\editor.exe data\map\second.bin`。
   验证编辑器改动的最小手法：打开一张图 → 改几格 → 保存 → 看右侧「检查」没有 ERROR、
   再跑一次游戏回归（存进去的东西能不能被游戏读，由它回答）。   **无界面自检是性价比最高的那条**：`build\editor.exe --check [<map.bin>] [--smoke]` ——
   不建窗口、不初始化 D3D，产出文本报告 `build/editor_check.txt` + 退出码（0 = 每张图 0 error），
   人和 AI 都能直接读；`--smoke` 会把 `game.log` 里的失败行一起抄进报告。
   （拿一张有 ERROR 的图去编译会 assert —— `src/level.cc` 那一串 assert 就是校验规则的来源；
    诊断入口现在先校验再编译，所以只会回报「不编译 + 原因」。）   编辑器菜单里的「保存并冒烟验证」会起 `build\main.exe --fast input_script test\smoke.txt` 并读退出码 ——
   那是「改完立刻让主程序回答一遍」的最短路（**不是热重载**：游戏启动时只读一次 .bin）。
   鼠标注入也可以用来验证（本轮拖动的实测就是这么做的）：
   `SetCursorPos` + `mouse_event` + `keybd_event`（Ctrl）；
   **拖动结果别靠截图目测，用 Ctrl+S 存盘后直接读 .bin 里的字段** —— 数字比像素可靠。

其他手段：`.vscode/launch.json` 里的 `Debug`（手动游玩）＋ 一条按脚本启动的（基线回归，
  等价于 `input_script test/baseline.txt`）。调试“读档恢复现场”时最有用的断点是 `game_state_load` 之后与
  `update_projectiles`（看到恢复的能量波继续飞并命中怪物）。
- `input_script <路径.txt>` 跑指定输入脚本（不传＝手动游玩）；`--trace <path.csv>` 每逻辑步输出一行（事后分析轨迹）。
- `save_state <路径.sav>`（脚本命令）在那一帧写一份存档（`save` 系统），用来造「从中途场景开始」的用例。
  脚本里的第一行 `save <路径>` 就是引用它。
- `--input <auto|gameinput|win32>` 选输入后端：只有 `win32` 吃 `PostMessageW` 注入，
  是「外部程序驱动 → F5 录制 → F8 标记 → F4 导出回归脚本」这条自动化链路的前提（默认 `auto` = GameInput 优先）。
- `--window [宽x高]` 窗口化启动（默认 1280x720，放在屏幕 (0,0)），截图验证画面时用它。
- `MONO_DEBUG_VIS` 打开碰撞盒线框：青=实体、绿=单向、黄=玩家物理盒、洋红=地面探针。
  探针是「玩家盒整体下移 1 像素」，画成线框会和玩家盒重合 → 装配层只画它多出来的那条底边，
  即一条**色带**（宽与底边精确；厚度按视口高度取，约 1/180、至少 6 设备像素——
  固定像素厚度在“4K 全屏整幅关卡都在视野里”的尺度下会细得看不见）。
  线框本身也是 4 条细长四边形拼的（D3D11 没有线宽状态），粗细 = `OUTLINE_THICKNESS_PX` 设备像素。
- F5 录制（连起点状态一起抓，并存成存档点）/ F8 断言标记 / 再按 F5 结束即导出脚本 / F6 循环回放 / F4 再导出一份 / F7 打印当前帧与角色状态。

## 6. 文档约定

- 改功能时**同一次改动里就要回头改相关文档**。参考型文档的生命周期比代码长，
  一旦落后就从"权威参照"退化成"误导源"——比没有文档更贵，因为读的人会先相信它。
  （`docs/audio-system.md` 就是因为一次重构没回头改，整章描述落后了一个版本。）
- 描述历史用删除线 + 一句说明，**但最危险的是用陈述句写着的旧内容**（它们看起来仍像现在时）。
- 文档只做增量补充，不要顺手重排结构或改写既有措辞。

## 7. 对 AI 的具体要求

- **先核实再改**。本次核对中，子任务报告的两个"错误"（碰撞盒与精灵脚底的对齐、`README` 的跳跃顶点）经实测一个是对的、
  一个是真错——结论必须落到代码/实测量上，不要靠"看起来合理"。
- **不要为纯措辞去改用户已经调过的注释**（用户会自己回退这类改动）。但**重构后遗留的错误注释要改**：
  `sprite.h` 里"解码在 game.cc"、`replay.h` 里"屏幕像素坐标"、`audio.h` 里"采样率统一 48000"都属于这一类。
- **不要在文档里加练习建议**，用户自己会练。
- 报结果时说清"哪些是测过的、哪些是推断的"，不要把推断写成结论。
- **性能测试要单独下指令**（用户 2026-09-22 定）：没有明确要求做性能分析时，**不要**设计或执行性能测试
  （包括"顺便量一下"）。真要测时必须**插桩**（临时的计时/计数器代码）来分析，
  得出结论后**把插桩代码删掉** —— 仓库里不该留下只有测量用途的代码/开关。
  例外：为了判断某个开关是否达到它自己宣称的目的（例如 `--fast` 到底快不快、瓶颈在哪）而量一两次，
  属于验证而不是性能测试；同样不留测量代码。
  **插桩怎么写才影响最小、最准**（2026-10-01 补的实操版）：
  - **计时区间里只允许 `__rdtsc()` + 一次 store**：格式化 / 分配 / IO / `log_write` 一律不许进去。
    量级对比：`game_update` 实测 ≈ 7.5k 周期/帧，而一次日志格式化 + 写缓冲和它是**同一个量级**，
    `LOG_WARN` / `LOG_ERROR` 还要立即 `WriteFile`（系统调用）。打印放进区间 = 量到的主要是打印本身。
    判据是「打印在不在计时窗口里」，不是「打了几次」。
  - **先记录、最后汇总打印一次**：只要均值就累加 `count` / `sum` / `min` / `max`（O(1) 内存，连数组都不用）。
    要中位数 / 分位数 / 离群点，或者要和 `--trace` 的某一帧对齐，才需要留原始样本 —— 那也必须
    **预先分配**（别在区间里 `arena_push`）并**定步长抽样**，别按绘制项攒（`--fast` 下是 4096×1961 ≈ 800 万条）。
  - **一次性测量不需要记录**：整段跑完掐头尾、打一个数 —— 那本来就是「最后打印」。
  - **别把 `Present` 包进要测的区间**：基线里 `Present` 等待 ≈ 330k 周期/帧，比 CPU 全部工作（≈79k）还大 4 倍，
    包进去就把要看的信号淹了（要看整帧节奏就单独给 `Present` 一个计数器）。
  - **收尾显式 `log_flush()`**：`LOG_INFO` / `LOG_DEBUG` 走缓冲、只在正常退出落盘，
    跑崩或被强杀就一起丢（`--trace` 同病）。
  - **数字必须带「哪个档 + 哪套参数」**：真实性能用 `release`（`/MT /O2 /GL /LTCG`），但它带 `/DNDEBUG` ——
    断言消失本身就会改变代码、也会顺手优化掉「只被 assert 使用」的东西，所以
    **验证用 debug 档、测性能用 release 档**，两组数字不能混着比；`--window` 同理
    （相机夹取范围随视口变 → 耗时与轨迹都不可比，见 §4.5 那条）。
  - 单位统一（历次基线都用 **TSC 周期/帧**，现代 x86 的 TSC 是不变量），别一半周期一半毫秒。
  **2026-09-25 量过一次的基线**（`__rdtsc` 插桩，/Od 构建，`--fast input_script test/baseline.txt`，
  1961 步 = 1961 帧，单位是 TSC 周期/帧）：`game_update` ≈ 7.5k、绘制列表提交（`scene_submit`）≈ 5.2k、
  逐项 D3D 提交（`renderer_sort_items` 4k + 那圈 `Map`/绑资源/`Draw` 46k）≈ 50k、
  光标 + 消息泵 + 清屏 ≈ 13k，而 `renderer_frame_end` 里的 **`Present` 等待 ≈ 330k**（占 `frame_end` 的 ~87%）。
  结论：**CPU 合计约 79k 周期/帧 ≈ 60Hz 预算的千分之二**，瓶颈从头到尾都在 Present（翻转队列/垂直同步）。
  → 没有热点值得手写 SIMD/intrinsic（`/arch:AVX2` + `/fp:fast` 已开着，该自动向量化的已经做了）；
  → 真要动渲染性能，方向是**减少每项一次 D3D 调用**（per-instance 数据 + 按纹理合批）与**视口剔除**，
  不是算术优化。触发条件：平台/实体数量上到几百（那时 `test_rect_overlap` 这类批处理才值得走 SOA + SIMD）。
  插桩代码已按约定删净（`include/perf_temp.h` 已删）。

## 8. 明确搁置（别当死代码删）

- `CollisionFlags`：当前无人消费，留给撞头/落地音效。
- 鼠标 / 右摇杆 / 扳机 / `UP`：已采集并可录制，游戏逻辑未使用，为后续矄准/UI/爬爬预留（**左摇杆已在用**：
  x 驱动水平移动，y 用于判定“下”）。
- `renderer_destroy_texture`、`game_audio_toggle_bgm`：目前无调用者。

## 9. 已知未决问题

- 冲刺期间 `velocity.y = 0`，而 `move_and_collide` 的 Y 轴有 `if (velocity->y != 0.0f)` 早退 →
  若冲刺启动时人已在单向平台内部（跳穿途中），会悬停在板里水平飞出去，结束后直接穿过板块掉下去。
  三种改法（恢复重力 / Y 轴无速度也解算 / 禁止在板内启动冲刺）都还没选。
- 跳跃策略有两件待定：
  ①**要不要「长按跳得更高」** —— 现在是「按下沿读一次 + 固定初速」（顶点 243px，二段跳同理）。
  推荐走「松开时截断上升速度」（松开沿把 `velocity.y` 乘一个系数）：满跳轨迹一字不变，关卡按 243px 设计的间距全不受影响，
  只多一个分支，脚本里 press/release 都是显式的、能精确复现；「按住期间减小重力」会重写整条上升曲线，所有跳跃断言都得重新取样。
  截断要作用于「当前正在上升的那一段」，二段跳时两段各自独立。
  ②**二段跳把关卡难度放松了**：空中时间从 0.68s 拉到约 1.1s，原本"必须冲刺"的 9 格坑现在靠两段跳也能过
  （回归脚本第 9 段仍按冲刺写，断言照样通过）。是否重排坑宽、还是收掉这次能力，等玩法定。
- ~~F4 导出的脚本不含初始状态，所以只有"启动即录"的段落能直接当脚本重跑~~
  ~~（2026-09-20 已解决）：`F4` 导出会写 `initial_state`（hex 快照），脚本载入时在第一个逻辑步之前恢复状态；
  `F8` 可在录制中标记断言节点，导出时一并写成 `assert_*`；标记随录制写进 `replay.bin`（格式版本 2），
  所以 F6 载入后 F4 也能导出（导出源优先新录制，否则用载入的那份）。
  快照定义收在 `include/game.h` 的 `GameStateSnapshot` + `src/game.cc` 的 `game_state_save/load`（回放与脚本共用）。
  代价：`ReplayState` 被它取代，旧 `build/replay.bin` 会被"incompatible, please re-record"拒载（需重录一次）。~~
  ↑ 上面这一段描述的形态**已被下面这条取代**（hex 与 `replay.bin` 都不存在了），保留只是为了说明历史。
  - **2026-09-22 再改（当前形态）**：hex 内嵌被删了 —— 脚本第一行改成 `save <路径.sav>` 引用**存档文件**，
    磁带只记「存档点引用 + 输入变化点 + 断言」，二进制 `build/replay.bin` 与
    `game_state_snapshot_to_text/from_text` 都已删除（磁带只有文本一种持久格式）。
    旧的 `build/replay.bin` 与 `build/save_state.txt` 现在没有任何代码读取，可以随时删。
- **编辑器的已知边界**：不做撤销/多选/对齐辅助线/多关卡管理，DPI 未处理（150% 机器上窗口被系统放大 1.5 倍渲染），
  属性面板暂不做字段反射（手写三个 helper + 触发条件）。都在 `editor/README.md` 里，
  连同「新建世界 = 游戏侧改造而不是加个文件」的理由。
