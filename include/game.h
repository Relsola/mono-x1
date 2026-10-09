#pragma once

#include "core.h"
#include "input.h"
#include "camera.h"
#include "sprite.h"
#include "shared/collision.h"
#include "shared/mono_math.h"
#include "ui.h"
#include "shared/level_asset.h"
// Level / WorldId / PlayerFacing / level_build_from_asset 在 level.h：编辑器也要用那一层，
// 所以它不能挂在 game.h 上（挂了就得把 input/camera/sprite/ui 一起拖进编辑器）
#include "shared/level.h"

// ============================================================================
// 游戏层：游戏状态、角色运动与场景实体
// ============================================================================

// 角色动作状态
enum PlayerState : u8
{
    PSTATE_IDLE, // 站立
    PSTATE_RUN,  // 跑动
    PSTATE_JUMP, // 上升
    PSTATE_FALL, // 下落
    PSTATE_DASH, // 冲刺
    PSTATE_COUNT
};

// 大地图必须**列得下所有世界的传送点**：列表容量与「每图上限 × 世界数」钉在一起。
// 不够时是这两个数字要一起调的事，不是运行时静默截断列表。
static_assert(LEVEL_ASSET_MAX_WAYPOINTS * LEVEL_ASSET_WORLD_COUNT <= UI_MAX_ITEMS,
              "传送点总量超过 UI_MAX_ITEMS：要么调小 LEVEL_ASSET_MAX_WAYPOINTS，要么调大 UI_MAX_ITEMS");

// 移动组件（资产层是 `LevelMoverAsset`）：**形状** + 一条轴（两点之间）+ 沿轴速度。
// 形状决定行为（资产层的注释是权威版）：
//   * `SQUARE`：实心、能站、**驮着走**（两个轴都驮）；`damaging` 打开时「被它挤住 / 压住」会重生。
//   * `CIRCLE`：实心（暂时不可穿过）、**不驮人**，恒定伤害 —— 只要碰到就重生。
// rect 是碰撞与渲染共用的当前几何（每步从轴上的位置算出来，见 update_movers）；
// t 是轴上的参数（从资产的 t0 出发，到端点折返），direction = +1 表示朝 point_b 走。
// t / direction / rect 是跨帧会变的，所以进快照。
struct Mover
{
    bool time_slowed;
    bool damaging;
    u32 shape; // LevelMoverShape
    Rect2D rect;
    v2 point_a;
    v2 point_b;
    f32 t;
    f32 direction;
    f32 half_w;
    f32 half_h;
    f32 speed;
};

// 一个世界的全部移动组件：定长数组 + 数量。
// 用「数组 + count」而不是逐个命名，是因为它跟快照走：`X(MoverSet, movers)` 一行
// 就把这一整块（含数量）纳入 save/load，不必改三个地方。
struct MoverSet
{
    u32 count;
    Mover items[LEVEL_ASSET_MAX_MOVERS];
};

// 最小怪物：恒定水平速度加重力，掉出世界后回到出生位置；不含任何追踪或决策。
struct Monster
{
    bool active;
    bool time_slowed;
    u32 respawn_count;
    Rect2D rect;
    f32 spawn_x;
    f32 spawn_y;
    f32 spawn_velocity_x;
    v2 velocity;
};

// 可消失平台（tile 'V'）的运行态。几何是**每行相邻的格合并出来的一块**（见 level.cc），
// 存在 Level 里；这里只存「这块现在处于哪个相位、还剩多久」。
// tile 没有身份，所以「哪一块」就用块下标 —— 顺序由 level.cc 的扫描顺序决定，稳定且确定，
// 于是它也不需要进关卡资产：改 tile 布局 = 改指纹（存档会拒载），块下标则跟着布局走。
enum VanishPhase : u8
{
    VANISH_PHASE_SOLID,   // 实体：正常贴图、参与碰撞
    VANISH_PHASE_WARNING, // 已经有人踩上，正在倒计时（变暗预告）
    VANISH_PHASE_GONE,    // 消失中：半透明、**不进碰撞列表**
    VANISH_PHASE_COUNT,
};

struct VanishBlock
{
    VanishPhase phase = VANISH_PHASE_SOLID;
    // 已经连续踩住多少帧（离开立刻清零）。用**整数帧**而不是浮点计时器：它只数固定步，
    // 所以「踩 9 帧不触发、踩 10 帧触发」可以精确写成断言；阈值见 game.cc 的 VANISH_HOLD_FRAMES。
    u16 hold_frames = 0;
    f32 timer = 0.0f; // 警告 / 消失的剩余秒数
};

// 整块状态打包成一个成员，才能作为一行进 GAME_STATE_PERSISTENT_FIELDS（快照要能整体序列化）
struct VanishState
{
    VanishBlock blocks[MAX_VANISH_BLOCKS] = {};
};

// 传送门的三段式相位机：淡出（在起点门上）→ 加载（已经落在目标门上）→ 淡入。
// 期间整步只推进相位：不吃输入、也不走物理。它刻意**不复用**穿门过渡那套（world_transition
// 是“顶着输入锁自动走入另一个世界”），因为两者的观感、时长与落点规则都不一样。
enum TeleportPhase : u8
{
    TELEPORT_NONE,
    TELEPORT_FADE_OUT,
    TELEPORT_LOADING,
    TELEPORT_FADE_IN,
    TELEPORT_PHASE_COUNT,
};

