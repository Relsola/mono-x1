# D3D12 渲染层实现说明

> 依据：D3D12 + DXGI（Flip Model），着色器 `vs_5_1` / `ps_5_1`。
> 本工程只用到 D3D12 的一个很小的子集：一个根签名、一个 PSO、一个顶点缓冲、一块常量环、一个 SRV 堆、一个围栏。

## 1. 这一层解决什么问题

**游戏侧只说「画什么」，渲染层负责「怎么画」。** 两侧的契约只有三样东西：

| 契约 | 形式 | 说明 |
| --- | --- | --- |
| 纹理 | `TextureHandle`（不透明 u32 索引） | 游戏/资源层只持有与传递，不解释它是什么 |
| 提交 | `renderer_push_sprite` / `renderer_push_rect_outline` | 只写进帧绘制列表，**不做任何 GPU 调用** |
| 帧 | `renderer_frame_begin` / `renderer_frame_end` | 前者开命令列表 + 设视图，后者排序 + 记录绘制 + 提交 + Present |

`include/renderer.h` 里**没有任何 D3D / DXGI 类型**（窗口用 `void *`、纹理用句柄）。
这条约定在 2026-10-01 被兑现了：从 D3D11 换成 D3D12 时 `renderer.h` 一行没改，
只重写了 `src/d3d12_renderer.cc` 一个实现文件。

## 2. 数据流

```
装配层（main/scene.cc）                     ← 读 debug_vis 上报的矩形，翻成 push_rect_outline
   │  push_sprite / push_rect_outline      ← 只提交数据，不碰命令列表
   ▼
帧绘制列表  DrawItem items[4096]           ← 排序与批处理都发生在这里
   │  frame_end：排序 → 逐项写常量环 + 绑根参数 → DrawInstanced(6,1,0,0)
   ▼
命令列表（本帧的 ID3D12GraphicsCommandList）
   │  Close → ExecuteCommandLists
   ▼
命令队列（单个 DIRECT 队列）
   │  Signal(围栏) → Present(1, 0)
   ▼
DWM 合成
```

**与 D3D11 时代最大的区别**：D3D11 是「push 之后 frame_end 直接对即时上下文发命令」，
D3D12 是「先把所有命令录进命令列表，最后一次性提交给队列」。所以多出来的三样东西是
命令分配器、命令列表、围栏 —— 以及它们带来的那个新问题：「这一段资源什么时候能复用」。

## 3. 术语矩阵

| 概念 | 术语 | 本项目的落点 |
| --- | --- | --- |
| 交换链 | swap chain | `IDXGISwapChain3`，双缓冲，Flip Model |
| 命令队列 | command queue | 1 个 `DIRECT` 队列：图形、拷贝、Present 全走它 |
| 命令分配器 | command allocator | 每帧一个（`FRAME_COUNT = 2`），提交前必须 `Reset` |
| 命令列表 | command list | 1 个 `ID3D12GraphicsCommandList`，每帧 `Reset` 复用 |
| 围栏 | fence | `frame_fence`（每帧一个值）+ `upload_fence`（纹理上传用） |
| 根签名 | root signature | 1.0 版：`[0]` 根 CBV b0、`[1]` 描述符表（1×SRV t0）、`[2]` 描述符表（1×采样器 s0） |
| PSO | pipeline state object | 一个：VS/PS + 输入布局 + 混合 + 光栅化 + 无深度 + RTV 格式 |
| 描述符堆 | descriptor heap | 3 个：RTV（2 个）、SRV（256 个，shader-visible）、SAMPLER（2 个，shader-visible） |
| 后台缓冲视图 | RTV | `rtv_heap` 里 0/1 两个描述符，Resize 后重建 |
| 资源视图 | SRV | 纹理表里每张纹理一个描述符，**句柄号 = 槽位号** |
| 采样器 | sampler | SAMPLER 堆里两个对象（槽 0 = POINT/CLAMP、槽 1 = POINT/WRAP），逐项切 —— **不能写成根签名的静态采样器**，见 §9 |
| 常量缓冲 | constant buffer (b0) | 一块常驻映射的上传堆缓冲，每帧切一段，逐项按 256B 对齐切槽 |
| 顶点缓冲 | vertex buffer | 单位四边形 6 顶点，上传堆、常驻映射、永不转状态（线框也复用它，见 §4） |
| 图元拓扑 | primitive topology | 只有 `TRIANGLELIST`（线框是 4 条细长四边形） |
| 资源屏障 | resource barrier | 后台缓冲 `PRESENT ↔ RENDER_TARGET`；纹理上传 `COPY_DEST → PIXEL_SHADER_RESOURCE` |
| 上传堆 | `D3D12_HEAP_TYPE_UPLOAD` | 顶点缓冲 / 常量环 / 纹理暂存；只能停在 `GENERIC_READ` |
| 调试层 | debug layer | `MONO_DEBUG_ANY` 编译期开启，`D3D12GetDebugInterface` 失败就只记一条 WARN |

