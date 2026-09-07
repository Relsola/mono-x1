#include "core.h"

// #define STBI_ONLY_PNG // 目前只保留 PNG 解码器
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

void game_init_asset(GameState *game_state)
{
    game_state->backdrop = load_sprite(L"data/test_background.bmp");

    // 角色放大
    game_state->player_bagdown = load_sprite(L"data/player/bagdown/f0.png");
    game_state->player_bagdown.scale = 4.0f;
}

void game_update(GameInput *game_input, GameState *game_state, f32 dt)
{
    // 输入计算：获取本帧期望的原始位移量（像素）
    constexpr f32 max_player_speed = 640.0f;
    // constexpr f32 player_acceleration = 3200.0f; // 按下方向键时，速度趋近目标速度的加速度
    // constexpr f32 player_deceleration = 6400.0f; // 松开方向键时，速度按摩擦力回落到 0 的减速度

    v2 input_dir = {};
    if (game_input->controller[0].current[GA_LEFT]) {
        input_dir.x -= 1.0f;
    }
    if (game_input->controller[0].current[GA_RIGHT]) {
        input_dir.x += 1.0f;
    }
    if (game_input->controller[0].current[GA_UP]) {
        input_dir.y += 1.0f;
    }
    if (game_input->controller[0].current[GA_DOWN]) {
        input_dir.y -= 1.0f;
    }

    // 首先对输入方向进行归一化（避免对角线方向速度快√2倍），然后乘以速度和固定的dt。
    v2 target_velocity = input_dir.normalized() * max_player_speed * dt;

    // 轴分离碰撞检测（X/Y 独立移动与沿墙滑动）
    f32 player_x = game_state->player_x;
    f32 player_y = game_state->player_y;
    f32 player_box_w = (f32)game_state->player_bagdown.width * game_state->player_bagdown.scale;
    f32 player_box_h = (f32)game_state->player_bagdown.height * game_state->player_bagdown.scale;
    // 实际碰撞箱在玩家中心向下，暂时为硬编码
    f32 player_box_offset_y = -(f32)game_state->player_bagdown.height;

    Rect2D *wall_colliders = game_state->wall_colliders;
    // constexpr u32 wall_count = sizeof(game_state->wall_colliders) / sizeof(*game_state->wall_colliders);
    constexpr u64 wall_count = ARRAY_SIZE(game_state->wall_colliders);

    // 先在 X 轴上尝试移动：碰撞时把玩家推到墙壁边缘，实现贴合
    if (target_velocity.x != 0.0f) {
        f32 next_x = player_x + target_velocity.x;
        Rect2D player_test_x = make_rect_center(next_x, player_y + player_box_offset_y, player_box_w, player_box_h);

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
        Rect2D hero_test_y = make_rect_center(player_x, next_y + player_box_offset_y, player_box_w, player_box_h);

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

    if (game_input->controller[0].current[GA_Q]) {
        game_state->camera.zoom = 2.5f;
    } else {
        game_state->camera.zoom = 1.0f;
    }

    game_state->camera.pos_x = game_state->player_x;
    game_state->camera.pos_y = game_state->player_y;
}
