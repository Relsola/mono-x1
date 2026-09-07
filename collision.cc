#include "core.h"

Rect2D make_rect_center(f32 center_x, f32 center_y, f32 width, f32 height)
{
    Rect2D r = {};
    r.center_x = center_x;
    r.center_y = center_y;
    r.half_w = width * 0.5f;
    r.half_h = height * 0.5f;
    return r;
}

// 检测两个 AABB 矩形是否发生重叠相交 (Separating Axis Theorem 分离轴定理 2D 特例)
bool test_rect_overlap(const Rect2D *a, const Rect2D *b)
{
    // 在 X 轴和 Y 轴上同时发生重叠，才判定为发生碰撞
    bool overlap_x = fabsf(a->center_x - b->center_x) < (a->half_w + b->half_w);
    bool overlap_y = fabsf(a->center_y - b->center_y) < (a->half_h + b->half_h);
    return overlap_x && overlap_y;
}