## 4. 关键设计决策

| 决定 | 备选 | 为什么 | 代价 |
| --- | --- | --- | --- |
| **帧绘制列表**（先收集、再统一提交） | 立即模式（push 时直接发命令） | 排序与状态切换只在装配层做一次；游戏侧完全不碰 GPU | 每帧一次排序 + 4096 项上限 |
| **单个 DIRECT 队列** | 图形/拷贝/计算分队列 | 多队列意味着跨队列围栏与资源状态共享；对这个规模的 2D 渲染没有收益（实测瓶颈一直在 Present） | 纹理上传与绘制串行（都在启动阶段，无观感影响） |
| **帧在途数 = 后台缓冲数 = 2** | 帧在途 3 / 缓冲 2 | 两者不等时 Present 会拿到一块「序号还没轮到」的缓冲，那时等围栏等的是错的帧 | 帧在途只有 2，CPU 提前量小 |
| **根 CBV + 两张描述符表** | 建 CBV 描述符堆 / 把采样器写成静态 | 每项只需一个 GPU 地址（根 CBV），省掉 CBV 描述符；采样器必须是描述符表（静态采样器无法逐项切换，见 §9） | 根签名参数是有限的；描述符堆有三个 |
| **常量走「环 + 逐项切槽」** | root constants（32 位常量直写命令列表） | 每项只写一个 8 字节的 GPU 地址；root constants 是每项 24 个 DWORD 直接进命令列表，项数多时命令列表体积明显更大 | 要维护环的偏移、每帧重置 |
| **上传堆资源永不转状态** | 把顶点缓冲放 DEFAULT 堆 + 屏障 | `GENERIC_READ` 同时含 `VERTEX_AND_CONSTANT_BUFFER` 与两个 `SHADER_RESOURCE` 位，所以**一次屏障都不需要** | 上传堆在系统内存里（这几 KB 无所谓） |
| **纹理走 DEFAULT 堆 + 暂存** | 直接把纹理建在上传堆 | 通用做法，也与「资源状态该被显式管理」的教学意图一致 | 多一个暂存缓冲 + 一次 `CopyTextureRegion` + 一次屏障 |
| **每条纹理上传各等一次围栏** | 批量上传 / 延迟释放暂存 | 所有纹理都在启动阶段建，一次等一次完全够用，读写顺序一眼可见 | 启动多几十次 GPU 往返（实测可忽略）；动态建纹理时要改成批量 |
| **Flip Model 交换链** | `DISCARD` / `SEQUENTIAL` | 窗口模式延迟更低、无撕裂，且 DX12 基本只能用 Flip Model | 不能 MSAA；Resize 前必须等 GPU 排空 |
| **PSO 一次性固化所有状态** | 每帧重设散装状态（D3D11 的做法） | 这是 DX12 的核心收益：`frame_begin` 里只剩视图、根签名、描述符堆三件事 | 改状态要改 PSO（或建第二个 PSO） |
| **句柄表 + 0 号槽保留** | 直接暴露 SRV 指针 | 游戏侧不持有 GPU 资源；句柄能安全校验 | 上限 256；销毁后的槽位不复用 |
| **单位四边形 + 缩放矩阵** | 每项生成 4 个顶点 | 顶点缓冲只有 6 个顶点，CPU 每项只算一个矩阵 | 旋转/缩放全靠矩阵，shader 必须用 `row_major` |
| **线框 = 4 条细长四边形** | `LINESTRIP` + 专用线框顶点缓冲 | D3D12 与 D3D11 一样没有线宽状态，`LINESTRIP` 恒为 1 像素且不随缩放变化；改成四边形后粗细可控（`OUTLINE_THICKNESS_PX`） | 每根线框 4 次 `DrawInstanced` 而不是 1 次，常量槽也要 4 份 |
| **排序键 = 层 → 层内次序 → 纹理** | 只按层 / 只按纹理 | 层决定大关系；层内次序保证遮挡；纹理放最后让相邻项能复用描述符表绑定 | 键只有 8+32+24 位（层 8 / order 32 / 纹理 24），超出会截断 |
| **`order` 参数必须存在** | 纯按纹理排序 | 踩过：纯纹理排序会把角色画到平台后面（见 §10） | 调用方要记得给 order |
| **世界坐标 → NDC 在 CPU 算** | 传相机矩阵给 shader | shader 退化成 `mul(model, pos)`，没有额外矩阵乘法 | CPU 每项 4 次除法 |
| **离线预编译 `.cso`**（`fxc`，2026-10-01 改） | ~~运行时编译 HLSL（`D3DCompileFromFile`）~~ | 启动不再编译 HLSL，运行期也不依赖 `d3dcompiler_47.dll` | ~~改 shader 不用重编 C++~~：现在改 shader 要重跑一次 `build.bat shaders`（不是热重载） |

