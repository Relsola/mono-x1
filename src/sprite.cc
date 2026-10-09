#include "sprite.h"
#include "shared/memory.h"
#include "shared/file.h"
#include "shared/logger.h"

// ============================================================================
// 游戏资源的加载与动画推进
//
// 图像解码器的实现集中在这一个编译单元，避免污染其他文件；
// 用 warning(push, 0) 屏蔽第三方代码的告警，保持本项目构建输出干净。
//
// 内存策略：stb_image 的分配走全局 scratch arena，STBI_FREE 是空操作 ——
// 贴图在启动阶段一次性解码完，scratch 之后会被 reset，不需要逐张释放。
// ============================================================================
#pragma warning(push, 0)
#define STBI_MALLOC(sz)                     scratch_push(&global_scratch, (sz))
#define STBI_REALLOC_SIZED(p, oldsz, newsz) scratch_realloc(&global_scratch, (p), (oldsz), (newsz))
#define STBI_FREE(p)                        ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"
#pragma warning(pop)

SpriteImage load_sprite(const wchar_t *filename)
{
    SpriteImage result = {};

    ReadFileRes file = read_file(filename);
    if (!file.contents) {
        LOG_ERROR("load_sprite: cannot open file: %ls", filename);
        return result;
    }

    // 把原始文件字节交给 stb_image 从内存解码。desired_channels 写死为 4（RGBA），
    // 匹配 DXGI_FORMAT_R8G8B8A8_UNORM，stb_image 会自动补 alpha=255。
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    u8 *pixels = stbi_load_from_memory((const u8 *)file.contents, (int)file.file_size,
                                       &width, &height, &channels_in_file, 4);

    if (!pixels) {
        LOG_ERROR("load_sprite: decode failed: %ls", filename);
        return result;
    }

    result.width = width;
    result.height = height;
    result.pixels = pixels;
    return result;
}

AnimationFrame *get_current_animation(SpriteAnimation *animation)
{
    assert(animation->frame_count > 0);
    assert(animation->current_frame < animation->frame_count);
    return &animation->frames[animation->current_frame];
}

void animation_update(SpriteAnimation *animation, f32 dt)
{
    if (animation->finished || animation->frame_count == 0) {
        return;
    }

    animation->elapsed += dt;
    while (animation->elapsed >= get_current_animation(animation)->duration) {
        AnimationFrame *frame = get_current_animation(animation);
        animation->elapsed -= frame->duration;
        animation->current_frame += 1;

        if (animation->current_frame >= animation->frame_count) {
            if (animation->looping) {
                animation->current_frame = 0;
            } else {
                animation->current_frame = animation->frame_count - 1;
                animation->finished = true;
                break;
            }
        }
    }
}
