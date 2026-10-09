#pragma once

#include "core.h"
#include "shared/collision.h"
#include "shared/level_asset.h"

// ============================================================================
// 关卡层：编辑资产编译出来的运行时关卡
//
// 谁是这一层的使用者：**游戏与编辑器都要**。编辑器做诊断时会把资产编译一遍（只为了看
// 合并出多少碰撞体、有没有挤住的缝），所以它必须能拿到 `Level` —— 而编辑器**不链**游戏逻辑
// （GameState / 输入 / 渲染 / UI 一概不认识）。
//
// 因此这一层单独成头：否则 editor_doc 为了一个 `Level` 就得包含 game.h，
// 把 input.h / camera.h / sprite.h / ui.h 一起拖进编辑器（这正是本次拆分修掉的那个分层漏洞）。
//
// 依赖方向：shared/level.h -> shared/collision.h -> core.h，以及 -> shared/level_asset.h
// ============================================================================

// 角色朝向
enum PlayerFacing : u8
{
    FACE_LEFT,
    FACE_RIGHT,
    FACE_COUNT
};

// 当前已加载世界的标识。世界数据会预先建好，普通连接只切换当前引用，
// 后续异步加载只需要保证目标世界在过渡结束前处于可用状态。
enum WorldId : u8
{
    WORLD_FIRST,
    WORLD_SECOND,
    WORLD_COUNT
};

// 世界数量是关卡资产层也需要知道的（门的 target_level 校验引用 LEVEL_ASSET_WORLD_COUNT），
// 但 asset 层不能反向依赖这一层 —— 所以用这条断言把两个数字钉在一起。
static_assert((u32)WORLD_COUNT == LEVEL_ASSET_WORLD_COUNT,
              "世界数量变了：请同时改 LEVEL_ASSET_WORLD_COUNT（include/shared/level_asset.h）");

// 一条普通地图连接的目标入口。角色完全穿过出口后，先从入口边缘自动走到这里，
// 过渡期间无敌且不响应玩家输入。
struct WorldConnection
{
    WorldId target_world;
    f32 entry_x;
    f32 entry_y;
    PlayerFacing facing;
};

// 每张图最多几块可消失平台（超了 level.cc 会 assert）。
// 它属于关卡（是「一张图能摆几块」），不属于运行态 —— 所以住在这一层而不是 game.h。
inline constexpr u32 MAX_VANISH_BLOCKS = 64;

// 一个关卡：碰撞体列表 + 出生点 + 世界边界
struct Level
{
    Platform *platforms;
    u32 platform_count;

    // 伤害矩形：运行期由 tile 里的地刺格按行合并出来（见 level.cc），不是资产里存的数组
    Rect2D *spikes;
    u32 spike_count;

    // 可消失平台的块矩形：同样由 tile 按行合并出来；状态在 GameState 的 vanish 里，按块下标对齐
    Rect2D *vanish_platforms;
    u32 vanish_platform_count;

    LevelMoverAsset *movers;
    u32 mover_count;
    LevelMonsterAsset *monsters;
    u32 monster_count;
    LevelPortalAsset *portals; // 传送门（rect + 配对角标），几何由资产直接引用
    u32 portal_count;

    // 传送点（大地图的目的地）：同样引用资产的数组；坐标是**脚底**语义
    LevelWaypointAsset *waypoints;
    u32 waypoint_count;

    f32 spawn_x;       // 出生点（玩家中心位置，脚底正好站在地面上）
    f32 spawn_y;
    Rect2D bounds;     // 世界边界（用于相机夹取与掉出判定）

    bool has_right_connection;
    WorldConnection right_connection;
    bool has_left_connection;
    WorldConnection left_connection;
};

// 把编辑器/磁盘资产编译为运行时关卡：tile 合并为碰撞矩形，实体描述保持独立。
void level_build_from_asset(Level *level, const LevelAsset *asset);
