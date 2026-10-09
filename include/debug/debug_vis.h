#pragma once

#include "shared/collision.h"

// ============================================================================
// 调试可视化收集器
//
// 各子系统只管「上报」需要可视化的数据（碰撞盒等），不关心怎么画；
// 装配层（main.cc）每帧把收集到的图元翻译成渲染层调用。
//
// 依赖方向：game -> debug_vis <- main -> renderer
// 三方互不认识：游戏层不认识渲染层，渲染层也不认识碰撞语义。
//
// MONO_DEBUG_VIS 关闭时所有入口是空宏，运行期零成本。
// ============================================================================

// 标签同时承担两件事：语义分类 + 装配层的调色板索引。
// 颜色表不在这个模块里（在 src/scene.cc 的 DEBUG_BOX_COLORS），新增标签必须同步给那边加一行
// —— 那边有 static_assert 兜底，漏了是编译错误。
enum DebugBoxTag : u8
{
    DEBUG_BOX_STATIC_SOLID,    // 关卡实体砖（静态：跨步保留）
    DEBUG_BOX_STATIC_ONE_WAY,  // 关卡单向平台（静态）
    DEBUG_BOX_SPIKE,           // 地刺（动态关卡数据）
    DEBUG_BOX_PORTAL,          // 传送门矩形（静态）
    DEBUG_BOX_MOVING_PLATFORM, // 移动组件（动态，含圆形）
    DEBUG_BOX_MONSTER,         // 怪物伤害盒（动态）
    DEBUG_BOX_PROJECTILE,      // 能量波碰撞盒（动态）
    DEBUG_BOX_PLAYER,          // 玩家物理碰撞盒（动态：每逻辑步上报）
    DEBUG_BOX_GROUND_PROBE,    // 地面判定探针（动态）
    DEBUG_BOX_COUNT
};

#if MONO_DEBUG_VIS

// 逻辑步开始：清空本步上报的临时图元（玩家盒、探针等）。
// 静态体**不在这里清**：它们只在关卡加载时登记一次，之后跨步保留。
void debug_vis_begin_step();

// 清空关卡切换前登记的静态体；切换后由新关卡重新登记。
void debug_vis_clear_static();

// 登记一个长期存在的碰撞盒，只增不减（关卡加载时调用一次）。
// 关卡切换/重载前调用 debug_vis_clear_static 清空，再登记新关卡。
void debug_vis_static_box(const Rect2D *box, DebugBoxTag tag);
// 登记一个只在本逻辑步有效的碰撞盒（每逻辑步都要重新上报）
void debug_vis_box(const Rect2D *box, DebugBoxTag tag);

// 取某个标签本步全部碰撞盒（静态 + 动态合并后）。
// 返回的指针指向模块内部的合并缓冲，**只在下一次 debug_vis_boxes 调用之前有效**：
// 立刻用完，不要存起来跨帧用。
const Rect2D *debug_vis_boxes(DebugBoxTag tag, u32 *count);

#else

// 三个写入口关闭时退化成空宏；debug_vis_boxes 没有空宏版本 —— 它是只读接口，
// 唯一的调用点在装配层的 #if MONO_DEBUG_VIS 里，别在别处不加 #if 地用它。
#define debug_vis_begin_step()         ((void)0)
#define debug_vis_clear_static()       ((void)0)
#define debug_vis_static_box(box, tag) ((void)0)
#define debug_vis_box(box, tag)        ((void)0)

#endif
