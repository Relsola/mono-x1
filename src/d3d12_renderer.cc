#include "renderer.h"
#include "shared/memory.h"
#include "shared/file.h" // read_file：读预编译好的着色器字节码
#include "core.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

#include "win32_prefix.h"
#include <dxgi1_6.h>
#include <d3d12.h>
#pragma comment(lib, "d3d12")
#pragma comment(lib, "dxgi")

#include <string.h> // memcpy（常量环与纹理暂存都是一次行级拷贝）

// COM 接口资源释放
#define SAFE_RELEASE(p)  if (p) { (p)->Release(); (p) = nullptr; }

// ============================================================================
// 渲染层内部数据（对游戏侧完全不透明）
// ============================================================================

// 纹理表容量与每帧绘制列表容量
constexpr u32 MAX_TEXTURES = 256;
constexpr u32 MAX_DRAW_ITEMS = 4096;

// 后台缓冲数 = 帧在途数：一块缓冲配一套每帧资源（命令分配器 + 常量环里的一段），
// 于是「上一次用这套资源的那批命令跑完了没有」只需要一个围栏值就能回答。
// 后台缓冲数与帧在途数取同一个值不是为了省事 —— 两者不等时 Present 会拿到一块
// 上一帧序号还没轮到的缓冲，那时"等围栏"等的是错的帧。
constexpr u32 SWAP_CHAIN_BUFFER_COUNT = 2;
constexpr u32 FRAME_COUNT = SWAP_CHAIN_BUFFER_COUNT;

// 常量缓冲视图的起点必须 256 字节对齐（D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT）。
// 每个绘制项一份 TransformConstants（96 字节），所以按 256 取整、用一个线性分配器逐项切槽。
constexpr u32 CONSTANT_SLOT_SIZE = 256;

// 一帧最多写多少份常量。线框一项会展开成 4 次绘制、每条边一份独立的 model 矩阵，
// 所以上限是「绘制项数 × 4」而不是绘制项数。
constexpr u32 MAX_CONSTANTS_PER_FRAME = MAX_DRAW_ITEMS * 4;
constexpr u32 FRAME_CONSTANT_BYTES = MAX_CONSTANTS_PER_FRAME * CONSTANT_SLOT_SIZE;

// 描述符堆：RTV 只需要后台缓冲那几个；SRV 与纹理表一一对应（0 号槽留给 TEXTURE_NONE）。
constexpr u32 RTV_DESCRIPTOR_COUNT = SWAP_CHAIN_BUFFER_COUNT;

// 采样器槽位。它们**不能**写成根签名里的静态采样器：静态采样器是按寄存器固定在根签名上的，
// 而“这一项要不要平铺”是逐项决定的，写死一个就只能整帧用一种 —— 实测踩过：平台（平铺项）
// 被 CLAMP 采到了 texel(31,31)，而 brick.png 的四边恰好是黑色勾缝，整块平台于是变成纯黑。
// 所以这里保留 D3D11 那套做法：两个采样器对象写进一个 SAMPLER 堆，逐项切 s0。
constexpr u32 SAMPLER_CLAMP = 0; // 精灵图：不重复，边缘取色
constexpr u32 SAMPLER_WRAP = 1;  // 平铺：用于背景与砖块等重复纹理
constexpr u32 SAMPLER_COUNT = 2;

// 描边线框的粗细（设备像素）。D3D12 与 D3D11 一样没有线宽状态，线框仍由 4 条细长四边形拼出。
constexpr f32 OUTLINE_THICKNESS_PX = 2.0f;

// XYZ 顶点坐标位置 + UV 纹理坐标（左上角(0,0) 右下角(1,1)）
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

// 常量缓冲（大小必须是 16 字节的整数倍）
//   布局必须与 shaders/triangle_vs.hlsl 的 cbuffer 一致：
//   model(64B) + [uv_scale | uv_offset](16B) + tint(16B) = 96B
struct TransformConstants
{
    Matrix4x4 model;
    f32 uv_scale_x;
    f32 uv_scale_y;
    f32 uv_offset_x;
    f32 uv_offset_y;
    f32 tint_r;
    f32 tint_g;
    f32 tint_b;
    f32 tint_a;
};

struct TextureEntry
{
    ID3D12Resource *resource; // 纹理本体（DEFAULT 堆，常驻 PIXEL_SHADER_RESOURCE）
};

// 每帧一套的资源：命令分配器 + 「上一次用它的是哪一批命令」
struct FrameSlot
{
    ID3D12CommandAllocator *allocator;
    u64 fence_value; // 0 = 从未提交过，不必等
};

// 帧绘制列表中的一项（与 D3D11 时代逐字段相同：CPU 侧的排序与批处理逻辑与 API 无关）
struct DrawItem
{
    TextureHandle texture;
    f32 x, y, w, h; // 世界坐标中心与尺寸
    f32 angle;
    f32 uv_repeat_px; // > 0 时纹理按该像素间隔重复
    SpriteLayer layer;
    int order; // 同层内的绘制次序，有符号表示可以使语义灵活
    bool flip_x;
    bool outline;      // true = 中空矩形线框（实际是用 4 条细长四边形拼的）
    bool screen_space; // true = x/y/w/h 是屏幕像素（UI 层），不经过相机与缩放
    SpriteStyle style; // UV 子矩形 + 着色（线框只看 tint）
};

struct Renderer
{
    ID3D12Device *device;
    ID3D12CommandQueue *queue;   // 直连队列：图形 + 拷贝 + Present 的宿主
    IDXGISwapChain3 *swap_chain; // 3 才有 GetCurrentBackBufferIndex

    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;

    // 后台缓冲与它的 RTV（每次 Resize 重建）
    ID3D12DescriptorHeap *rtv_heap;
    u32 rtv_descriptor_size;
    ID3D12Resource *back_buffers[SWAP_CHAIN_BUFFER_COUNT];

    // 纹理的 SRV 描述符堆（shader-visible）。0 号槽位保留给 TEXTURE_NONE，永不被绑定
    ID3D12DescriptorHeap *srv_heap;
    u32 srv_descriptor_size;

    // 采样器描述符堆（shader-visible）：两个采样器对象，逐项切 s0（见 SAMPLER_CLAMP 的注释）
    ID3D12DescriptorHeap *sampler_heap;
    u32 sampler_descriptor_size;

    // 帧循环：一块常驻的图形命令分配器/命令列表 + 一个围栏回答"这一帧的每帧资源能复用了"
    ID3D12CommandAllocator *frame_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList *command_list;
    bool command_list_open; // renderer_destroy 里要不要补一次 Close
    ID3D12Fence *frame_fence;
    u64 frame_fence_value;
    HANDLE frame_fence_event;
    FrameSlot frames[FRAME_COUNT]; // "上一次用这套每帧资源的是哪批命令"