## 5. 一帧的完整链路

```
renderer_frame_begin(camera_x, camera_y, zoom, screen_w, screen_h)
  ├─ 记住视图参数，item_count = 0，常量游标归零
  ├─ 等这一帧上一轮的围栏（帧在途 = 2 的兑现点）
  ├─ 命令分配器 Reset → 命令列表 Reset(分配器, PSO)
  ├─ 屏障：后台缓冲 PRESENT → RENDER_TARGET
  ├─ OMSetRenderTargets(RTV) + ClearRenderTargetView({0.06, 0.10, 0.18, 1.0})
  └─ 每帧固定状态：视口 / 裁剪矩形 / 描述符堆 / 根签名 / 拓扑 / 顶点缓冲
          ↓
  （游戏层 push 数据：只写 items[]，不碰命令列表）
          ↓
renderer_frame_end()
  ├─ renderer_sort_items()        插入排序（项数少，稳定且无额外内存）
  ├─ 逐项：
  │    世界坐标 → 相对相机像素 → 除以半屏 → NDC
  │        rel = (item - camera) * zoom
  │        ndc_pos   = rel / (screen/2)
  │        ndc_scale = size * zoom / (screen/2)      flip_x 时 scale_x 取负
  │    （屏幕空间的 UI 项跳过相机那一步：像素 → NDC 直接换算，y 翻一次）
  │    从常量环切一个 256B 槽，CPU 写 model + uv_scale/uv_offset + tint
  │        uv_scale  = (w / uv_repeat_px, h / uv_repeat_px)，无平铺时为 (1,1)
  │        uv_offset = UV 子矩形左上角（取多帧条带的一帧时用，否则为 0）
  │        tint      = 逐项着色（与采样结果相乘；灰度图 + 调色板颜色就是这么做的）
  │    SetGraphicsRootConstantBufferView(0, 槽的 GPU 地址)
  │    SetGraphicsRootDescriptorTable(1, 纹理的 SRV 句柄)   ← 与上一项同图时跳过
  │    SetGraphicsRootDescriptorTable(2, 采样器句柄)      ← 平铺与否变了才重设
  │    DrawInstanced(6, 1, 0, 0)
  │        （线框展开成 4 次 DrawInstanced：上下两条横贯 + 左右两条补中间）
  ├─ 屏障：后台缓冲 RENDER_TARGET → PRESENT
  ├─ Close → ExecuteCommandLists → Signal(围栏) → 记下这个值
  └─ Present(1, 0)               1 = 等垂直同步（present_interval 为 0 时整段跳过）
```

**三层排序键**：

```
key = (layer << 56) | ((order + 0x80000000) << 24) | (texture & 0xFFFFFF)
       └ 层          └ 层内次序（平移到无符号）        └ 纹理句柄
```

先按层（背景 → 世界 → 调试 → UI），同层按 `order`（平台 0 < 角色 10），最后按纹理 —— 纹理放在最低位是为了让**相邻项尽量用同一张纹理**。到 D3D12 这一层，「减少资源绑定切换」不再是目的本身（`frame_end` 里对同图相邻项直接跳过 `SetGraphicsRootDescriptorTable`），但顺序稳定对 GPU 仍是有利的。

## 6. 着色器

两个 `.hlsl` **源文件**都在 `shaders/` 下；`build.bat` 用 `fxc` 把它们编译成 `build/shaders/*.cso`
（`vs_5_1` / `ps_5_1`、入口 `main`、编译标志 0），运行时从**进程工作目录**（仓库根）读那两个 `.cso`。
`d3d12_renderer.cc` 因此不包含 `<d3dcompiler.h>`，也不在启动时编译 HLSL：
**只改 `.hlsl` 不重跑构建是无效的** —— 改 shader 必须跑一次 `build.bat shaders`（或任何一次 `build.bat`）。
这是「离线编译」唯一的代价（没有热重载）。PSO 会把字节码拷走，所以 `ReadFileRes` 只要活到
`CreateGraphicsPipelineState` 之后（D3D11 时代这个寿命要求属于 `CreateInputLayout`）。

