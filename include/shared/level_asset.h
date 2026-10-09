#pragma once

#include <stddef.h> // offsetof：实体注册表用它描述字段位置

#include "shared/collision.h"
#include "shared/mono_math.h" // v2：移动组件的两个轴端点

// ============================================================================
// 关卡资产：编辑器与游戏共用的关卡定义
//
// .bin 只保存无指针的编辑语义：tile、地刺、门与动态实体的出生描述。
// 它不保存 Platform 指针、纹理句柄、速度积分结果或任何 GameState 数据；
// 游戏加载后由 level.cc 编译为运行时 Level，编辑器未来也直接读写这一层。
// ============================================================================

// 版本 2：地刺由「矩形实体段」改成**一种 tile**（'^'），因为它要按格连续拖动布置、
// 将来还要按格贴图。reader 保留 v1 的读入分支：把 v1 的地刺矩形转成对应的地刺格（见 level_asset.cc）。
//
// 版本 3：新增 tile `'V'`（可消失平台）。**布局与 v2 完全相同**，只是多了一个 tile 值 ——
// 旧程序读到新图会在 level.cc 的「未知 tile」断言上直接 abort，所以按约定升版本号，
// 让旧程序明确拒载（而不是跑到一半炸掉）。
//
// 版本 4：新增实体「传送门」（`LevelPortalAsset`）—— header 多一个数量字段（四类实体），
// tile 段后依次是门 / 移动组件 / 怪物 / **传送门**（顺序 = 实体注册表的顺序）。
//
// 版本 5：新增实体「传送点」（`LevelWaypointAsset`，大地图的目的地）—— header 又多一个数量字段
// （变成九个 u32：magic/version/columns/rows 之外的 5 个数量），段顺序同上、传送点接在传送门之后。
// **header 的 u32 个数至此是 8 → 7 → 8 → 9**（v1 = 8、v2/v3 = 7、v4 = 8、v5+ = 9），
// 所以长度算法只能按版本查一次个数，不能用“是不是 v1”这种布尔判（见 level_asset.cc 的 helper）。
//
// 版本 6：活动平台从「水平往返（min_x / max_x / velocity_x）」改成「两点任意方向往返
// （point_a / point_b / speed）+ 伤害开关（damaging）」。**header 个数不变（还是 9）**，
// 只是平台段变长 —— 所以它是本节里唯一“只有段变长”的升级。
// 老图（v1~v5）仍能读：v5 及更早的平台会迁移成「水平往返、不伤害」（端点取原来的 min_x/max_x，
// 两个端点的先后按原来 velocity_x 的符号定，所以旧图的运动轨迹与迁移前**逐帧一致**）。
//
// 版本 7：平台变成通用的「移动组件」（`LevelMoverAsset`）：
//   ①本体位置改成**轴上的参数 `t0`**（不再存绝对中心）—— 本体的中心由 `level_asset_mover_rect()`
//     从「轴上的点 + 半宽半高」算出来，所以“本体飘在轴外”这种状态存不下来；
//   ②新增 `shape`：方形 = 可站可驮（可选伤害），圆形 = 碰到就伤害；
//   ③每个世界的数量上限从 1 放到 `LEVEL_ASSET_MAX_MOVERS`。
// **header 个数仍为 9**，段长仍为 40 字节（v6 是 9 个 f32 + 1 个 u32，v7 是 8 个 f32 + 2 个 u32），
// 但字段顺序不同，所以 reader 必须按版本分开读。
// 老图（v1~v6）的迁移是无损的：t0 由「旧中心在轴上的投影」推出 —— 旧图的本体中心本来就在轴上
// （旧运行时会在第一帧把它拉上去），所以推出的是它本来会在的位置。
inline constexpr u32 LEVEL_ASSET_MAGIC = 0x314C564Cu; // 文件字节为 "LVL1"
inline constexpr u32 LEVEL_ASSET_VERSION = 7;

// 关卡资产路径缓冲（宽字符，含结尾 '\0'）。同一把尺子在 writer 的临时文件路径上也要用：
// 写 '<path>.tmp' 需要多 4 个字符的余量，所以调用方的缓冲不能刚好卡满。
inline constexpr u32 LEVEL_ASSET_PATH_SIZE = 260;

