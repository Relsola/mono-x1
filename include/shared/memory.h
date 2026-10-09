#pragma once

#include "core.h"

// ============================================================================
// 内存：线性分配器
//
//   持久分配（arena_*）  —— 整个程序生命周期，从这里分配出去的不回收
//   临时分配（scratch_*）—— 用完 scratch_reset 一把清掉，用于解码贴图这类中间数据
//
// 为什么 Array<T> 也放在这里：它的内存直接取自 arena（init 就是 arena_push），
// 两者拆开会多出一层没有任何意义的依赖。
//
// 它在 include/shared/ 里：编辑器也用它（撤销环的快照缓冲就在 arena 上），所以它只能依赖 core.h。
// 实现见 src/platform_win32.cc（后备存储是 VirtualAlloc，属于平台层）。
// 依赖方向：shared/memory.h -> core.h
// ============================================================================

// 初始化持久分配器（整个程序生命周期只调一次）；失败返回 false
bool arena_init(u64 size);

// 持久线性分配器：8 字节对齐，只增不减
void *arena_push(u64 size);
void *arena_realloc(void *p, u64 oldsz, u64 newsz);

// 临时线性分配
struct ScratchArena
{
    u8 *base;
    u64 size;
    u64 used;
};

// 全局临时分配器（贴图解码一类的中间数据）
inline ScratchArena global_scratch = {};

void scratch_init(ScratchArena *arena, u64 size);
void scratch_shutdown(ScratchArena *arena);
void scratch_reset(ScratchArena *arena);
void *scratch_push(ScratchArena *arena, u64 size);
void *scratch_realloc(ScratchArena *arena, void *p, u64 oldsz, u64 newsz);

// ============================================================================
// 动态数组（内存来自 arena，会增长、但不会单独释放）
// ============================================================================

template <typename T>
struct Array
{
    T *data = nullptr;
    u32 size = 0;
    u32 cap = 16;
};

template <typename T>
Array<T> init(u32 cap)
{
    Array<T> result = {};
    result.cap = cap;
    result.data = (T *)arena_push(sizeof(T) * cap);
    return result;
}

// 确保容量并返回下一个可写槽位
template <typename T>
T *array_push_slot(Array<T> *arr)
{
    if (arr->data == nullptr) {
        arr->data = (T *)arena_push(sizeof(T) * arr->cap);
    }

    if (arr->cap == arr->size) {
        arr->cap *= 2;
        arr->data = (T *)arena_realloc(arr->data, sizeof(T) * arr->size, sizeof(T) * arr->cap);
    }

    return &arr->data[arr->size++];
}

template <typename T>
void inline array_push(Array<T> *arr, T item)
{
    *array_push_slot(arr) = item;
}