struct Teleport
{
    TeleportPhase phase = TELEPORT_NONE;
    f32 timer = 0.0f;    // 当前阶段剩余秒数
    u32 dest_portal = 0; // 目标门在**当前世界**的下标（暂不支持跨世界）

    // 触发是**显式按键**（GA_UP）：角色中心站在门矩形里时按上键才传送。
    // 因为是一次明确的按键（边沿只来一次、按住不重复），所以不需要“刚用过就抑制”那一套。
};

// 主角发射的一枚能量波；rect 同时用于碰撞与色块渲染。
struct Projectile
{
    bool active;
    Rect2D rect;
    f32 velocity_x;
};

inline constexpr u32 MAX_PROJECTILES = 8;

// 固定容量投射物池：cooldown 属于发射能力，不属于某一枚飞行中的能量波。
struct ProjectilePool
{
    Projectile items[MAX_PROJECTILES];
    f32 cooldown;
};

// 发动点固定的圆形伪时停范围。active 期间可减速接触范围的活动物体；
// cooldown 从发动瞬间开始计，避免能力结束后额外再等待完整 5 秒。
struct TimeStop
{
    bool active;
    f32 center_x;
    f32 center_y;
    f32 radius;
    f32 timer;
    f32 cooldown;
};

// 游戏状态
struct GameState
{
    // 玩家中心位置（世界像素坐标，y 轴向上）
    f32 player_x = 0.0f;
    f32 player_y = 0.0f;
    // 上一逻辑步的位置，用于渲染插值（消除高刷屏抖动）
    f32 prev_player_x = 0.0f;
    f32 prev_player_y = 0.0f;

    v2 velocity = {};        // 速度（像素 / 秒）
    bool grounded = false;   // 本逻辑步结束时是否有支撑

    // 朝向与动作状态
    PlayerFacing facing = FACE_RIGHT;
    PlayerState state = PSTATE_FALL;
    f32 dash_timer = 0.0f;          // 冲刺剩余时间
    f32 dash_cooldown = 0.0f;       // 冲刺冷却剩余时间
    f32 dash_dir_x = 1.0f;          // 冲刺方向（水平，启动瞬间锁定）
    f32 coyote_timer = 0.0f;        // 土狼时间剩余
    f32 jump_buffer_timer = 0.0f;   // 跳跃缓冲剩余
    f32 drop_through_timer = 0.0f;  // 主动下穿单向平台后的忽略剩余时间
    u32 air_jumps_left = 0;         // 二段跳剩余次数（落地 / 重生时恢复，见 PLAYER_AIR_JUMP_COUNT）

    Camera2D camera = {};
    Camera2D prev_camera = {}; // 上一逻辑步的摄像机，用于渲染插值

    // 所有世界在初始化时就常驻于 arena；level 是当前世界的一份轻量引用副本。
    LevelAsset level_assets[WORLD_COUNT] = {};
    Level worlds[WORLD_COUNT] = {};
    WorldId world = WORLD_FIRST;
    Level level = {};

    MoverSet movers = {};
    MoverSet prev_movers = {}; // 渲染插值用的上一逻辑步几何
    Monster monster = {};
    Monster prev_monster = {};
    VanishState vanish = {}; // 可消失平台的相位/计时器（跨帧影响演化 → 进快照）
    Teleport teleport = {};  // 传送门的三段式相位机（同样跨帧影响演化 → 进快照）
    UiState ui = {};         // 大地图 UI（打开时世界暂停；选中项决定传送到哪个世界 → 进快照）
    ProjectilePool projectiles = {};
    ProjectilePool prev_projectiles = {};
    TimeStop time_stop = {};
    bool world_transition_active = false;
    f32 transition_target_x = 0.0f;
    f32 transition_target_y = 0.0f;

    SpriteImage backdrop;
    SpriteAnimation player_bagdown_animation = {};
    PlayerCollider player_collider;
};

// 把编辑器/磁盘资产编译为运行时关卡：tile 合并为碰撞矩形，实体描述保持独立。
// 声明在 level.h（那里是关卡层）。

// ============================================================================
// 状态快照（存档系统与它的使用者——录制回放、输入脚本——共用同一份定义）
// ============================================================================

