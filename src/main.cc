#pragma comment(lib, "user32")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")
#pragma comment(lib, "GameInput.lib")
#pragma comment(lib, "Imm32.lib")
#pragma comment(lib, "xaudio2.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h> // Flip Model 交换链
#include <d3dcompiler.h>
#include <gameinput.h>
#include <xaudio2.h>

#include "core.h"

// COM 接口资源释放
#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

// ============================================================================
// 全局变量
// ============================================================================
struct D3D11_State;

global_variable constexpr wchar_t GAME_NAME[] = L"Mono";
global_variable constexpr u8 MAX_GAME_KEY_COUNT = 8; // 一次最大处理 8 个按键，一般来说足够了

// 运行标识
global_variable bool global_running = true;
global_variable bool global_mouse_on = true;

// 游戏输入
global_variable IGameInput *global_IGame_input;
global_variable GameInput global_game_input = {};

// 全屏切换坐标
global_variable bool global_is_fullscreen = true;
global_variable RECT global_windowed_rect = {};  // 窗口模式下的屏幕坐标矩形
global_variable DWORD global_windowed_style = 0; // 窗口模式下的窗口样式

// 全局状态指针
global_variable D3D11_State *global_d3d11;
global_variable GameState *global_game_state;

#if _DEBUG_BUILD
#include "xg/replay.h"
// 录制回放调试
global_variable ReplayRecorder global_recorder = {};
#endif

// ============================================================================
// Arena 和 IO
// ============================================================================

struct ArenaMemory
{
    u8 *base;
    u64 size;
    u64 used;
};

// 线性分配器
global_variable ArenaMemory global_arena = {};

void *arena_push(u64 size)
{
    // 8 字节对齐：保证返回值对 f32 / 指针 / 普通结构体都对齐，避免未对齐访问
    global_arena.used = (global_arena.used + 7) & ~7ull;
    assert((global_arena.used + size) <= global_arena.size);
    void *result = global_arena.base + global_arena.used;
    global_arena.used += size;
    return result;
}

void *arena_realloc(void *p, u64 oldsz, u64 newsz)
{
    assert(oldsz <= newsz);
    void *result = arena_push(newsz);
    memcpy(result, p, oldsz);
    return result;
}

ReadFileRes read_file(const wchar_t *filename)
{
    ReadFileRes result = {};
    HANDLE file_handle = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return result;
    }

    LARGE_INTEGER file_size_info;
    if (!GetFileSizeEx(file_handle, &file_size_info)) {
        return result;
    }

    result.contents = VirtualAlloc(0, file_size_info.QuadPart, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD bytes_read; // 实际读取的字节数
    result.file_size = safe_cast_u64(file_size_info.QuadPart);
    if (!ReadFile(file_handle, result.contents, result.file_size, &bytes_read, 0) || result.file_size != bytes_read) {
        free_file_memory(result.contents);
        result.contents = nullptr;
    }

    CloseHandle(file_handle);
    return result;
}

bool write_file(const wchar_t *filename, u32 size, void *memory)
{
    bool result = false;

    HANDLE file_handle = CreateFileW(filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return result;
    }

    DWORD bytes_read;
    if (WriteFile(file_handle, memory, size, &bytes_read, 0)) {
        result = bytes_read == size;
    }

    CloseHandle(file_handle);
    return result;
}

void free_file_memory(void *memory)
{
    if (memory) {
        VirtualFree(memory, 0, 0);
    }
}

// ============================================================================
// 输入
// ============================================================================

internal bool input_init(IGameInput **IGame_input)
{
    // 创建 GameInput 实例
    HRESULT hr = GameInputCreate(IGame_input);
    if (FAILED(hr)) {
        SAFE_RELEASE(*IGame_input);
        return false;
    }
    return true;
}

// 当前只处理键盘
internal void input_update(IGameInput *IGame_input, GameInput *input)
{
    for (u32 i = 0; i < GA_COUNT; ++i) {
        input->controller[0].previous[i] = input->controller[0].current[i]; // 上一帧存档
        input->controller[0].current[i] = false;                            // 清零后重新采集本帧
    }

    // 获取所有键盘和手柄当前帧最新输入
    IGameInputReading *reading = nullptr;
    HRESULT hr = IGame_input->GetCurrentReading(GameInputKindKeyboard, nullptr, &reading);
    if (SUCCEEDED(hr) && reading) {
        GameInputKind kind = reading->GetInputKind();
        if (kind == GameInputKindKeyboard) {
            u32 keyCount = reading->GetKeyCount();
            if (keyCount > 0) {
                // 截断保护
                if (keyCount > MAX_GAME_KEY_COUNT) {
                    keyCount = MAX_GAME_KEY_COUNT;
                }

                GameInputKeyState keyStates[MAX_GAME_KEY_COUNT];
                if (SUCCEEDED(reading->GetKeyState(keyCount, keyStates))) {
                    for (u32 i = 0; i < keyCount; ++i) {
                        switch (keyStates[i].virtualKey) {
                        case 0x26:
                            input->controller[0].current[GA_UP] = true;
                            break;
                        case 0x28:
                            input->controller[0].current[GA_DOWN] = true;
                            break;
                        case 0x25:
                            input->controller[0].current[GA_LEFT] = true;
                            break;
                        case 0x27:
                            input->controller[0].current[GA_RIGHT] = true;
                            break;
                        case 0x20:
                            input->controller[0].current[GA_SPACE] = true;
                            break;
                        case 0x51:
                            input->controller[0].current[GA_Q] = true;
                            break;
                        case 0x45:
                            input->controller[0].current[GA_E] = true;
                            break;
                        }
                    }
                }
            }
        }

        SAFE_RELEASE(reading);
    }

    // 统一计算边沿状态 pressed = 本帧刚按下（上升沿） released = 本帧刚松开（下降沿）
    for (u32 i = 0; i < GA_COUNT; ++i) {
        input->controller[0].pressed[i] = input->controller[0].current[i] && !input->controller[0].previous[i];
        input->controller[0].released[i] = !input->controller[0].current[i] && input->controller[0].previous[i];
    }
}