enum LevelTile : u8
{
    LEVEL_TILE_EMPTY = '.',
    LEVEL_TILE_SOLID = '#',
    LEVEL_TILE_ONE_WAY = '=',
    LEVEL_TILE_SPAWN = 'P',
    LEVEL_TILE_SPIKE = '^',  // 只造成伤害、不参与物理阻挡；运行期由 level.cc 合并成伤害矩形
    // 可消失平台：踩上后先变暗预告、倒计时结束消失（不可碰撞）、再过一段时间恢复实体。
    // 它在 level.cc 里合并成**独立的一段**（不与实体/单向合并，否则消失时会带走邻居），
    // 相位与计时器是运行态，住在 GameState（时长是 src/game.cc 的常量）。
    LEVEL_TILE_VANISH = 'V',
};

enum LevelConnectionSide : u32
{
    LEVEL_CONNECTION_LEFT,
    LEVEL_CONNECTION_RIGHT,
    LEVEL_CONNECTION_SIDE_COUNT,
};

enum LevelConnectionFacing : u32
{
    LEVEL_CONNECTION_FACE_LEFT,
    LEVEL_CONNECTION_FACE_RIGHT,
    LEVEL_CONNECTION_FACING_COUNT,
};

// 门落点是角色脚底的世界坐标；游戏按玩家碰撞盒把它转换为角色中心坐标。
struct LevelConnectionAsset
{
    LevelConnectionSide side;
    u32 target_level;
    f32 entry_x;
    f32 entry_y;
    LevelConnectionFacing facing;
};

// 移动组件：形状 + 一条轴（两点之间的直线）+ 沿轴速度。轴可以任意方向（水平 / 竖直 / 斜着都行），
// 每个世界可以放多个（上限见 LEVEL_ASSET_MAX_MOVERS）。它先朝 `point_b` 走，到端点原路折返。
//
// **本体按定义就在轴上**：文件里存的是轴上的参数 `t0`（0 = A、1 = B），本体的中心由
// `level_asset_mover_rect()` 从「轴上的点 + 半宽半高」算出来 —— 所以「本体不在轴上」这种状态存不下来。
//
// 形状决定行为（运行时的 `Mover`，见 include/game.h）：
//   * `SQUARE`：实心、可以站上去被带着走（两个轴都驮）；`damaging` 打开时「被它挤住 / 压住」会重生
//     （被驮着走不会受伤）。这一档以后可能演化成「压路机」，所以开关留着。
//   * `CIRCLE`：实心、**不驮人**，恒定伤害 —— 只要碰到就重生；`damaging` 对圆形无意义（忽略）。
//     半径就是 `half_w`；写的时候 `half_h` 与它相同，好让派生出来的 AABB 是个正方形。
//
// `speed` 是**沿轴**的速度（不是分量速度），所以斜着放时不会「看着变慢」。
enum LevelMoverShape : u32
{
    LEVEL_MOVER_SQUARE, // 可站、可驮（可选伤害）
    LEVEL_MOVER_CIRCLE, // 不可穿过、碰到就伤害
    LEVEL_MOVER_SHAPE_COUNT,
};

struct LevelMoverAsset
{
    v2 point_a;
    v2 point_b;
    f32 t0;       // 起始位置在轴上的参数：0 = point_a，1 = point_b（运行时夹在 [0,1]）
    f32 half_w;   // 半宽；圆形时是半径
    f32 half_h;   // 半高；圆形时不用（写出去与 half_w 相同）
    f32 speed;    // 沿轴速度 px/s
    u32 shape;    // LevelMoverShape
    u32 damaging; // 0/1，只对方形有意义
};

