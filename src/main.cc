#pragma comment(lib, "user32")
#pragma comment(lib, "Imm32.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "advapi32")

#include "win32_prefix.h"

#include "core.h"
#include "shared/memory.h"
#include "shared/logger.h"
#include "audio.h"
#include "debug/debug_vis.h"
#include "game_audio.h"
#include "game.h"
#include "camera.h"
#include "input.h"
#include "debug/input_script.h"
#include "renderer.h"
#include "debug/replay.h"
#include "save.h"
#include "scene.h"
#include "debug/trace.h"
#include "shared/mono_math.h"

#include <stdio.h>  // swscanf_s（解析 --window 的 "宽x高"）
#include <string.h> // memcpy（命令行 token 复制）

// ============================================================================
// 全局变量
// ============================================================================
global_variable constexpr wchar_t GAME_NAME[] = L"Mono";
global_variable constexpr wchar_t LOG_FILE_PATH[] = L"game.log";

global_variable bool global_running = true;

// 全屏切换坐标
global_variable bool global_is_fullscreen = true;
global_variable RECT global_windowed_rect = {};  // 窗口模式下的屏幕坐标矩形
global_variable DWORD global_windowed_style = 0; // 窗口模式下的窗口样式

// 客户区尺寸变化（WM_SIZE 记录，渲染循环里处理）
global_variable bool global_resize_pending = false;
global_variable u32 global_pending_client_width = 0;
global_variable u32 global_pending_client_height = 0;

global_variable GameState *global_game_state = nullptr;

// 逻辑步计数器（60Hz，从 0 开始）：脚本、轨迹文件与 F7 取样共用同一条时间轴。
// 放在这里而不是主循环的局部变量里，是因为热键处理（窗口回调）也要读它
global_variable u32 global_logic_step = 0;

// 本帧的输入状态（由输入层或磁带在固定逻辑步写入）
global_variable GameInput global_game_input = {};

// 输入源：同一时刻只有一个在驱动 GameInput（互斥，避免隐式覆盖）。
// 磁带的两种用法（F6 回放 / input_script 脚本）走的是同一条执行路径，所以这里只有两个值 ——
// 「跑完循环」还是「跑完退出」是磁带自己的属性（InputTape::loop），不是装配层要分辨的事。
enum InputSource : u8
{
    INPUT_SOURCE_BACKEND, // 真实设备（后端轮询；同时可以录制）
    INPUT_SOURCE_TAPE,    // 磁带（回放 F6 / 脚本 input_script）
};

// `--fast` 的呈现节奏（0 = 从不呈现）。实测（基线 1961 帧，单次跑，全屏 3840x2160）：
//   每帧呈现 14.03s / 每 16 帧 2.02s / 每 64 帧 1.01s / 从不 1.01s
// 也就是「Present 会被翻转队列限速」才是快跑模式的瓶颈，而偶尔呈现是免费的：
// 取 64 是为了窗口还有画面、Present 那条路仍被执行（选 0 也是 1.01s，但窗口全程黑着）。
constexpr u32 FAST_PRESENT_INTERVAL = 64;

// 命令行参数解析结果，启动时解析一次，之后只读
// parse_command_line 进行解析
struct CommandLine
{
    bool windowed = false;                               // --window [宽x高]：窗口模式启动
    u32 window_width = 1280;                             // 窗口模式默认宽
    u32 window_height = 720;                             // 窗口模式默认高
    wchar_t sav_load_path[260] = {};                     // --load <路径.sav>：启动即读档
    InputBackendKind input_backend = INPUT_BACKEND_AUTO; // --input <auto|gameinput|win32>
    bool fast = false; // --fast：逻辑步不吃真实时间，快速进行磁带回归，每 64 帧才呈现一次
                       // 它是节奏开关（`renderer_set_present_interval` 也不是调试入口），不依赖
                       // input_script / trace 是否存在，所以**不跟着下面的 #if 走** ——
                       // 踩过：字段在 #if 里、三处使用在 #if 外，关掉 MONO_DEBUG_INPUT 就编不过

#if MONO_DEBUG_INPUT
    wchar_t input_script_path[260] = {}; // --input_script <path.txt>
#endif

#if MONO_DEBUG_BUILD
    wchar_t trace_path[260] = {}; // --trace <路径.csv>：逐逻辑步输出轨迹
#endif
};