// ============================================================================
// 数学与变换
// ============================================================================

// XYZ 顶点坐标位置
// UV 纹理坐标 左上角(0, 0) 右上角(1, 0) 左下角(0, 1) 右下角(1, 1)
struct Vertex
{
    f32 x, y, z;
    f32 u, v;
};

// 4x4 行优先矩阵，与 HLSL 中的 row_major float4x4 对应
struct Matrix4x4
{
    f32 m[4][4];
};

// 常量缓冲区，对齐到 16 字节
struct TransformConstants
{
    Matrix4x4 model;
};

// 生成 2D 仿射变换矩阵：先按宽高缩放，再按弧度旋转，最后平移到屏幕坐标系 (NDC: [-1, 1])
// 角度 (angle_radians) 的单位为弧度 (1 弧度等于该圆的半径， 2π 弧度就是一个整圆)
internal Matrix4x4 make_model_matrix_2d(f32 pos_x, f32 pos_y, f32 scale_x, f32 scale_y, f32 angle_radians)
{
    // | Scosθ -Ssinθ  0  X |  => 缩放 + 旋转 + 线性平移
    // | Ssinθ  Scosθ  0  Y |
    // |   0      0    1  0 |  => Z 轴不变
    // |   0      0    0  1 |  => 保持齐次坐标w不变
    // 展开得到
    // X' = Scosθ - Ssinθ + X
    // Y' = Ssinθ + Scosθ + Y
    // Z' = Z
    // W' = 1

    f32 sine = sinf(angle_radians);
    f32 cosine = cosf(angle_radians);

    Matrix4x4 result = {};
    result.m[0][0] = scale_x * cosine;
    result.m[0][1] = -scale_y * sine;
    result.m[1][0] = scale_x * sine;
    result.m[1][1] = scale_y * cosine;

    result.m[0][3] = pos_x;
    result.m[1][3] = pos_y;

    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;
    return result;
}

// ============================================================================
// DX11 渲染层与纹理封装
// ============================================================================

struct D3D11_State
{
    ID3D11Device *device;                       // GPU 设备
    ID3D11DeviceContext *context;               // 即时上下文
    IDXGISwapChain1 *swap_chain;                // 交换链（Flip Model）
    ID3D11RenderTargetView *render_target_view; // 后台缓冲的渲染目标视图
    ID3D11VertexShader *vertex_shader;          // 顶点着色器
    ID3D11PixelShader *pixel_shader;            // 像素着色器
    ID3D11InputLayout *input_layout;            // 输入布局

    ID3D11Buffer *unit_quad_vertex_buffer;   // 标准单位矩形顶点缓冲 [-0.5, 0.5] (用于实心精灵填充)
    ID3D11Buffer *transform_constant_buffer; // 常量缓冲区
    ID3D11SamplerState *texture_sampler;     // 采样器
    ID3D11BlendState *alpha_blend_state;     // Alpha 混合状态（让 2D 精灵半透明正确显示）

#if _DEBUG_VIS
    ID3D11Buffer *unit_box_line_vertex_buffer; // 标准单位线框顶点缓冲 (用于调试碰撞箱线框绘制)
#endif

    // HLSL 编译结果
    ID3DBlob *vs_blob;
    ID3DBlob *ps_blob;

    // 窗口大小调整
    UINT pending_client_width;
    UINT pending_client_height;
    bool is_resize_pending;
};

internal ID3DBlob *compile_shader(LPCWSTR source_file, LPCSTR entry_point, LPCSTR target)
{
    ID3DBlob *compiled = nullptr;
    ID3DBlob *err = nullptr;

    HRESULT result = D3DCompileFromFile(
        source_file,                       // HLSL 文件路径
        nullptr,                           // 宏定义
        D3D_COMPILE_STANDARD_FILE_INCLUDE, // 允许 HLSL 使用 #include
        entry_point,                       // 入口函数名
        target,                            // 目标 profile
        0,                                 // 编译标志，这里不额外打开调试
        0,                                 // 效果层编译标志
        &compiled,                         // 输出：编译好的字节码
        &err                               // 输出：编译错误信息
    );

    if (FAILED(result)) {
        if (err) {
            OutputDebugStringA((char *)err->GetBufferPointer());
            SAFE_RELEASE(err);
        }
        SAFE_RELEASE(compiled);
    } else {
        SAFE_RELEASE(err);
    }

    return compiled;
}

