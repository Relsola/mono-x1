// 关卡编辑器的宿主：Win32 窗口 + D3D11 + ImGui。
//
// 为什么自己建 D3D11 设备、不复用游戏的 d3d12_renderer.cc：
//   那个渲染器对外只给「纹理句柄 + 绘制列表」，刻意不暴露 device/queue/backbuffer（分层铁律），
//   而 ImGui 的 dx11 后端需要 device + context + 自己的 render target。
//   所以编辑器自带一份最小的设备/交换链代码（约 100 行），完全不认识游戏的渲染层。
//   （2026-10-01 游戏渲染层迁到 D3D12 后，这里**刻意留在 D3D11**：编辑器只用 ImGui 画界面，
//    迁过去要额外引入 imgui_impl_dx12 + d3dx12.h、把宿主从 100 行撑到两百多行，
//    而且它的 `--check` / `--selftest` 不建窗口，那段代码根本测不到。代价是仓库里
//    同时存在两代设备代码 —— 两个程序各自独立，不共享进程也不共享渲染器。）
//
// 着色器与数据路径都按**进程工作目录**解析（和游戏一样，从仓库根启动）：
//   data/map/*.bin、build/editor_imgui.ini

#pragma comment(lib, "user32")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")

#include "win32_prefix.h"

#include <d3d11.h>
#include <dxgi1_3.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "core.h"
#include "shared/memory.h"
#include "shared/file.h"
#include "shared/logger.h"
#include "editor_doc.h"
#include "editor_edit.h"
#include "editor_ui.h"

// 编辑器自己的 arena：比游戏小得多（不用装音频解码/贴图），
// 但要注意诊断会按次分配（见 editor_doc.cc 的注释），256MB 留足了余量。
inline constexpr u64 EDITOR_ARENA_SIZE = MB(256);

inline constexpr u32 EDITOR_WINDOW_WIDTH = 1920;
inline constexpr u32 EDITOR_WINDOW_HEIGHT = 1200;

global_variable HWND global_window;
global_variable ID3D11Device *global_device;
global_variable ID3D11DeviceContext *global_device_context;
global_variable IDXGISwapChain1 *global_swap_chain;
global_variable ID3D11RenderTargetView *global_render_target;
global_variable bool global_running = true;
global_variable EditorUiState global_editor;

// ImGui 的后置声明（它的头里带了 windows.h 依赖，所以声明在包含之后）
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

internal void editor_create_render_target()
{
    ID3D11Texture2D *back_buffer = nullptr;
    if (FAILED(global_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back_buffer))) {
        return;
    }
    global_device->CreateRenderTargetView(back_buffer, nullptr, &global_render_target);
    back_buffer->Release();
}

internal void editor_resize(u32 width, u32 height)
{
    if (!global_swap_chain || width == 0 || height == 0) {
        return;
    }
    // 释放对后台缓冲的全部引用后才能 ResizeBuffers
    global_device_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (global_render_target) {
        global_render_target->Release();
        global_render_target = nullptr;
    }
    global_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    editor_create_render_target();
}

// 与游戏一致：进程自己声明 per-monitor-v2 DPI 感知。
// 不声明的话 Windows 会把整个窗口坐标虚拟化：150% 的机器上 1920×1200 的客户区会变成
// 2880×1800 物理像素，而 ImGui 拿到的还是虚拟尺寸（GetDpiScaleForHwnd 返回 1.0），
// 最后 DXGI 把 1920×1200 的画布拉伸到 2880×1800 —— 界面看着一样大，但文字是软的。
// 返回值是系统 DPI 缩放，用来把下面那个「逻辑尺寸」换算成物理客户区。
internal f32 editor_setup_dpi_awareness()
{
    // 失败就继续（比如清单里已经声明过）—— 那时 GetDpiForSystem 会给 96，算出 1.0，行为同以前
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    UINT dpi = GetDpiForSystem();
    if (dpi == 0) {
        dpi = 96;
    }
    return (f32)dpi / 96.0f;
}