**为什么是 5.1 而不是 6.x**：`5.1` 是随 D3D12 一起引入的那一档（相对 5.0 多了寄存器空间），也是 `fxc` 能出的最高版本；
`6.x` 要走 `dxc`，产出必须**签名过的 DXIL**（编译期多一个 `dxil.dll` 的环境约束），而 Mesh Shader / 光追 / Wave 这些
6.x 独有特性我们一个都用不上 —— 对这个「乘一个矩阵 + 采一次纹素 + 乘 tint」的着色器，收益是零。
真要用了再换：那时改的还是 `build.bat` 那两个 `-T`。

```hlsl
// triangle_vs.hlsl
cbuffer TransformConstants : register(b0)
{ row_major float4x4 model; float2 uv_scale; float2 uv_offset; float4 tint; };
output.position = mul(model, float4(pos, 1.0f));   // model 已合并 Scale→Rotate→Translate
output.uv       = uv * uv_scale + uv_offset;       // 平铺与取条带帧是同一个式子
```

```hlsl
// triangle_ps.hlsl
Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);
return texture0.Sample(sampler0, input.uv) * tint;  // tint 做逐项着色
```

三个必须对齐的点：

- **`row_major` 与 C++ 的 `Matrix4x4::m[4][4]` 行布局一致**（否则要转置）
- **常量缓冲大小必须是 16 字节的整数倍**（`TransformConstants` = 4×4 矩阵 64B + `uv_scale`/`uv_offset` 合占 16B + `tint` 16B = 96B）
- **常量对 VS 与 PS 是同一个根参数，但可见性必须写 `ALL`**：D3D11 时代要「两个 stage 各绑一次」，
  D3D12 里根签名的 `ShaderVisibility` 写成 `PIXEL` 的话顶点着色器读到的 `model` 全是 0 —— 画面直接变形，不是编译错误

### UV 子矩形与逐项着色

两件事共用同一个「UV 仿射变换 + 乘法着色」：

- **平铺**（平台/背景）：`uv_scale = (w / uv_repeat_px, h / uv_repeat_px)`、`uv_offset = 0`，采样器选 WRAP
- **取多帧条带的一帧**（例：传送时的两帧加载动画）：`uv_scale = (1/count, 1)`、`uv_offset = (i/count, 0)`，采样器选 CLAMP，
  **约定条带横排**（竖排就要改这套公式，所以干脆把它写在注释里当约定）
- **着色**：`tint` 与采样结果逐分量相乘 —— 灰色 × 颜色 = 那个颜色的图案。所以美术只要给**灰度**图，
  配对颜色、淡入淡出、变暗/变虚这些状态都只是改一个 tint，不必为每种颜色存一张贴图（可消失平台的三态也是这么做的）

`uv_repeat_px` 与条带取帧是互斥的两条路：前者自己算 `uv_scale`，所以取帧时把 `uv_repeat_px` 传 0。

## 7. API 参考

| 函数 | 作用 | 实现要点 |
| --- | --- | --- |
| `renderer_create(native_window, w, h)` | 设备/队列/交换链/描述符堆/根签名/PSO/缓冲 | 任何一步失败都会 `renderer_destroy` 后返回 `nullptr`（`ok` 标志位 + 一个 `defer`） |
| `renderer_resize(w, h)` | 客户区变化后重建后台缓冲 | **必须先等所有在途帧的围栏跑完**（= 排空队列），再释放后台缓冲引用，然后 `ResizeBuffers` + 重建 RTV |
| `renderer_create_texture(rgba, w, h)` | 上传 RGBA8 纹理 | DEFAULT 堆 + 暂存（256B 行距）+ 一次 `CopyTextureRegion` + 屏障 + 等围栏；失败返回 `TEXTURE_NONE` |
| `renderer_destroy_texture(handle)` | 释放纹理资源 | 槽位不复用；**不等围栏**（见 §9 的那条警告） |
| `renderer_frame_begin(...)` | 开命令列表 + 设视图 + 固定状态 + 清屏 | 每帧重置分配器与命令列表，不做状态缓存（PSO 之后本来就没几件事） |
| `renderer_push_sprite(...)` | 提交一个精灵 | `order` 决定同层次序；`uv_repeat_px > 0` 触发平铺；`SpriteStyle` 带 UV 子矩形与 tint |
| `renderer_push_rect_outline(...)` | 提交一个中空矩形线框 | 走同一个绘制列表，`LAYER_DEBUG` 默认层；线宽 = `OUTLINE_THICKNESS_PX` 设备像素（除以 zoom 换成世界长度）；`SpriteStyle` 只用 tint；`order` 与 `renderer_push_sprite` 同义 —— 给实心项描边时要传它的 `order + 1` |
| `renderer_push_ui_rect(...)` / `_outline(...)` | 提交一个**屏幕空间**矩形（UI 层） | 参数是客户区像素的左上角 + 尺寸，固定进 `LAYER_UI`；不经过相机与缩放，线宽也不需要除 zoom |
| `renderer_frame_end()` | 排序 + 记录绘制 + 提交 + Present | 跳过 `TEXTURE_NONE` 的项；`frame_begin` 失败过（列表没开）就直接返回 |
| `renderer_set_present_interval(interval)` | 呈现节奏 | 0 = 不 Present（`--fast` 用）。绘制与 `DrawInstanced` 照常执行，只是不往屏幕提交 |