global_variable CommandLine global_command_line = {};

// ============================================================================
// 运行环境信息打印
// ============================================================================

internal void log_os_version()
{
    using FnRtlGetVersion = LONG(WINAPI *)(RTL_OSVERSIONINFOW *);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return;
    }

    FnRtlGetVersion RtlGetVersionFn = (FnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    if (!RtlGetVersionFn) {
        return;
    }

    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (RtlGetVersionFn(&info) != 0) {
        return;
    }

    LOG_INFO("OS: Windows %lu.%lu (build %lu)", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
}

internal void log_cpu_info()
{
    wchar_t cpu_name[256] = {};
    DWORD cpu_name_size = sizeof(cpu_name);
    LONG status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString",
        RRF_RT_REG_SZ,
        nullptr,
        cpu_name,
        &cpu_name_size);

    SYSTEM_INFO sys_info = {};
    GetSystemInfo(&sys_info);

    const wchar_t *name = (status == ERROR_SUCCESS) ? cpu_name : L"unknown";
    LOG_INFO("CPU: %ls (%lu logical processors)", name, sys_info.dwNumberOfProcessors);
}

internal void log_memory_info()
{
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        LOG_INFO("RAM: %llu MB", (u64)(memory.ullTotalPhys / (1024ull * 1024ull)));
    }
}

