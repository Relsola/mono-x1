#pragma once

#include "core.h"
#include "renderer.h" // 只为了 TextureHandle 这个句柄类型

// ============================================================================
// 游戏资源：精灵图与帧动画
//
// 这一层只描述「一张图 / 一段动画长什么样」，以及它对应渲染层的哪个句柄：
//   - 解码（stb_image）在 src/sprite.cc，上传到 GPU 由装配层（main.cc）调 renderer_create_texture 完成
//   - 本层不认识 D3D，也不调用任何渲染器 API，句柄的生成与解释都留在渲染层
//
// 依赖方向：sprite.h -> renderer.h（仅取句柄类型）+ core.h（基础类型）
// ============================================================================

// 由 arena 分配，一张已解码的精灵图（CPU 侧像素 + 渲染层纹理句柄）
struct SpriteImage
{
    u8 *pixels; // RGBA 像素数据，自顶向下、按行连续排列，每像素 4 字节
    int width;
    int height;
    TextureHandle texture = TEXTURE_NONE; // 上传到渲染层后写入

    // 相对原图的显示缩放
    f32 scale = 1.0f;
};

// 动画帧
struct AnimationFrame
{
    SpriteImage image;
    f32 duration;
};

// 精灵图完整动画
struct SpriteAnimation
{
    AnimationFrame *frames;
    u32 frame_count;
    u32 current_frame;
    f32 elapsed;
    bool looping;
    bool finished;
};

// ---------------------------------------------------------------------------
// 接口（实现见 src/sprite.cc）
// ---------------------------------------------------------------------------

// 从文件解码出一张 RGBA8 精灵图；失败时返回的 result.pixels 为 nullptr
SpriteImage load_sprite(const wchar_t *filename);

// 当前帧（帧号越界会触发断言）
AnimationFrame *get_current_animation(SpriteAnimation *animation);

// 按 dt 推进动画；looping 为假时停在最后一帧并置 finished
void animation_update(SpriteAnimation *animation, f32 dt);
