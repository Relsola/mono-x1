#pragma once

#include "core.h"
#include "shared/mono_math.h"

// ============================================================================
// 2D 碰撞系统 (AABB: Axis-Aligned Bounding Box)
//
// 这里只有「几何原语 + 平台的碰撞语义 + 两个求解函数」：
//   make_rect_center / test_rect_overlap  几何
//   Platform / ColliderKind               关卡里的静态碰撞体
//   move_and_collide / probe_ground_platform  求解
//
// 不认识「角色」「关卡」这些游戏概念 —— 那些在 game.h。
// 它在 include/shared/ 里：编辑器也编译 collision.cc（关卡编译与诊断都要这套几何），
// 所以它不能反过来依赖任何游戏侧的头。
// 实现见 src/collision.cc，依赖方向：shared/collision.h -> core.h + shared/mono_math.h（基础类型 + v2）
// ============================================================================

// 2D 轴对齐矩形碰撞盒（以中心点坐标 + 宽高定义）
struct Rect2D
{
    f32 center_x;
    f32 center_y;
    f32 half_w; // 半宽（中心到左右边缘距离）
    f32 half_h; // 半高（中心到上下边缘距离）
};

// 辅助创建 Rect2D (输入中心坐标与总宽高)
Rect2D make_rect_center(f32 center_x, f32 center_y, f32 width, f32 height);

// 检测两个 AABB 矩形是否发生重叠相交 (Separating Axis Theorem 分离轴定理 2D 特例)
bool test_rect_overlap(const Rect2D *a, const Rect2D *b);

// 平台的碰撞语义
enum ColliderKind : u8
{
    COLLIDER_SOLID,   // 实体：上下左右都阻挡
    COLLIDER_ONE_WAY, // 单向：上方可站、下方可跳穿；侧面只在「从侧面撞上去」时阻挡
};

// 关卡中的一块静态平台
struct Platform
{
    Rect2D rect;
    ColliderKind kind;
};

// 轴分离移动的结果。注意：当前调用方不消费它（地面判定交给探针，见下），
// 保留是为了后续做撞头/落地音效与特效时不必再改函数签名。
struct CollisionFlags
{
    bool hit_left;
    bool hit_right;
    bool hit_top;    // 向上撞头
    bool hit_bottom; // 向下着地
};

// 地面探针深度：把碰撞盒下移这么多像素再判定，能恰好在相切时也检测到支撑
inline constexpr f32 GROUND_PROBE_DEPTH = 1.0f;

// 轴分离移动：按 velocity * dt 移动碰撞盒并与平台求解贴合，被挡住的轴速度清零。
// ignore_one_way 为 true 时忽略单向平台（主动下穿期间）
CollisionFlags move_and_collide(Rect2D *box, v2 *velocity, f32 dt,
                                const Platform *platforms, u32 platform_count, bool ignore_one_way);

// 地面探针：把碰撞盒下移 GROUND_PROBE_DEPTH 像素再判定重叠，
// 返回支撑它的平台（没有则返回 nullptr）。
//
// 为什么不用 move_and_collide 的 hit_bottom 判定「站在地上」：
// 那个标志依赖「这一步刚好产生了向下的位移」，只要某一步 vy 为 0 就会整段跳过 Y 轴求解，
// 角色明明贴地却被判成空中。探针是纯位置查询，与速度、与浮点误差累积都无关，
// 而且返回平台指针后还能知道脚下是哪一块（下+跳下穿单向平台要用到这个信息）。
const Platform *probe_ground_platform(const Rect2D *box, const Platform *platforms,
                                      u32 platform_count, bool ignore_one_way);