internal void log_system_info()
{
    SYSTEMTIME st = {};
    GetSystemTime(&st);
    LOG_INFO("Time: %04u-%02u-%02uT%02u:%02u:%02uZ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    log_os_version();
    log_cpu_info();
    log_memory_info();
}

// ============================================================================
// 命令行参数
// ============================================================================

// 目前的全部开关（README「调试输出」一节同源）：
//   --window [宽x高]   窗口模式启动（默认 1280x720，放在屏幕左上角，便于截图）
//   --fast             磁带回归：每个循环正好推一个逻辑步（不吃真实时间）且每 64 帧才呈现一次
//   --load <路径.sav>  启动即读档（存档系统是正式系统，这条开关不受调试宏影响）
//   input_script <路径.txt> 显式启用输入脚本
//   --input <后端>     auto（默认）/ gameinput / win32：选输入后端，win32 才能被外部注入按键
//   --trace <路径.csv>  逐逻辑步输出轨迹（只在 MONO_DEBUG_BUILD 的构建里生效）
// 解析只在启动时做一次，结果写进 global_command_line，之后各功能只读那个结构。
// 不要退回成「各处自己 wcsstr 捞字符串」：那样看不出总共有哪些开关，
// 而且子串匹配会让 --windowed 之类误命中 --window。

// 取下一个空白分隔的 token：返回起点，*end 收到终点；没有更多 token 时返回 nullptr。
// token 只是 [begin, end) 一段，要当字符串用先经 copy_token
internal const wchar_t *next_token(const wchar_t *pos, const wchar_t **end)
{
    while (*pos == L' ' || *pos == L'\t') {
        ++pos;
    }
    if (*pos == L'\0') {
        return nullptr;
    }

    const wchar_t *begin = pos;
    while (*pos != L'\0' && *pos != L' ' && *pos != L'\t') {
        ++pos;
    }
    *end = pos;
    return begin;
}

// token 是否整词等于 name（不做前缀匹配：--window 不该被 --windowed 命中）
internal bool token_equals(const wchar_t *begin, const wchar_t *end, const wchar_t *name)
{
    while (begin < end && *name != L'\0' && *begin == *name) {
        ++begin;
        ++name;
    }
    return begin == end && *name == L'\0';
}

// 把 token 复制进定长缓冲并补 '\0'；放不下时截断并返回 false
internal bool copy_token(const wchar_t *begin, const wchar_t *end, wchar_t *out, u32 out_size)
{
    u32 len = (u32)(end - begin);
    bool fits = (len + 1 <= out_size);
    if (!fits) {
        len = out_size - 1;
    }

    memcpy(out, begin, (u64)len * sizeof(wchar_t));
    out[len] = L'\0';
    return fits;
}

// 解析 "宽x高"；格式不对或含 0 都算失败，调用方保留默认尺寸
internal bool parse_size_token(const wchar_t *begin, const wchar_t *end, u32 *width, u32 *height)
{
    wchar_t text[32];
    if (!copy_token(begin, end, text, array_size(text))) {
        return false;
    }

    u32 parsed_w = 0;
    u32 parsed_h = 0;
    if (swscanf_s(text, L"%ux%u", &parsed_w, &parsed_h) != 2 || parsed_w == 0 || parsed_h == 0) {
        return false;
    }

    *width = parsed_w;
    *height = parsed_h;
    return true;
}

// 命令行解析 —— 全程序唯一入口
internal void parse_command_line(const wchar_t *args)
{
    // lpCmdLine 不会是空指针（没有参数时是空字符串）
    if (args[0] == L'\0') {
        return;
    }

    const wchar_t *at = args;
    for (;;) {
        const wchar_t *token_end = nullptr;
        const wchar_t *token = next_token(at, &token_end);
        if (!token) {
            break;
        }
        at = token_end;

        if (token_equals(token, token_end, L"--window")) {
            global_command_line.windowed = true;

            // 尺寸可选，缺省就用默认值；以 '-' 开头的下一个选项不当值用
            const wchar_t *value_end = nullptr;
            const wchar_t *value = next_token(at, &value_end);
            if (value && *value != L'-') {
                at = value_end;
                if (!parse_size_token(value, value_end,
                                      &global_command_line.window_width,
                                      &global_command_line.window_height)) {
                    LOG_WARN("command line: expected --window <width>x<height>, using default size");
                }
            }
        } else if (token_equals(token, token_end, L"--fast")) {
            global_command_line.fast = true;
        } else if (token_equals(token, token_end, L"--load")) {
            // 存档系统是正式系统：这条开关不用调试宏包住
            const wchar_t *value_end = nullptr;
            const wchar_t *value = next_token(at, &value_end);
            if (!value || *value == L'-') {
                LOG_WARN("command line: --load expects a path");
            } else {
                at = value_end;
                if (!copy_token(value, value_end, global_command_line.sav_load_path,
                                array_size(global_command_line.sav_load_path))) {
                    LOG_WARN("command line: --load path is too long, ignored");
                }
            }
        } else if (token_equals(token, token_end, L"input_script")) {
            const wchar_t *value_end = nullptr;
            const wchar_t *value = next_token(at, &value_end);
            if (!value || *value == L'-') {
                LOG_WARN("command line: input_script expects a path");
            } else {
                at = value_end;
#if MONO_DEBUG_INPUT
                if (!copy_token(value, value_end, global_command_line.input_script_path,
                                array_size(global_command_line.input_script_path))) {
                    LOG_WARN("command line: input_script path is too long, ignored");
                }
#endif
            }
        } else if (token_equals(token, token_end, L"--input")) {
            // 后端名→枚举的映射只在这里：命令行词汇表属于 CLI 层，输入层只认枚举
            const wchar_t *value_end = nullptr;
            const wchar_t *value = next_token(at, &value_end);
            if (!value || *value == L'-') {
                LOG_WARN("command line: --input expects auto / gameinput / win32");
            } else {
                at = value_end;
                if (token_equals(value, value_end, L"auto")) {
                    global_command_line.input_backend = INPUT_BACKEND_AUTO;
                } else if (token_equals(value, value_end, L"gameinput")) {
                    global_command_line.input_backend = INPUT_BACKEND_GAMEINPUT;
                } else if (token_equals(value, value_end, L"win32")) {
                    global_command_line.input_backend = INPUT_BACKEND_WIN32;
                } else {
                    LOG_WARN("command line: unknown input backend, expected auto / gameinput / win32");
                }
            }
        } else if (token_equals(token, token_end, L"--trace")) {
            // 路径可选（漏了就只当没写）
            const wchar_t *value_end = nullptr;
            const wchar_t *value = next_token(at, &value_end);
            bool has_path = (value != nullptr) && (*value != L'-');
            if (has_path) {
                at = value_end;
            }
#if MONO_DEBUG_BUILD
            if (has_path && !copy_token(value, value_end, global_command_line.trace_path,
                                        array_size(global_command_line.trace_path))) {
                LOG_WARN("command line: --trace path is too long, ignored");
            }
#endif
        } else if (*token == L'-') {
            // 未知选项只提示一句，不影响其它参数
            wchar_t unknown[64];
            copy_token(token, token_end, unknown, array_size(unknown));
            LOG_WARN("command line: unknown option \"%ls\"", unknown);
        }
    }
}

// ============================================================================
// Windows 相关
// ============================================================================

// 无边框全屏与窗口模式切换
internal void toggle_fullscreen(HWND hwnd)
{
    // GWL_STYLE：读取窗口的样式标志位（是否带标题栏、边框、可缩放、最小化按钮等）
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);

    if (!global_is_fullscreen) {
        // 窗口模式 -> 无边框全屏
        // 直接保存屏幕坐标矩形而非 WINDOWPLACEMENT
        global_windowed_style = style;
        GetWindowRect(hwnd, &global_windowed_rect);

        MONITORINFO monitor_info = {};
        monitor_info.cbSize = sizeof(MONITORINFO);

        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor_info)) {
            // 去掉 WS_OVERLAPPEDWINDOW 窗口变成无边框
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            // 一次性修改窗口的位置、尺寸和 Z 顺序，并让样式修改生效
            SetWindowPos(hwnd, HWND_TOP,
                         monitor_info.rcMonitor.left,
                         monitor_info.rcMonitor.top,
                         monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
                         monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            global_is_fullscreen = true;
        }
    } else {
        // 无边框全屏 -> 窗口模式：用保存的屏幕坐标矩形精确恢复
        SetWindowLong(hwnd, GWL_STYLE, global_windowed_style);
        SetWindowPos(hwnd, HWND_TOP,
                     global_windowed_rect.left, global_windowed_rect.top,
                     global_windowed_rect.right - global_windowed_rect.left,
                     global_windowed_rect.bottom - global_windowed_rect.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        global_is_fullscreen = false;
    }
}