## 8. 与调试可视化的接入点

碰撞层与游戏层只调 `debug_vis_*` 上报矩形，**装配层**（`src/scene.cc`）遍历标签把它们翻成 `renderer_push_rect_outline`。

这么做而不是给渲染层加一个「画线」专用 API 的理由：**线框也是绘制项**，它因此免费获得了排序、分层（`LAYER_DEBUG` 永远在最上面）、以及"看到的盒就是算碰撞的盒"的一致性。

颜色由标签决定（青=实体、绿=单向、黄=玩家、洋红=地面探针），每种颜色是一张 **1×1 的纯色纹理**。

一个例外：地面探针是「玩家盒整体下移 1 像素」，画成线框会和玩家盒重合（看起来只是黄框下面多了根洋红边）。
所以它由装配层改成只画**多出来的那条底边**：一条**色带**（1×1 纹理的 alpha 调到 200，走精灵那条路径）。
只抬高**厚度**（按视口高度取，约 1/180、至少 6 设备像素），宽度与底边保持精确：
向下加厚会假装探针伸得更低，看起来就像「该判定却没判定」。

## 9. 坑与限制

- **采样器不能写成根签名里的静态采样器**（本项目实测踩过，也是换代后唯一一处“看起来更优雅、其实错”的决定）：
  静态采样器是按**寄存器**固定在根签名上的，而“这一项要不要平铺”是**逐项**决定的（`uv_repeat_px > 0`）——
  写死一个就只能整帧用一种。当时的症状是：平台（平铺项）被 CLAMP 采样，UV 全部超出 [0,1] 而撞到 texel(31,31)，
  而 `data/base/brick.png` 的四边恰好是黑色勾缝，于是**整块平台渲染成纯黑**，
  而 `test\run_tests.bat all fast` 照样 7/7 全过（用例全是逻辑断言，黑屏也能过）。
  正确做法是回到 D3D11 那套：两个采样器对象写进 SAMPLER 堆，逐项切 s0
- **Flip Model 的 Resize 顺序**：D3D11 只要先解绑 RTV；D3D12 还必须**等 GPU 排空**（等所有在途帧的围栏），
  否则刚释放的后台缓冲可能还在被 GPU 读，`ResizeBuffers` 直接失败
- **采样数必须是 1**：Flip Model 不支持 MSAA（想要抗锯齿得自己做后处理）
- **`MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)`**：禁掉 DXGI 自带的 Alt+Enter 全屏，否则和我们自己的 F11 切换打架
- **关闭背面剔除**：`flip_x` 是用"负缩放"实现的，会翻转三角形绕序，不关剔除就会整个精灵消失
- **裁剪矩形没有默认值**：`RSSetScissorRects` 漏掉就是整个画面不画（D3D11 有默认全屏，这个坑是换代才会遇到的）
- **上传堆资源的初始状态只能是 `GENERIC_READ`**：填别的会被调试层拒。好处是它同时含
  `VERTEX_AND_CONSTANT_BUFFER` 与两个 `SHADER_RESOURCE` 位，所以顶点缓冲与常量环**一次状态转换都不需要**
- **常量缓冲视图的起点必须 256 字节对齐**：所以「每项一份 96 字节的常量」必须按 256 切槽；
  一帧的槽上限 `MAX_CONSTANTS_PER_FRAME = MAX_DRAW_ITEMS × 4` —— 线框一项会展开成 4 次绘制、每条边一份 model 矩阵
- **`CopyTextureRegion` 的源行距必须 256 字节对齐**：不能一句 memcpy 铺平，要按 `GetCopyableFootprints` 的 `RowPitch` 一行一行拷
- **深度/模板状态不能留零**：`D3D12_DEPTH_STENCIL_DESC` 里 0 不是任何 `D3D12_COMPARISON_FUNC` / `D3D12_STENCIL_OP` 的合法值，
  即使 `DepthEnable = FALSE` 也要填成 `ALWAYS` / `KEEP`