    // 纹理上传：一次性使用（所有纹理都在启动阶段建好），所以每条上传直接等它跑完
    ID3D12CommandAllocator *upload_allocator;
    ID3D12GraphicsCommandList *upload_list;
    ID3D12Fence *upload_fence;
    u64 upload_fence_value;
    HANDLE upload_fence_event;

    // 顶点缓冲（单位四边形，6 顶点）常驻映射的上传堆缓冲
    ID3D12Resource *quad_vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW quad_vertex_view;

    // 常量环：UPLOAD 堆、常驻映射。每帧线性切 FRAME_CONSTANT_BYTES 一段，
    // 每项按 CONSTANT_SLOT_SIZE 对齐。等围栏保证"这一段的上一轮读者"已经跑完
    ID3D12Resource *constant_buffer;
    u8 *constant_mapped;
    u64 constant_gpu_address;
    u32 constant_cursor;     // 本帧已切掉的槽数
    u64 constant_frame_base; // 本帧那段在缓冲里的字节偏移

    TextureEntry textures[MAX_TEXTURES];
    u32 texture_count; // 句柄从 1 开始，0 保留为 TEXTURE_NONE

    DrawItem items[MAX_DRAW_ITEMS];
    u32 item_count;

    // 本帧视图参数
    f32 camera_x;
    f32 camera_y;
    f32 camera_zoom;
    u32 screen_width;
    u32 screen_height;

    u32 back_buffer_index; // 本帧画到哪块后台缓冲
    u32 frame_index;       // 本帧用哪套每帧资源（0 .. FRAME_COUNT-1）

    // 每 N 帧呈现一次；0 = 不呈现（--fast 用，Present 会被翻转队列限速）
    u32 present_interval = 1;
    u32 present_countdown = 0;
};

// ============================================================================
// 小工具
// ============================================================================

// 等 GPU 跑过某个围栏值。事件对象只在这一处用，所以不封成"围栏对象"那种抽象
internal void fence_wait(ID3D12Fence *fence, HANDLE event_handle, u64 value)
{
    if (fence->GetCompletedValue() >= value) {
        return;
    }
    fence->SetEventOnCompletion(value, event_handle);
    WaitForSingleObject(event_handle, INFINITE);
}

// 建一块**常驻映射**的上传堆缓冲（顶点缓冲 / 常量环 / 纹理暂存共用这一个入口）。
// 上传堆资源只有唯一的合法状态 GENERIC_READ，而它同时包含 VERTEX_AND_CONSTANT_BUFFER
// 与两个 SHADER_RESOURCE 位，所以这三处用法都不需要任何资源屏障。
internal ID3D12Resource *create_upload_buffer(ID3D12Device *device, u64 size, void **cpu_address)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1; // 单 GPU 节点，必须显式写 1（留 0 会被判定非法）
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource *resource = nullptr;
    HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     __uuidof(ID3D12Resource), (void **)&resource);
    if (FAILED(result) || !resource) {
        return nullptr;
    }

    if (cpu_address) {
        result = resource->Map(0, nullptr, cpu_address);
        if (FAILED(result)) {
            resource->Release();
            return nullptr;
        }
    }
    return resource;
}

// 描述符句柄三连：RTV 的 CPU 句柄、SRV 的 CPU/GPU 句柄。
// 描述符堆只是一段内存，这三行就是"第 index 个描述符在哪儿"的唯一定义
internal D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(Renderer *renderer, u32 index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = renderer->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)index * (SIZE_T)renderer->rtv_descriptor_size;
    return handle;
}

internal D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle(Renderer *renderer, u32 index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = renderer->srv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)index * (SIZE_T)renderer->srv_descriptor_size;
    return handle;
}

internal D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_handle(Renderer *renderer, u32 index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = renderer->srv_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (u64)index * (u64)renderer->srv_descriptor_size;
    return handle;
}

internal D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle(Renderer *renderer, u32 index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = renderer->sampler_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)index * (SIZE_T)renderer->sampler_descriptor_size;
    return handle;
}

internal D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle(Renderer *renderer, u32 index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = renderer->sampler_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (u64)index * (u64)renderer->sampler_descriptor_size;
    return handle;
}

// 记下"选了哪块卡"与最高特性等级。它是每台机器都不一样的环境信息，
// 正是 LOG_INFO（发布版也收集）的用途 —— 排查"在我这儿好好的"时第一眼就看它
internal void log_adapter(ID3D12Device *device, IDXGIFactory4 *factory)
{
    IDXGIAdapter1 *adapter = nullptr;
    LUID luid = device->GetAdapterLuid();
    if (FAILED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void **)&adapter))) {
        return;
    }

    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        LOG_INFO("GPU: %ls (%llu MB dedicated)", desc.Description,
                 (u64)(desc.DedicatedVideoMemory / MB(1)));
    }
    adapter->Release();
}

// ============================================================================
// 生命周期
// ============================================================================

void renderer_destroy(Renderer *renderer);

// 建根签名。三个参数、零个静态采样器：
//   [0] 根 CBV b0 —— 每项一次 SetGraphicsRootConstantBufferView，不需要 CBV 描述符堆
//   [1] 描述符表（1 个 SRV，t0）—— 纹理变了才重设
//   [2] 描述符表（1 个采样器，s0）—— 平铺与否变了才重设（两个采样器对象见 renderer_create）
// 用 versioned 接口（D3D12SerializeRootSignature 是它的过时前身），版本取 1.0：
// 这套签名一个 1.1 的特性都没用到，而 1.0 是兼容面最广的那一档
internal bool create_root_signature(Renderer *renderer)
{
    D3D12_DESCRIPTOR_RANGE srv_range = {};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE sampler_range = {};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = 1;
    sampler_range.BaseShaderRegister = 0;
    sampler_range.RegisterSpace = 0;
    sampler_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    // VS 与 PS 读的是同一份 b0，所以可见性必须是 ALL（写 PIXEL 的话顶点着色器读到的 model 全零）
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &srv_range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &sampler_range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC signature_desc = {};
    signature_desc.NumParameters = (UINT)array_size(parameters);
    signature_desc.pParameters = parameters;
    signature_desc.NumStaticSamplers = 0;
    signature_desc.pStaticSamplers = nullptr;
    // 我们靠顶点缓冲喂数据，这条标志必须给，否则 PSO 创建时报输入布局与根签名不兼容
    signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0 = signature_desc; // Desc_1_0 吃的就是 D3D12_ROOT_SIGNATURE_DESC（1.1 才换成带 1 后缀的那套）

    ID3DBlob *signature = nullptr;
    ID3DBlob *error = nullptr;
    HRESULT result = D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
    if (FAILED(result) || !signature) {
        LOG_ERROR("D3D12SerializeVersionedRootSignature failed: %s",
                  error ? (const char *)error->GetBufferPointer() : "(no message)");
        SAFE_RELEASE(error);
        return false;
    }

    result = renderer->device->CreateRootSignature(0, signature->GetBufferPointer(),
                                                   signature->GetBufferSize(),
                                                   __uuidof(ID3D12RootSignature),
                                                   (void **)&renderer->root_signature);
    SAFE_RELEASE(signature);
    SAFE_RELEASE(error);
    if (FAILED(result)) {
        LOG_ERROR("CreateRootSignature failed");
        return false;
    }
    return true;
}