// 资源统一清理释放
internal void d3d11_shutdown(D3D11_State *state)
{
    if (!state) {
        return;
    }

    SAFE_RELEASE(state->device);
    SAFE_RELEASE(state->context);
    SAFE_RELEASE(state->swap_chain);
    SAFE_RELEASE(state->render_target_view);
    SAFE_RELEASE(state->vertex_shader);
    SAFE_RELEASE(state->pixel_shader);
    SAFE_RELEASE(state->input_layout);

    SAFE_RELEASE(state->unit_quad_vertex_buffer);
    SAFE_RELEASE(state->transform_constant_buffer);
    SAFE_RELEASE(state->texture_sampler);
    SAFE_RELEASE(state->alpha_blend_state);

    SAFE_RELEASE(state->vs_blob);
    SAFE_RELEASE(state->ps_blob);

#if _DEBUG_VIS
    SAFE_RELEASE(state->unit_box_line_vertex_buffer);
#endif
}

// 窗口尺寸变化时，重建交换链后台缓冲与渲染目标视图
internal bool d3d11_resize(D3D11_State *state, UINT client_width, UINT client_height)
{
    if (client_width == 0 || client_height == 0) {
        return true;
    }

    // 先解除渲染目标的绑定，再释放旧的渲染目标视图
    state->context->OMSetRenderTargets(0, nullptr, nullptr);
    SAFE_RELEASE(state->render_target_view);

    // 重建后台缓冲：0 表示保持当前缓冲数量，UNKNOWN 表示保持当前格式
    HRESULT result = state->swap_chain->ResizeBuffers(
        0,                           // 保持创建交换链时的缓冲数量
        client_width, client_height, // 宽高
        DXGI_FORMAT_UNKNOWN,         // 保持原有的缓冲格式
        0);
    if (FAILED(result)) {
        return false;
    }

    // 重新取得后台缓冲，并创建新的渲染目标视图
    ID3D11Texture2D *back_buffer = nullptr;
    result = state->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back_buffer);
    if (FAILED(result)) {
        return false;
    }

    result = state->device->CreateRenderTargetView(back_buffer, nullptr, &state->render_target_view);
    back_buffer->Release();
    return SUCCEEDED(result);
}

internal bool d3d11_initialize(HWND hwnd, UINT client_width, UINT client_height, D3D11_State *state)
{
    // 按优先级请求显卡支持的 Direct3D 能力级别
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_10_0;

    // 创建设备与即时上下文
    HRESULT result = D3D11CreateDevice(
        nullptr,                  // 自动选择默认显卡
        D3D_DRIVER_TYPE_HARDWARE, // 使用真实 GPU 硬件驱动
        nullptr,                  // 软件光栅化句柄（仅软件驱动时使用）
        0,                        // 创建标志：当前不打开调试层
        feature_levels,           // 可接受的功能级别列表
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &state->device,
        &feature_level,
        &state->context
    );
    if (FAILED(result) || !state->device) {
        return false;
    }

    // 创建 Flip Model 交换链由 DWM 直接合成，窗口模式下延迟更低、无撕裂
    // 且避免旧式 Blt 模型在高刷新率下与 VSync 节奏错位导致的偶发卡顿
    IDXGIFactory2 *factory2 = nullptr;
    // TODO 调试时可传 DXGI_CREATE_FACTORY_DEBUG（需系统已安装 Graphics Tools / DirectX 调试层）
    result = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&factory2);
    if (FAILED(result) || !factory2) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = 2; // 双缓冲
    swap_chain_desc.Width = client_width;
    swap_chain_desc.Height = client_height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1; // Flip Model 不支持 MSAA，采样数必须为 1
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Flip Model
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;

    result = factory2->CreateSwapChainForHwnd(
        state->device,
        hwnd,
        &swap_chain_desc,
        nullptr,           // 全屏描述：窗口模式传空
        nullptr,           // 限制输出到指定显示器：不限制
        &state->swap_chain // 输出：Flip Model 交换链
    );
    if (FAILED(result) || !state->swap_chain) {
        factory2->Release();
        return false;
    }

    // 保留 DXGI 默认的 Alt+Enter 禁用设置，全屏切换由 F11 和窗口样式逻辑处理
    factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(result)) {
        factory2->Release();
        return false;
    }
    factory2->Release();

    // 从交换链取得后台缓冲资源
    ID3D11Texture2D *back_buffer = nullptr;
    result = state->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back_buffer);
    if (FAILED(result)) {
        return false;
    }

    // 指定窗口模式和输出视图
    result = state->device->CreateRenderTargetView(back_buffer, nullptr, &state->render_target_view);
    back_buffer->Release();
    if (FAILED(result)) {
        return false;
    }

    // -----------------------------------------------------------------------------------
    // 编译 HLSL
    ID3DBlob *vs_blob = compile_shader(L"shaders/triangle_vs.hlsl", "main", "vs_5_0");
    ID3DBlob *ps_blob = compile_shader(L"shaders/triangle_ps.hlsl", "main", "ps_5_0");
    state->vs_blob = vs_blob;
    state->ps_blob = ps_blob;
    if (!vs_blob || !ps_blob) {
        return false;
    }

    // 创建顶点着色器和像素着色器
    result = state->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &state->vertex_shader);
    if (FAILED(result)) {
        return false;
    }
    result = state->device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &state->pixel_shader);
    if (FAILED(result)) {
        return false;
    }

    // -----------------------------------------------------------------------------------
    // 创建输入布局：顶点前 3 个 float 是位置，后 2 个 float 是纹理坐标 (u, v)。
    D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    result = state->device->CreateInputLayout(
        input_layout_desc,
        ARRAYSIZE(input_layout_desc),
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        &state->input_layout
    );
    if (FAILED(result)) {
        return false;
    }

    // -------------------------------------------------------------------
    // 纹理顶点缓冲区
    // 顶点顺时针连接为正面
    // 两个三角形组成一个矩形。UV 的 (0, 0) 和 (1, 1) 对应纹理的两个对角。
    Vertex unit_quad_vertices[] = {
        { -0.5f, -0.5f, 0.0f, 0.0f, 1.0f }, // 左下
        { -0.5f,  0.5f, 0.0f, 0.0f, 0.0f }, // 左上
        {  0.5f,  0.5f, 0.0f, 1.0f, 0.0f }, // 右上
        { -0.5f, -0.5f, 0.0f, 0.0f, 1.0f }, // 左下
        {  0.5f,  0.5f, 0.0f, 1.0f, 0.0f }, // 右上
        {  0.5f, -0.5f, 0.0f, 1.0f, 1.0f }, // 右下
    };

    D3D11_BUFFER_DESC vertex_buffer_desc = {};
    vertex_buffer_desc.ByteWidth = sizeof(unit_quad_vertices); // 顶点数据总大小
    vertex_buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;          // 不可变数据
    vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;   // 绑定为顶点缓冲

    D3D11_SUBRESOURCE_DATA vertex_buffer_init = {};
    vertex_buffer_init.pSysMem = unit_quad_vertices;

    result = state->device->CreateBuffer(&vertex_buffer_desc, &vertex_buffer_init, &state->unit_quad_vertex_buffer);
    if (FAILED(result)) {
        return false;
    }