- **上限**：纹理 256 张（超出写日志返回 `TEXTURE_NONE`）、每帧绘制项 4096（超出 `assert`，release 下是未定义行为 ——
  与 D3D11 时代同一条约定）、常量环 16384 槽（结构性上限，写之前仍有显式检查而不是只靠断言）
- **销毁纹理后槽位不复用**：`renderer_destroy_texture` 只释放资源，不回收表项。
  **D3D11 里绑到已释放的纹理只是采样结果未定义，D3D12 里那张描述符会指向一个已释放的资源**
  （调试层会报出来）。当前所有纹理都是常驻的，所以没暴露；真要动态销毁纹理得先把在途帧排空
- **没有批处理**：每个绘制项一次 `DrawInstanced`（6 个顶点）。项数少时完全够用；
  要合并得把 UV 偏移塞进顶点/常量，并让同图集的项排序相邻
- **`renderer_create` 用 `*renderer = {}` 清零**：`Renderer` 没有虚函数，所以安全；
  **如果将来给它加虚成员（比如某个 COM 回调），必须改成 placement new**，否则 arena 的全零内存会留下空虚表指针 ——
  这个坑在 `docs/audio-system.md` §13 更新记录（2026-09-19）里有完整复现记录（AudioState 就这么崩过）
- **`renderer_destroy_texture` 目前无调用者**：纹理全生命周期常驻，会话结束时随设备一起销毁
- **调试层**：`MONO_DEBUG_ANY` 编译期开启（debug 档默认开）。没装"图形工具"可选功能时
  `D3D12GetDebugInterface` 失败，只记一条 WARN 继续跑 —— 它是开发工具，不是运行的必需条件。
  **实测它不拖慢 `--fast` 回归**（baseline 0.94~0.97s，与 D3D11 时代的 1.0s 同级）

## 10. 排查表

| 症状 | 可能原因 | 排查位置 |
| --- | --- | --- |
| 画面全黑 | HLSL 编译失败（构建期报错：`build.bat` 打印 fxc 原文并以退出码 1 收尾，不会产出新 exe）；`.cso` 读不到（日志里有 `renderer: cannot read build/shaders/*.cso`）；忘了 `RSSetScissorRects`；后台缓冲没转 `RENDER_TARGET`；PSO 创建失败（日志 `CreateGraphicsPipelineState failed`） | `create_pipeline_state` 里读 `.cso` 的分支、`frame_begin` 的视图段 |
| 第一次 Present 就失败 / 画面卡住 | 忘了在 Present 前把后台缓冲转回 `PRESENT` | `frame_end` 末尾的屏障 |
| Resize 之后报 `DXGI_ERROR_INVALID_CALL` | 没有先等 GPU 排空就释放后台缓冲引用 | `renderer_resize` 的围栏等待 |
| 窗口缩放后画面拉伸/黑边 | `ResizeBuffers` 失败 | `renderer_resize` |
| 调试层报「descriptor not set / heap not set」 | 漏了 `SetDescriptorHeaps` 或 `SetGraphicsRootDescriptorTable` | `frame_begin` / `frame_end` |
| 调试层报「resource state mismatch」 | 屏障的 `StateBefore` 与实际不符（典型：忘了 `frame_begin` 那次 `PRESENT → RENDER_TARGET`） | 两处 `ResourceBarrier` |
| 精灵被别的物件盖住 | 忘了给 `order`（默认 0，会退化成按纹理排序） | `renderer_push_sprite` 的 `order` 参数 |
| 角色画在平台后面 | 排序键的第二段（order）没给 | `draw_item_sort_key` |
| 平铺纹理只拉伸不重复 | 没给 `uv_repeat_px`，或采样器用了 CLAMP | `uv_scale`、`frame_end` 里的采样器选择 |
| 平铺纹理整块变成一种颜色（例：平台全黑） | 平铺项被 CLAMP 采样：UV 超出 [0,1] 后全部撞到边缘 texel。根因通常是采样器被写成了静态采样器（见 §9） | `create_root_signature` 的 `[2]` 参数、`frame_end` 的 `sampler_slot` |
| 精灵左右颠倒了但位置没错 | `flip_x` 只翻转缩放（负值），位置仍由中心决定 —— 这是预期行为 | `renderer_draw_quad` 的 `ndc_scale_x` |
| 画面脏乱 / 闪一下旧内容 | 常量环那一段被提前复用（帧在途数与缓冲数不一致，或围栏值记错了帧） | `frame_begin` 的围栏等待、`frame_end` 的 `Signal` |
| 改了 shader 没生效（或启动就报 `cannot read build/shaders/*.cso`） | `.cso` 是**构建产物**，运行期不会看 `.hlsl`；路径按**进程工作目录**解析（在仓库根启动 → `build/shaders/*.cso`） | `build.bat` 的两个 `fxc` 调用、`create_pipeline_state` 里两行 `read_file(L"build/shaders/...")` |
| 选错了显卡 | `D3D12CreateDevice(nullptr, ...)` 取的是系统默认适配器；启动日志里的 `GPU:` 行就是它 | `renderer_create` 的建设备那一步、`log_adapter` |