// 建 PSO。D3D11 时代散在各处的状态（混合 / 光栅化 / 输入布局 / 拓扑）在这里一次固化 ——
// 这也是 DX12 与 D3D11 在"每帧要设多少东西"上最大的差别：PSO 之后每帧只剩视图、根签名、描述符堆
internal bool create_pipeline_state(Renderer *renderer)
{
    // 着色器：读 build.bat 用 fxc 预编译好的字节码（build/shaders/*.cso，vs_5_1 / ps_5_1），
    // 运行期不编译 HLSL。PSO 会把字节码拷走，所以两份字节码活到这个函数结束即可。
    ReadFileRes vs_code = read_file(L"build/shaders/triangle_vs.cso");
    ReadFileRes ps_code = read_file(L"build/shaders/triangle_ps.cso");
    if (!vs_code.contents || !ps_code.contents) {
        LOG_ERROR("renderer: cannot read build/shaders/*.cso - run build.bat shaders first");
        return false;
    }

    // 顶点前 3 个 float 是位置，后 2 个 float 是纹理坐标
    D3D12_INPUT_ELEMENT_DESC input_elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Alpha Blend：标准透明度混合 (SrcAlpha + InvSrcAlpha)
    D3D12_BLEND_DESC blend = {};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].LogicOpEnable = FALSE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // 2D 精灵用水平镜像（flip_x）时会翻转三角形绕序，因此关闭背面剔除
    D3D12_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthClipEnable = TRUE; // 2D 全在 z = 0，这一项无观感影响，取常规值
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // 没有深度/模板缓冲。字段不能留零：0 不是任何 D3D12_COMPARISON_FUNC / D3D12_STENCIL_OP 的合法值
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.StencilEnable = FALSE;
    depth.StencilReadMask = 0xFF;
    depth.StencilWriteMask = 0xFF;
    depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.BackFace = depth.FrontFace;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = renderer->root_signature;
    desc.VS = { vs_code.contents, vs_code.file_size };
    desc.PS = { ps_code.contents, ps_code.file_size };
    desc.BlendState = blend;
    desc.SampleMask = 0xFFFFFFFFu;
    desc.RasterizerState = rasterizer;
    desc.DepthStencilState = depth;
    desc.InputLayout = { input_elements, (UINT)array_size(input_elements) };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HRESULT result = renderer->device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                                   (void **)&renderer->pipeline_state);
    if (FAILED(result)) {
        LOG_ERROR("CreateGraphicsPipelineState failed");
        return false;
    }
    return true;
}

// 建后台缓冲的 RTV。Resize 之后要重建一次，所以单独是一个函数
internal bool create_render_targets(Renderer *renderer)
{
    for (u32 i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
        HRESULT result = renderer->swap_chain->GetBuffer(i, __uuidof(ID3D12Resource),
                                                         (void **)&renderer->back_buffers[i]);
        if (FAILED(result) || !renderer->back_buffers[i]) {
            LOG_ERROR("swap chain GetBuffer(%u) failed", i);
            return false;
        }
        renderer->device->CreateRenderTargetView(renderer->back_buffers[i], nullptr, rtv_handle(renderer, i));
    }
    return true;
}

Renderer *renderer_create(void *native_window, u32 client_width, u32 client_height)
{
    HWND hwnd = (HWND)native_window;
    Renderer *renderer = (Renderer *)arena_push(sizeof(Renderer));
    *renderer = {};

    // 失败收摊只写一次：下面每个阶段各有自己的返回点，但"半初始化的 renderer 怎么销毁"
    // 是同一件事 —— `*renderer = {}` 已经把每个指针清成空，renderer_destroy 对空指针安全。
    bool ok = false;
    defer {
        if (!ok) {
            renderer_destroy(renderer);
        }
    };

    // ------------------------------------------------------------------
    // 调试层：它必须在创建设备**之前**打开。装没装"图形工具"是机器的事，
    // 没装就只记一句 —— 调试层是开发工具，不是运行的必需条件
        
#if MONO_DEBUG_ANY
    {
        ID3D12Debug *debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void **)&debug)) && debug) {
            debug->EnableDebugLayer();
            debug->Release();
            LOG_WARN("renderer: D3D12 debug layer enabled");
        } else {
            LOG_WARN("renderer: D3D12 debug layer unavailable (install the Graphics Tools optional feature)");
        }
    }
