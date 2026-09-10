#pragma once

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define internal        static
#define local_persist   static
#define global_variable static

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float    f32;
typedef double   f64;

template <typename T>
constexpr inline T MAX(T a, T b) { return a > b ? a : b; }

template <typename T>
constexpr inline T MIN(T a, T b) { return a < b ? a : b; }

// 简单的插值计算
template <typename T>
constexpr inline T lerp(T a, T b, f32 t) { return a + (b - a) * t; }

consteval u64 KB(u64 n) { return n << 10; }
consteval u64 MB(u64 n) { return KB(n) << 10; }
consteval u64 GB(u64 n) { return MB(n) << 10; }

// 安全截断转换
constexpr inline u32 safe_cast_u64(u64 value)
{
    assert(value <= 0XFFFFFFFF);
    return (u32)value;
}

template <typename T, u64 N>
char (*ptr_array(T (&)[N]))[N];
#define ARRAY_SIZE(A) (sizeof(*ptr_array(A)))

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

// ============================================================================
// Arena 和 IO
// ============================================================================

// 持久的线性分配器
void *arena_push(u64 size);
void *arena_realloc(void *p, u64 oldsz, u64 newsz);

struct ReadFileRes
{
    u32 file_size;
    void *contents;
};

ReadFileRes read_file(const wchar_t *filename);
bool write_file(const wchar_t *filename, u32 size, void *memory, bool append = false);
void free_file_memory(void *memory);

// 临时线性分配
struct ScratchArena
{
    u8 *base;
    u64 size;
    u64 used;
};

// 全局临时线性分配器
inline ScratchArena global_scratch = {};

void scratch_init(ScratchArena *arena, u64 size);
void scratch_shutdown(ScratchArena *arena);
void scratch_reset(ScratchArena *arena);
void *scratch_push(ScratchArena *arena, u64 size);
void *scratch_realloc(ScratchArena *arena, void *p, u64 oldsz, u64 newsz);

// ============================================================================
// 动态数组
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

// ============================================================================
// 游戏输入
// ============================================================================

enum GameAction : u8
{
    GA_LEFT,  // 左移动
    GA_RIGHT, // 右移动
    GA_UP,    // 上移动
    GA_DOWN,  // 下移动
    GA_Q,     // 放大
    GA_E,     // 缩小
    GA_SPACE, // 跳跃
    GA_COUNT  // 动作总数
};

enum MouseButton : u8
{
    MOUSE_LEFT,   // 左键
    MOUSE_MIDDLE, // 中键
    MOUSE_RIGHT,  // 右键
    MOUSE_BUTTON_COUNT
};

struct MouseInput
{
    bool current[MOUSE_BUTTON_COUNT];  // 当前帧状态
    bool previous[MOUSE_BUTTON_COUNT]; // 上一帧状态
    bool pressed[MOUSE_BUTTON_COUNT];  // 本帧刚按下（上升沿）
    bool released[MOUSE_BUTTON_COUNT]; // 本帧刚松开（下降沿）

    f32 x;
    f32 y;
    f32 wheel_delta; // 本帧滚轮增量
};

struct PlayerInput
{
    bool is_pad; // 本帧是否有手柄参与

    // TODO 由于使用累加器，在需要精确跳跃等情况时可能需要在 main 的固定步循环里消费
    bool current[GA_COUNT];  // 当前帧状态
    bool previous[GA_COUNT]; // 上一帧状态
    bool pressed[GA_COUNT];  // 本帧刚按下（上升沿）
    bool released[GA_COUNT]; // 本帧刚松开（下降沿）

    // 左右摇杆在水平和垂直方向上的位置 [-1.0, 1.0]
    f32 left_stick_x;
    f32 left_stick_y;
    f32 right_stick_x;
    f32 right_stick_y;
    // 扳机键 [0.0, 1.0]
    f32 left_trigger;
    f32 right_trigger;
};