// 本体的派生矩形：中心 = 轴上的点，尺寸 = 半宽半高。t0 先夹到 [0,1]，
// 所以取出来的位置一定落在 A、B 之间 —— 与运行时同一套算法，编辑器和校验都调它。
inline Rect2D level_asset_mover_rect(const LevelMoverAsset *mover)
{
    f32 t = clamp(mover->t0, 0.0f, 1.0f);
    return Rect2D{ .center_x = mover->point_a.x + (mover->point_b.x - mover->point_a.x) * t,
                   .center_y = mover->point_a.y + (mover->point_b.y - mover->point_a.y) * t,
                   .half_w = mover->half_w,
                   .half_h = mover->half_h };
}

struct LevelMonsterAsset
{
    Rect2D rect;
    f32 spawn_x;
    f32 spawn_y;
    f32 velocity_x;
};

// 传送门：一对同 pair_id 的门互相传送。落点规则在游戏侧：
// **角色水平中心 = 门中心、脚底 = 门矩形的底边**（所以门应该贴地摆放）。
// 暂不支持跨世界 —— 一旦跨世界，落点还要带上目标世界。
struct LevelPortalAsset
{
    Rect2D rect;
    u32 pair_id; // 调色板下标（LEVEL_PORTAL_PALETTE），同时也是配对键
};

// 传送点：大地图上的目的地（地图**之间**的传送）。它存的是**一个点**而不是矩形 ——
// 它属于「地图信息」（这一带叫什么、从哪进），不属于场景里的实体；标在编辑器画布上时
// 画成一个记号而不是一个可拖动缩放的方框。
// 坐标是**角色脚底**（与出生点、门落点的语义一致），不是角色中心 —— 落点规则在游戏侧。
struct LevelWaypointAsset
{
    f32 x;
    f32 y;
};

// 加载后的资产视图：所有数组由 arena 常驻，指针只存在于内存中，不会序列化。
struct LevelAsset
{
    u32 tile_columns;
    u32 tile_rows;
    f32 tile_size;
    LevelTile *tiles; // row-major，长度 = tile_columns * tile_rows

    LevelConnectionAsset *connections;
    u32 connection_count;
    LevelMoverAsset *movers;
    u32 mover_count;
    LevelMonsterAsset *monsters;
    u32 monster_count;

    LevelPortalAsset *portals;
    u32 portal_count;

    LevelWaypointAsset *waypoints;
    u32 waypoint_count;
};

// ============================================================================
// 网格 ↔ 世界坐标的换算：公式只留这一处
//
// 这套约定是**游戏侧既有的**（不是编辑器发明的），而且抄错一格就是经典偏移 bug：
//   row 0 = **最上面**一行，所以 row 越大 y 越负；
//   格心 y = -(row + 0.5) * 格边长；格**底边** y = -(row + 1) * 格边长
//   （出生点 / 门落点 / 传送点都是「脚底」语义，统一用底边）。
// 使用者：level.cc（编译关卡）、level_asset.cc（校验）、编辑器（画布 / 命中 / 显示）。
// ============================================================================

inline constexpr f32 level_cell_center_x(u32 col, f32 tile_size)
{
    return ((f32)col + 0.5f) * tile_size;
}

inline constexpr f32 level_cell_center_y(u32 row, f32 tile_size)
{
    return -((f32)row + 0.5f) * tile_size;
}

// 一行里从 col_begin 开始、共 cell_count 格的横向条带的中心 x
// （地刺 / 可消失平台 / 合并碰撞体都是「按行合并」，中心都走这一条）
inline constexpr f32 level_row_span_center_x(u32 col_begin, f32 cell_count, f32 tile_size)
{
    return ((f32)col_begin + cell_count * 0.5f) * tile_size;
}

// 格子**底边** y —— 出生点 / 门落点 / 传送点的「脚底」坐标
inline constexpr f32 level_cell_bottom_y(u32 row, f32 tile_size)
{
    return -((f32)row + 1.0f) * tile_size;
}

// 网格外沿构成的关卡边界：x ∈ [0, 列数×格]、y ∈ [-行数×格, 0]。
// 取**外沿**而不是首/末格中心（旧写法两端各差半格，相机夹取后画面上下会各露 32px 空白）。
inline Rect2D level_grid_bounds(u32 columns, u32 rows, f32 tile_size)
{
    Rect2D bounds = {};
    bounds.center_x = (f32)columns * tile_size * 0.5f;
    bounds.center_y = -(f32)rows * tile_size * 0.5f;
    bounds.half_w = (f32)columns * tile_size * 0.5f;
    bounds.half_h = (f32)rows * tile_size * 0.5f;
    return bounds;
}