#endif

    // ------------------------------------------------------------------
    // 设备。特性等级取 11_0：它是 D3D12 的地板线，也是兼容面最广的一档 ——
    // 我们一个 12_x 的特性都没用（本来就只有一块四边形和一个采样）
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                       (void **)&renderer->device);
    if (FAILED(result) || !renderer->device) {
        LOG_ERROR("D3D12CreateDevice failed (no DX12-capable adapter?)");
        return nullptr;
    }

    // ------------------------------------------------------------------
    // 直连命令队列：图形、拷贝、Present 全走它。
    // 单队列是刻意的选择 —— 多队列意味着跨队列的围栏与资源状态共享，
    // 对这个规模的 2D 渲染没有任何收益（实测瓶颈一直是 Present，见 docs/d3d12-renderer.md）
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; // 队列类型用的就是命令列表类型那套枚举
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;
    result = renderer->device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
                                                 (void **)&renderer->queue);
    if (FAILED(result) || !renderer->queue) {
        LOG_ERROR("CreateCommandQueue failed");
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Flip Model 交换链：由 DWM 直接合成，窗口模式下延迟更低、无撕裂。
    // DX12 的交换链挂在**命令队列**上（D3D11 挂设备），这是两代之间最容易忘的一处差别
    IDXGIFactory4 *factory = nullptr;
    result = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
    if (FAILED(result) || !factory) {
        LOG_ERROR("CreateDXGIFactory2 failed");
        return nullptr;
    }
    defer { factory->Release(); };

    log_adapter(renderer->device, factory);

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
    swap_chain_desc.Width = client_width;
    swap_chain_desc.Height = client_height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1; // Flip Model 不支持 MSAA，采样数必须为 1
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;

    IDXGISwapChain1 *swap_chain1 = nullptr;
    result = factory->CreateSwapChainForHwnd(renderer->queue, hwnd, &swap_chain_desc,
                                             nullptr, // 全屏描述：窗口模式传空
                                             nullptr, // 限制输出到指定显示器：不限制
                                             &swap_chain1);
    if (FAILED(result) || !swap_chain1) {
        LOG_ERROR("CreateSwapChainForHwnd failed");
        return nullptr;
    }
    // GetCurrentBackBufferIndex 在 3 上；有它就不必自己推算"Present 之后轮到哪块缓冲"
    result = swap_chain1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&renderer->swap_chain);
    swap_chain1->Release();
    if (FAILED(result) || !renderer->swap_chain) {
        LOG_ERROR("IDXGISwapChain3 is unavailable");
        return nullptr;
    }

    // 禁用 DXGI 默认的 Alt+Enter 全屏，全屏切换由窗口样式逻辑处理
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // ------------------------------------------------------------------
    // 帧循环的每帧资源：命令分配器 + 命令列表 + 围栏 + 事件
    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        result = renderer->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          __uuidof(ID3D12CommandAllocator),
                                                          (void **)&renderer->frame_allocators[i]);
        if (FAILED(result)) {
            LOG_ERROR("CreateCommandAllocator failed");
            return nullptr;
        }
    }

    result = renderer->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 renderer->frame_allocators[0], nullptr,
                                                 __uuidof(ID3D12GraphicsCommandList),
                                                 (void **)&renderer->command_list);
    if (FAILED(result)) {
        LOG_ERROR("CreateCommandList failed");
        return nullptr;
    }
    // CreateCommandList 出来就是"已开始录制"的状态，立刻关掉；以后每帧 frame_begin 里 Reset
    result = renderer->command_list->Close();
    if (FAILED(result)) {
        LOG_ERROR("command list Close failed");
        return nullptr;
    }

    result = renderer->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                           (void **)&renderer->frame_fence);
    if (FAILED(result)) {
        LOG_ERROR("CreateFence failed");
        return nullptr;
    }
    renderer->frame_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!renderer->frame_fence_event) {
        LOG_ERROR("CreateEventW failed");
        return nullptr;
    }

    // ------------------------------------------------------------------
    // 纹理上传通道（同样的三件套，独立一份是因为它的记录时机与帧循环无关）
    result = renderer->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      __uuidof(ID3D12CommandAllocator),
                                                      (void **)&renderer->upload_allocator);
    if (FAILED(result)) {
        LOG_ERROR("CreateCommandAllocator (upload) failed");
        return nullptr;
    }

    result = renderer->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 renderer->upload_allocator, nullptr,
                                                 __uuidof(ID3D12GraphicsCommandList),
                                                 (void **)&renderer->upload_list);
    if (FAILED(result)) {
        LOG_ERROR("CreateCommandList (upload) failed");
        return nullptr;
    }
    renderer->upload_list->Close();

    result = renderer->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                           (void **)&renderer->upload_fence);
    if (FAILED(result)) {
        LOG_ERROR("CreateFence (upload) failed");
        return nullptr;
    }
    renderer->upload_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!renderer->upload_fence_event) {
        LOG_ERROR("CreateEventW (upload) failed");
        return nullptr;
    }

    // ------------------------------------------------------------------
    // 描述符堆：RTV 一个（后台缓冲用，不必 shader 可见），SRV 一个（纹理表，必须 shader 可见）
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = RTV_DESCRIPTOR_COUNT;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    result = renderer->device->CreateDescriptorHeap(&rtv_heap_desc, __uuidof(ID3D12DescriptorHeap),
                                                    (void **)&renderer->rtv_heap);
    if (FAILED(result)) {
        LOG_ERROR("CreateDescriptorHeap (RTV) failed");
        return nullptr;
    }
    renderer->rtv_descriptor_size = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = MAX_TEXTURES;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = renderer->device->CreateDescriptorHeap(&srv_heap_desc, __uuidof(ID3D12DescriptorHeap),
                                                    (void **)&renderer->srv_heap);
    if (FAILED(result)) {
        LOG_ERROR("CreateDescriptorHeap (SRV) failed");
        return nullptr;
    }
    renderer->srv_descriptor_size = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = {};
    sampler_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_heap_desc.NumDescriptors = SAMPLER_COUNT;
    sampler_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = renderer->device->CreateDescriptorHeap(&sampler_heap_desc, __uuidof(ID3D12DescriptorHeap),
                                                    (void **)&renderer->sampler_heap);
    if (FAILED(result)) {
        LOG_ERROR("CreateDescriptorHeap (sampler) failed");
        return nullptr;
    }
    renderer->sampler_descriptor_size = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // 两个采样器对象（槽 0 CLAMP / 槽 1 WRAP）。像素风：最近像素、无 Mip、各向异性 1
    for (u32 i = 0; i < SAMPLER_COUNT; ++i) {
        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        D3D12_TEXTURE_ADDRESS_MODE address = (i == SAMPLER_WRAP) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                                                                 : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressU = address;
        sampler.AddressV = address;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.0f;
        sampler.MaxAnisotropy = 1;
        // 没有比较过滤器时 ComparisonFunc 不参与运算，但字段必须填一个合法值
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        renderer->device->CreateSampler(&sampler, sampler_cpu_handle(renderer, i));
    }

    if (!create_render_targets(renderer)) {
        return nullptr;
    }

    // ------------------------------------------------------------------
    // 根签名与 PSO
    if (!create_root_signature(renderer)) {
        return nullptr;
    }
    if (!create_pipeline_state(renderer)) {
        return nullptr;
    }

    // ------------------------------------------------------------------
    // 顶点缓冲：UV 的 (0,0) 与 (1,1) 对应矩形的一组对角。
    // 常驻映射，写完就再也不动 —— 它是一次性数据，不是"每帧要更新"的缓冲
    Vertex quad_vertices[] = {
        { -0.5f, -0.5f, 0.0f, 0.0f, 1.0f }, // 左下
        { -0.5f, 0.5f,  0.0f, 0.0f, 0.0f }, // 左上
        { 0.5f,  0.5f,  0.0f, 1.0f, 0.0f }, // 右上
        { -0.5f, -0.5f, 0.0f, 0.0f, 1.0f }, // 左下
        { 0.5f,  0.5f,  0.0f, 1.0f, 0.0f }, // 右上
        { 0.5f, -0.5f,  0.0f, 1.0f, 1.0f }, // 右下
    };

    void *quad_mapped = nullptr;
    renderer->quad_vertex_buffer = create_upload_buffer(renderer->device, sizeof(quad_vertices), &quad_mapped);
    if (!renderer->quad_vertex_buffer) {
        LOG_ERROR("create quad vertex buffer failed");
        return nullptr;
    }
    memcpy(quad_mapped, quad_vertices, sizeof(quad_vertices));

    renderer->quad_vertex_view.BufferLocation = renderer->quad_vertex_buffer->GetGPUVirtualAddress();
    renderer->quad_vertex_view.SizeInBytes = sizeof(quad_vertices);
    renderer->quad_vertex_view.StrideInBytes = sizeof(Vertex);

    // ------------------------------------------------------------------
    // 常量环：FRAME_COUNT 段，每段 FRAME_CONSTANT_BYTES。
    // 一段里逐项按 CONSTANT_SLOT_SIZE 切，切满就是"绘制项太多"，交给 assert 报
    renderer->constant_buffer = create_upload_buffer(renderer->device,
                                                     (u64)FRAME_CONSTANT_BYTES * FRAME_COUNT,
                                                     (void **)&renderer->constant_mapped);
    if (!renderer->constant_buffer) {
        LOG_ERROR("create constant buffer ring failed");
        return nullptr;
    }
    renderer->constant_gpu_address = renderer->constant_buffer->GetGPUVirtualAddress();

    renderer->texture_count = 1; // 0 号槽位保留给 TEXTURE_NONE
    renderer->screen_width = client_width;
    renderer->screen_height = client_height;
    renderer->frame_index = 0;

    ok = true; // 唯一的成功出口：上面的守卫因此不再销毁
    return renderer;
}