struct GameInput
{
    PlayerInput player; // 玩家输入（键盘 + 手柄）
    MouseInput mouse;   // 全局鼠标输入
};

// ============================================================================
// 2D 碰撞系统 (AABB: Axis-Aligned Bounding Box)
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

// ============================================================================
// 游戏资源与状态
// ============================================================================

// 由 arena 分配，一张已解码的精灵图
struct SpriteImage
{
    u8 *pixels; // RGBA 像素数据，自顶向下、按行连续排列，每像素 4 字节
    i32 width;
    i32 height;
    void *view; // 纹理资源指针

    // 相对于世界坐标
    f32 world_x = 0.0;
    f32 world_y = 0.0;

    // 缩放
    f32 scale = 1.0f;
};

// 动画帧
struct AnimationFrame
{
    SpriteImage image;
    f32 duration;
};

// 精灵图完整动画
struct SpriteAnimation
{
    AnimationFrame *frames;
    u32 frame_count;
    u32 current_frame;
    f32 elapsed;
    bool looping;
    bool finished;
};

// 角色碰撞箱
struct PlayerCollider
{
    f32 width;
    f32 height;
    f32 offset_x;
    f32 offset_y;
};

// 角色当前帧动画
AnimationFrame *get_current_animation(SpriteAnimation *animation);

// 2D 摄像机：记录当前视口在游戏世界中的中心点与缩放级别
struct Camera2D
{
    f32 pos_x;       // 世界坐标 X（像素单位）
    f32 pos_y;       // 世界坐标 Y（像素单位）
    f32 zoom = 1.0f; // 视野缩放（1.0 = 原比例，>1.0 放大，<1.0 缩小拉远）
};

// 固定逻辑步长 60Hz，所有游戏逻辑（移动/碰撞等）都以固定的 dt 推进，
inline constexpr f32 FIXED_TIMESTEP = 1.0f / 60.0f;
// 最大累积 15 步，防止死亡螺旋
inline constexpr f32 MAX_ACCUMULATOR = FIXED_TIMESTEP * 15;

// 游戏状态
struct GameState
{
    // 玩家相对世界的位移 像素偏移而不是不是归一化的值
    f32 player_x = 0.0f;
    f32 player_y = 0.0f;
    // 上一逻辑步的位置，用于渲染插值（消除高刷屏抖动）
    f32 prev_player_x = 0.0f;
    f32 prev_player_y = 0.0f;

    v2 velocity = {};

    Camera2D camera = {};
    Camera2D prev_camera = {}; // 上一逻辑步的摄像机，用于渲染插值

    SpriteImage backdrop;
    SpriteAnimation player_bagdown_animation = {};
    PlayerCollider player_collider;

    // 模拟的墙壁
    Rect2D wall_colliders[11] = {
        // 外围边界墙 (构建世界边界感)
        make_rect_center(0.0f, 1050.0f, 2600.0f, 48.0f),  // 北围墙
        make_rect_center(0.0f, -1050.0f, 2600.0f, 48.0f), // 南围墙
        make_rect_center(-1300.0f, 0.0f, 48.0f, 2100.0f), // 西围墙
        make_rect_center(1300.0f, 0.0f, 48.0f, 2100.0f),  // 东围墙
        // 中心 4 根地标立柱
        make_rect_center(-350.0f, 250.0f, 100.0f, 100.0f),
        make_rect_center(350.0f, 250.0f, 100.0f, 100.0f),
        make_rect_center(-350.0f, -250.0f, 100.0f, 100.0f),
        make_rect_center(350.0f, -250.0f, 100.0f, 100.0f),
        // 走廊与房舍隔断
        make_rect_center(0.0f, 500.0f, 400.0f, 48.0f),
        make_rect_center(-650.0f, 0.0f, 48.0f, 450.0f),
        make_rect_center(650.0f, 0.0f, 48.0f, 450.0f),
    };
};

void game_init_asset(GameState *game_state);

void game_update(GameInput *game_input, GameState *game_state, f32 dt);