// 光标可见性 —— 全程序唯一的决策点，每帧应用一次：
//   全屏且 UI 未打开时鼠标没有用处，隐藏它（也避免指针停在画面中央）；
//   其余情况显示光标（UI 打开时哪怕现在只用键盘选项，也得看得出鼠标可用）。
// 不写成全局变量：变量需要一个写入者，而光标可见性是「策略」不是「状态」——
// 真正的状态（是否全屏、UI 是否打开）各有各的主人。
// 为什么不只在 WM_SETCURSOR 里做：那条消息只在**鼠标移动 / 窗口激活**时来，
// 而 UI 是被按键（ESC / 手柄 Start）打开的 —— 那一刻鼠标没动，靠消息就永远等不到光标，
// 玩家得先晃一下鼠标才看得见指针。
internal void apply_cursor_visibility()
{
    // global_game_state 在主循环开始之前就已经指向 game_state，不会是空
    bool visible = global_game_state->ui.open || !global_is_fullscreen;
    SetCursor(visible ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
}

// 消息回调
internal LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // 所有消息都转发给输入后端（非消息后端为空实现，运行期无分支）
    input_on_message((u32)uMsg, (u64)wParam, (u64)lParam);

    LRESULT result = 0;

    switch (uMsg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_KEYDOWN:
        // ESC 不再在这里退出：它现在是一个普通动作（GA_MAP = 开关大地图），
        // 交给输入层与固定步长处理 —— 否则它永远最先被窗口过程吃掉，UI 拿不到它。
        // 「退出游戏」改用 Alt+F4 / 关闭窗口（WM_CLOSE 仍然在这里处理）。
        if (wParam == VK_F11 && !(lParam & (1 << 30))) {
            toggle_fullscreen(hwnd);
        }
#if MONO_DEBUG_INPUT
        if (wParam == VK_F7 && !(lParam & (1 << 30)) && global_game_state) {
            // 打印当前逻辑步与玩家状态，写脚本时用来取样
            replay_log_state(global_logic_step, global_game_state);
        }

        // 录制 / 回放 / 标记 / 导出。全部键都在磁带没在跑时才有效（F6 自己负责结束回放），
        // 于是「脚本跑到一半被热键接管」这类干扰不会发生
        if (!(lParam & (1 << 30)) && global_game_state) {
            switch (wParam) {
            case VK_F5:
                if (replay_is_recording()) {
                    replay_record_stop(); // 结束并导出脚本
                } else if (replay_is_playing()) {
                    LOG_WARN("replay: cannot record while a tape is playing");
                } else {
                    replay_record_start(global_game_state, &global_game_input);
                }
                break;
            case VK_F6:
                if (replay_is_playing()) {
                    replay_end();
                } else if (!replay_start_last(global_game_state, &global_game_input)) {
                    LOG_WARN("replay: nothing to replay (record with F5 first, or run a script)");
                }
                break;
            case VK_F8:
                replay_mark_assert(global_game_state);
                break;
            case VK_F4:
                if (!replay_export_script()) {
                    LOG_WARN("replay: nothing to export (record with F5, or run a script first)");
                }
                break;
            default:
                break;
            }
        }
#endif
        break;
    case WM_SIZE:
        // 不是最小化窗口事件
        if (wParam != SIZE_MINIMIZED) {
            global_pending_client_width = LOWORD(lParam);
            global_pending_client_height = HIWORD(lParam);
            global_resize_pending = true;
        }
        break;
    case WM_SETCURSOR:
        // 光标可见性不在这里判：策略每帧应用一次（见 apply_cursor_visibility 的注释）。
        // 返回 TRUE = 已处理，别再让系统按窗口类去设一次默认光标
        result = TRUE;
        break;
    default:
        result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    return result;
}

internal bool windows_start_init()
{
    log_init(LOG_FILE_PATH);

    // 初始化持久分配器（1GB）
    if (!arena_init(GB(1))) {
        return false;
    }

    // DPI 感知
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    return true;
}

// ============================================================================
// 帧计时
// ============================================================================

// 帧间真实耗时。`--fast` 不用它（那时逻辑步不吃真实时间），所以它只在正常模式下有意义。
// 包成结构体而不是散在 wWinMain 里的局部变量：主循环里就只剩一行，读代码时不必关心 QPC 的细节
struct FrameClock
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER previous;
};

