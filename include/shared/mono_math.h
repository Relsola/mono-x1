#pragma once

#include "core.h"

// ============================================================================
// mono_math：本工程**唯一**的数学层
//
// 三条约定：
//   1. **数学相关的东西只从这里来**。要 sqrtf / fabsf / expf 就直接用（<math.h> 已经在本文件里
//      包含过了），**不要再单独包含 <math.h>**，也不要往 core.h 里加数学头。
//   2. 标量工具（MIN / MAX / clamp / lerp）与二维向量（v2）都住在这里，
//      不再散在 core.h —— core.h 只管类型别名与哈希那种与数学无关的基础件。
//   3. 它在 include/shared/ 是因为**编辑器也要用它**：v2 出现在共享的关卡资产结构里
//      （`LevelMoverAsset::point_a` / `point_b`），所以它必须留在「游戏与编辑器共用」那一层。
//
// 依赖方向：shared/mono_math.h -> core.h（基础类型别名）
// ============================================================================

#include <math.h>

inline constexpr f32 PI = 3.14159265358979323846f;
// 二维对角方向归一化分量
inline constexpr f32 INV_SQRT_2 = 0.70710678f;

template <typename T>
constexpr inline T MAX(T a, T b) { return a > b ? a : b; }

template <typename T>
constexpr inline T MIN(T a, T b) { return a < b ? a : b; }

// 夹取到 [lo, hi]：代替散在各处的 MIN(MAX(v, lo), hi)（那个写法读起来要反着想一遍）
template <typename T>
constexpr inline T clamp(T value, T lo, T hi)
{
    return (value < lo) ? lo : ((value > hi) ? hi : value);
}

// 简单的插值计算
template <typename T>
constexpr inline T lerp(T a, T b, f32 t) { return a + (b - a) * t; }

// ============================================================================
// 2D 向量计算
// ============================================================================

struct v2
{
    f32 x, y;

    v2 operator+(v2 rhs) const { return { x + rhs.x, y + rhs.y }; }
    v2 operator-(v2 rhs) const { return { x - rhs.x, y - rhs.y }; }
    v2 operator*(f32 scalar) const { return { x * scalar, y * scalar }; }

    v2 &operator+=(v2 rhs)
    {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }

    v2 &operator*=(f32 value)
    {
        x *= value;
        y *= value;
        return *this;
    }

    f32 length_sq() const { return x * x + y * y; }
    f32 length() const { return sqrtf(length_sq()); }

    // 零向量归一化时返回零向量，避免除零
    v2 normalized() const
    {
        f32 len = length();
        return (len > 0.0f) ? v2{ x / len, y / len } : v2{ 0.0f, 0.0f };
    }
};