## 11. 扩展指南

| 想做的事 | 落点 |
| --- | --- |
| 加一个图层 | `SpriteLayer` 枚举（排序键自动覆盖） |
| 加常量（如 tint 颜色） | `TransformConstants` + HLSL 的 cbuffer 同步改，注意 16 字节对齐；**常量一旦超过 96 字节，顺带看一眼 `CONSTANT_SLOT_SIZE` 还装不装得下** |
| 加图集 / 合并批次 | 把 UV 偏移放进顶点或常量缓冲，并让同图集项在排序键里相邻 |
| 改用实例化绘制 | `DrawItem` 的字段正好可以喂给 per-instance 顶点缓冲；那时「每项一份常量」可以整段省掉 |
| 加第二个 PSO（比如描边 / 加色混合） | `pipeline_state` 旁边加一个，按需 `SetPipelineState` |
| 再加一种采样方式（比如镜像寻址） | `SAMPLER_COUNT` 加一槽 + `renderer_create` 里多建一个采样器对象 + `DrawItem` 里多一个标志（**不要**改成静态采样器，理由见 §9） |
| 动态创建纹理 | 把「每条一次 Execute + 等围栏」改成批量：多次拷贝录进同一条命令列表、一次等围栏；并且 `destroy_texture` 要先排空在途帧 |
| ~~换 DX12~~ | ~~`renderer.h` 不动，重写 `src/d3d11_renderer.cc`；已知要动的地方：常量缓冲要自己管环、加资源屏障、描述符堆~~ —— **2026-10-01 已完成**，见 §12 |

## 12. 变更记录

### 2026-09-19：解耦与注释补全

- `TextureHandle` / `TEXTURE_NONE` 从 `core.h` 移到 **`renderer.h`** —— 句柄是渲染层产出的数据，类型应该跟生产者走；`include/renderer.h` 因此保持零平台依赖，`include/` 下不再有任何文件需要 D3D 类型
- 同批把精灵资源类型移出 `core.h`（`include/sprite.h`），因为 `SpriteImage` 要用 `TextureHandle`
- `order` 参数与排序键的注释补全（它存在的**唯一**理由是避免"角色被平台盖住"）
- 本文件建立

### 2026-09-20：文档校正

- 数据流图把“游戏/装配层（`main.cc`、`debug_vis`）”改为“装配层 `main.cc`”：`debug_vis` 只是纯数据上报，不做任何渲染层的翻译
- 排序键位宽修正为 **8 + 32 + 24**（层 / order / 纹理）：原文的 24+8+24 与 `draw_item_sort_key` 的移位不符
- 着色器路径口径统一：按**进程工作目录**解析 `shaders/*.hlsl`，`build/shaders/` 只是给“在 build 目录启动”准备的副本
  （2026-10-01 起改为读 `build/shaders/*.cso`，见 §6）
- 跨文档引用修正（虚表 placement new 在 `audio-system.md` 的 §13）

### 2026-09-25：装配层拆分与描边次序

- 场景图遍历（背景平铺 / 平台 / 地刺 / 可消失平台 / 门与加载动画 / 移动组件 / 怪物 / 能量波 / 时停 / 调试盒 / 玩家 / UI）
  与程序化占位贴图从 `main.cc` 的 `wWinMain` 搬到 **`src/scene.cc`**（+ `include/scene.h` 的 `SceneTextures` / `SceneFrame`）；
  所以数据流图里的「装配层」现在是两处：`main.cc`（初始化 + 主循环 + 相机插值吸附）与 `scene.cc`（每帧绘制提交）
- `renderer_push_rect_outline` 增加 `order` 参数（以前硬写 0：「边框压在门板之上」只靠 `white_texture` 比
  `portal_texture` 后创建、句柄更大 —— 那是巧合不是约定）
- 一条 `angle == 0` 的快路径：绝大多数绘制项不带旋转，不再每项算一次 `sinf` / `cosf`

### 2026-10-01：着色器改为离线预编译