internal void frame_clock_init(FrameClock *clock)
{
    QueryPerformanceFrequency(&clock->frequency);
    QueryPerformanceCounter(&clock->previous);
}

internal f32 frame_clock_tick(FrameClock *clock)
{
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);

    f32 seconds = (f32)(now.QuadPart - clock->previous.QuadPart) / (f32)clock->frequency.QuadPart;
    clock->previous = now;
    return seconds;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int)
{
    if (!windows_start_init()) {
        return 0;
    }
    // defer：拿到一个资源就登记一条，退出时按登记的**逆序**自动清理。
    // 于是下面每条失败路径都只需要 `return 0`，不必各自抄一份清理清单
    // （以前 5 条退出路径的清理集合各不相同，只能靠人肉维护）
    defer { log_shutdown(); };

    parse_command_line(lpCmdLine);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = GAME_NAME;
    // 注册窗口类
    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("RegisterClassExW failed");
        return 0;
    }

    // 主显示器的完整矩形（DPI 感知已开启，这里拿到的是物理像素）
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor_info);

    int monitor_x = monitor_info.rcMonitor.left;
    int monitor_y = monitor_info.rcMonitor.top;
    int monitor_w = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    int monitor_h = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;

    // 命令行 --window [宽x高]：窗口模式启动（放在屏幕左上角，方便截图与多窗口调试）
    bool windowed_start = global_command_line.windowed;

    // 窗口模式下的客户区尺寸（默认 1280x720，写在 CommandLine 里）
    u32 windowed_client_w = global_command_line.window_width;
    u32 windowed_client_h = global_command_line.window_height;

    // F11 从全屏切回窗口模式时要恢复到的矩形：带边框，居中放置
    RECT windowed_rect = { 0, 0, (LONG)windowed_client_w, (LONG)windowed_client_h };
    AdjustWindowRect(&windowed_rect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowed_w = windowed_rect.right - windowed_rect.left;
    int windowed_h = windowed_rect.bottom - windowed_rect.top;
    int windowed_x = windowed_start ? monitor_x : monitor_x + (monitor_w - windowed_w) / 2;
    int windowed_y = windowed_start ? monitor_y : monitor_y + (monitor_h - windowed_h) / 2;

    global_windowed_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    global_windowed_rect = { windowed_x, windowed_y, windowed_x + windowed_w, windowed_y + windowed_h };
    global_is_fullscreen = !windowed_start;

    // 创建窗口：全屏是无边框窗口，必须贴在显示器原点并正好铺满显示器；
    // 窗口模式用带边框窗口按上面算好的矩形放置。
    // （这两者不能用同一组坐标：窗口模式的居中位置直接拿来建全屏窗口会跨屏）
    int create_x = windowed_start ? windowed_x : monitor_x;
    int create_y = windowed_start ? windowed_y : monitor_y;
    int create_w = windowed_start ? windowed_w : monitor_w;
    int create_h = windowed_start ? windowed_h : monitor_h;

    HWND hwnd = CreateWindowExW(
#if MONO_DEBUG_ANY
        0, // 调试中不加 WS_EX_TOPMOST：否则全屏时会把 IDE/终端完全盖住，看不到日志
#else
        WS_EX_TOPMOST,
#endif
        GAME_NAME, GAME_NAME,
        windowed_start ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE) : (WS_POPUP | WS_VISIBLE),
        create_x, create_y, create_w, create_h,
        nullptr, nullptr, hInstance, nullptr);

    if (hwnd == nullptr) {
        LOG_ERROR("CreateWindowExW failed");
        return 0;
    }
    // 正常退出时窗口已经被 WM_CLOSE 销毁过，IsWindow 用来跳过那次重复销毁
    defer {
        if (IsWindow(hwnd)) {
            DestroyWindow(hwnd);
        }
    };

    // 禁用输入法上下文
    ImmAssociateContext(hwnd, nullptr);

    // ============================================================================
    // 输入与渲染初始化
    // ============================================================================

    input_init(hwnd, global_command_line.input_backend);
    defer { input_shutdown(); };

    // 客户区尺寸：窗口模式是请求值，全屏就是整个显示器
    u32 client_width = windowed_start ? windowed_client_w : (u32)monitor_w;
    u32 client_height = windowed_start ? windowed_client_h : (u32)monitor_h;

    Renderer *renderer = renderer_create(hwnd, client_width, client_height);
    if (!renderer) {
        LOG_ERROR("renderer_create failed");
        return 0;
    }
    defer { renderer_destroy(renderer); };

    // 磁带回归：把呈现节奏放松到每 64 帧一次。它必须与 `--fast` 的「每循环一个逻辑步」配合才有意义
    // —— 光放松呈现的话，逻辑仍然按墙钟以 60Hz 推进，回放不会变快
    if (global_command_line.fast) {
        renderer_set_present_interval(renderer, FAST_PRESENT_INTERVAL);
        LOG_WARN("command line: --fast drives one logic step per frame without waiting for real time (script/replay only)");
    }

    // ============================================================================
    // 音频系统初始化
    // ============================================================================
    AudioState *audio = audio_create();
    // audio_destroy 对 nullptr 与半初始化状态都安全（音频不可用时只降级为静音）
    defer { audio_destroy(audio); };
    if (!audio) {
        // 音频不是运行的必需条件（例如没有输出设备），失败只降级为静音
        LOG_WARN("audio_create failed, running muted");
    } else {
        // 解析素材到 arena 常驻并起播背景音乐。解析很贵，音频不可用时不做无意义的解析
        game_audio_init(audio);
    }

    // ============================================================================
    // 游戏资源
    // ============================================================================
    GameState game_state = {};
    global_game_state = &game_state;

    scratch_init(&global_scratch, MB(64));
    defer { scratch_shutdown(&global_scratch); };
    if (!game_init_asset(&game_state, client_width, client_height)) {
        LOG_ERROR("game_init_asset failed (check data/map/*.bin)");
        return 0;
    }

    // 快照自检（save → load → save 逐位一致）：进主循环之前跑一次。
    // 失败只报一声 —— 存档与回放都不可信，但没必要让游戏起不来
    if (!game_state_snapshot_selftest(&game_state)) {
        LOG_WARN("game: snapshot self-test failed — saves and replays cannot be trusted");
    }

    // 纹理上传：关卡/角色自己的贴图 + 程序化占位贴图。
    // 具体有哪些贴图收在 scene.h 的 SceneTextures 里（以前是这里二十来个散落的局部变量）
    SceneTextures textures = {};
    scene_init(renderer, &textures, &game_state);

    scratch_reset(&global_scratch);

    // ============================================================================
    // 命令行：--load <路径.sav> 启动即读档；input_script <路径.txt> 启用输入脚本；
    //         --trace <路径.csv> 输出逐逻辑步轨迹
    // ============================================================================

    // 存档系统是正式系统，这条开关与调试宏无关
    if (global_command_line.sav_load_path[0] != L'\0') {
        if (!save_restore(global_command_line.sav_load_path, &game_state)) {
            LOG_WARN("save: %ls cannot be loaded, starting from spawn", global_command_line.sav_load_path);
        }
    }

    // 脚本的第一条语句给出存档点，读档发生在第一个逻辑步之前（引擎入口里做）。
    // 与 --load 一起用时以脚本的存档点为准。
    // 载入失败就直接收摊并给退出码 1：脚本是给自动化用的，静默掉进「手动游玩」比报错更难发现
    int exit_code = 0;