// ============================================================================
// 语义约束：编辑器与游戏共用的**唯一数字来源**
//
// 这些不是格式约束（格式约束在 reader 里：长度、枚举范围、min_x <= max_x），
// 而是「这样写出来游戏能不能跑」。两份实现的先例在本工程踩过（手柄按钮映射曾经一表一实现、
// 漏改静默失效），所以数字只留一份，谁要判就引用这里。
// ============================================================================

// 运行时每个世界最多几个移动组件（game.cc 的 assert 引用这个常量）。
// 上限只受数组大小限制，调大它不需要改格式（文件里是数量 + 数组）。
inline constexpr u32 LEVEL_ASSET_MAX_MOVERS = 8;

// 运行时每个世界只支持一个怪物（game.cc 的 assert 引用这个常量）
inline constexpr u32 LEVEL_ASSET_MAX_MONSTERS = 1;

// 出生点必须恰好一个：它是关卡的单例属性，只是寄居在 tile 网格里。
// 多个 P 时 level.cc 是「最后一个赢」（静默），所以这里必须当成硬错误。
inline constexpr u32 LEVEL_ASSET_REQUIRED_SPAWN_TILES = 1;

// 两块上下堆叠的平台，**行差**至少 3 格。行差 1 = 实心块（墙/地面，正常）；
// 行差 2 = 中间只空 1 格（64px），角色碰撞盒高约 90px —— 挤不进去、会卡住（这是要抓的那一类）。
inline constexpr u32 LEVEL_ASSET_MIN_ROW_DELTA = 3;

// 世界数量（与 game.h 的 WORLD_COUNT 对应，那边有 static_assert 卡住）。
// 世界 ID 是位置决定的：first.bin = 0、second.bin = 1，所以门的 target_level 不能越出这个范围。
inline constexpr u32 LEVEL_ASSET_WORLD_COUNT = 2;

// 每张图最多几个传送门（运行期是定长数组；超了校验报 ERROR）
inline constexpr u32 LEVEL_ASSET_MAX_PORTALS = 8;

// 每张图最多几个传送点。上限与 UI 的列表容量对得上：
// `LEVEL_ASSET_MAX_WAYPOINTS * LEVEL_ASSET_WORLD_COUNT <= UI_MAX_ITEMS`（game.h 里有 static_assert），
// 也就是「大地图一定列得下所有传送点」—— 如果哪天不够，是这两个数字要一起调的事。
inline constexpr u32 LEVEL_ASSET_MAX_WAYPOINTS = 4;

// 同一个 pair_id 必须**恰好**两个门（一对一互传）；1 个或 3 个以上都会被校验拒掉
inline constexpr u32 LEVEL_ASSET_PORTALS_PER_PAIR = 2;

// 传送门配对的**显示调色板**：`pair_id` 就是下标 —— **同色 = 配对**。
// 它压在共享层而不是格式层，因为文件里只存下标（颜色是显示语义）；
// 而编辑器与游戏必须用同一张表，否则会出现「编辑器里同色、游戏里不同色」，关卡设计就失去意义了。
//
// 条数不需要与「最多几对门」对齐：每张图最多 LEVEL_ASSET_MAX_PORTALS / 2 对，
// 但表是给所有图共用的，摆多几个只是为了给关卡留出不同的颜色选择。
inline constexpr u32 LEVEL_PORTAL_PALETTE_COUNT = 12;

struct LevelPortalColor
{
    u8 r;
    u8 g;
    u8 b;
};

inline constexpr LevelPortalColor LEVEL_PORTAL_PALETTE[LEVEL_PORTAL_PALETTE_COUNT] = {
    { 236, 92, 92 },   // 红
    { 92, 200, 236 },  // 青
    { 236, 200, 92 },  // 黄
    { 152, 116, 236 }, // 紫
    { 116, 236, 140 }, // 绿
    { 236, 148, 92 },  // 橙
    { 236, 92, 180 },  // 品红
    { 92, 236, 220 },  // 蓝绿
    { 180, 236, 92 },  // 黄绿
    { 140, 148, 236 }, // 蓝紫
    { 236, 236, 236 }, // 白
    { 148, 148, 148 }, // 灰
};

