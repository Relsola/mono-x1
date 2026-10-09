#include "shared/collision.h"
#include "shared/mono_math.h"

Rect2D make_rect_center(f32 center_x, f32 center_y, f32 width, f32 height)
{
    return Rect2D{ .center_x = center_x, .center_y = center_y,
                   .half_w = width * 0.5f, .half_h = height * 0.5f };
}

// 检测两个 AABB 矩形是否发生重叠相交 (Separating Axis Theorem 分离轴定理 2D 特例)
bool test_rect_overlap(const Rect2D *a, const Rect2D *b)
{
    // 在 X 轴和 Y 轴上同时发生重叠，才判定为发生碰撞
    bool overlap_x = fabsf(a->center_x - b->center_x) < (a->half_w + b->half_w);
    bool overlap_y = fabsf(a->center_y - b->center_y) < (a->half_h + b->half_h);
    return overlap_x && overlap_y;
}

// 碰撞盒求交时的内缩量：角色正好站在平台顶面时，浮点误差会让
// 「刚好相切」被当成重叠，从而把支撑面误判成侧向墙把角色推出去
constexpr f32 COLLISION_EPSILON = 0.01f;

// 判断某个平台能否作为「从上方落下」的支撑：
// 单向平台需要移动前碰撞盒底边就在平台顶面之上；主动下穿期间忽略所有单平台
internal bool platform_supports_from_above(const Platform *platform, f32 box_bottom_before, bool ignore_one_way)
{
    if (platform->kind == COLLIDER_ONE_WAY && ignore_one_way) {
        return false;
    }

    f32 platform_top = platform->rect.center_y + platform->rect.half_h;
    return box_bottom_before >= platform_top - 1.0f;
}

// 单向平台能否当作「侧墙」：
// 只有移动前水平方向与平台完全不重叠时算侧墙（即从侧面撞上去）。
// 否则说明玩家本来就在平台的正上/正下方（例如起跳穿板时人还在板子里），
// 这时把它当侧墙会把人从板子里横推出去，跳穿会被打断。
internal bool platform_blocks_from_side(const Platform *platform, f32 left_before, f32 right_before)
{
    f32 platform_left = platform->rect.center_x - platform->rect.half_w;
    f32 platform_right = platform->rect.center_x + platform->rect.half_w;
    return right_before <= platform_left || left_before >= platform_right;
}

CollisionFlags move_and_collide(Rect2D *box, v2 *velocity, f32 dt,
                                const Platform *platforms, u32 platform_count, bool ignore_one_way)
{
    CollisionFlags flags = {};

    // ---- X 轴：实体平台永远阻挡；单向平台只在「从侧面撞上去」时阻挡 ----
    if (velocity->x != 0.0f) {
        // 移动前的水平范围，用来区分「从侧面撞上去」与「人本来就在板子里」
        f32 left_before = box->center_x - box->half_w;
        f32 right_before = box->center_x + box->half_w;

        Rect2D test = *box;
        test.center_x += velocity->x * dt;
        // 竖直方向微量内缩：站立时与支撑面相切不算水平碰撞
        test.half_h -= COLLISION_EPSILON;

        f32 resolved_x = test.center_x;
        for (u32 i = 0; i < platform_count; ++i) {
            const Platform *platform = &platforms[i];
            if (platform->kind != COLLIDER_SOLID &&
                !platform_blocks_from_side(platform, left_before, right_before)) {
                continue;
            }

            // 用当前解算结果重新判定，避免一次位移跨过多块平台时解算值失真
            Rect2D probe = test;
            probe.center_x = resolved_x;
            if (!test_rect_overlap(&probe, &platform->rect)) {
                continue;
            }

            if (velocity->x > 0.0f) {
                resolved_x = MIN(resolved_x, platform->rect.center_x - platform->rect.half_w - box->half_w);
                flags.hit_right = true;
            } else {
                resolved_x = MAX(resolved_x, platform->rect.center_x + platform->rect.half_w + box->half_w);
                flags.hit_left = true;
            }
        }

        box->center_x = resolved_x;
        if (flags.hit_left || flags.hit_right) {
            velocity->x = 0.0f;
        }
    }

    // ---- Y 轴：上升只挡实体，下落时单向平台从上方阻挡 ----
    if (velocity->y != 0.0f) {
        // 移动前的底边位置，用于判定单向平台是否「在脚下」
        f32 bottom_before = box->center_y - box->half_h;

        Rect2D test = *box;
        test.center_y += velocity->y * dt;
        // 水平方向微量内缩：与侧面相切不算竖直碰撞
        test.half_w -= COLLISION_EPSILON;

        f32 resolved_y = test.center_y;
        for (u32 i = 0; i < platform_count; ++i) {
            const Platform *platform = &platforms[i];

            Rect2D probe = test;
            probe.center_y = resolved_y;
            if (!test_rect_overlap(&probe, &platform->rect)) {
                continue;
            }

            if (velocity->y > 0.0f) {
                if (platform->kind != COLLIDER_SOLID) {
                    continue;
                }
                resolved_y = MIN(resolved_y, platform->rect.center_y - platform->rect.half_h - box->half_h);
                flags.hit_top = true;
            } else {
                if (!platform_supports_from_above(platform, bottom_before, ignore_one_way)) {
                    continue;
                }
                resolved_y = MAX(resolved_y, platform->rect.center_y + platform->rect.half_h + box->half_h);
                flags.hit_bottom = true;
            }
        }

        box->center_y = resolved_y;
        if (flags.hit_top || flags.hit_bottom) {
            velocity->y = 0.0f;
        }
    }

    return flags;
}

const Platform *probe_ground_platform(const Rect2D *box, const Platform *platforms,
                                      u32 platform_count, bool ignore_one_way)
{
    // 把碰撞盒下移一个探针深度再判定：只要脚下有支撑就会与平台重叠
    Rect2D probe = *box;
    probe.center_y -= GROUND_PROBE_DEPTH;

    f32 bottom_before = box->center_y - box->half_h;
    for (u32 i = 0; i < platform_count; ++i) {
        const Platform *platform = &platforms[i];
        if (!test_rect_overlap(&probe, &platform->rect)) {
            continue;
        }
        if (!platform_supports_from_above(platform, bottom_before, ignore_one_way)) {
            continue;
        }
        return platform;
    }

    return nullptr;
}