void renderer_destroy(Renderer *renderer)
{
    // GPU 可能还在跑：先排空队列，再拆资源（拆掉正在被读的缓冲是 DX12 里最常见的崩溃原因）
    if (renderer->queue && renderer->frame_fence) {
        ++renderer->frame_fence_value;
        renderer->queue->Signal(renderer->frame_fence, renderer->frame_fence_value);
        if (renderer->frame_fence_event) {
            fence_wait(renderer->frame_fence, renderer->frame_fence_event, renderer->frame_fence_value);
        }
    }

    // 命令分配器只有在命令列表关闭之后才能释放
    if (renderer->command_list && renderer->command_list_open) {
        renderer->command_list->Close();
        renderer->command_list_open = false;
    }

    for (u32 i = 0; i < renderer->texture_count; ++i) {
        SAFE_RELEASE(renderer->textures[i].resource);
    }
    renderer->texture_count = 0;

    for (u32 i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
        SAFE_RELEASE(renderer->back_buffers[i]);
    }

    SAFE_RELEASE(renderer->constant_buffer);
    SAFE_RELEASE(renderer->quad_vertex_buffer);
    SAFE_RELEASE(renderer->pipeline_state);
    SAFE_RELEASE(renderer->root_signature);
    SAFE_RELEASE(renderer->sampler_heap);
    SAFE_RELEASE(renderer->srv_heap);
    SAFE_RELEASE(renderer->rtv_heap);
    SAFE_RELEASE(renderer->upload_fence);
    SAFE_RELEASE(renderer->upload_list);
    SAFE_RELEASE(renderer->upload_allocator);

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        SAFE_RELEASE(renderer->frame_allocators[i]);
    }
    SAFE_RELEASE(renderer->frame_fence);
    SAFE_RELEASE(renderer->command_list);
    SAFE_RELEASE(renderer->swap_chain);
    SAFE_RELEASE(renderer->queue);
    SAFE_RELEASE(renderer->device);

    // 两个事件对象与上面那批 COM 资源不同：它们是内核句柄，没有引用计数
    if (renderer->upload_fence_event) {
        CloseHandle(renderer->upload_fence_event);
        renderer->upload_fence_event = nullptr;
    }
    if (renderer->frame_fence_event) {
        CloseHandle(renderer->frame_fence_event);
        renderer->frame_fence_event = nullptr;
    }
}

bool renderer_resize(Renderer *renderer, u32 client_width, u32 client_height)
{
    if (!renderer || client_width == 0 || client_height == 0) {
        return false;
    }

    // DX12 的 ResizeBuffers 要求"已经没有任何后台缓冲的引用"。等所有在途帧跑完
    // 就等于把队列排空了，之后释放引用才是安全的（D3D11 只要解绑 RTV，这里不行）
    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        if (renderer->frames[i].fence_value != 0) {
            fence_wait(renderer->frame_fence, renderer->frame_fence_event, renderer->frames[i].fence_value);
        }
    }

    for (u32 i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
        SAFE_RELEASE(renderer->back_buffers[i]);
    }

    HRESULT result = renderer->swap_chain->ResizeBuffers(0, client_width, client_height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        LOG_ERROR("ResizeBuffers failed");
        return false;
    }

    if (!create_render_targets(renderer)) {
        return false;
    }

    renderer->screen_width = client_width;
    renderer->screen_height = client_height;
    return true;
}

// ============================================================================
// 纹理
// ============================================================================