// ============================================================================
// 实体注册表：**加一种实体，只改这张表 + 一处属性面板**
//
// 「实体」= 有身份的物件（数组项）：可单独选中、改参数、删除。没有身份的格子值（墙/单向/出生点）
// 不在这里。判据见 docs/level-assets.md。
//
// 顺序 = 文件里各段的顺序，也是 LevelAssetEntityKind 的值。
// 编辑器与校验都从这张表读名字 / 数组位置 / 元素大小 / 有没有矩形 / 数量上限，
// 于是不用各自再写一遍 4 个 switch —— 那种「漏一个 case」在表里是不存在的（漏了就是少一行）。
//
// 游戏的运行态（Level / GameState）不消费本表：它的每类实体有各自不同的运行字段与初始化逻辑，
// 强行统一只会多一层间接。这张表服务的是**编辑语义**这一层。
// ============================================================================

enum LevelAssetEntityKind : u32
{
    LEVEL_ASSET_ENTITY_CONNECTION,
    LEVEL_ASSET_ENTITY_MOVER,
    LEVEL_ASSET_ENTITY_MONSTER,
    LEVEL_ASSET_ENTITY_PORTAL,
    LEVEL_ASSET_ENTITY_WAYPOINT,
    LEVEL_ASSET_ENTITY_KIND_COUNT,
};

// 这种实体没有矩形（门的位置由「哪一侧边缘」决定，不在这里存）
inline constexpr u32 LEVEL_ASSET_NO_RECT = 0xFFFFFFFFu;

struct LevelAssetEntityMeta
{
    // 界面名（编辑器界面与校验消息共用同一份）：直接叫「地刺」，
    // 与代码里的 SpikeZone 同名同义（曾经叫「伤害区」，容易让人以为它与地刺是两件东西）
    const char *name;
    u32 array_offset; // offsetof(LevelAsset, spikes) 这一类
    u32 count_offset; // offsetof(LevelAsset, spike_count) 这一类
    u32 element_size;
    u32 rect_offset;  // 元素内 Rect2D 的偏移；LEVEL_ASSET_NO_RECT = 没有矩形
    u32 max_count;    // 运行时限几个（UINT32_MAX = 不限）
};

inline constexpr LevelAssetEntityMeta LEVEL_ASSET_ENTITY_TABLE[LEVEL_ASSET_ENTITY_KIND_COUNT] = {
    { "门", offsetof(LevelAsset, connections), offsetof(LevelAsset, connection_count), sizeof(LevelConnectionAsset),
      LEVEL_ASSET_NO_RECT, UINT32_MAX },
    // 移动组件没有**存储**的矩形（本体位置是轴上的参数 t0，矩形由 level_asset_mover_rect 算出来），
    // 所以 rect_offset = NO_RECT：通用矩形检查跳过它，校验里另有一条针对它的派生矩形检查。
    { "移动组件", offsetof(LevelAsset, movers), offsetof(LevelAsset, mover_count), sizeof(LevelMoverAsset),
      LEVEL_ASSET_NO_RECT, LEVEL_ASSET_MAX_MOVERS },
    { "怪物", offsetof(LevelAsset, monsters), offsetof(LevelAsset, monster_count), sizeof(LevelMonsterAsset),
      offsetof(LevelMonsterAsset, rect), LEVEL_ASSET_MAX_MONSTERS },
    { "传送门", offsetof(LevelAsset, portals), offsetof(LevelAsset, portal_count), sizeof(LevelPortalAsset),
      offsetof(LevelPortalAsset, rect), LEVEL_ASSET_MAX_PORTALS },
    // 传送点没有矩形（它是一个点：地图信息，不是场景物件），所以 rect_offset = NO_RECT
    { "传送点", offsetof(LevelAsset, waypoints), offsetof(LevelAsset, waypoint_count), sizeof(LevelWaypointAsset),
      LEVEL_ASSET_NO_RECT, LEVEL_ASSET_MAX_WAYPOINTS },
};

