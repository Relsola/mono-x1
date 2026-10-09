#include "debug/debug_vis.h"
#include "shared/logger.h"

#if MONO_DEBUG_VIS

// 每个标签一份缓冲：静态体与动态体分开存，取用时把两者拼在同一个数组里。
// 容量写死就够了：超出说明有实体没被回收（或关卡碰撞体本来就超过容量），
// 多余的会被丢弃而不是越界 —— 但丢弃必须留痕，见 debug_box_overflow_warn。
constexpr u32 DEBUG_BOX_CAPACITY = 256;

// 溢出只可能来自「动态体没被清」或「关卡碰撞体太多」。丢一个盒子的表现是
// 线框莫名少一块，很难往容量上想，所以要报一次警告；
// 只报一次是因为 LOG_WARN 会立即落盘，每逻辑步都报会把日志刷爆。
internal void debug_box_overflow_warn()
{
    local_persist bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN("debug_vis: box capacity %u exceeded, extra boxes dropped", DEBUG_BOX_CAPACITY);
    }
}

struct DebugBoxList
{
    Rect2D boxes[DEBUG_BOX_CAPACITY];
    u32 count;
};

// 跨帧保留（关卡碰撞体）
global_variable DebugBoxList global_static_boxes[DEBUG_BOX_COUNT];
// 每逻辑步重建（玩家、探针等动态体）
global_variable DebugBoxList global_step_boxes[DEBUG_BOX_COUNT];
// 取用时静态 + 动态合并到这里
global_variable DebugBoxList global_merged_boxes[DEBUG_BOX_COUNT];

internal void debug_box_push(DebugBoxList *list, const Rect2D *box)
{
    if (list->count >= DEBUG_BOX_CAPACITY) {
        debug_box_overflow_warn();
        return;
    }
    list->boxes[list->count++] = *box;
}

void debug_vis_begin_step()
{
    for (u32 i = 0; i < DEBUG_BOX_COUNT; ++i) {
        global_step_boxes[i].count = 0;
    }
}

void debug_vis_clear_static()
{
    for (u32 i = 0; i < DEBUG_BOX_COUNT; ++i) {
        global_static_boxes[i].count = 0;
    }
}

void debug_vis_static_box(const Rect2D *box, DebugBoxTag tag)
{
    debug_box_push(&global_static_boxes[tag], box);
}

void debug_vis_box(const Rect2D *box, DebugBoxTag tag)
{
    debug_box_push(&global_step_boxes[tag], box);
}

const Rect2D *debug_vis_boxes(DebugBoxTag tag, u32 *count)
{
    DebugBoxList *merged = &global_merged_boxes[tag];
    const DebugBoxList *statics = &global_static_boxes[tag];
    const DebugBoxList *steps = &global_step_boxes[tag];

    // 两个列表各自都没满、合起来也可能超过容量（静态体先占位，动态体只能被截掉）
    if (statics->count + steps->count > DEBUG_BOX_CAPACITY) {
        debug_box_overflow_warn();
    }

    u32 used = 0;
    for (u32 i = 0; i < statics->count && used < DEBUG_BOX_CAPACITY; ++i) {
        merged->boxes[used++] = statics->boxes[i];
    }
    for (u32 i = 0; i < steps->count && used < DEBUG_BOX_CAPACITY; ++i) {
        merged->boxes[used++] = steps->boxes[i];
    }

    merged->count = used;
    *count = used;
    return merged->boxes;
}

#endif // MONO_DEBUG_VIS
