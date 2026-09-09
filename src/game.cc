#include "core.h"

#define STBI_MALLOC(sz)                     scratch_push(&global_scratch, (sz))
#define STBI_REALLOC_SIZED(p, oldsz, newsz) scratch_realloc(&global_scratch, (p), (oldsz), (newsz))
#define STBI_FREE(p)
#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"

internal SpriteImage load_sprite(const wchar_t *filename)
{
    SpriteImage result = {};

    ReadFileRes file = read_file(filename);
    if (!file.contents) {
        return result;
    }

    // 把原始文件字节交给 stb_image 从内存解码。desired_channels 写死为 4（RGBA），
    // 匹配 DX11 的 DXGI_FORMAT_R8G8B8A8_UNORM，stb_image 会自动补 alpha=255。
    // 图像格式(PNG/JPEG/BMP…)由 stb_image 按文件头自动识别，无需指定。
    i32 width = 0, height = 0, channels_in_file = 0;
    u8 *pixels = stbi_load_from_memory((const u8 *)file.contents,
                                       (i32)file.file_size,
                                       &width, &height,
                                       &channels_in_file, 4);

    free_file_memory(file.contents);
    if (!pixels) {
        return result;
    }

    result.width = width;
    result.height = height;
    result.pixels = pixels;
    return result;
}

// 更新动画到下一帧
internal void animation_update(SpriteAnimation *animation, f32 dt)
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

AnimationFrame *get_current_animation(SpriteAnimation *animation)
{
    assert(animation->frame_count > 0);
    assert(animation->current_frame < animation->frame_count);
    return &animation->frames[animation->current_frame];
}

void game_init_asset(GameState *game_state)
{
    game_state->backdrop = load_sprite(L"data/test_background.bmp");

    SpriteAnimation *animation = &game_state->player_bagdown_animation;
    // 暂时硬编码11张
    animation->frame_count = 11;
    animation->looping = true;
    animation->frames = (AnimationFrame *)arena_push(sizeof(AnimationFrame) * 11);

    // 当前先以 12 FPS 循环播放 f0 到 f10。
    constexpr f32 frame_duration = 1.0f / 12.0f;
    for (u32 i = 0; i < animation->frame_count; ++i) {
        wchar_t filename[128];
        swprintf_s(filename, L"data/player/bagdown/f%u.png", i);
        animation->frames[i].image = load_sprite(filename);
        animation->frames[i].image.scale = 4.0f;
        animation->frames[i].duration = frame_duration;
    }

    // 身体碰撞箱独立于当前动画帧，位置以角色脚底中心为参考。
    SpriteImage *first_frame = &get_current_animation(animation)->image;
    game_state->player_collider.width = (f32)first_frame->width * first_frame->scale * 0.8f;
    game_state->player_collider.height = (f32)first_frame->height * first_frame->scale * 0.8f;
    game_state->player_collider.offset_x = 0.0f;
    game_state->player_collider.offset_y = -game_state->player_collider.height * 0.2f;
}

// 圆形径向死区 + 线性重映射
// 摇杆到中心点的距离小于死区时整体归零；超过死区后线性重映射到 [0, 1]。
internal v2 stick_to_dir(f32 x, f32 y, f32 deadzone)
{
    v2 result = {};
    f32 len = sqrtf(x * x + y * y);
    if (len > deadzone) {
        // TODO 1.0f 线性调优
        f32 scale = (len - deadzone) / (1.0f - deadzone);
        result = v2{ (x / len) * scale, (y / len) * scale };
    }
    return result;
}

