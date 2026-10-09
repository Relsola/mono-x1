#pragma once

// 本文件只包含**它自己的定义**需要的系统头：类型别名要 <stdint.h>、safe_cast_u64 要 assert（<assert.h>）。
// 其余系统头（<string.h> / <stdio.h> / <stdarg.h> …）由**用它的人**自己包含 —— 否则往任意一个 .cc 里
// 加一个轻量标准库头，都会顺着头文件传染给全部编译单元。想往这里加头时先问一句：
// 「core.h 自己的定义用得到它吗」。
// 与数学有关的东西（v2 / MIN / MAX / clamp / lerp / <math.h>）住在 shared/mono_math.h，不在这里。
#include <assert.h>
#include <stdint.h>

#define internal        static
#define local_persist   static
#define global_variable static

// 「是否有任意调试功能被编译进来」。像 LOG_DEBUG、窗口置顶这类“跟着调试走”的开关用它，
// 而不要绑到某一个具体模块的宏上 —— 那会在“只开另一个模块”的配置下静默失效。
// 新增调试宏时，记得把它加进这一行。
#if MONO_DEBUG_TMP || MONO_DEBUG_VIS || MONO_DEBUG_BUILD || MONO_DEBUG_INPUT
#define MONO_DEBUG_ANY 1
#else
#define MONO_DEBUG_ANY 0
#endif

using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using f32 = float;
using f64 = double;

consteval u64 KB(u64 n) { return n << 10; }
consteval u64 MB(u64 n) { return KB(n) << 10; }
consteval u64 GB(u64 n) { return MB(n) << 10; }

// FNV-1a 32 位哈希。两个使用者共用同一套参数：存档的完整性校验和（src/save.cc）
// 与关卡几何指纹（src/game.cc）—— 参数散落两处时，改了一处就再也比不上旧值。
inline constexpr u32 FNV1A_OFFSET_BASIS = 2166136261u;
inline constexpr u32 FNV1A_PRIME = 16777619u;

inline u32 fnv1a_32(u32 hash, const void *data, u64 size)
{
    const u8 *bytes = (const u8 *)data;
    for (u64 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

// 按值哈希标量（float 也走它：位模式是确定的，而结构体的填充字节不是）——
// 逐字段哈希时用它，避免为了省事去哈希一整块结构体内存。
template <typename T>
inline u32 fnv1a_value(u32 hash, T value)
{
    return fnv1a_32(hash, &value, sizeof(value));
}

// 安全截断转换
constexpr inline u32 safe_cast_u64(u64 value)
{
    assert(value <= 0XFFFFFFFF);
    return (u32)value;
}

template <typename T, int N>
consteval int array_size(T (&)[N]) { return N; }

// ============================================================================
// 2D 碰撞系统 (AABB: Axis-Aligned Bounding Box)
// ============================================================================

// 角色碰撞箱（尺寸比例与脚底对齐方式见 game.cc 的 PLAYER_COLLIDER_* 常量）
struct PlayerCollider
{
    f32 width;
    f32 height;
    f32 offset_x;
    f32 offset_y;
};

// 固定逻辑步长 60Hz，所有游戏逻辑（移动/碰撞等）都以固定的 dt 推进，
inline constexpr f32 FIXED_TIMESTEP = 1.0f / 60.0f;
// 最大累积 15 步，防止死亡螺旋
inline constexpr f32 MAX_ACCUMULATOR = FIXED_TIMESTEP * 15;

// ============================================================================
// defer：离开作用域时执行的清理块（RAII 的最小形态）
// ============================================================================

// 用法：就地把 `{}` 写成一个 lambda 体，离开作用域时**按登记的逆序**执行。
//
//     input_init(hwnd, preferred);
//     defer { input_shutdown(); };
//
// 它用在成对资源（create / destroy）上：拿到资源后立刻登记一句，于是「每条失败路径都要
// 记得清理」这个矩阵就不存在了 —— 函数里只需要 `return`。
//
// 三条限制：
//   1. 清理块里不能失败、也不能上报错误（这个工程没有异常），错误只能自己吞掉；
//      要传错误信息就靠返回值 —— 它只负责「把资源还回去」。
//   2. 它**不能取消**（同 Go 的 defer）。需要「成功时不清理」就带一个标志位：
//          bool ok = false;
//          defer { if (!ok) { discard(x); } };
//          ...
//          ok = true; return x;
//      不要把「所有权的交接」藏进清理块里。
//   3. 名字带行号，所以**同一行只能写一个 defer**（拆成两行就行）。
//
// 展开成 `Defer _defer_<行号> = [&] { ... };`：`{}` 就是那个 lambda 的 body。
template <typename F>
struct Defer
{
    F fn;
    Defer(F f) : fn(f) {}
    ~Defer() { fn(); }
};

#define MONO_CONCAT_(a, b) a##b
#define MONO_CONCAT(a, b) MONO_CONCAT_(a, b)
#define defer Defer MONO_CONCAT(_defer_, __LINE__) = [&]