#if _DEBUG_VIS
    // 碰撞箱调试线框专用顶点：闭合矩形 5 个顶点 (Line Strip 连接顺序: 左下 -> 左上 -> 右上 -> 右下 -> 左下)
    Vertex unit_box_line_vertices[] = {
        { -0.5f, -0.5f, 0.0f, 0.0f, 0.0f },
        { -0.5f,  0.5f, 0.0f, 0.0f, 0.0f },
        {  0.5f,  0.5f, 0.0f, 0.0f, 0.0f },
        {  0.5f, -0.5f, 0.0f, 0.0f, 0.0f },
        { -0.5f, -0.5f, 0.0f, 0.0f, 0.0f },
    };

    D3D11_BUFFER_DESC line_buffer_desc = {};
    line_buffer_desc.ByteWidth = sizeof(unit_box_line_vertices);
    line_buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    line_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA line_buffer_init = {};
    line_buffer_init.pSysMem = unit_box_line_vertices;

    result = state->device->CreateBuffer(&line_buffer_desc, &line_buffer_init, &state->unit_box_line_vertex_buffer);
    if (FAILED(result)) {
        return false;
    }
#endif

    // -------------------------------------------------------------------
    // 采样器
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // 边缘使用最近像素
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;  // U 超出 0~1 时使用边缘像素
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;  // V 超出 0~1 时使用边缘像素
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    result = state->device->CreateSamplerState(&sampler_desc, &state->texture_sampler);
    if (FAILED(result)) {
        return false;
    }

    // -------------------------------------------------------------------
    // Alpha Blend 混合状态：实现 2D 精灵透明度 (SrcAlpha + InvSrcAlpha)
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;      // 源颜色乘以自己的 Alpha 权重
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; // 1 - α 输出背景
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;          // 标准透明度混合
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    result = state->device->CreateBlendState(&blend_desc, &state->alpha_blend_state);
    if (FAILED(result)) {
        return false;
    }

    // -------------------------------------------------------------------
    // 动态常量缓冲区：CPU 每帧 Map/Unmap 写入，顶点着色器从 b0 读取。
    D3D11_BUFFER_DESC constant_buffer_desc = {};
    constant_buffer_desc.ByteWidth = sizeof(TransformConstants);
    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    // 第二个参数是 nullptr 表示不提供初始数据
    result = state->device->CreateBuffer(&constant_buffer_desc, nullptr, &state->transform_constant_buffer);
    if (FAILED(result)) {
        return false;
    }

    return true;
}