TextureHandle renderer_create_texture(Renderer *renderer, const void *rgba_pixels, int width, int height)
{
    assert(rgba_pixels && width > 0 && height > 0);

    if (renderer->texture_count >= MAX_TEXTURES) {
        LOG_ERROR("texture table full (%u)", MAX_TEXTURES);
        return TEXTURE_NONE;
    }

    // 纹理本体放 DEFAULT 堆（GPU 独占的显存），初态 COPY_DEST 等上传
    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = (u64)width;
    texture_desc.Height = (u32)height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1; // 2D 精灵不需要渐进纹理
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    ID3D12Resource *texture = nullptr;
    // 失败路径上的 `return TEXTURE_NONE` 不需要清理任何东西：还没拿到任何所有权
    HRESULT result = renderer->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               __uuidof(ID3D12Resource), (void **)&texture);
    if (FAILED(result) || !texture) {
        LOG_ERROR("CreateCommittedResource (texture %dx%d) failed", width, height);
        return TEXTURE_NONE;
    }
    defer { SAFE_RELEASE(texture); };

    // ------------------------------------------------------------------
    // 暂存：D3D12 不能直接把一块系统内存当纹理用，必须经 CopyTextureRegion 拷进显存。
    // 而且行距必须按 256 字节对齐（D3D12_TEXTURE_DATA_PITCH_ALIGNMENT），
    // 所以不能一句 memcpy 铺完 —— 要一行一行拷到 footprint 给的 RowPitch 上
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    u32 row_count = 0;
    u64 row_size = 0;
    u64 total_size = 0;
    renderer->device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint,
                                            &row_count, &row_size, &total_size);

    void *staging_mapped = nullptr;
    ID3D12Resource *staging = create_upload_buffer(renderer->device, total_size, &staging_mapped);
    if (!staging) {
        LOG_ERROR("create staging buffer for texture %dx%d failed", width, height);
        return TEXTURE_NONE;
    }
    defer { SAFE_RELEASE(staging); };

    const u8 *source = (const u8 *)rgba_pixels;
    for (u32 row = 0; row < row_count; ++row) {
        memcpy((u8 *)staging_mapped + (u64)row * footprint.Footprint.RowPitch,
               source + (u64)row * row_size,
               row_size);
    }

    // ------------------------------------------------------------------
    // 上传：记录 → 执行 → 等它跑完。
    // 所有纹理都在启动阶段建，所以"一条上传等一次"完全够用，读写顺序也一眼可见；
    // 真要动态建纹理再改成批量上传（把多次拷贝录进同一条命令列表、一次等围栏）
    result = renderer->upload_allocator->Reset();
    if (SUCCEEDED(result)) {
        result = renderer->upload_list->Reset(renderer->upload_allocator, nullptr);
    }
    if (FAILED(result)) {
        LOG_ERROR("reset upload command list failed");
        return TEXTURE_NONE;
    }

    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source_location = {};
    source_location.pResource = staging;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source_location.PlacedFootprint = footprint;

    renderer->upload_list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);

    // 拷完才允许采样：COPY_DEST -> PIXEL_SHADER_RESOURCE，之后这条纹理再也不转状态
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->upload_list->ResourceBarrier(1, &barrier);

    result = renderer->upload_list->Close();
    if (FAILED(result)) {
        LOG_ERROR("close upload command list failed");
        return TEXTURE_NONE;
    }

    ID3D12CommandList *lists[] = { renderer->upload_list };
    renderer->queue->ExecuteCommandLists(1, lists);

    ++renderer->upload_fence_value;
    renderer->queue->Signal(renderer->upload_fence, renderer->upload_fence_value);
    fence_wait(renderer->upload_fence, renderer->upload_fence_event, renderer->upload_fence_value);

    // ------------------------------------------------------------------
    // SRV：句柄就是描述符堆里的槽位号，所以"返回句柄"与"填描述符"是同一件事的两面
    TextureHandle handle = renderer->texture_count++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    renderer->device->CreateShaderResourceView(texture, &srv, srv_cpu_handle(renderer, handle));

    renderer->textures[handle].resource = texture;

    // 所有权交接给纹理表（上面那条 defer 因此不再释放它），暂存缓冲由 defer 释放 ——
    // 这一步已经等过围栏，GPU 不再引用它了
    texture = nullptr;
    return handle;
}

void renderer_destroy_texture(Renderer *renderer, TextureHandle texture)
{
    if (!renderer || texture == TEXTURE_NONE || texture >= renderer->texture_count) {
        return;
    }
    // 只释放资源本体，描述符槽位不复用（与 D3D11 时代同一条约定）。
    // 注意这里没有等围栏：被释放的纹理如果还有绘制项引用它，绑到的描述符会指向已释放资源
    SAFE_RELEASE(renderer->textures[texture].resource);
}

// ============================================================================
// 每帧绘制
// ============================================================================

