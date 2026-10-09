#pragma once

#include "core.h"

// ============================================================================
// 渲染层接口
//
// 只暴露句柄与数据，不出现任何 D3D / DXGI 类型：
//   1. 纹理用 TextureHandle（索引）表示，游戏侧不持有 GPU 资源；
//   2. 绘制走「帧绘制列表」：游戏每帧只提交数据，由渲染层负责排序、切换纹理与提交。
// ============================================================================

// 纹理句柄：由渲染层解释的索引，游戏侧只持有、不解释。
using TextureHandle = u32;
inline constexpr TextureHandle TEXTURE_NONE = 0;

enum SpriteLayer : u8
{
    LAYER_BACKGROUND, // 背景（最先画）
    LAYER_WORLD,      // 世界物件与角色
    LAYER_DEBUG,      // 调试可视化
    LAYER_UI,         // UI（最后画）
    LAYER_COUNT
};

struct Renderer; // 不透明，实现见 d3d12_renderer.cc

// 创建渲染器（内存取自 arena）；失败返回 nullptr
Renderer *renderer_create(void *native_window, u32 client_width, u32 client_height);
void renderer_destroy(Renderer *renderer);

// 客户区尺寸变化后重建后台缓冲
bool renderer_resize(Renderer *renderer, u32 client_width, u32 client_height);

// 上传一张 RGBA8 像素图，返回纹理句柄
TextureHandle renderer_create_texture(Renderer *renderer, const void *rgba_pixels, int width, int height);
void renderer_destroy_texture(Renderer *renderer, TextureHandle texture);

// 帧开始：设定视图
void renderer_frame_begin(Renderer *renderer,
                          f32 camera_x, f32 camera_y, f32 camera_zoom,
                          u32 screen_width, u32 screen_height);

// 精灵的 UV 子矩形与着色。默认值 = 整张纹理 + 不着色
//   offset 纹理偏移 / scale 纹理取值范围
//   多帧条带：把 uv 缩到 1/count、平移到 i/count 就取到条带里的一帧（约定条带横排）
//   灰度贴图着色：采样结果与 tint_* 相乘 —— 灰色 × 颜色 = 那个颜色的图案
//   线框（renderer_push_rect_outline）只用 tint，uv_* 会被忽略
// 与 uv_repeat_px 平铺是互斥的两条路：平铺时 uv_scale 由世界尺寸/`uv_repeat_px` 算出（走 WRAP 采样器），
// 取条带帧时把 uv_repeat_px 传 0（走 CLAMP 采样器）。
struct SpriteStyle
{
    f32 uv_offset_x = 0.0f;
    f32 uv_offset_y = 0.0f;
    f32 uv_scale_x = 1.0f;
    f32 uv_scale_y = 1.0f;
    f32 tint_r = 1.0f;
    f32 tint_g = 1.0f;
    f32 tint_b = 1.0f;
    f32 tint_a = 1.0f;
};

// 提交一个世界坐标精灵
//   x / y / width / height 世界坐标
//   uv_repeat_px > 0 时，纹理每 uv_repeat_px 个世界像素重复一次（配合 WRAP 采样器平铺）
//   flip_x 水平镜像（用于角色朝向）
//   order 同一层内的绘制次序（小的先画），用于保证遮挡关系；相同 order 会按纹理合并批次
//   style 额外的 UV 子矩形与着色
void renderer_push_sprite(Renderer *renderer, TextureHandle texture,
                          f32 x, f32 y, f32 width, f32 height,
                          f32 angle = 0.0f, SpriteLayer layer = LAYER_WORLD,
                          f32 uv_repeat_px = 0.0f, bool flip_x = false, int order = 0,
                          const SpriteStyle &style = {});

// 提交一个中空矩形线框（调试用）
//   线宽由渲染层定死（按设备像素给，不受相机缩放影响），实现是 4 条细长四边形
//   style 只用 tint（线框各条边铺满整张纹理，UV 子矩形对它没有意义）
//   order 与 renderer_push_sprite 同义（同层内的绘制次序）。给实心项描边时要显式传它的 order + 1
//   否则「边框压在门板之上」只能靠纹理句柄大小决定（句柄小的先画），那是巧合而不是约定。
void renderer_push_rect_outline(Renderer *renderer, TextureHandle texture,
                                f32 x, f32 y, f32 width, f32 height,
                                SpriteLayer layer = LAYER_DEBUG,
                                int order = 0,
                                const SpriteStyle &style = {});

// 屏幕空间矩形（UI 层用）：x, y 是客户区像素里的**左上角**，width/height 是像素尺寸。
// 与世界坐标精灵共用同一条绘制列表与同一份着色器，区别只在 model 矩阵：屏幕矩形不经过相机与
// 相机缩放，所以少三个参数（没有 angle / uv_repeat_px / flip_x）。点它就是 LAYER_UI。
// 为什么把 UI 也做在同一个绘制列表里：排序、批次合并、Present 节奏全都自动继承，
// 不必为 UI 单开一条渲染路径 —— 代价是要有一个「这个项是屏幕空间」的标志位。
void renderer_push_ui_rect(Renderer *renderer, TextureHandle texture,
                           f32 x, f32 y, f32 width, f32 height,
                           int order = 0,
                           const SpriteStyle &style = {});

void renderer_push_ui_rect_outline(Renderer *renderer, TextureHandle texture,
                                   f32 x, f32 y, f32 width, f32 height,
                                   int order = 0,
                                   const SpriteStyle &style = {});

// 帧结束：按（层 → 纹理）排序提交绘制，最后 Present
void renderer_frame_end(Renderer *renderer);

// 呈现节奏：每 interval 帧调一次 Present（1 = 默认，每帧都呈现且等垂直同步）。
// interval = 0 表示**不呈现**（`--fast` 用）：绘制列表与 Draw 调用照常执行，只是不往屏幕提交 ——
// 实测限速全在 Present 上（它对 DXGI 翻转队列同步），关掉它快跑模式才真的快。
// 副作用：窗口画面停止更新（这不是无头模式，我们自己的渲染代码仍然整条跑过）。
void renderer_set_present_interval(Renderer *renderer, u32 interval);