- 渲染层不再在运行期编译 HLSL：`compile_shader`（`D3DCompileFromFile`）删除，`Renderer` 里的两个
  `ID3DBlob *vs_blob/ps_blob`、`<d3dcompiler.h>` 与 `#pragma comment(lib, "d3dcompiler")` 一并去掉；
  `renderer_create` 改成 `read_file(L"build/shaders/triangle_vs.cso")` / `triangle_ps.cso`，
  字节码的寿命由 `ReadFileRes` 的析构保证（要活到 `CreateInputLayout` 之后）
- `build.bat` 相应增加 `fxc` 两步（`vs_5_0` / `ps_5_0`，入口 `main`，标志 0）与参数识别：
  默认 debug（与以前完全一致） / `release`（`/MT /O2 /DNDEBUG /GL /LTCG` + 四个调试宏全关） / `shaders`（只重编着色器）
- 代价：改 shader 要重跑 `build.bat shaders`，且运行期必须要 `build/shaders/*.cso` 在位（读不到就
  `LOG_ERROR` + `renderer_create` 失败）。**跑过构建就不会缺**：三种模式都会重编着色器

### 2026-10-01：D3D11 → D3D12（本文件随之改名）

- **`include/renderer.h` 一行没改** —— 它本来就不含任何 D3D/DXGI 类型（窗口 `void *`、纹理句柄）。
  实现文件 `src/d3d11_renderer.cc` → `src/d3d12_renderer.cc`（`build.bat` 的 `source_files` 与
  `compile_commands.json` 同步；`.clangd` 只放公共参数、不列文件，所以没动）
- 着色器目标 `vs_5_0` / `ps_5_0` → `vs_5_1` / `ps_5_1`（`build.bat` 那两个 `-T`）。理由见 §6：
  5.1 是 D3D12 时代的地板线，6.x 要走 `dxc` + 签名 DXIL 而收益为零
- 新增的东西（相对 D3D11 的即时上下文）：**命令分配器 / 命令列表 / 围栏**（帧在途 2，与后台缓冲数一致）、
  **根签名**（根 CBV b0 + 1×SRV 描述符表 + 2 个静态采样器）、**PSO**（D3D11 那些散装状态一次固化）、
  **两个描述符堆**（RTV 2 个 + SRV 256 个）、**资源屏障**（后台缓冲 `PRESENT ↔ RENDER_TARGET`，
  纹理 `COPY_DEST → PIXEL_SHADER_RESOURCE`）、**常量环**（每帧一段、逐项按 256B 切槽、根 CBV 寻址）
- 交换链挂在**命令队列**上（D3D11 挂设备），用 `IDXGISwapChain3::GetCurrentBackBufferIndex`；
  `renderer_resize` 新增「等 GPU 排空」这一步
- 调试层：`MONO_DEBUG_ANY` 编译期开启（`D3D12GetDebugInterface` + `EnableDebugLayer`），
  装不了就只记 WARN。**实测调试层对 `--fast` 回归没有可测量的影响**，所以没做成运行期开关
- **编辑器（`build\editor.exe`）刻意留在 D3D11**：它只用 ImGui 画界面，迁过去要额外引入
  `imgui_impl_dx12` + `d3dx12.h` 并把宿主从约 100 行撑到两百多行，而它的 `--check` / `--selftest`
  都不建窗口、那段代码测不到。代价是仓库里同时存在两代设备代码（两个程序互不共享进程）
- 验证：`test\run_tests.bat all fast` **7/7 通过**（含确定性守卫的两遍 trace 对拍）；
  baseline 快跑 0.94~0.97s（与 D3D11 时代记录的 1.0s 同级）；日志里调试层零条校验消息

### 2026-10-01：修正——采样器不能用静态采样器（同一天发现）

- 第一版把两个采样器写成了根签名里的**静态**采样器（想省掉一个描述符堆）。这是错的：静态采样器按寄存器
  固定在根签名上，而平铺与否是逐项决定的。症状是**平台整块渲染成纯黑**（UV 超出 [0,1] 被 CLAMP 到 texel(31,31)，
  而 `brick.png` 四边是黑色勾缝），而逻辑回归 7/7 全过 —— 用例覆盖不到画面
- 改成 3 个根参数（多一个 `[2]` 采样器描述符表）+ 一个 `SAMPLER` 描述符堆（2 个采样器对象），
  逐项在 `frame_end` 里按 `uv_repeat_px > 0` 切 s0；着色器**一行没改**（它仍然只用 `s0`）
- 这条也给「改完必须跑一次验证」补了一个更硬的口径：**渲染层改动必须目视看画面**，
  回归通过只证明逻辑没坏（详见本文件 §9 与 `AGENTS.md` §5）