internal void create_texture(ID3D11Device *device, SpriteImage *sprite)
{
    assert(sprite->pixels);

    // 纹理描述
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = sprite->width;
    texture_desc.Height = sprite->height;
    texture_desc.MipLevels = 1;                          // 不生成多级渐远纹理，用于 UI 或 2D 精灵图
    texture_desc.ArraySize = 1;                          // 不是纹理数组，单张图片
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;    // 颜色布局
    texture_desc.SampleDesc.Count = 1;                   // 关闭多重采样抗锯齿（MSAA）
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;          // 不可变
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // 绑定为着色器资源

    // 准备 CPU 初始数据
    D3D11_SUBRESOURCE_DATA texture_init = {};
    texture_init.pSysMem = sprite->pixels;
    texture_init.SysMemPitch = sprite->width * sizeof(u32); // 行距
    texture_init.SysMemSlicePitch = texture_init.SysMemPitch * sprite->height;

    // 创建 GPU 纹理
    ID3D11Texture2D *gpu_texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&texture_desc, &texture_init, &gpu_texture);
    if (FAILED(hr)) {
        return;
    }

    // 创建着色器资源视图，使用纹理描述中默认的完整格式
    hr = device->CreateShaderResourceView(gpu_texture, nullptr, (ID3D11ShaderResourceView **)&sprite->view);
    gpu_texture->Release();
}

#if _DEBUG_TMP
// 创建程序化生成的石砖贴图 (32x32) 用于墙体/地标建筑物可视化
internal bool create_brick_texture(ID3D11Device *device, SpriteImage *sprite)
{
    constexpr u32 size = 32;
    u32 pixels[size * size];

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            // 边缘与接缝深灰色，砖块表面浅石灰色
            bool is_border = (x == 0 || x == size - 1 || y == 0 || y == size - 1 || y == 16 || (y < 16 && x == 16));
            u8 r = is_border ? 40 : 130;
            u8 g = is_border ? 45 : 135;
            u8 b = is_border ? 55 : 145;
            u8 a = 255;
            pixels[y * size + x] = ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = pixels;
    init_data.SysMemPitch = size * sizeof(u32);
    init_data.SysMemSlicePitch = init_data.SysMemPitch * size;

    ID3D11Texture2D *gpu_texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &init_data, &gpu_texture);
    if (FAILED(hr)) {
        return false;
    }

    hr = device->CreateShaderResourceView(gpu_texture, nullptr, (ID3D11ShaderResourceView **)&sprite->view);
    gpu_texture->Release();
    if (FAILED(hr)) {
        return false;
    }

    sprite->width = size;
    sprite->height = size;
    return true;
}
#endif

internal void texture_release(SpriteImage *sprite)
{
    assert(sprite->view);
    ((ID3D11ShaderResourceView *)sprite->view)->Release();
    sprite->view = nullptr;
    sprite->width = 0;
    sprite->height = 0;
}

internal void draw_sprite_base(
    D3D11_State *d3d,
    Camera2D *camera,
    SpriteImage *sprite,
    f32 world_x, f32 world_y,     // 相对世界中心偏移位置
    f32 world_w, f32 world_h,     // 最终在世界上展示的宽高
    UINT screen_w, UINT screen_h, // 屏幕大小
    f32 angle_radians = 0.0f)
{
    // 世界坐标转为摄像机相对坐标（受缩放 zoom 影响）
    f32 rel_x = (world_x - camera->pos_x) * camera->zoom;
    f32 rel_y = (world_y - camera->pos_y) * camera->zoom;

    // 将相对像素坐标和像素尺寸转换为 NDC 空间下的平移 (pos) 与缩放 (scale)
    f32 half_screen_w = (f32)screen_w * 0.5f;
    f32 half_screen_h = (f32)screen_h * 0.5f;

    // 归一化偏移
    f32 ndc_pos_x = rel_x / half_screen_w;
    f32 ndc_pos_y = rel_y / half_screen_h;

    // 归一化缩放
    // 计算纹理经过相机缩放后相对屏幕的实际大小
    f32 ndc_scale_x = (world_w * camera->zoom) / half_screen_w;
    f32 ndc_scale_y = (world_h * camera->zoom) / half_screen_h;

    // 更新常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped_constants = {};
    HRESULT hr = d3d->context->Map(d3d->transform_constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_constants);
    if (SUCCEEDED(hr)) {
        TransformConstants *constants = (TransformConstants *)mapped_constants.pData;
        constants->model = make_model_matrix_2d(ndc_pos_x, ndc_pos_y, ndc_scale_x, ndc_scale_y, angle_radians);
        d3d->context->Unmap(d3d->transform_constant_buffer, 0);
    }

    // 绑定 sprite 纹理并绘制
    d3d->context->PSSetShaderResources(0, 1, (ID3D11ShaderResourceView *const *)&sprite->view);
    d3d->context->Draw(6, 0);
}

internal void draw_sprite_player(D3D11_State *d3d, Camera2D *camera, SpriteImage *sprite,
                                 f32 player_x, f32 player_y,
                                 UINT screen_w, UINT screen_h)
{
    // 获取原图实际像素尺寸
    f32 tex_w = (f32)sprite->width;
    f32 tex_h = (f32)sprite->height;

    // 进行缩放
    f32 sprite_w = tex_w * sprite->scale;
    f32 sprite_h = tex_h * sprite->scale;

    draw_sprite_base(d3d, camera, sprite,
                     player_x, player_y,
                     sprite_w, sprite_h,
                     screen_w, screen_h);
}