// 某种实体的数量与数组基址（纯读，无分配）
inline u32 level_asset_entity_count(const LevelAsset *asset, u32 kind)
{
    return *(const u32 *)((const u8 *)asset + LEVEL_ASSET_ENTITY_TABLE[kind].count_offset);
}

inline void *level_asset_entity_data(const LevelAsset *asset, u32 kind)
{
    return *(void *const *)((const u8 *)asset + LEVEL_ASSET_ENTITY_TABLE[kind].array_offset);
}

// 第 index 个元素的地址；越界返回 nullptr
inline void *level_asset_entity_at(const LevelAsset *asset, u32 kind, u32 index)
{
    if (index >= level_asset_entity_count(asset, kind)) {
        return nullptr;
    }
    return (u8 *)level_asset_entity_data(asset, kind) + (u64)index * LEVEL_ASSET_ENTITY_TABLE[kind].element_size;
}

// 第 index 个元素的矩形；越界或这种实体没有矩形时返回 nullptr
inline const Rect2D *level_asset_entity_rect(const LevelAsset *asset, u32 kind, u32 index)
{
    const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
    if (meta->rect_offset == LEVEL_ASSET_NO_RECT) {
        return nullptr;
    }
    const u8 *element = (const u8 *)level_asset_entity_at(asset, kind, index);
    return element ? (const Rect2D *)(element + meta->rect_offset) : nullptr;
}

// 同上，但给的是可写指针（编辑器拖动用）。**存储**了矩形的实体才有；移动组件返回 nullptr
// （它的矩形是派生的，要改就改 t0 / 半宽半高）。
inline Rect2D *level_asset_entity_rect_mut(LevelAsset *asset, u32 kind, u32 index)
{
    const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
    if (meta->rect_offset == LEVEL_ASSET_NO_RECT) {
        return nullptr;
    }
    u8 *element = (u8 *)level_asset_entity_at(asset, kind, index);
    return element ? (Rect2D *)(element + meta->rect_offset) : nullptr;
}

// 第 index 个元素**当前**的矩形，写进 *out（越界返回 false）。
// 与上面两个的区别：它不关心矩形是存储的还是派生的 —— 编辑器（画布 / 命中测试 / 拖动）
// 与语义校验都用它，就不必各自记一遍「哪些实体没有存储矩形」。
inline bool level_asset_entity_rect_now(const LevelAsset *asset, u32 kind, u32 index, Rect2D *out)
{
    const Rect2D *stored = level_asset_entity_rect(asset, kind, index);
    if (stored) {
        *out = *stored;
        return true;
    }
    if (kind == LEVEL_ASSET_ENTITY_MOVER) {
        const LevelMoverAsset *mover = (const LevelMoverAsset *)level_asset_entity_at(asset, kind, index);
        if (mover) {
            *out = level_asset_mover_rect(mover);
            return true;
        }
    }
    return false;
}

// ============================================================================
// 语义校验
//
// 判据：**「能读」不等于「能玩」**。level_asset_load 只验格式与范围，
// 它结构性无法知道「每个世界只允许一个怪物」这类游戏侧约束 —— 那些约束在这里，
// 由编辑器（保存前）、将来的工具/CI 共用。游戏侧不调用它（游戏用 assert 卡自己的假设）。
// ============================================================================

enum LevelAssetIssueLevel : u32
{
    LEVEL_ASSET_ISSUE_WARN,
    LEVEL_ASSET_ISSUE_ERROR, // 出现 ERROR 的关卡不该被保存，游戏侧也一定有问题
};