void game_update(GameInput *game_input, GameState *game_state, f32 dt)
{
    animation_update(&game_state->player_bagdown_animation, dt);

    constexpr f32 max_player_speed = 640.0f;
    constexpr f32 stick_deadzone = 0.2f; // 摇杆死区阈值（经验值：XInput 默认约 24%，Steam 常见 20%）
    // constexpr f32 player_acceleration = 3200.0f; // 按下方向键时，速度趋近目标速度的加速度
    // constexpr f32 player_deceleration = 6400.0f; // 松开方向键时，速度按摩擦力回落到 0 的减速度

    PlayerInput *controller = &game_input->player;

    v2 input_dir = {};
    if (controller->current[GA_LEFT]) {
        input_dir.x -= 1.0f;
    }
    if (controller->current[GA_RIGHT]) {
        input_dir.x += 1.0f;
    }
    if (controller->current[GA_UP]) {
        input_dir.y += 1.0f;
    }
    if (controller->current[GA_DOWN]) {
        input_dir.y -= 1.0f;
    }

    // 摇杆模拟方向：越过死区后按满速处理（忽略幅度调速，后续再做加速度系统）
    v2 stick_dir = stick_to_dir(controller->left_stick_x, controller->left_stick_y, stick_deadzone);
    if (stick_dir.length_sq() > 0.0f) {
        input_dir = stick_dir; // 当前只要越过死区就映射为 1
    }

    // 归一化方向（避免对角线数字输入速度快 √2 倍），速度恒定为最大速度
    v2 target_velocity = input_dir.normalized() * max_player_speed * dt;

    // 轴分离碰撞检测（X/Y 独立移动与沿墙滑动）
    f32 player_x = game_state->player_x;
    f32 player_y = game_state->player_y;
    f32 player_box_w = game_state->player_collider.width;
    f32 player_box_h = game_state->player_collider.height;
    f32 player_box_offset_x = game_state->player_collider.offset_x;
    f32 player_box_offset_y = game_state->player_collider.offset_y;

    Rect2D *wall_colliders = game_state->wall_colliders;
    // constexpr u32 wall_count = sizeof(game_state->wall_colliders) / sizeof(*game_state->wall_colliders);
    constexpr u64 wall_count = ARRAY_SIZE(game_state->wall_colliders);

    // 先在 X 轴上尝试移动：碰撞时把玩家推到墙壁边缘，实现贴合
    if (target_velocity.x != 0.0f) {
        f32 next_x = player_x + target_velocity.x;
        Rect2D player_test_x = make_rect_center(next_x + player_box_offset_x, player_y + player_box_offset_y, player_box_w, player_box_h);

        f32 resolved_x = next_x;
        for (u32 i = 0; i < wall_count; ++i) {
            if (test_rect_overlap(&player_test_x, &wall_colliders[i])) {
                if (target_velocity.x > 0.0f) {
                    // 向右移动，贴到墙的左边缘
                    f32 edge_x = wall_colliders[i].center_x - wall_colliders[i].half_w - player_box_w * 0.5f;
                    resolved_x = MIN(resolved_x, edge_x);
                } else {
                    // 向左移动，贴到墙的右边缘
                    f32 edge_x = wall_colliders[i].center_x + wall_colliders[i].half_w + player_box_w * 0.5f;
                    resolved_x = MAX(resolved_x, edge_x);
                }
            }
        }
        game_state->player_x = resolved_x;
    }

    // 再在 Y 轴上尝试移动：碰撞时把玩家推到墙壁边缘，实现贴合
    if (target_velocity.y != 0.0f) {
        f32 next_y = player_y + target_velocity.y;
        Rect2D hero_test_y = make_rect_center(player_x + player_box_offset_x, next_y + player_box_offset_y, player_box_w, player_box_h);

        f32 resolved_y = next_y;
        for (u32 i = 0; i < wall_count; ++i) {
            if (test_rect_overlap(&hero_test_y, &wall_colliders[i])) {
                if (target_velocity.y > 0.0f) {
                    // 向上移动，贴到墙的下边缘
                    f32 edge_y = wall_colliders[i].center_y - wall_colliders[i].half_h - player_box_h * 0.5f - player_box_offset_y;
                    resolved_y = MIN(resolved_y, edge_y);
                } else {
                    // 向下移动，贴到墙的上边缘
                    f32 edge_y = wall_colliders[i].center_y + wall_colliders[i].half_h + player_box_h * 0.5f - player_box_offset_y;
                    resolved_y = MAX(resolved_y, edge_y);
                }
            }
        }
        game_state->player_y = resolved_y;
    }

    if (controller->current[GA_Q]) {
        game_state->camera.zoom = 2.5f;
    } else {
        game_state->camera.zoom = 1.0f;
    }

    game_state->camera.pos_x = game_state->player_x;
    game_state->camera.pos_y = game_state->player_y;
}