#if _DEBUG_VIS
// 创建纯黄色 1x1 纯色贴图（用于碰撞线框的颜色着色）
internal bool create_solid_color_texture(ID3D11Device *device, u8 r, u8 g, u8 b, u8 a, SpriteImage *sprite)
{
    u32 pixel = ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = &pixel;
    init_data.SysMemPitch = sizeof(u32);
    init_data.SysMemSlicePitch = sizeof(u32);

    ID3D11Texture2D *gpu_texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &init_data, &gpu_texture);
    if (FAILED(hr)) {
        return false;
    }

    hr = device->CreateShaderResourceView(gpu_texture, nullptr, (ID3D11ShaderResourceView **)&sprite->view);
    gpu_texture->Release();
    if (FAILED(hr)) {
        return false;
    }

    sprite->width = 1;
    sprite->height = 1;
    return true;
}

// 绘制 2D 碰撞箱调试线框（黄色中空矩形）
internal void draw_rect_outline_world(
    D3D11_State *d3d,
    Camera2D *camera,
    SpriteImage *sprite,
    f32 center_x, f32 center_y,
    f32 width, f32 height,
    UINT screen_w, UINT screen_h)
{
    f32 rel_x = (center_x - camera->pos_x) * camera->zoom;
    f32 rel_y = (center_y - camera->pos_y) * camera->zoom;

    f32 half_screen_w = (f32)screen_w * 0.5f;
    f32 half_screen_h = (f32)screen_h * 0.5f;

    f32 ndc_pos_x = rel_x / half_screen_w;
    f32 ndc_pos_y = rel_y / half_screen_h;
    f32 ndc_scale_x = (width * camera->zoom) / half_screen_w;
    f32 ndc_scale_y = (height * camera->zoom) / half_screen_h;

    // 更新常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped_constants = {};
    HRESULT hr = d3d->context->Map(d3d->transform_constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_constants);
    if (SUCCEEDED(hr)) {
        TransformConstants *constants = (TransformConstants *)mapped_constants.pData;
        constants->model = make_model_matrix_2d(ndc_pos_x, ndc_pos_y, ndc_scale_x, ndc_scale_y, 0.0f);
        d3d->context->Unmap(d3d->transform_constant_buffer, 0);
    }

    // 切换图元为 Line Strip，绑定 5 顶点的线框缓冲
    d3d->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    d3d->context->IASetVertexBuffers(0, 1, &d3d->unit_box_line_vertex_buffer, &stride, &offset);

    d3d->context->PSSetShaderResources(0, 1, (ID3D11ShaderResourceView *const *)&sprite->view);
    d3d->context->Draw(5, 0); // 5 个顶点连成闭合线框

    // 绘制完成后恢复图元拓扑为三角形列表和实心 Quad 顶点缓冲
    d3d->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d->context->IASetVertexBuffers(0, 1, &d3d->unit_quad_vertex_buffer, &stride, &offset);
}
#endif

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

// 消息回调
internal LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT result = 0;

    switch (uMsg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
        } else if (wParam == VK_F11 && !(lParam & (1 << 30))) {
            toggle_fullscreen(hwnd);
        }
#if _DEBUG_BUILD
        if (wParam == VK_F5 && !(lParam & (1 << 30))) {
            // 录制：第一次按下开始，第二次按下结束并保存
            if (global_recorder.is_recording) {
                replay_stop_recording(&global_recorder, REPLAY_FILE_NAME);
            } else if (global_game_state) {
                replay_start_recording(&global_recorder, global_game_state);
            }
        } else if (wParam == VK_F6 && !(lParam & (1 << 30))) {
            // 回放：第一次按下加载并循环重放，第二次按下结束
            if (global_recorder.is_replaying) {
                replay_end(&global_recorder);
            } else if (global_game_state && replay_load(&global_recorder, REPLAY_FILE_NAME)) {
                replay_begin(&global_recorder, global_game_state);
            }
        }
#endif
        break;
    case WM_SIZE: {
        // 不是最小化窗口事件
        if (global_d3d11 && wParam != SIZE_MINIMIZED) {
            // 记录新的客户区尺寸，留到渲染循环里再真正 resize
            global_d3d11->pending_client_width = LOWORD(lParam);
            global_d3d11->pending_client_height = HIWORD(lParam);
            global_d3d11->is_resize_pending = true;
        }
        break;
    }
    case WM_SETCURSOR:
        if (global_mouse_on) {
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        } else {
            SetCursor(nullptr);
            return TRUE;
        }
    default:
        result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    return result;
}