// 机器可读的问题种类（文本给人看，种类 + 实体种类给测试与「点击跳过去」用）
enum LevelAssetIssueKind : u32
{
    LEVEL_ASSET_ISSUE_GRID_INVALID,          // ERROR 网格本身不成立（空资产/零尺寸/指针与数量不一致）
    LEVEL_ASSET_ISSUE_ENTITY_COUNT,          // ERROR 某类实体数量超过运行时限值（看 entity_kind）
    LEVEL_ASSET_ISSUE_SPAWN_COUNT,           // ERROR 出生点不是恰好 1 个
    LEVEL_ASSET_ISSUE_DEGENERATE_RECT,       // ERROR 半宽/半高 <= 0
    LEVEL_ASSET_ISSUE_NON_FINITE,            // ERROR NaN / Inf
    LEVEL_ASSET_ISSUE_PLATFORM_RANGE,        // ERROR min_x > max_x
    LEVEL_ASSET_ISSUE_OUT_OF_BOUNDS,         // ERROR 整个矩形在关卡外 / WARN 部分越界
    LEVEL_ASSET_ISSUE_CONNECTION_TARGET,     // ERROR target_level 越界或指向自己
    LEVEL_ASSET_ISSUE_PORTAL_PAIR,           // ERROR 传送门配对角标越界 / 同一色号的门不是恰好 2 个
    LEVEL_ASSET_ISSUE_SPAWN_IN_ENTITY,       // WARN  出生点落在某个实体矩形里（看 entity_kind）
    LEVEL_ASSET_ISSUE_SPAWN_NO_GROUND,       // WARN  出生点正下方没有支撑
    LEVEL_ASSET_ISSUE_TIGHT_ROW_GAP,         // WARN  同列上下两块平台行差只有 2 格（中间只空 1 格）

    LEVEL_ASSET_ISSUE_KIND_COUNT,
};

inline constexpr u32 LEVEL_ASSET_MAX_ISSUES = 64;
inline constexpr u32 LEVEL_ASSET_ISSUE_TEXT_SIZE = 192;

// 一条问题。text 是固定缓冲（不带指针、不分配内存），所以整份报告可以直接放栈上
struct LevelAssetIssue
{
    LevelAssetIssueLevel level;
    LevelAssetIssueKind kind;
    int entity_kind;  // LevelAssetEntityKind；-1 = 与某类实体无关
    int entity_index; // 该实体在数组里的下标；-1 = 不适用
    int col;          // -1 = 不适用
    int row;
    char text[LEVEL_ASSET_ISSUE_TEXT_SIZE];
};

struct LevelAssetIssues
{
    LevelAssetIssue items[LEVEL_ASSET_MAX_ISSUES];
    u32 count;
    u32 error_count;
    u32 warn_count;
    bool truncated; // true = 问题多到装不下（编辑器必须把它显示出来）
};

// 校验一份资产；out 被完整重写。asset 为空/网格为空时算作一条 ERROR。
// self_world_id = 这份资产自己的世界 ID（用于「门指向自己」的检查）；未知时传 -1。
void level_asset_validate(const LevelAsset *asset, int self_world_id, LevelAssetIssues *out);

// ============================================================================
// 写入 / 读取
// ============================================================================

// 读取并校验 .bin。失败时不改动 out；成功后的数组内存由 arena 持有。
bool level_asset_load(const wchar_t *path, LevelAsset *out);

// 写出 .bin。原子性由三步保证：
//   1. 写 `<path>.tmp`（原文件全程不动）；
//   2. 立刻用 level_asset_load 读回来，并逐字段与内存中的资产比对（写错一个字段就当场失败）；
//   3. 通过后才 file_move_replace 覆盖原文件。
// 任一步失败都会删掉临时文件并返回 false，原文件保持不变。
// 注意：本函数**不做语义校验**（那是 level_asset_validate 的事，见上）；
// 它只保证「写出去的东西 == 交进来的东西，且读取器能接受」。
bool level_asset_save(const wchar_t *path, const LevelAsset *asset);

// 两份资产是否逐字段相同（tiles + 每一类实体的每个字段 + 数量）。
// **不能 memcmp 整个结构体**：填充字节未初始化 —— 实现里逐字段比（与上面第 2 步用的是同一份）。
// 用途：编辑器的「与上次保存/打开的那份比，内容有没有变」（dirty），
// 以及撤销栈的「内容没变就不落一格快照」。
bool level_asset_equal(const LevelAsset *a, const LevelAsset *b);