internal Matrix4x4 make_model_matrix_2d(f32 pos_x, f32 pos_y, f32 scale_x, f32 scale_y, f32 angle_radians)
{
    // | Scosθ -Ssinθ  0  X |      X' = X*Scosθ - Y*Ssinθ + X
    // | Ssinθ  Scosθ  0  Y |      Y' = X*Ssinθ + Y*Scosθ + Y
    // |   0      0    1  0 |      Z' = Z
    // |   0      0    0  1 |      W' = 1
    //
    // 不带旋转的项（平台 / 地刺 / 角色 / UI 文字 / 线框 —— 也就是绝大多数）angle 正好是 0：
    // 此时 sin/cos 是常量，省掉每个绘制项的两次三角函数（线框一项还会展开成 4 个 quad）。
    f32 sine = 0.0f;
    f32 cosine = 1.0f;
    if (angle_radians != 0.0f) {
        sine = sinf(angle_radians);
        cosine = cosf(angle_radians);
    }

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

void renderer_frame_begin(Renderer *renderer, f32 camera_x, f32 camera_y, f32 camera_zoom,
                          u32 screen_width, u32 screen_height)
{
    renderer->camera_x = camera_x;
    renderer->camera_y = camera_y;
    renderer->camera_zoom = camera_zoom;
    renderer->screen_width = screen_width;
    renderer->screen_height = screen_height;
    renderer->item_count = 0;
    renderer->constant_cursor = 0;

    // 这一套每帧资源上一次是给第 (frame_index) 帧用的：等它跑完才能重置分配器、
    // 才能往常量环那一段里写（那正是 GPU 可能还在读的内存）
    const FrameSlot *slot = &renderer->frames[renderer->frame_index];
    if (slot->fence_value != 0) {
        fence_wait(renderer->frame_fence, renderer->frame_fence_event, slot->fence_value);
    }

    ID3D12CommandAllocator *allocator = renderer->frame_allocators[renderer->frame_index];
    HRESULT result = allocator->Reset();
    if (SUCCEEDED(result)) {
        result = renderer->command_list->Reset(allocator, renderer->pipeline_state);
    }
    if (FAILED(result)) {
        LOG_ERROR("reset frame command list failed");
        return;
    }
    renderer->command_list_open = true;

    renderer->back_buffer_index = renderer->swap_chain->GetCurrentBackBufferIndex();

    // 后台缓冲：Present 态的缓冲不能直接当渲染目标 —— 这是 DX12 与 D3D11 最直观的一处差别
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = renderer->back_buffers[renderer->back_buffer_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    renderer->command_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(renderer, renderer->back_buffer_index);
    renderer->command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    constexpr f32 clear_color[] = { 0.06f, 0.10f, 0.18f, 1.0f };
    renderer->command_list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

    // 视口与裁剪矩形每帧都要设：DX12 没有默认值，漏掉裁剪矩形会整个画面不画
    D3D12_VIEWPORT viewport = {};
    viewport.Width = (f32)screen_width;
    viewport.Height = (f32)screen_height;
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = { 0, 0, (LONG)screen_width, (LONG)screen_height };
    renderer->command_list->RSSetViewports(1, &viewport);
    renderer->command_list->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap *heaps[] = { renderer->srv_heap, renderer->sampler_heap };
    renderer->command_list->SetDescriptorHeaps((UINT)array_size(heaps), heaps);
    renderer->command_list->SetGraphicsRootSignature(renderer->root_signature);
    renderer->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer->command_list->IASetVertexBuffers(0, 1, &renderer->quad_vertex_view);

    // 常量环里分给本帧的那一段
    renderer->constant_frame_base = (u64)renderer->frame_index * FRAME_CONSTANT_BYTES;
}

// 提交一个四边形（世界坐标中心与尺寸）：所有绘制项最终都走这里，线框的四条边也不例外
internal void renderer_draw_quad(Renderer *renderer, f32 center_x, f32 center_y, f32 width, f32 height,
                                 f32 angle, f32 uv_repeat_px, bool flip_x, bool screen_space,
                                 const SpriteStyle &style)
{
    f32 half_screen_w = (f32)renderer->screen_width * 0.5f;
    f32 half_screen_h = (f32)renderer->screen_height * 0.5f;

    f32 ndc_pos_x = 0.0f;
    f32 ndc_pos_y = 0.0f;
    f32 ndc_scale_x = 0.0f;
    f32 ndc_scale_y = 0.0f;
    if (screen_space) {
        // 屏幕像素（左上角原点、y 向下）→ NDC（中心原点、y 向上）：x 轴直接归一化，y 轴翻一次
        ndc_pos_x = (center_x / half_screen_w) - 1.0f;
        ndc_pos_y = 1.0f - (center_y / half_screen_h);
        ndc_scale_x = width / half_screen_w;
        ndc_scale_y = height / half_screen_h;
    } else {
        // 世界坐标 -> 相对相机的像素偏移 -> NDC
        f32 rel_x = (center_x - renderer->camera_x) * renderer->camera_zoom;
        f32 rel_y = (center_y - renderer->camera_y) * renderer->camera_zoom;

        ndc_pos_x = rel_x / half_screen_w;
        ndc_pos_y = rel_y / half_screen_h;
        ndc_scale_x = (width * renderer->camera_zoom) / half_screen_w;
        ndc_scale_y = (height * renderer->camera_zoom) / half_screen_h;
        if (flip_x) {
            ndc_scale_x = -ndc_scale_x;
        }
    }

    // 从常量环里切下一个槽。写进的是 CPU 映射地址，GPU 通过根 CBV 读同一段；
    // 这一段在"这一帧的围栏跑完"之前不会被复用（帧在途数 = 缓冲区数保证的）
    assert(renderer->constant_cursor < MAX_CONSTANTS_PER_FRAME);
    if (renderer->constant_cursor >= MAX_CONSTANTS_PER_FRAME) {
        return; // 「绘制项上限 × 4」是结构性上限，正常到不了；真到了也不能把环写穿
    }
    u64 offset = renderer->constant_frame_base + (u64)renderer->constant_cursor * CONSTANT_SLOT_SIZE;
    ++renderer->constant_cursor;

    TransformConstants constants = {};
    constants.model = make_model_matrix_2d(ndc_pos_x, ndc_pos_y, ndc_scale_x, ndc_scale_y, angle);

    f32 uv_scale_x = style.uv_scale_x;
    f32 uv_scale_y = style.uv_scale_y;
    if (uv_repeat_px > 0.0f) {
        uv_scale_x = width / uv_repeat_px;
        uv_scale_y = height / uv_repeat_px;
    }
    constants.uv_scale_x = uv_scale_x;
    constants.uv_scale_y = uv_scale_y;
    constants.uv_offset_x = style.uv_offset_x;
    constants.uv_offset_y = style.uv_offset_y;
    constants.tint_r = style.tint_r;
    constants.tint_g = style.tint_g;
    constants.tint_b = style.tint_b;
    constants.tint_a = style.tint_a;

    memcpy(renderer->constant_mapped + offset, &constants, sizeof(constants));

    // 根 CBV 直接吃一个 GPU 地址：不需要 CBV 描述符，也就不需要第二个描述符堆
    renderer->command_list->SetGraphicsRootConstantBufferView(0, renderer->constant_gpu_address + offset);
    renderer->command_list->DrawInstanced(6, 1, 0, 0);
}

// 排序键：层 → 层内次序 → 纹理（相同键保持提交顺序，可合并纹理切换）
internal u64 draw_item_sort_key(const DrawItem *item)
{
    u64 order = (u64)(u32)(item->order + 0x80000000); // 平移到无符号区间
    return ((u64)item->layer << 56) | (order << 24) | (u64)(item->texture & 0x00FFFFFFu);
}

internal void renderer_sort_items(Renderer *renderer)
{
    for (u32 i = 1; i < renderer->item_count; ++i) {
        DrawItem current = renderer->items[i];
        u64 key = draw_item_sort_key(&current);

        u32 j = i;
        while (j > 0 && draw_item_sort_key(&renderer->items[j - 1]) > key) {
            renderer->items[j] = renderer->items[j - 1];
            --j;
        }
        renderer->items[j] = current;
    }
}

void renderer_frame_end(Renderer *renderer)
{
    // frame_begin 里重置失败（设备被移除之类）时不要再往下走：那条路径上命令列表是关着的
    if (!renderer->command_list_open) {
        return;
    }

    renderer_sort_items(renderer);

    ID3D12GraphicsCommandList *list = renderer->command_list;
    u32 bound_texture = 0xFFFFFFFFu; // 与任何合法句柄都不同
    u32 bound_sampler = 0xFFFFFFFFu; // 与 SAMPLER_CLAMP / SAMPLER_WRAP 都不同

    for (u32 i = 0; i < renderer->item_count; ++i) {
        const DrawItem *item = &renderer->items[i];
        if (item->texture == TEXTURE_NONE) {
            continue;
        }

        // 排序键的最后一段（纹理）在这里才真正兑现：相邻同图不重设描述符表
        if (item->texture != bound_texture) {
            list->SetGraphicsRootDescriptorTable(1, srv_gpu_handle(renderer, item->texture));
            bound_texture = item->texture;
        }

        // 平铺与否决定用哪个采样器（uv_repeat_px > 0 = 要走 WRAP）
        u32 sampler_slot = (item->uv_repeat_px > 0.0f) ? SAMPLER_WRAP : SAMPLER_CLAMP;
        if (sampler_slot != bound_sampler) {
            list->SetGraphicsRootDescriptorTable(2, sampler_gpu_handle(renderer, sampler_slot));
            bound_sampler = sampler_slot;
        }

        if (!item->outline) {
            renderer_draw_quad(renderer, item->x, item->y, item->w, item->h,
                               item->angle, item->uv_repeat_px, item->flip_x, item->screen_space,
                               item->style);
            continue;
        }

        // 线框：上下两条横贯整宽，左右两条只补中间那段（四角才不会叠两层）。
        // 线框的 angle / uv_repeat_px / flip_x 恒为零值（见 renderer_push_rect_outline）
        const SpriteStyle &style = item->style;
        // 线宽是**设备像素**：世界空间要除以相机缩放换算成世界长度，屏幕空间本来就是像素
        f32 thickness = item->screen_space ? OUTLINE_THICKNESS_PX
                                           : OUTLINE_THICKNESS_PX / renderer->camera_zoom;
        f32 max_thickness = 0.5f * (item->w < item->h ? item->w : item->h);
        if (thickness > max_thickness) {
            thickness = max_thickness; // 盒子比边框还小：别让四条边互相盖住
        }
        f32 inner_h = item->h - thickness * 2.0f;
        if (inner_h < 0.0f) {
            inner_h = 0.0f;
        }
        f32 inset_x = (item->w - thickness) * 0.5f;
        f32 inset_y = (item->h - thickness) * 0.5f;

        renderer_draw_quad(renderer, item->x, item->y + inset_y,
                           item->w, thickness, 0.0f, 0.0f, false, item->screen_space, style);
        renderer_draw_quad(renderer, item->x, item->y - inset_y,
                           item->w, thickness, 0.0f, 0.0f, false, item->screen_space, style);
        renderer_draw_quad(renderer, item->x - inset_x, item->y,
                           thickness, inner_h, 0.0f, 0.0f, false, item->screen_space, style);
        renderer_draw_quad(renderer, item->x + inset_x, item->y,
                           thickness, inner_h, 0.0f, 0.0f, false, item->screen_space, style);
    }

    // 交给 Present 之前必须回到 PRESENT 态，否则第一次呈现就会失败（或画面全黑）
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = renderer->back_buffers[renderer->back_buffer_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);

    HRESULT result = list->Close();
    renderer->command_list_open = false;
    if (FAILED(result)) {
        LOG_ERROR("frame command list Close failed");
    }

    ID3D12CommandList *lists[] = { list };
    renderer->queue->ExecuteCommandLists(1, lists);

    // 提交顺序即依赖顺序：这一帧的命令跑完时，它用过的分配器与常量环那一段都可以复用了
    ++renderer->frame_fence_value;
    renderer->queue->Signal(renderer->frame_fence, renderer->frame_fence_value);
    renderer->frames[renderer->frame_index].fence_value = renderer->frame_fence_value;

    // 呈现：默认每帧一次且等垂直同步（Flip Model 下稳定且无撕裂）。
    // --fast 会把它放松成每 64 帧一次 —— 翻转队列的限速才是快跑模式的真正瓶颈
    if (renderer->present_interval > 0 &&
        (renderer->present_countdown % renderer->present_interval) == 0) {
        renderer->swap_chain->Present(1, 0);
    }
    ++renderer->present_countdown;

    renderer->frame_index = (renderer->frame_index + 1) % FRAME_COUNT;
}

void renderer_set_present_interval(Renderer *renderer, u32 interval)
{
    renderer->present_interval = interval;
    renderer->present_countdown = 0;
}

// ============================================================================
// 提交侧
// ============================================================================

void renderer_push_sprite(Renderer *renderer, TextureHandle texture,
                          f32 x, f32 y, f32 width, f32 height,
                          f32 angle, SpriteLayer layer, f32 uv_repeat_px, bool flip_x, int order,
                          const SpriteStyle &style)
{
    if (renderer->item_count >= MAX_DRAW_ITEMS) {
        assert(!"draw item list overflow");
        return;
    }

    renderer->items[renderer->item_count++] = DrawItem{ .texture = texture,
                                                        .x = x,
                                                        .y = y,
                                                        .w = width,
                                                        .h = height,
                                                        .angle = angle,
                                                        .uv_repeat_px = uv_repeat_px,
                                                        .layer = layer,
                                                        .order = order,
                                                        .flip_x = flip_x,
                                                        .style = style };
}

// 三个 push 入口都是**整体赋值**（不是逐字段写回 items[n++]）：新增字段时漏写只会得到零值，
// 而不是上一帧残留的脏数据（旧写法在字段变多时就踩过这个）。字段必须按声明顺序写。
void renderer_push_rect_outline(Renderer *renderer, TextureHandle texture,
                                f32 x, f32 y, f32 width, f32 height, SpriteLayer layer,
                                int order, const SpriteStyle &style)
{
    if (renderer->item_count >= MAX_DRAW_ITEMS) {
        assert(!"draw item list overflow");
        return;
    }

    // 线框的每条边都铺满整张纹理，UV 子矩形对它没有意义
    SpriteStyle flat = style;
    flat.uv_offset_x = 0.0f;
    flat.uv_offset_y = 0.0f;
    flat.uv_scale_x = 1.0f;
    flat.uv_scale_y = 1.0f;

    renderer->items[renderer->item_count++] = DrawItem{ .texture = texture,
                                                        .x = x,
                                                        .y = y,
                                                        .w = width,
                                                        .h = height,
                                                        .layer = layer,
                                                        .order = order,
                                                        .outline = true,
                                                        .style = flat };
}

// 屏幕空间矩形的两个入口共用这一段：只差 outline 标志（线框的 rect 语义与实心一样，都是左上角 + 尺寸）
internal void renderer_push_screen_rect(Renderer *renderer, TextureHandle texture,
                                       f32 x, f32 y, f32 width, f32 height,
                                       int order, const SpriteStyle &style, bool outline)
{
    if (renderer->item_count >= MAX_DRAW_ITEMS) {
        assert(!"draw item list overflow");
        return;
    }

    SpriteStyle final_style = style;
    if (outline) {
        final_style.uv_offset_x = 0.0f;
        final_style.uv_offset_y = 0.0f;
        final_style.uv_scale_x = 1.0f;
        final_style.uv_scale_y = 1.0f;
    }

    // 绘制项统一存**中心**（世界精灵本来就是中心语义），所以这里把左上角换算一次
    renderer->items[renderer->item_count++] = DrawItem{ .texture = texture,
                                                        .x = x + width * 0.5f,
                                                        .y = y + height * 0.5f,
                                                        .w = width,
                                                        .h = height,
                                                        .layer = LAYER_UI,
                                                        .order = order,
                                                        .outline = outline,
                                                        .screen_space = true,
                                                        .style = final_style };
}

void renderer_push_ui_rect(Renderer *renderer, TextureHandle texture,
                           f32 x, f32 y, f32 width, f32 height,
                           int order, const SpriteStyle &style)
{
    renderer_push_screen_rect(renderer, texture, x, y, width, height, order, style, false);
}

void renderer_push_ui_rect_outline(Renderer *renderer, TextureHandle texture,
                                   f32 x, f32 y, f32 width, f32 height,
                                   int order, const SpriteStyle &style)
{
    renderer_push_screen_rect(renderer, texture, x, y, width, height, order, style, true);
}