internal bool windows_start_init()
{
    // 初始化输入
    if (!input_init(&global_IGame_input)) {
        return false;
    }

    // 初始化 Arena
    global_arena.size = GB(1);
    global_arena.base = (u8 *)VirtualAlloc(0, global_arena.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (global_arena.base == nullptr) {
        return false;
    }

    // DPI 感知
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    if (!windows_start_init()) {
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = GAME_NAME;
    // 注册窗口类
    if (!RegisterClassExW(&wc)) {
        return 0;
    }

    // 屏幕分辨率
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    // 默认窗口大小和视口
    UINT client_width = 1280;
    UINT client_height = 720;

    // 自动计算加上标题栏和边框后的外框窗口大小，并让默认窗口居中显示
    RECT windowed_rect = { 0, 0, (LONG)client_width, (LONG)client_height };
    AdjustWindowRect(&windowed_rect, WS_OVERLAPPEDWINDOW, FALSE);

    i32 windowed_w = windowed_rect.right - windowed_rect.left;
    i32 windowed_h = windowed_rect.bottom - windowed_rect.top;
    i32 windowed_x = (screen_width - windowed_w) / 2;
    i32 windowed_y = (screen_height - windowed_h) / 2;

    // 启动时记录窗口模式的样式与位置，供 Alt+Enter 切回窗口模式时恢复
    global_windowed_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    global_windowed_rect = { windowed_x, windowed_y, windowed_x + windowed_w, windowed_y + windowed_h };

    // 创建窗口
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        GAME_NAME, GAME_NAME,
        WS_POPUP | WS_VISIBLE,       // 无边框 / 立刻显示窗口
        0, 0,                        // 水平和垂直位置
        screen_width, screen_height, // 宽度和高度
        nullptr, nullptr, hInstance, nullptr);

    if (hwnd == nullptr) {
        return 0;
    }

    // 禁用输入法上下文，初始化输入
    ImmAssociateContext(hwnd, nullptr);

    // ============================================================================
    // DX11和纹理资源初始化
    // ============================================================================

    D3D11_State d3d11 = {};
    // 初始化为全屏无边框大小
    client_width = screen_width;
    client_height = screen_height;
    if (!d3d11_initialize(hwnd, client_width, client_height, &d3d11)) {
        d3d11_shutdown(&d3d11);
        DestroyWindow(hwnd);
        return 0;
    }
    global_d3d11 = &d3d11;

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (f32)client_width;   // 视口宽度
    viewport.Height = (f32)client_height; // 视口高度
    viewport.MaxDepth = 1.0f;               // 深度最大值

    // 初始化游戏状态
    GameState game_state = {};
    global_game_state = &game_state;

    game_init_asset(&game_state);
    create_texture(d3d11.device, &game_state.backdrop);
    create_texture(d3d11.device, &game_state.player_bagdown);

#if _DEBUG_TMP
    // 自定义石砖贴图纹理
    SpriteImage wall_texture = {};
    create_brick_texture(d3d11.device, &wall_texture);
#endif

#if _DEBUG_VIS
    // 创建黄色纯色贴图（用于碰撞箱调试线框）
    SpriteImage debug_yellow_texture = {};
    create_solid_color_texture(d3d11.device, 255, 230, 0, 255, &debug_yellow_texture);
#endif

    // 计算每帧耗时
    LARGE_INTEGER performance_frequency = {};
    LARGE_INTEGER previous_counter = {};
    QueryPerformanceFrequency(&performance_frequency);
    QueryPerformanceCounter(&previous_counter);

    // 固定步长累加器：把真实经过的时间切成 FIXED_TIMESTEP 的小份，
    // 剩余不足一步的时间留到下一帧继续累计，保证逻辑更新频率恒定。
    f32 accumulator = 0.0f;

    // ============================================================================
    // Windows 消息循环
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
        if (d3d11.is_resize_pending) {
            client_width = d3d11.pending_client_width;
            client_height = d3d11.pending_client_height;
            d3d11.is_resize_pending = false;
            if (!d3d11_resize(&d3d11, client_width, client_height)) {
                global_running = false;
                continue;
            }
            viewport.Width = (f32)client_width;
            viewport.Height = (f32)client_height;
        }

        // 测量此帧的实际经过时间，然后将其输入累加器。
        LARGE_INTEGER current_counter = {};
        QueryPerformanceCounter(&current_counter);
        f32 frame_dt = (f32)(current_counter.QuadPart - previous_counter.QuadPart) / (f32)performance_frequency.QuadPart;
        previous_counter = current_counter;

        // 固定步长更新：以 FIXED_TIMESTEP 为单位递增逻辑，使游戏始终以恒定的 60Hz 运行，而不受渲染帧率的影响
        accumulator += frame_dt;
        // 最大累加器时间阶段
        if (accumulator > MAX_ACCUMULATOR) {
            accumulator = MAX_ACCUMULATOR;
        }
        while (accumulator >= FIXED_TIMESTEP) {
#if _DEBUG_BUILD
            if (global_recorder.is_replaying) {
                replay_tick(&global_recorder, &game_state, &global_game_input.controller[0]);
            } else {
                input_update(global_IGame_input, &global_game_input);
                if (global_recorder.is_recording) {
                    replay_record_input(&global_recorder, &global_game_input.controller[0]);
                }
            }
#else
            input_update(global_IGame_input, &global_game_input);
#endif
            // 保存上一逻辑步状态，供渲染插值使用
            game_state.prev_player_x = game_state.player_x;
            game_state.prev_player_y = game_state.player_y;
            game_state.prev_camera = game_state.camera;

            game_update(&global_game_input, &game_state, FIXED_TIMESTEP);
            accumulator -= FIXED_TIMESTEP;
        }

        // 渲染插值：用本渲染帧剩余时间在「上一逻辑步」与「当前逻辑步」之间插值，
        // 让 60Hz 的逻辑在更高刷新率的屏幕上平滑呈现，消除卡顿感。
        f32 alpha = accumulator / FIXED_TIMESTEP;
        Camera2D render_camera = game_state.camera;
        render_camera.pos_x = lerp(game_state.prev_camera.pos_x, game_state.camera.pos_x, alpha);
        render_camera.pos_y = lerp(game_state.prev_camera.pos_y, game_state.camera.pos_y, alpha);
        render_camera.zoom = lerp(game_state.prev_camera.zoom, game_state.camera.zoom, alpha);
        f32 render_player_x = lerp(game_state.prev_player_x, game_state.player_x, alpha);
        f32 render_player_y = lerp(game_state.prev_player_y, game_state.player_y, alpha);

        // ==========================================================
        //  渲染管线
        // ==========================================================

        // 清屏
        constexpr f32 clear_color[] = { 0.06f, 0.10f, 0.18f, 1.0f };
        d3d11.context->OMSetRenderTargets(1, &d3d11.render_target_view, nullptr);
        d3d11.context->ClearRenderTargetView(d3d11.render_target_view, clear_color);

        // 开启 Alpha Blend
        constexpr f32 blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
        d3d11.context->OMSetBlendState(d3d11.alpha_blend_state, blend_factor, 0XFFFFFFFF);

        // 设置渲染区域、输入布局、图元类型
        d3d11.context->RSSetViewports(1, &viewport);
        d3d11.context->IASetInputLayout(d3d11.input_layout);
        d3d11.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // 3 个顶点组成一个三角形

        // 把顶点着色器和像素着色器绑定到上下文里
        d3d11.context->VSSetShader(d3d11.vertex_shader, nullptr, 0);
        d3d11.context->PSSetShader(d3d11.pixel_shader, nullptr, 0);

        // 从槽位 b0 开始，绑定 1 个常量缓冲区 transform_constant_buffer
        d3d11.context->VSSetConstantBuffers(0, 1, &d3d11.transform_constant_buffer);

        // 绑定采样器
        d3d11.context->PSSetSamplers(0, 1, &d3d11.texture_sampler);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        d3d11.context->IASetVertexBuffers(0, 1, &d3d11.unit_quad_vertex_buffer, &stride, &offset);

        // 绘制 3x3 拼接的超大世界背景地图
        for (i32 tile_y = -1; tile_y <= 1; ++tile_y) {
            for (i32 tile_x = -1; tile_x <= 1; ++tile_x) {
                draw_sprite_base(&d3d11, &render_camera, &game_state.backdrop,
                                 (f32)tile_x * (f32)game_state.backdrop.width,
                                 (f32)tile_y * (f32)game_state.backdrop.height,
                                 (f32)game_state.backdrop.width,
                                 (f32)game_state.backdrop.height,
                                 client_width, client_height);
            }
        }

#if _DEBUG_VIS
        constexpr u32 wall_count = ARRAYSIZE(game_state.wall_colliders);
        Rect2D *wall_colliders = game_state.wall_colliders;

        // 绘制世界中的石砖墙体与地标柱子（用于直观观察摄像机在大世界中的位移）
        for (u32 i = 0; i < wall_count; ++i) {
            draw_sprite_base(&d3d11, &render_camera, &wall_texture,
                             wall_colliders[i].center_x, wall_colliders[i].center_y,
                             wall_colliders[i].half_w * 2.0f, wall_colliders[i].half_h * 2.0f,
                             client_width, client_height);
        }

        // 绘制所有墙体与立柱的黄色碰撞线框
        for (u32 i = 0; i < wall_count; ++i) {
            draw_rect_outline_world(
                &d3d11, &render_camera, &debug_yellow_texture,
                wall_colliders[i].center_x, wall_colliders[i].center_y,
                wall_colliders[i].half_w * 2.0f, wall_colliders[i].half_h * 2.0f,
                client_width, client_height);
        }

        // 绘制主角当前实时的脚底黄色物理碰撞盒
        draw_rect_outline_world(
            &d3d11, &render_camera, &debug_yellow_texture,
            render_player_x, render_player_y - (f32)game_state.player_bagdown.height,
            (f32)game_state.player_bagdown.width * game_state.player_bagdown.scale,
            (f32)game_state.player_bagdown.height * game_state.player_bagdown.scale,
            client_width, client_height);
#endif

        // 绘制玩家
        draw_sprite_player(&d3d11, &render_camera, &game_state.player_bagdown,
                           render_player_x, render_player_y,
                           client_width, client_height);

        d3d11.swap_chain->Present(1, 0); // 1 = 等待垂直同步（Flip Model 下稳定且无撕裂）
    }

    global_d3d11 = nullptr;
    d3d11_shutdown(&d3d11);
    texture_release(&game_state.backdrop);
    texture_release(&game_state.player_bagdown);
    return 0;
}