internal LRESULT CALLBACK editor_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return 1;
    }

    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            editor_resize((u32)LOWORD(lparam), (u32)HIWORD(lparam));
        }
        return 0;
    case WM_SYSCOMMAND:
        // 屏蔽 Alt 打开的窗口菜单（编辑器里 Alt 常被当组合键用）
        if ((wparam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_CLOSE:
        global_running = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

internal bool editor_create_device_and_swap_chain(HWND hwnd, u32 width, u32 height)
{
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_10_0;

    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, feature_levels,
                                       array_size(feature_levels), D3D11_SDK_VERSION, &global_device,
                                       &feature_level, &global_device_context);
    if (FAILED(result) || !global_device) {
        MessageBoxW(nullptr, L"D3D11CreateDevice 失败", L"编辑器启动失败", MB_OK | MB_ICONERROR);
        return false;
    }

    IDXGIFactory2 *factory2 = nullptr;
    result = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&factory2);
    if (FAILED(result) || !factory2) {
        MessageBoxW(nullptr, L"CreateDXGIFactory2 失败", L"编辑器启动失败", MB_OK | MB_ICONERROR);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount = 2;
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; // Flip Model 不支持 MSAA
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    result = factory2->CreateSwapChainForHwnd(global_device, hwnd, &desc, nullptr, nullptr, &global_swap_chain);
    if (FAILED(result) || !global_swap_chain) {
        factory2->Release();
        MessageBoxW(nullptr, L"CreateSwapChainForHwnd 失败", L"编辑器启动失败", MB_OK | MB_ICONERROR);
        return false;
    }

    factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory2->Release();

    editor_create_render_target();
    return global_render_target != nullptr;
}

// 中文字体：ImGui 自带的字体没有 CJK 字形，不换字体的话界面上的中文全是 '?'。
// 1.92 起字形是按需加载的，所以不需要手写 glyph ranges —— 只要字体文件本身有那些字。
internal void editor_load_fonts(f32 dpi_scale)
{
    static const wchar_t *const candidates[] = {
        L"C:\\Windows\\Fonts\\msyh.ttc",  // 微软雅黑
        L"C:\\Windows\\Fonts\\msyh.ttf",
        L"C:\\Windows\\Fonts\\simhei.ttf", // 黑体
        L"C:\\Windows\\Fonts\\simsun.ttc", // 宋体
    };

    ImGuiIO &io = ImGui::GetIO();
    f32 size = 17.0f * dpi_scale;

    for (const auto &candidate : candidates) {
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        char utf8[260] = {};
        if (!wide_to_utf8(candidate, utf8, sizeof(utf8))) {
            continue;
        }
        // AddFontFromFileTTF 在文件打不开时会断言，所以上面先确认文件真的存在
        if (io.Fonts->AddFontFromFileTTF(utf8, size)) {
            return;
        }
    }

    io.Fonts->AddFontDefault();
    editor_message(&global_editor.messages, "没找到中文字体，界面上的中文会显示成 '?'（改 editor_main.cc 的字体路径）");
}

// 命令行：--check [<map.bin>] [--smoke] [--out <报告.txt>]，位置参数是关卡路径。
// 不给 --check 就是正常开窗口（位置参数决定打开哪张图，不给则默认第一世界）。
// 与游戏一样：按空白切分、**不去引号**，所以带空格的路径传不进来。
struct EditorCommandLine
{
    bool check;          // 无界面自检
    bool smoke;          // 自检完再让主程序跑一遍 smoke 用例
    bool selftest;       // 跑编辑语义的单元用例（editor_edit.cc）
    wchar_t map_path[EDITOR_PATH_SIZE];    // 开关后面跟着的路径会被单独拷出来（以 '\0' 结尾）
    wchar_t report_path[EDITOR_PATH_SIZE];
};

// 把 [begin, end) 拷成以 '\0' 结尾的缓冲：命令行里 token 之间是空白，
// 拿指针指着它就会把后面的参数一起读进去（以前只有路径一个参数所以没暴露）
internal void command_line_copy_token(const wchar_t *begin, const wchar_t *end, wchar_t *out, u32 out_size)
{
    u32 len = 0;
    while (begin + len != end && len + 1 < out_size) {
        out[len] = begin[len];
        ++len;
    }
    out[len] = L'\0';
}

internal bool token_equals(const wchar_t *begin, const wchar_t *end, const wchar_t *text)
{
    u32 i = 0;
    while (begin + i != end && text[i] != L'\0') {
        if (begin[i] != text[i]) {
            return false;
        }
        ++i;
    }
    return (begin + i == end) && text[i] == L'\0';
}

// cmd_line 传 GetCommandLineW()，不会是空指针
internal void editor_parse_command_line(const wchar_t *cmd_line, EditorCommandLine *out)
{
    *out = {};
    const wchar_t *at = cmd_line;
    while (*at != L'\0') {
        while (*at == L' ' || *at == L'\t') {
            ++at;
        }
        if (*at == L'\0') {
            break;
        }
        const wchar_t *begin = at;
        while (*at != L'\0' && *at != L' ' && *at != L'\t') {
            ++at;
        }
        const wchar_t *end = at;

        if (token_equals(begin, end, L"--check")) {
            out->check = true;
        } else if (token_equals(begin, end, L"--selftest")) {
            out->selftest = true;
        } else if (token_equals(begin, end, L"--smoke")) {
            out->smoke = true;
        } else if (token_equals(begin, end, L"--out")) {
            // 下一个 token 是报告路径
            while (*at == L' ' || *at == L'\t') {
                ++at;
            }
            if (*at != L'\0') {
                const wchar_t *value_begin = at;
                while (*at != L'\0' && *at != L' ' && *at != L'\t') {
                    ++at;
                }
                command_line_copy_token(value_begin, at, out->report_path, EDITOR_PATH_SIZE);
            }
        } else if (*begin != L'-' && out->map_path[0] == L'\0') {
            command_line_copy_token(begin, end, out->map_path, EDITOR_PATH_SIZE);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmd_line, int)
{
    if (!arena_init(EDITOR_ARENA_SIZE)) {
        MessageBoxW(nullptr, L"arena 初始化失败", L"编辑器启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 日志必须在最早期就指到自己的文件：没显式初始化时 logger 会用默认路径（game.log），
    // 而编辑器随时会起游戏，两边挤在同一个文件里会互相抹掉
    log_init(L"editor.log");

    EditorCommandLine command_line = {};
    editor_parse_command_line(cmd_line, &command_line);

    // 无界面自检：不建窗口、不初始化 D3D/ImGui —— 所以放在一切窗口工作之前。
    // 它给的是「文本报告 + 退出码」，人和 AI 都能直接用（见 editor/README.md）
    if (command_line.check) {
        bool ok = editor_check_run((command_line.map_path[0] != L'\0') ? command_line.map_path : nullptr,
                                   command_line.smoke,
                                   (command_line.report_path[0] != L'\0') ? command_line.report_path : nullptr);
        LOG_INFO("editor: --check exit %d", ok ? 0 : 1);
        log_shutdown();
        return ok ? 0 : 1;
    }

    // 两份单元用例：编辑语义（命中 / 拖动 / 吸附）+ 文档与撤销。结果写 editor.log，退出码给自动化
    if (command_line.selftest) {
        u32 edit_failed = editor_edit_run_selftest() ? 0 : 1;
        u32 doc_failed = editor_doc_run_selftest();
        bool ok = edit_failed == 0 && doc_failed == 0;
        LOG_INFO("editor: --selftest exit %d (edit %u failed, doc %u failed)", ok ? 0 : 1, edit_failed, doc_failed);
        log_shutdown();
        return ok ? 0 : 1;
    }

    const f32 system_scale = editor_setup_dpi_awareness();
    LOG_INFO("editor: starting windowed editor (dpi scale %.2f)", system_scale);

    // 写成「先清零再逐个赋值」而不是 `= { sizeof(...) }` 一行式：后者只初始化了 cbSize 这一个字段，
    // clang 的 -Wmissing-field-initializers 会把它报成错误（与 main.cc 的 WNDCLASSEXW 写法也统一了）
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = editor_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"MonoEditorWindowClass";
    RegisterClassExW(&window_class);

    RECT rect = { 0, 0, (LONG)(EDITOR_WINDOW_WIDTH * system_scale), (LONG)(EDITOR_WINDOW_HEIGHT * system_scale) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    // 放在屏幕 (0,0)：便于截图验证（与游戏 --window 的取向一致）
    global_window = CreateWindowExW(0, window_class.lpszClassName, L"关卡编辑器 — Mono", WS_OVERLAPPEDWINDOW, 0, 0,
                                    rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!global_window) {
        MessageBoxW(nullptr, L"CreateWindowExW 失败", L"编辑器启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!editor_create_device_and_swap_chain(global_window, (u32)(EDITOR_WINDOW_WIDTH * system_scale),
                                             (u32)(EDITOR_WINDOW_HEIGHT * system_scale))) {
        return 1;
    }

    ShowWindow(global_window, SW_SHOWDEFAULT);
    UpdateWindow(global_window);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    // 不要往仓库根写 imgui.ini
    io.IniFilename = "build/editor_imgui.ini";

    f32 dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(global_window);
    if (dpi_scale <= 0.0f) {
        dpi_scale = 1.0f;
    }
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(global_window);
    ImGui_ImplDX11_Init(global_device, global_device_context);

    editor_ui_initialize(&global_editor);
    global_editor.ui_scale = dpi_scale;
    editor_load_fonts(dpi_scale);

    // 命令行给了路径就打开它，否则默认打开第一世界；都不行就新建一个模板
    if (command_line.map_path[0] != L'\0') {
        if (!editor_doc_open(&global_editor.doc, command_line.map_path)) {
            editor_message(&global_editor.messages, "打不开命令行给的路径，改开默认关卡");
        }
    }
    if (!global_editor.doc.asset.tiles) {
        if (!editor_doc_open(&global_editor.doc, L"data/map/first.bin")) {
            editor_doc_new(&global_editor.doc, 64, 24, 64.0f);
            editor_message(&global_editor.messages, "默认关卡打不开，已新建空模板（另存为才会落盘）");
        }
    }

    while (global_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!global_running) {
            break;
        }
        if (IsIconic(global_window)) {
            Sleep(10); // 最小化时别满载跑
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        editor_ui_frame(&global_editor);
        if (global_editor.request_quit) {
            global_running = false;
        }

        ImGui::Render();

        const f32 clear_color[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
        global_device_context->OMSetRenderTargets(1, &global_render_target, nullptr);
        global_device_context->ClearRenderTargetView(global_render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        global_swap_chain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (global_render_target) {
        global_render_target->Release();
    }
    if (global_swap_chain) {
        global_swap_chain->Release();
    }
    if (global_device_context) {
        global_device_context->Release();
    }
    if (global_device) {
        global_device->Release();
    }

    LOG_INFO("editor: window closed");
    log_shutdown();
    return 0;
}