// 它是「影响确定性回放的那部分 GameState」。**下面这份列表是唯一来源**：
// 快照结构体、写档、读档都由它生成，所以「结构体加了字段、save/load 忘了抄」这种不对称
// 在结构上不可能发生 —— 想加一个进快照的字段，就在列表里加一行；
// 加漏了它不会进快照（行为同以前），但要同步三处的约定已经不存在了。
//
// 判据（唯一需要人判断的地方）：**跨帧会变、且影响后续演化的量属于状态**。
// 渲染插值的历史位置（prev_*）、动画当前帧、音频播放相位属于「每次运行都会重新长出来」，
// 因此不进列表 —— 这正是「合理重现世界、允许丢一点」的边界，丢的必须是不影响演化的量。
//
// 三道防线（按代价从低到高）：
//   1. 本列表（结构：快照结构体 / save / load 同源）；
//   2. `game_state_snapshot_selftest`（运行期：save→load→save 必须逐位一致，启动时跑一次）；
//   3. 确定性守卫（行为：同一条磁带跑两遍对拍 trace，见 test/run_tests.bat）——
//      前两道只能保证「列进列表的都对」，只有第三道能发现「该进列表却漏了」。
//
// 文件格式（头、校验、关卡指纹）见 include/save.h，不在这里。
// 顺序即快照字段顺序，也就是存档负载的布局。
#define GAME_STATE_PERSISTENT_FIELDS(X) \
    X(f32, player_x)                    \
    X(f32, player_y)                    \
    X(v2, velocity)                     \
    X(bool, grounded)                   \
    X(PlayerFacing, facing)             \
    X(PlayerState, state)               \
    X(f32, dash_timer)                  \
    X(f32, dash_cooldown)               \
    X(f32, dash_dir_x)                  \
    X(f32, coyote_timer)                \
    X(f32, jump_buffer_timer)           \
    X(f32, drop_through_timer)          \
    X(u32, air_jumps_left)              \
    X(Camera2D, camera)                 \
    X(WorldId, world)                   \
    X(MoverSet, movers)                 \
    X(Monster, monster)                 \
    X(VanishState, vanish)              \
    X(Teleport, teleport)               \
    X(ProjectilePool, projectiles)      \
    X(TimeStop, time_stop)              \
    X(bool, world_transition_active)    \
    X(f32, transition_target_x)         \
    X(f32, transition_target_y)         \
    X(UiState, ui)

struct GameStateSnapshot
{
#define GAME_STATE_FIELD_DECL(type, name) type name;
    GAME_STATE_PERSISTENT_FIELDS(GAME_STATE_FIELD_DECL)
#undef GAME_STATE_FIELD_DECL
};

// 存档：先整体清零（连填充字节一起），所以同一个状态编出来的字节永远一致
void game_state_save(const GameState *game_state, GameStateSnapshot *out);
// 校验快照中的枚举与 bool 表示，拒绝不属于当前运行时状态空间的数据。
bool game_state_snapshot_valid(const GameStateSnapshot *snapshot);
// 读档：先校验快照；成功时把渲染插值的历史位置也对齐到当前值，否则会看到一帧从旧位置滑过来
bool game_state_load(GameState *game_state, const GameStateSnapshot *snapshot);

// 快照自检：save → load → save 必须逐位一致（证明 load 把 save 写出的每一位都装回去了，
// 也顺带证明 game_state_snapshot_valid 不会误伤合法状态）。初始化后调用一次即可，
// 返回 false 只说明「存档与回放都不可信」，调用方报一声就够（没必要让游戏起不来）。
bool game_state_snapshot_selftest(const GameState *game_state);

// 关卡几何指纹：把「所有世界的碰撞几何 + 出生点 + 边界 + 门 + 动态实体出生描述」哈希成一个 32 位值。
// 用途：存档里记下写档时的关卡形态，读档时比对 —— 关卡改了（.bin 资产或合并规则）就拒载旧存档。
// 只哈希**字段值**（走 fnv1a_value）：结构体的填充字节不保证被初始化，按块哈希会得到不确定的值。
u32 game_level_fingerprint(const GameState *game_state);

// 关卡与角色初始化。viewport 尺寸用于把相机先夹进关卡边界（否则第一帧渲染会露出关卡外）。
// 资产文件缺失或格式无效时返回 false。
bool game_init_asset(GameState *game_state, u32 viewport_width, u32 viewport_height);

// 动作状态名（日志 / 脚本 / 轨迹文件共用）
const char *player_state_name(PlayerState state);

// 世界名（界面与脚本共用一份）：现在就是 FIRST / SECOND
const char *game_world_name(WorldId world);

// 当前传送相位的进度（0..1；不在传送中时为 0）。
// 为什么放在这一层：相位的**时长**常量住在 game.cc（带动画、动时长就在一处改），
// 而渲染侧只想要一个 0..1 的进度去推帧号 —— 把时长拄过去会被两份数字绑在一起。
// 它是纯查询（不写状态），所以不需要进快照：回放/脚本重跑算出来的帧号天然一致。
f32 game_teleport_phase_progress(const Teleport *teleport);

// viewport 尺寸参与相机夹取，由主循环传入
void game_update(GameInput *game_input, GameState *game_state, f32 dt, u32 viewport_width, u32 viewport_height);

// 大地图传送：落到某个世界的某个传送点（不像穿门那样从边缘自动走入）。
// 落点 =「角色水平中心 = 点的 x、脚底 = 点的 y」—— 传送点存的就是脚底坐标，
// 所以编辑器里把它摆在站得住的位置就行；万一摆进墙里，会先顶上最近那层地面顶面（与传送门同一套保护）。
void game_teleport_to_waypoint(GameState *game_state, WorldId target_world, u32 waypoint_index,
                               u32 viewport_width, u32 viewport_height);