#if MONO_DEBUG_INPUT
    if (global_command_line.input_script_path[0] != L'\0' &&
        !input_script_run(global_command_line.input_script_path, &game_state, &global_game_input)) {
        LOG_ERROR("Input script: cannot load %ls", global_command_line.input_script_path);
        exit_code = 1;
        global_running = false;
    }
#endif

#if MONO_DEBUG_BUILD
    if (global_command_line.trace_path[0] != L'\0') {
        trace_open(global_command_line.trace_path);
    }
#endif

    FrameClock frame_clock = {};
    frame_clock_init(&frame_clock);

    // `--fast`：逻辑步不吃真实时间（每个循环正好一步）。命令行解析之后就不再改变，
    // 所以提到循环外 —— 热路径里每帧都要判它，而全局变量在函数调用之后必须重新载入
    const bool fast_mode = global_command_line.fast;

    // 固定步长累加器：把真实经过的时间切成 FIXED_TIMESTEP 的小份，
    // 剩余不足一步的时间留到下一帧继续累计，保证逻辑更新频率恒定。
    f32 accumulator = 0.0f;

    log_system_info();

    // ============================================================================
    // 主循环
    // ============================================================================
    while (global_running) {
        MSG msg = {};
        // 非阻塞消息循环
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                global_running = false;
            }
            DispatchMessageW(&msg);
        }

        // 处理挂起的窗口尺寸变化
        if (global_resize_pending) {
            global_resize_pending = false;
            client_width = global_pending_client_width;
            client_height = global_pending_client_height;
            if (client_width > 0 && client_height > 0) {
                renderer_resize(renderer, client_width, client_height);
            }
        }

        // 光标策略每帧应用一次（UI 可能被按键打开，那条路上没有 WM_SETCURSOR）
        apply_cursor_visibility();

        // 输入源二选一（互斥）：磁带在跑时由它驱动，否则轮询真实设备
        InputSource input_source = INPUT_SOURCE_BACKEND;
#if MONO_DEBUG_INPUT
        if (replay_is_playing()) {
            input_source = INPUT_SOURCE_TAPE;
        }
#endif

        // 每渲染帧采集一次输入（后端轮询）。只在真实设备驱动时采集：
        // 磁带期间如果照常轮询，攒下来的粘滞边沿会在切回设备的第一帧一次性灌进游戏
        if (input_source == INPUT_SOURCE_BACKEND) {
            input_begin_frame();
        }

        // 本帧的真实耗时 → 累加器。`--fast` 直接喂一个固定步（跳过 QPC）：每个循环正好推进
        // 一个逻辑步，逻辑因此不再受墙钟限制（呈现节奏另由 FAST_PRESENT_INTERVAL 放松到 1/64）
        // —— 这是单条用例能秒级跑完的关键。代价：它只能配磁带用，手动游玩会被快进到不可玩（启动时 WARN 一句）
        f32 frame_dt = fast_mode ? FIXED_TIMESTEP : frame_clock_tick(&frame_clock);

        // 固定步长更新：以 FIXED_TIMESTEP 为单位递增逻辑，使游戏始终以恒定的 60Hz 运行
        accumulator += frame_dt;
        if (accumulator > MAX_ACCUMULATOR) {
            accumulator = MAX_ACCUMULATOR;
        }

        while (accumulator >= FIXED_TIMESTEP) {
            // 同一时刻只用一个输入源驱动 GameInput：
            // 磁带整帧接管输入（含边沿），所以这条路径不经过 input_step
            if (input_source == INPUT_SOURCE_TAPE) {
                replay_before_step(&global_game_input);
            } else {
                input_step(&global_game_input);
                replay_record_input(&global_game_input);
            }

            // 调试可视化：本逻辑步的临时图元从零开始收集（静态体保留）
            debug_vis_begin_step();

            // 保存上一逻辑步状态，供渲染插值使用
            game_state.prev_player_x = game_state.player_x;
            game_state.prev_player_y = game_state.player_y;
            game_state.prev_camera = game_state.camera;
            game_state.prev_movers = game_state.movers;
            game_state.prev_monster = game_state.monster;
            game_state.prev_projectiles = game_state.projectiles;

            game_update(&global_game_input, &game_state, FIXED_TIMESTEP, client_width, client_height);

            // 磁带：求值本帧的操作（断言 / 探针 / 写档）；脚本跑完时由它通知主循环退出
            if (replay_after_step(&game_state, &global_game_input)) {
                global_running = false;
            }

            // 轨迹文件：记录本逻辑步结束后的状态（磁带未在跑时也能用）
            trace_step(global_logic_step, &global_game_input, &game_state);
            ++global_logic_step;

            accumulator -= FIXED_TIMESTEP;
        }

        // 渲染插值：用本渲染帧剩余时间在「上一逻辑步」与「当前逻辑步」之间插值，
        // 让 60Hz 的逻辑在更高刷新率的屏幕上平滑呈现，消除卡顿感。
        f32 alpha = accumulator / FIXED_TIMESTEP;
        Camera2D render_camera = game_state.camera;
        render_camera.pos_x = lerp(game_state.prev_camera.pos_x, game_state.camera.pos_x, alpha);
        render_camera.pos_y = lerp(game_state.prev_camera.pos_y, game_state.camera.pos_y, alpha);
        render_camera.zoom = lerp(game_state.prev_camera.zoom, game_state.camera.zoom, alpha);

        // 相机吸附到**整数设备像素**。
        // 内容在屏幕上的位置是 (世界 - 相机) * zoom，而精灵走 POINT 采样（最近邻）：相机带小数
        // 就意味着每个 1px 细节（细线、砖缝）在两帧里可能落在相邻的像素上 → 来回跳
        // （pixel shimmer）。指数跟随的尾段每帧只挪零点几像素，所以抖动集中在那里。
        // 吸附量必须是 1 个设备像素（除以 zoom），否则 zoom = 2 时只吸到半个像素；
        // 只吸附这份**渲染用**的相机 —— 进快照/回放/trace 的逻辑相机保持小数，确定性不受影响。
        f32 render_zoom = render_camera.zoom;
        render_camera.pos_x = roundf(render_camera.pos_x * render_zoom) / render_zoom;
        render_camera.pos_y = roundf(render_camera.pos_y * render_zoom) / render_zoom;

        // ========================================================================
        // 提交本帧绘制列表（世界物件 → 调试可视化 → UI）
        //
        // 具体画什么全在 scene.cc —— 装配层只管「什么时候画、用哪个相机」。
        // 插值系数 alpha 在这里算出来交给它：逻辑步保持 60Hz 的整数步，画面才是平滑的。
        // ========================================================================
        renderer_frame_begin(renderer, render_camera.pos_x, render_camera.pos_y, render_camera.zoom,
                             client_width, client_height);

        SceneFrame scene_frame = {};
        scene_frame.game_state = &game_state;
        scene_frame.alpha = alpha;
        scene_frame.camera_zoom = render_zoom;
        scene_frame.client_width = client_width;
        scene_frame.client_height = client_height;
        scene_submit(renderer, &textures, &scene_frame);

        renderer_frame_end(renderer);
    }

    // ============================================================================
    // 收尾
    // ============================================================================
#if MONO_DEBUG_INPUT
    // 启动阶段已经判过「脚本根本没载入」的那种失败（exit_code = 1），不要被引擎的 0 覆盖
    if (exit_code == 0) {
        exit_code = replay_exit_code();
    }
#endif
    trace_close();
    return exit_code;
}
