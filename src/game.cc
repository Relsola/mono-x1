#include "core.h"
#include "shared/memory.h"
#include "game.h"
#include "game_audio.h"
#include "debug/debug_vis.h"
#include "shared/mono_math.h"

#include <stdio.h>  // swprintf_s（动画帧文件名）
#include <string.h> // memcmp（快照自检）

// 注：精灵图的解码与动画推进在 src/sprite.cc，这里只负责游戏逻辑与关卡数据。

// ============================================================================
// 角色运动与关卡数值（手感调优集中在这里）
// ============================================================================

inline constexpr f32 PLAYER_MOVE_SPEED = 640.0f;       // 水平移动速度
inline constexpr f32 PLAYER_GRAVITY = 4200.0f;         // 重力加速度
inline constexpr f32 PLAYER_MAX_FALL_SPEED = 1600.0f;  // 最大下落速度
inline constexpr f32 PLAYER_JUMP_SPEED = 1430.0f;      // 起跳初速（顶点约 243 像素 ≈ 3.8 格）
inline constexpr u32 PLAYER_AIR_JUMP_COUNT = 1;        // 离地后还能再跳几次（1 = 二段跳；两段共用同一个初速）
inline constexpr f32 PLAYER_COYOTE_TIME = 0.1f;        // 土狼时间：离开地面后仍可起跳的宽限
inline constexpr f32 PLAYER_JUMP_BUFFER_TIME = 0.1f;   // 跳跃缓冲：落地前提前按下的宽限
inline constexpr f32 PLAYER_DROP_THROUGH_TIME = 0.2f;  // 主动下穿单向平台后忽略它的时间
inline constexpr f32 PLAYER_DASH_SPEED = 1600.0f;      // 冲刺速度
inline constexpr f32 PLAYER_DASH_TIME = 0.15f;         // 冲刺持续时间（位移约 240 像素）
inline constexpr f32 PLAYER_DASH_COOLDOWN = 0.4f;      // 冲刺后冷却
inline constexpr f32 PLAYER_TRANSITION_SPEED = 640.0f; // 穿门后自动走入目标世界的速度
inline constexpr f32 TIME_STOP_RADIUS = 448.0f;
inline constexpr f32 TIME_STOP_DURATION = 3.0f;
inline constexpr f32 TIME_STOP_COOLDOWN = 5.0f;
inline constexpr f32 TIME_STOP_SPEED_SCALE = 0.08f;

// 圆形移动组件的接触容差（像素）：它是实心的，滑动碰撞会把玩家推到「刚好贴住」的位置，
// 所以“碰到”要用「距离 <= 半径 + 容差」判，纯严格小于会漏帧。见 mover_circle_touches_box。
inline constexpr f32 MOVER_CIRCLE_CONTACT_TOLERANCE = 1.0f;
inline constexpr f32 PROJECTILE_WIDTH = 40.0f;
inline constexpr f32 PROJECTILE_HEIGHT = 20.0f;
inline constexpr f32 PROJECTILE_SPEED = 2400.0f;
inline constexpr f32 PROJECTILE_COOLDOWN = 1.0f;
inline constexpr f32 STICK_DEADZONE = 0.2f;   // 摇杆死区（XInput 官方常量 7849/32767 ≈ 24%）
inline constexpr f32 STICK_DOWN_PRESS = 0.7f; // 把“摇杆向下”当按钮用：要压到底（理由见游戏更新里的注释）

// 一帧内角色位移超过它就当成「传送」（掉出关卡重生、将来的传送门/关卡切换）：相机直接贴合。
// 正常移动一帧最多 PLAYER_DASH_SPEED / 60Hz ≈ 27 像素，所以 64 有足够余量；
// 若将来出现更快的位移手段，这个值要跟着调。
inline constexpr f32 CAMERA_TELEPORT_MARGIN = 64.0f;

// 碰撞盒相对精灵尺寸的比例：只在水平方向收窄，竖直方向让盒底对齐精灵脚底
inline constexpr f32 PLAYER_COLLIDER_WIDTH_RATIO = 0.55f;
inline constexpr f32 PLAYER_COLLIDER_HEIGHT_RATIO = 0.7f;

global_variable const wchar_t *const LEVEL_ASSET_PATHS[WORLD_COUNT] = {
    L"data/map/first.bin",
    L"data/map/second.bin",
};

internal CameraProfile camera_profile_for();
internal u32 build_collision_platforms(const GameState *game_state, Platform *out);
internal f32 player_center_y_from_feet(const GameState *game_state, f32 feet_y);
internal void place_player_at_feet(GameState *game_state, f32 x, f32 feet_y, u32 viewport_width,
                                   u32 viewport_height);

// 每步拼出来的碰撞平台数组容量（静态 + 活动 + 可消失）
constexpr u32 MAX_COLLISION_PLATFORMS = 256;

internal void debug_register_level(const Level *level)
{
#if MONO_DEBUG_VIS
    debug_vis_clear_static();
    for (u32 i = 0; i < level->platform_count; ++i) {
        const Platform *platform = &level->platforms[i];
        DebugBoxTag tag = (platform->kind == COLLIDER_ONE_WAY) ? DEBUG_BOX_STATIC_ONE_WAY : DEBUG_BOX_STATIC_SOLID;
        debug_vis_static_box(&platform->rect, tag);
    }
    for (u32 i = 0; i < level->spike_count; ++i) {
        debug_vis_static_box(&level->spikes[i], DEBUG_BOX_SPIKE);
    }
    for (u32 i = 0; i < level->portal_count; ++i) {
        debug_vis_static_box(&level->portals[i].rect, DEBUG_BOX_PORTAL);
    }
#else
    (void)level;
#endif
}

internal void initialize_movers(GameState *game_state)
{
    MoverSet *set = &game_state->movers;
    *set = {};
    const u32 count = game_state->level.mover_count;
    assert(count <= LEVEL_ASSET_MAX_MOVERS &&
           "移动组件数量超过 LEVEL_ASSET_MAX_MOVERS（编辑器校验引用的是同一个数字）");
    set->count = count;
    for (u32 i = 0; i < count; ++i) {
        const LevelMoverAsset *asset = &game_state->level.movers[i];
        // 只写「资产 + 起点」决定的字段，其余（time_slowed）由聚合初始化补零：
        // 以后给 Mover 加字段时，这里不会静默地沿用上一帧的残值
        set->items[i] = Mover{ .damaging = (asset->damaging != 0),
                               .shape = asset->shape,
                               .rect = level_asset_mover_rect(asset),
                               .point_a = asset->point_a,
                               .point_b = asset->point_b,
                               // 起点由资产给出（t0 夹在 [0,1]）；先朝 point_b 走
                               .t = clamp(asset->t0, 0.0f, 1.0f),
                               .direction = 1.0f,
                               .half_w = asset->half_w,
                               .half_h = asset->half_h,
                               .speed = asset->speed };
    }
}

internal void initialize_monster(GameState *game_state)
{
    Monster *monster = &game_state->monster;
    *monster = {};
    if (game_state->level.monster_count == 0) {
        return;
    }
    assert(game_state->level.monster_count == LEVEL_ASSET_MAX_MONSTERS &&
           "当前原型每个世界只支持一个怪物（LEVEL_ASSET_MAX_MONSTERS，"
           "编辑器的校验引用的是同一个数字）");

    const LevelMonsterAsset *asset = &game_state->level.monsters[0];
    *monster = Monster{ .active = true,
                        .rect = asset->rect,
                        .spawn_x = asset->spawn_x,
                        .spawn_y = asset->spawn_y,
                        .spawn_velocity_x = asset->velocity_x,
                        .velocity = v2{ asset->velocity_x, 0.0f } };
}

// 把相机直接贴合到角色（dt = 0 = 不平滑）：初始化、穿门、大地图传送都要一次到位，
// 否则会看到相机从旧世界/旧位置滑过去。同时把渲染插值的历史位置也对齐，避免插值出中间帧。
internal void snap_camera_to_player(GameState *game_state, u32 viewport_width, u32 viewport_height)
{
    game_state->camera.pos_x = game_state->player_x;
    game_state->camera.pos_y = game_state->player_y;
    camera_follow(&game_state->camera, game_state->player_x, game_state->player_y,
                  &game_state->level.bounds, viewport_width, viewport_height, 0.0f,
                  camera_profile_for());
    game_state->prev_camera = game_state->camera;
    game_state->prev_player_x = game_state->player_x;
    game_state->prev_player_y = game_state->player_y;
}

// 装入一个世界：关卡引用、该世界的动态实体，以及**属于旧世界坐标的东西**必须一起清掉。
// 伪时停范围、飞行中的能量波、可消失平台相位、传送门相位都是按当前世界的下标/坐标对齐的，
// 留着会让旧世界的坐标残留到新世界。
// 它只负责「换世界」不管角色站在哪：穿门（switch_world）与大地图传送（game_teleport_to_world_spawn）
// 的差别只在落点规则，所以那部分各自写。
internal void enter_world(GameState *game_state, WorldId target_world)
{
    game_state->time_stop = {};
    game_state->projectiles = {};
    game_state->prev_projectiles = {};
    game_state->world = target_world;
    game_state->level = game_state->worlds[target_world];
    initialize_movers(game_state);
    game_state->prev_movers = game_state->movers;
    initialize_monster(game_state);
    game_state->prev_monster = game_state->monster;
    // 可消失平台的状态按「块下标」对齐当前世界的几何，换世界必须清掉：
    // 两张图的块数不一定一样，旧世界残留的相位会套到新世界的块上。
    game_state->vanish = {};
    game_state->teleport = {}; // 相位/目标门下标也是「按门下标对齐当前世界」的，换世界必须清
    debug_register_level(&game_state->level);
}

internal void switch_world(GameState *game_state, WorldId target_world, const WorldConnection *connection,
                           u32 viewport_width, u32 viewport_height)
{
    enter_world(game_state, target_world);

    // 从目标地图对应的门洞边缘开始自动走入；目标位置保证角色完整显示并与门留出距离。
    f32 player_half_w = game_state->player_collider.width * 0.5f;
    f32 world_left = game_state->level.bounds.center_x - game_state->level.bounds.half_w;
    f32 world_right = game_state->level.bounds.center_x + game_state->level.bounds.half_w;
    game_state->player_x = (connection->facing == FACE_RIGHT) ? (world_left + player_half_w + 1.0f)
                                                              : (world_right - player_half_w - 1.0f);
    game_state->player_y = connection->entry_y;
    game_state->velocity = {};
    game_state->grounded = true;
    game_state->facing = connection->facing;
    game_state->state = PSTATE_RUN;
    game_state->dash_timer = 0.0f;
    game_state->dash_cooldown = 0.0f;
    game_state->coyote_timer = PLAYER_COYOTE_TIME;
    game_state->jump_buffer_timer = 0.0f;
    game_state->drop_through_timer = 0.0f;
    game_state->air_jumps_left = PLAYER_AIR_JUMP_COUNT;
    game_state->world_transition_active = true;
    game_state->transition_target_x = connection->entry_x;
    game_state->transition_target_y = connection->entry_y;

    // 世界坐标无需连续，但相机必须在新世界的合法范围内从第一帧开始跟随。
    snap_camera_to_player(game_state, viewport_width, viewport_height);
}

// ============================================================================
// 关卡与角色初始化
// ============================================================================

// 相机档位的选择策略 —— 游戏侧**唯一**决定「这一刻用哪套相机手感」的地方。
//
// 现在只有一个档位，所以不接参数；以后要按场景分支（比如“需要画面绝对稳的桥段”“高速段”）
// 就写在这里，需要角色/关卡信息时再加参数。两条硬约束：
//   1. 只看固定步长里的数据（玩家位置/速度/关卡）——看真实时间或渲染帧会让回放/脚本对拍飘；
//   2. 数值不要写在这里，只选档位名；手感数值统一在 src/camera.cc 的 CAMERA_PROFILES 表。
internal CameraProfile camera_profile_for()
{
    return CAMERA_PROFILE_DEFAULT;
}

bool game_init_asset(GameState *game_state, u32 viewport_width, u32 viewport_height)
{
    for (u32 i = 0; i < WORLD_COUNT; ++i) {
        if (!level_asset_load(LEVEL_ASSET_PATHS[i], &game_state->level_assets[i])) {
            return false;
        }
        level_build_from_asset(&game_state->worlds[i], &game_state->level_assets[i]);
    }
    game_state->world = WORLD_FIRST;
    game_state->level = game_state->worlds[WORLD_FIRST];

    game_state->backdrop = load_sprite(L"data/test_background.bmp");

    SpriteAnimation *animation = &game_state->player_bagdown_animation;
    animation->frame_count = 11; // 暂时硬编码 11 张
    animation->looping = true;
    animation->frames = (AnimationFrame *)arena_push(sizeof(AnimationFrame) * animation->frame_count);

    // 以 12 FPS 循环播放 f0 到 f10
    constexpr f32 frame_duration = 1.0f / 12.0f;
    for (u32 i = 0; i < animation->frame_count; ++i) {
        wchar_t filename[128];
        swprintf_s(filename, L"data/player/bagdown/f%u.png", i);
        animation->frames[i].image = load_sprite(filename);
        animation->frames[i].image.scale = 4.0f;
        animation->frames[i].duration = frame_duration;
    }

    // 身体碰撞箱独立于当前动画帧，位置以角色脚底中心为参考
    SpriteImage *first_frame = &get_current_animation(animation)->image;
    game_state->player_collider.width = (f32)first_frame->width * first_frame->scale * PLAYER_COLLIDER_WIDTH_RATIO;
    game_state->player_collider.height = (f32)first_frame->height * first_frame->scale * PLAYER_COLLIDER_HEIGHT_RATIO;
    game_state->player_collider.offset_x = 0.0f;
    game_state->player_collider.offset_y = -game_state->player_collider.height * 0.2f;

    // 资产里的出生点和门落点都是角色脚底坐标；这里统一转换为角色中心坐标。
    for (u32 i = 0; i < WORLD_COUNT; ++i) {
        game_state->worlds[i].spawn_y = player_center_y_from_feet(game_state, game_state->worlds[i].spawn_y);
        if (game_state->worlds[i].has_left_connection) {
            game_state->worlds[i].left_connection.entry_y =
                player_center_y_from_feet(game_state, game_state->worlds[i].left_connection.entry_y);
        }
        if (game_state->worlds[i].has_right_connection) {
            game_state->worlds[i].right_connection.entry_y =
                player_center_y_from_feet(game_state, game_state->worlds[i].right_connection.entry_y);
        }
    }
    game_state->level = game_state->worlds[WORLD_FIRST];

    game_state->player_x = game_state->level.spawn_x;
    game_state->player_y = game_state->level.spawn_y;
    game_state->prev_player_x = game_state->player_x;
    game_state->prev_player_y = game_state->player_y;
    game_state->velocity = {};
    game_state->air_jumps_left = PLAYER_AIR_JUMP_COUNT;
    game_state->state = PSTATE_FALL;
    initialize_movers(game_state);
    game_state->prev_movers = game_state->movers;
    initialize_monster(game_state);
    game_state->prev_monster = game_state->monster;

    game_state->camera.pos_x = game_state->player_x;
    game_state->camera.pos_y = game_state->player_y;
    // 立刻贴合到（已夹取的）目标位置：dt=0 表示不平滑。
    // 否则开头几帧会看到相机从出生点未夹取的位置滑向合法位置
    camera_follow(&game_state->camera, game_state->player_x, game_state->player_y,
                  &game_state->level.bounds, viewport_width, viewport_height, 0.0f,
                  camera_profile_for());
    game_state->prev_camera = game_state->camera;

    debug_register_level(&game_state->level);
    return true;
}

const char *player_state_name(PlayerState state)
{
    switch (state) {
    case PSTATE_IDLE:
        return "IDLE";
    case PSTATE_RUN:
        return "RUN";
    case PSTATE_JUMP:
        return "JUMP";
    case PSTATE_FALL:
        return "FALL";
    case PSTATE_DASH:
        return "DASH";
    default:
        return "?";
    }
}

const char *game_world_name(WorldId world)
{
    switch (world) {
    case WORLD_FIRST:
        return "FIRST";
    case WORLD_SECOND:
        return "SECOND";
    default:
        return "?";
    }
}

// ============================================================================
// 状态快照与关卡指纹
//
// 存档（include/save.h）、录制回放与输入脚本共用这一份。**字段表就是唯一来源**
// （include/game.h 的 GAME_STATE_PERSISTENT_FIELDS）：快照结构体、写档、读档都由它生成，
// 所以「结构体加了字段而 save/load 忘了抄」这种不对称在结构上不可能发生。
// 真正需要人判断的只剩一件事：新字段该不该进状态（跨帧会变、且影响后续演化）。
// ============================================================================

void game_state_save(const GameState *game_state, GameStateSnapshot *out)
{
    // 整体清零：连填充字节一起确定下来，同一个状态编出来的字节才逐位一致（便于 diff 与对拍）
    *out = {};

#define GAME_STATE_FIELD_SAVE(type, name) out->name = game_state->name;
    GAME_STATE_PERSISTENT_FIELDS(GAME_STATE_FIELD_SAVE)
#undef GAME_STATE_FIELD_SAVE
}

internal bool snapshot_bool_valid(const bool *value)
{
    // 存档文件的字节可以绕过 C++ 的 bool 写入规则，所以在读取 bool 值前检查其原始表示。
    return *(const u8 *)value <= 1;
}

bool game_state_snapshot_valid(const GameStateSnapshot *snapshot)
{
    if ((u8)snapshot->facing >= FACE_COUNT || (u8)snapshot->state >= PSTATE_COUNT ||
        (u8)snapshot->world >= WORLD_COUNT || (u8)snapshot->teleport.phase >= TELEPORT_PHASE_COUNT) {
        return false;
    }
    // 数量是存档里的一个字节序列，越界就拒载（它决定下面循环走几项）
    if (snapshot->movers.count > LEVEL_ASSET_MAX_MOVERS) {
        return false;
    }

    bool valid = snapshot_bool_valid(&snapshot->grounded) &&
                 snapshot_bool_valid(&snapshot->monster.active) &&
                 snapshot_bool_valid(&snapshot->monster.time_slowed) &&
                 snapshot_bool_valid(&snapshot->time_stop.active) &&
                 snapshot_bool_valid(&snapshot->ui.open) &&
                 snapshot_bool_valid(&snapshot->world_transition_active);
    for (u32 i = 0; i < snapshot->movers.count; ++i) {
        valid = valid && snapshot_bool_valid(&snapshot->movers.items[i].time_slowed) &&
                snapshot_bool_valid(&snapshot->movers.items[i].damaging) &&
                snapshot->movers.items[i].shape < LEVEL_MOVER_SHAPE_COUNT;
    }
    // 选中项下标是数组/列表下标，越界就拒载（与 phase 那几项同一类检查）
    valid = valid && snapshot->ui.selected < UI_MAX_ITEMS;
    for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
        valid = valid && snapshot_bool_valid(&snapshot->projectiles.items[i].active);
    }
    for (u32 i = 0; i < MAX_VANISH_BLOCKS; ++i) {
        valid = valid && (u8)snapshot->vanish.blocks[i].phase < VANISH_PHASE_COUNT;
    }
    return valid;
}

bool game_state_load(GameState *game_state, const GameStateSnapshot *snapshot)
{
    if (!game_state_snapshot_valid(snapshot)) {
        return false;
    }

#define GAME_STATE_FIELD_LOAD(type, name) game_state->name = snapshot->name;
    GAME_STATE_PERSISTENT_FIELDS(GAME_STATE_FIELD_LOAD)
#undef GAME_STATE_FIELD_LOAD

    // level 是「当前世界」的一份轻量引用副本，所以要在 world 被装回去之后再重新绑定
    game_state->level = game_state->worlds[game_state->world];

    // 渲染插值在「上一逻辑步」与「当前逻辑步」之间取点：读档后先把历史位置对齐，
    // 否则恢复的那一帧会从旧位置滑过来（画面上一闪）
    game_state->prev_player_x = game_state->player_x;
    game_state->prev_player_y = game_state->player_y;
    game_state->prev_camera = game_state->camera;
    game_state->prev_movers = game_state->movers;
    game_state->prev_monster = game_state->monster;
    game_state->prev_projectiles = game_state->projectiles;
    debug_register_level(&game_state->level);
    return true;
}

// 自检：save → load → save 必须逐位一致。
// 它证明的是「load 把 save 写出的每一位都装回去了」（结构保证之外的最后一网）；
// 而「本该进列表却漏了某个字段」属于语义判断，只能靠行为层面的确定性守卫
// （test/run_tests.bat 把同一条磁带跑两遍对拍 trace）发现 —— 别把这条自检当成全部。
bool game_state_snapshot_selftest(const GameState *game_state)
{
    GameStateSnapshot first = {};
    game_state_save(game_state, &first);

    // 浅拷贝就够：这份副本只是用来跑一遍 load，不会当真正的游戏状态使用
    GameState restored = *game_state;
    if (!game_state_load(&restored, &first)) {
        return false;
    }

    GameStateSnapshot second = {};
    game_state_save(&restored, &second);
    return memcmp(&first, &second, sizeof(GameStateSnapshot)) == 0;
}

// ============================================================================
// 关卡几何指纹（存档的「这份存档还配得上现在的关卡吗」那一重校验）
// ============================================================================

internal u32 fingerprint_rect(u32 hash, const Rect2D *rect)
{
    hash = fnv1a_value(hash, rect->center_x);
    hash = fnv1a_value(hash, rect->center_y);
    hash = fnv1a_value(hash, rect->half_w);
    hash = fnv1a_value(hash, rect->half_h);
    return hash;
}

// 每个世界都要哈希：存档里记着「当前在哪个世界」，任一张地图改了都该让旧存档失效
internal u32 fingerprint_world(u32 hash, const Level *level)
{
    hash = fnv1a_value(hash, level->spawn_x);
    hash = fnv1a_value(hash, level->spawn_y);
    hash = fingerprint_rect(hash, &level->bounds);

    hash = fnv1a_value(hash, level->platform_count);
    for (u32 i = 0; i < level->platform_count; ++i) {
        hash = fingerprint_rect(hash, &level->platforms[i].rect);
        hash = fnv1a_value(hash, (u32)level->platforms[i].kind);
    }

    hash = fnv1a_value(hash, level->spike_count);
    for (u32 i = 0; i < level->spike_count; ++i) {
        hash = fingerprint_rect(hash, &level->spikes[i]);
    }

    // 可消失平台：块的位置就是「可踩的几何」，改了它存档同样失去意义
    // （相位/计时器是运行态，不进指纹）
    hash = fnv1a_value(hash, level->vanish_platform_count);
    for (u32 i = 0; i < level->vanish_platform_count; ++i) {
        hash = fingerprint_rect(hash, &level->vanish_platforms[i]);
    }

    hash = fnv1a_value(hash, level->mover_count);
    for (u32 i = 0; i < level->mover_count; ++i) {
        const LevelMoverAsset *asset = &level->movers[i];
        hash = fnv1a_value(hash, asset->point_a.x);
        hash = fnv1a_value(hash, asset->point_a.y);
        hash = fnv1a_value(hash, asset->point_b.x);
        hash = fnv1a_value(hash, asset->point_b.y);
        hash = fnv1a_value(hash, asset->t0);
        hash = fnv1a_value(hash, asset->half_w);
        hash = fnv1a_value(hash, asset->half_h);
        hash = fnv1a_value(hash, asset->speed);
        hash = fnv1a_value(hash, asset->shape);
        hash = fnv1a_value(hash, asset->damaging);
    }

    hash = fnv1a_value(hash, level->monster_count);
    for (u32 i = 0; i < level->monster_count; ++i) {
        const LevelMonsterAsset *asset = &level->monsters[i];
        hash = fingerprint_rect(hash, &asset->rect);
        hash = fnv1a_value(hash, asset->spawn_x);
        hash = fnv1a_value(hash, asset->spawn_y);
        hash = fnv1a_value(hash, asset->velocity_x);
    }

    // 传送门：位置与配对改了，存档里的位置同样失去意义
    hash = fnv1a_value(hash, level->portal_count);
    for (u32 i = 0; i < level->portal_count; ++i) {
        const LevelPortalAsset *portal = &level->portals[i];
        hash = fingerprint_rect(hash, &portal->rect);
        hash = fnv1a_value(hash, portal->pair_id);
    }

    // 门：存档不存「穿到哪」，但入口落点改了之后，存档里的位置同样失去意义
    hash = fnv1a_value(hash, level->has_left_connection);
    if (level->has_left_connection) {
        hash = fnv1a_value(hash, (u32)level->left_connection.target_world);
        hash = fnv1a_value(hash, level->left_connection.entry_x);
        hash = fnv1a_value(hash, level->left_connection.entry_y);
        hash = fnv1a_value(hash, (u32)level->left_connection.facing);
    }
    hash = fnv1a_value(hash, level->has_right_connection);
    if (level->has_right_connection) {
        hash = fnv1a_value(hash, (u32)level->right_connection.target_world);
        hash = fnv1a_value(hash, level->right_connection.entry_x);
        hash = fnv1a_value(hash, level->right_connection.entry_y);
        hash = fnv1a_value(hash, (u32)level->right_connection.facing);
    }

    return hash;
}

u32 game_level_fingerprint(const GameState *game_state)
{
    u32 hash = FNV1A_OFFSET_BASIS;
    for (u32 world = 0; world < WORLD_COUNT; ++world) {
        hash = fingerprint_world(hash, &game_state->worlds[world]);
    }
    return hash;
}

// ============================================================================
// 角色运动
// ============================================================================

// 圆形径向死区 + 线性重映射：摇杆到中心点的距离小于死区时整体归零
internal v2 stick_to_dir(f32 x, f32 y, f32 deadzone)
{
    v2 result = {};
    // 先在平方域比死区：摇杆静止（或只有漂移）是最常见的情形，这样能省掉那次 sqrtf
    // —— 这个函数每个逻辑步都被无条件调用，而边界处正好是 0（见调用点）。
    f32 len_sq = x * x + y * y;
    if (len_sq <= deadzone * deadzone) {
        return result;
    }
    f32 len = sqrtf(len_sq);
    f32 scale = (len - deadzone) / (1.0f - deadzone);
    result = v2{ (x / len) * scale, (y / len) * scale };
    return result;
}

internal void player_collider_box(const GameState *game_state, Rect2D *box)
{
    *box = make_rect_center(game_state->player_x + game_state->player_collider.offset_x,
                            game_state->player_y + game_state->player_collider.offset_y,
                            game_state->player_collider.width,
                            game_state->player_collider.height);
}

// 资产里的出生点 / 门落点 / 传送点存的都是**脚底**坐标，而角色位置是中心：这里是唯一的换算处。
// 两处各写一遍的话，「脚底贴地」这件事迟早会在某一处退化成「中心贴地」（相差约 63px）。
internal f32 player_center_y_from_feet(const GameState *game_state, f32 feet_y)
{
    return feet_y - game_state->player_collider.offset_y + game_state->player_collider.height * 0.5f;
}

internal void player_respawn(GameState *game_state)
{
    game_state->player_x = game_state->level.spawn_x;
    game_state->player_y = game_state->level.spawn_y;
    game_state->velocity = {};
    game_state->grounded = false;
    game_state->state = PSTATE_FALL;
    game_state->dash_timer = 0.0f;
    game_state->dash_cooldown = 0.0f;
    game_state->coyote_timer = 0.0f;
    game_state->jump_buffer_timer = 0.0f;
    game_state->drop_through_timer = 0.0f;
    game_state->air_jumps_left = PLAYER_AIR_JUMP_COUNT;
    game_state->world_transition_active = false;
}

void game_teleport_to_waypoint(GameState *game_state, WorldId target_world, u32 waypoint_index,
                               u32 viewport_width, u32 viewport_height)
{
    if ((u32)target_world >= WORLD_COUNT) {
        return;
    }
    if (waypoint_index >= game_state->worlds[target_world].waypoint_count) {
        return;
    }
    if (target_world != game_state->world) {
        enter_world(game_state, target_world);
    }
    // enter_world 之后 level 已经指向目标世界，所以这里读它（与 worlds[] 是同一份数据）
    const LevelWaypointAsset *waypoint = &game_state->level.waypoints[waypoint_index];
    place_player_at_feet(game_state, waypoint->x, waypoint->y, viewport_width, viewport_height);
}

// 可消失平台的两个时长（秒）：踩住够久之后先变暗预告这么久（也是给玩家的容错窗口），
// 然后消失这么久。改这两个数会影响手感和回归取样（断言按帧号写）。
inline constexpr f32 VANISH_WARNING_SECONDS = 2.0f;
inline constexpr f32 VANISH_GONE_SECONDS = 2.0f;

// 触发门槛：要**连续**踩住这么多逻辑步（60Hz → 10 帧 ≈ 0.17 秒）才开始预告。
// 为什么不是“碰到就触发”：判定用的是“盒下移 1px 后重叠”，所以从边缘蹭过去、
// 斜着擦一下顶边、冲刺横穿一格都会命中 —— 太敏感会让每次路过都白白消耗掉一块地形。
// 离开则计数清零（要“连续”），一旦够了帧数就不可回退（见下面 WARNING 分支）。
inline constexpr u16 VANISH_HOLD_FRAMES = 10;

// 每逻辑步推进可消失平台的相位。
//
// 两条判据都不依赖「撞上了哪块平台」的指针归属（碰撞列表是每步重新拼的副本，
// 拿它的指针去反查下标很脆），而是直接用矩形关系表达：
//   触发 = **连续踩住** VANISH_HOLD_FRAMES 帧（“踩在上面” = 盒下移 GROUND_PROBE_DEPTH 后重叠，
//          与地面探针同一判据，所以“站住”才算；离开就把计数清零）
//   恢复 = 玩家盒没压在它里面 —— 否则恢复瞬间会把玩家卡进块里
// 触发是**不可逆**的：一旦进入预告期，倒计时一定走完（见下面 WARNING 分支）。
// 必须在 build_collision_platforms 之前调用：相位决定这一帧它算不算碰撞体。
internal void update_vanish_platforms(GameState *game_state, f32 dt)
{
    const Level *level = &game_state->level;
    assert(level->vanish_platform_count <= MAX_VANISH_BLOCKS);

    Rect2D box = {};
    player_collider_box(game_state, &box);
    Rect2D probe = box;
    probe.center_y -= GROUND_PROBE_DEPTH;

    for (u32 i = 0; i < level->vanish_platform_count; ++i) {
        const Rect2D *rect = &level->vanish_platforms[i];
        VanishBlock *block = &game_state->vanish.blocks[i];
        bool standing_on = test_rect_overlap(&probe, rect);

        switch (block->phase) {
        case VANISH_PHASE_SOLID:
            if (!standing_on) {
                block->hold_frames = 0; // 要“连续”踩住：离开就重新数
            } else if (block->hold_frames >= VANISH_HOLD_FRAMES - 1) {
                block->phase = VANISH_PHASE_WARNING;
                block->hold_frames = 0;
                block->timer = VANISH_WARNING_SECONDS;
            } else {
                ++block->hold_frames;
            }
            break;
        case VANISH_PHASE_WARNING:
            // 不可回退：踩上就一定会消失，离开也不会重置（“踩一下就走”照样走完整个倒计时）。
            // 玩家在预告期离不离开只影响他自己，不影响这块的去向 —— 预告期是纯倒计时。
            block->timer -= dt;
            if (block->timer <= 0.0f) {
                block->phase = VANISH_PHASE_GONE;
                block->timer = VANISH_GONE_SECONDS;
            }
            break;
        case VANISH_PHASE_GONE:
            block->timer -= dt;
            // 玩家还压在块里就先不恢复（否则他会被卡在半空中/块里面）
            if (block->timer <= 0.0f && !test_rect_overlap(&box, rect)) {
                block->phase = VANISH_PHASE_SOLID;
                block->timer = 0.0f;
            }
            break;
        default:
            assert(!"非法的可消失平台相位（读档校验没拦住）");
            block->phase = VANISH_PHASE_SOLID;
            block->timer = 0.0f;
            break;
        }
    }
}

// 传送的三段时长（秒）：淡出 → 加载 → 淡入，合计 1.0s。改这几个数会影响回归取样（断言按帧号写）。
inline constexpr f32 TELEPORT_FADE_OUT_SECONDS = 0.3f;
inline constexpr f32 TELEPORT_LOADING_SECONDS = 0.4f;
inline constexpr f32 TELEPORT_FADE_IN_SECONDS = 0.3f;

// 落点里「优先弹出贴墙」：门有可能被摆进墙体（底边低于地面/平台顶面），角色就会被塞进实体里 ——
// 而传送期间整步不走物理，等解算器去挤它是不行的（它会顺着重力方向被挤出去，对单向平台就是从
// 下方穿过去掉下关卡）。所以这里在落点处一次把脚底顶到「角色盒压到的最上面那块平台的顶面」，
// 也就是离它最近的那层地面（角色盒高约 90px，最多跨两格，所以“最上面那块”必然就是它上面那层面）。
// 单向平台也要算进去：站在它上面本来就是它的正常用法（实测踩过 —— 门摆在第 13 行单向平台上、
// 底边比平台顶面低 32px，当时只看 COLLIDER_SOLID，结果角色被解算器挤穿平台掉了 ~400px）。
// 顶完再查一遍（防两层厚墙只顶穿一层），最多几轮，避免病态摆法把角色推到天上。
internal void teleport_depenetrate_up(GameState *game_state)
{
    Platform platforms[MAX_COLLISION_PLATFORMS] = {};
    const u32 platform_count = build_collision_platforms(game_state, platforms);

    for (u32 pass = 0; pass < 4; ++pass) {
        Rect2D box = {};
        player_collider_box(game_state, &box);

        bool found = false;
        f32 target_top = 0.0f;
        for (u32 i = 0; i < platform_count; ++i) {
            if (!test_rect_overlap(&box, &platforms[i].rect)) {
                continue;
            }
            f32 top = platforms[i].rect.center_y + platforms[i].rect.half_h;
            if (!found || top > target_top) {
                target_top = top;
                found = true;
            }
        }
        if (!found) {
            break;
        }
        // 脚底（box.center_y - box.half_h）贴到 target_top：按同一个盒子算出位移再作用到角色上
        game_state->player_y += target_top - (box.center_y - box.half_h);
    }
}

// 落点：把「水平坐标 + **脚底** y」应用到角色上。传送门（矩形底边）与传送点（一个点）都用它 ——
// 落点规则因此只有这一处实现，不会出现「门贴底边、点贴中心」这种各自漂的情况。
// 顺序：位置换算 → 顶出墙体 → 归位 → 相机贴合（相机不平滑过去，否则会看到它从旧位置滑）。
internal void place_player_at_feet(GameState *game_state, f32 x, f32 feet_y, u32 viewport_width,
                                   u32 viewport_height)
{
    game_state->player_x = x;
    game_state->player_y = player_center_y_from_feet(game_state, feet_y);
    teleport_depenetrate_up(game_state); // 落点被埋进墙里时优先把人顶到墙顶上
    game_state->velocity = {};
    game_state->grounded = false;
    game_state->state = PSTATE_FALL;
    game_state->dash_timer = 0.0f;
    game_state->coyote_timer = 0.0f;
    game_state->jump_buffer_timer = 0.0f;
    game_state->drop_through_timer = 0.0f;
    game_state->air_jumps_left = PLAYER_AIR_JUMP_COUNT;
    game_state->world_transition_active = false;

    // 相机直接贴合：传送的意义就是「不该看到它从旧位置滑过去」（dt = 0）
    snap_camera_to_player(game_state, viewport_width, viewport_height);
}

// 推进传送相位。位置只在「淡出结束」那一步换到目标门上 —— 所以加载态与淡入都是在目标门上演的。
internal void update_teleport(GameState *game_state, f32 dt, u32 viewport_width, u32 viewport_height)
{
    Teleport *teleport = &game_state->teleport;
    teleport->timer -= dt;
    if (teleport->timer > 0.0f) {
        return;
    }

    switch (teleport->phase) {
    case TELEPORT_FADE_OUT:
        if (teleport->dest_portal < game_state->level.portal_count) {
            // 门的落点 = 矩形底边（与传送点用的 place_player_at_feet 同一条规则）
            const Rect2D *rect = &game_state->level.portals[teleport->dest_portal].rect;
            place_player_at_feet(game_state, rect->center_x, rect->center_y - rect->half_h, viewport_width,
                                 viewport_height);
        }
        teleport->phase = TELEPORT_LOADING;
        teleport->timer = TELEPORT_LOADING_SECONDS;
        break;
    case TELEPORT_LOADING:
        teleport->phase = TELEPORT_FADE_IN;
        teleport->timer = TELEPORT_FADE_IN_SECONDS;
        break;
    case TELEPORT_FADE_IN:
        teleport->phase = TELEPORT_NONE;
        teleport->timer = 0.0f;
        break;
    default:
        teleport->phase = TELEPORT_NONE;
        teleport->timer = 0.0f;
        break;
    }
}

// 当前相位走完了多少（0..1）。时长常量只住在这个文件里，渲染侧拿它去推动画帧号。
f32 game_teleport_phase_progress(const Teleport *teleport)
{
    switch (teleport->phase) {
    case TELEPORT_FADE_OUT:
        return 1.0f - teleport->timer / TELEPORT_FADE_OUT_SECONDS;
    case TELEPORT_LOADING:
        return 1.0f - teleport->timer / TELEPORT_LOADING_SECONDS;
    case TELEPORT_FADE_IN:
        return 1.0f - teleport->timer / TELEPORT_FADE_IN_SECONDS;
    case TELEPORT_NONE:
    case TELEPORT_PHASE_COUNT:
        break;
    }
    return 0.0f;
}

// 使用传送门：**角色中心站在门矩形里**时按上键（GA_UP 的上升沿）传送到同色号的另一扇。
//
// 为什么中心算而不是碰撞盒重叠：半个身子探进门里不算「站在门里」，中心判据更符合直觉，
// 也不会因为碰撞盒比门宽（盒 70px、门 64px）而在门边上就生效。
// 为什么不做「按住 N 帧」那种触发器：上键本身就是一次明确的操作（边沿只来一次、按住不重复），
// 不像“踩到地形”那样会被路过误碰 —— 所以这里不需要门槛，也不需要“刚用过就抑制”。
internal void update_portals(GameState *game_state, const PlayerInput *controller)
{
    const Level *level = &game_state->level;
    Teleport *teleport = &game_state->teleport;
    if (teleport->phase != TELEPORT_NONE || level->portal_count == 0 || !controller->pressed[GA_UP]) {
        return;
    }

    for (u32 i = 0; i < level->portal_count; ++i) {
        const Rect2D *rect = &level->portals[i].rect;
        bool inside = game_state->player_x > rect->center_x - rect->half_w &&
                      game_state->player_x < rect->center_x + rect->half_w &&
                      game_state->player_y > rect->center_y - rect->half_h &&
                      game_state->player_y < rect->center_y + rect->half_h;
        if (!inside) {
            continue;
        }
        // 同色号的另一扇门（校验保证每个色号恰好两扇；真坏了就什么都不做）
        u32 pair_id = level->portals[i].pair_id;
        for (u32 j = 0; j < level->portal_count; ++j) {
            if (j != i && level->portals[j].pair_id == pair_id) {
                teleport->phase = TELEPORT_FADE_OUT;
                teleport->timer = TELEPORT_FADE_OUT_SECONDS;
                teleport->dest_portal = j;
                break;
            }
        }
        break;
    }
}

internal u32 build_collision_platforms(const GameState *game_state, Platform *out)
{
    assert(game_state->level.platform_count + game_state->movers.count +
               game_state->level.vanish_platform_count <=
           MAX_COLLISION_PLATFORMS);
    u32 count = 0;
    for (u32 i = 0; i < game_state->level.platform_count; ++i) {
        out[count++] = game_state->level.platforms[i];
    }
    // 移动组件都是实心的（圆形暂时也不可穿过）
    for (u32 i = 0; i < game_state->movers.count; ++i) {
        out[count].rect = game_state->movers.items[i].rect;
        out[count].kind = COLLIDER_SOLID;
        ++count;
    }
    // 可消失平台：只在「不是消失中」的时候进列表 —— 「不可碰撞」就是「不在这个数组里」，
    // 比在 collision.cc 里加一种“幽灵 collider”简单（解算器不用认识它）。
    for (u32 i = 0; i < game_state->level.vanish_platform_count; ++i) {
        if (game_state->vanish.blocks[i].phase == VANISH_PHASE_GONE) {
            continue;
        }
        out[count].rect = game_state->level.vanish_platforms[i];
        out[count].kind = COLLIDER_SOLID;
        ++count;
    }
    return count;
}

internal bool time_stop_affects_rect(const TimeStop *time_stop, const Rect2D *rect)
{
    if (!time_stop->active) {
        return false;
    }

    f32 min_x = rect->center_x - rect->half_w;
    f32 max_x = rect->center_x + rect->half_w;
    f32 min_y = rect->center_y - rect->half_h;
    f32 max_y = rect->center_y + rect->half_h;
    f32 closest_x = MAX(min_x, MIN(time_stop->center_x, max_x));
    f32 closest_y = MAX(min_y, MIN(time_stop->center_y, max_y));
    f32 dx = time_stop->center_x - closest_x;
    f32 dy = time_stop->center_y - closest_y;
    return dx * dx + dy * dy <= time_stop->radius * time_stop->radius;
}

// 圆形移动组件的「碰到就受伤」判据：圆心到玩家盒的最近距离 <= 半径（含一点容差）。
// 容差是必须的：它是实心的，滑动碰撞会把玩家推到**刚好贴住**的位置，
// 纯 `<` 会因为浮点尾差漏掉那一帧（表现是“贴着圆走却不受伤”）。
internal bool mover_circle_touches_box(const Mover *mover, const Rect2D *box)
{
    f32 closest_x = MAX(box->center_x - box->half_w, MIN(mover->rect.center_x, box->center_x + box->half_w));
    f32 closest_y = MAX(box->center_y - box->half_h, MIN(mover->rect.center_y, box->center_y + box->half_h));
    f32 dx = mover->rect.center_x - closest_x;
    f32 dy = mover->rect.center_y - closest_y;
    f32 radius = mover->half_w + MOVER_CIRCLE_CONTACT_TOLERANCE;
    return dx * dx + dy * dy <= radius * radius;
}

// 移动组件：沿轴推进 + 方形驮人。
// 「这一步花了多少时间」换算成轴参数 t 的增量（speed * dt / 轴长），斜轴不会跑偏；
// 水平轴上它与 center_x += speed * dt 逐帧等价，所以旧图轨迹不变（基线用例依赖这一点）。
internal void update_movers(GameState *game_state, f32 dt)
{
    Rect2D player_box = {};
    player_collider_box(game_state, &player_box);
    Rect2D ground_probe = player_box;
    ground_probe.center_y -= GROUND_PROBE_DEPTH;

    for (u32 i = 0; i < game_state->movers.count; ++i) {
        Mover *mover = &game_state->movers.items[i];
        mover->time_slowed = time_stop_affects_rect(&game_state->time_stop, &mover->rect);
        f32 move_dt = mover->time_slowed ? dt * TIME_STOP_SPEED_SCALE : dt;

        v2 axis = mover->point_b - mover->point_a;
        f32 axis_length = sqrtf(axis.length_sq());
        if (axis_length <= 0.0f) {
            continue; // 端点重合（校验会报 ERROR，这里只是不让它除零）
        }

        mover->t += (mover->speed * mover->direction * move_dt) / axis_length;
        if (mover->t >= 1.0f) {
            mover->t = 1.0f;
            mover->direction = -1.0f;
        } else if (mover->t <= 0.0f) {
            mover->t = 0.0f;
            mover->direction = 1.0f;
        }

        f32 old_x = mover->rect.center_x;
        f32 old_y = mover->rect.center_y;
        Rect2D old_rect = mover->rect; // 这一步**移动之前**的几何
        mover->rect.center_x = mover->point_a.x + axis.x * mover->t;
        mover->rect.center_y = mover->point_a.y + axis.y * mover->t;

        // 驮人只对方形平台：圆形是伤害体，把它当驮体就永远碰不到人了。
        // 驮行会把角色两个轴都挪走，所以「站在上面被带着走」是安全的（不会受伤）。
        if (mover->shape != LEVEL_MOVER_SQUARE) {
            continue;
        }
        // 判定要用**移动前**的几何：角色在帧初站的是旧位置，而这一步平台已经走开了 ——
        // 拿新矩形判的话，平台朝下走的那一段会直接判成“没接触”，角色就被留在原地掉下去
        // （实测：斜轴平台到端点反向的下一帧掉下来）。
        bool carries_player = test_rect_overlap(&ground_probe, &old_rect) &&
                              player_box.center_y >= old_rect.center_y;
        if (carries_player) {
            f32 dx = mover->rect.center_x - old_x;
            f32 dy = mover->rect.center_y - old_y;
            game_state->player_x += dx;
            game_state->player_y += dy;
            // 探针与玩家盒跟着走：否则后面的组件看到的是旧位置（多组件时的顺序依赖）
            player_box.center_x += dx;
            player_box.center_y += dy;
            ground_probe.center_x += dx;
            ground_probe.center_y += dy;
        }
    }
}

internal void monster_respawn(Monster *monster)
{
    monster->rect.center_x = monster->spawn_x;
    monster->rect.center_y = monster->spawn_y;
    monster->velocity = v2{ monster->spawn_velocity_x, 0.0f };
    monster->time_slowed = false;
    ++monster->respawn_count;
}

internal void update_monster(GameState *game_state, f32 dt)
{
    Monster *monster = &game_state->monster;
    if (!monster->active) {
        return;
    }

    monster->time_slowed = time_stop_affects_rect(&game_state->time_stop, &monster->rect);
    f32 move_dt = monster->time_slowed ? dt * TIME_STOP_SPEED_SCALE : dt;
    monster->velocity.y -= PLAYER_GRAVITY * move_dt;
    monster->velocity.y = MAX(monster->velocity.y, -PLAYER_MAX_FALL_SPEED);
    move_and_collide(&monster->rect, &monster->velocity, move_dt,
                     game_state->level.platforms, game_state->level.platform_count, false);

    f32 level_bottom = game_state->level.bounds.center_y - game_state->level.bounds.half_h;
    if (monster->rect.center_y + monster->rect.half_h < level_bottom) {
        monster_respawn(monster);
    }
}

internal void update_projectiles(GameState *game_state, const Platform *platforms, u32 platform_count, f32 dt)
{
    ProjectilePool *pool = &game_state->projectiles;
    if (pool->cooldown > 0.0f) {
        pool->cooldown -= dt;
        if (pool->cooldown < 0.0f) {
            pool->cooldown = 0.0f;
        }
    }

    for (u32 projectile_index = 0; projectile_index < MAX_PROJECTILES; ++projectile_index) {
        Projectile *projectile = &pool->items[projectile_index];
        if (!projectile->active) {
            continue;
        }

        projectile->rect.center_x += projectile->velocity_x * dt;
        for (u32 i = 0; i < platform_count; ++i) {
            if (test_rect_overlap(&projectile->rect, &platforms[i].rect)) {
                projectile->active = false;
                break;
            }
        }

        Monster *monster = &game_state->monster;
        if (projectile->active && monster->active && test_rect_overlap(&projectile->rect, &monster->rect)) {
            monster_respawn(monster);
            projectile->active = false;
        }
    }
}

internal void try_fire_projectile(GameState *game_state)
{
    ProjectilePool *pool = &game_state->projectiles;
    if (pool->cooldown > 0.0f) {
        return;
    }

    Projectile *projectile = nullptr;
    for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
        if (!pool->items[i].active) {
            projectile = &pool->items[i];
            break;
        }
    }
    if (!projectile) {
        return;
    }

    f32 direction = (game_state->facing == FACE_RIGHT) ? 1.0f : -1.0f;
    Rect2D player_box = {};
    player_collider_box(game_state, &player_box);
    projectile->active = true;
    projectile->rect = make_rect_center(
        player_box.center_x + direction * (player_box.half_w + PROJECTILE_WIDTH * 0.5f + 1.0f),
        player_box.center_y, PROJECTILE_WIDTH, PROJECTILE_HEIGHT);
    projectile->velocity_x = direction * PROJECTILE_SPEED;
    pool->cooldown = PROJECTILE_COOLDOWN;
}

internal void update_world_transition(GameState *game_state, f32 dt)
{
    f32 direction = (game_state->transition_target_x >= game_state->player_x) ? 1.0f : -1.0f;
    game_state->player_x += direction * PLAYER_TRANSITION_SPEED * dt;
    game_state->player_y = game_state->transition_target_y;
    game_state->facing = (direction > 0.0f) ? FACE_RIGHT : FACE_LEFT;
    game_state->state = PSTATE_RUN;

    bool arrived = (direction > 0.0f) ? (game_state->player_x >= game_state->transition_target_x)
                                      : (game_state->player_x <= game_state->transition_target_x);
    if (arrived) {
        game_state->player_x = game_state->transition_target_x;
        game_state->player_y = game_state->transition_target_y;
        game_state->world_transition_active = false;
        game_state->grounded = true;
    }
}

void game_update(GameInput *game_input, GameState *game_state, f32 dt, u32 viewport_width, u32 viewport_height)
{
    // UI 层先跑：打开时它在世界之上（整个世界暂停），关闭的那一步也要跳过游戏逻辑
    // —— 否则“按确认”的 GA_JUMP 会在同一步漏给角色，多跳一下。
    if (ui_update(game_state, game_input, viewport_width, viewport_height)) {
        return;
    }

    animation_update(&game_state->player_bagdown_animation, dt);

    TimeStop *time_stop = &game_state->time_stop;
    if (time_stop->timer > 0.0f) {
        time_stop->timer -= dt;
        if (time_stop->timer <= 0.0f) {
            time_stop->timer = 0.0f;
            time_stop->active = false;
        }
    }
    if (time_stop->cooldown > 0.0f) {
        time_stop->cooldown -= dt;
        if (time_stop->cooldown < 0.0f) {
            time_stop->cooldown = 0.0f;
        }
    }

    if (!game_state->world_transition_active && game_input->player.pressed[GA_TIME_STOP] &&
        time_stop->cooldown <= 0.0f) {
        time_stop->active = true;
        time_stop->center_x = game_state->player_x;
        time_stop->center_y = game_state->player_y;
        time_stop->radius = TIME_STOP_RADIUS;
        time_stop->timer = TIME_STOP_DURATION;
        time_stop->cooldown = TIME_STOP_COOLDOWN;
    }

    update_movers(game_state, dt);
    update_monster(game_state, dt);

    if (game_state->teleport.phase != TELEPORT_NONE) {
        // 传送期间整步只推进相位：不吃输入、也不走物理（三段一共 1.0s；落点已保证脚底贴门底边）。
        // 它是「真正传送」的机制 —— 与穿门过渡（world_transition 那套自动走入）刻意不复用。
        update_teleport(game_state, dt, viewport_width, viewport_height);
        camera_follow(&game_state->camera, game_state->player_x, game_state->player_y,
                      &game_state->level.bounds, viewport_width, viewport_height, dt, camera_profile_for());
#if MONO_DEBUG_VIS
        Rect2D teleport_box = {};
        player_collider_box(game_state, &teleport_box);
        debug_vis_box(&teleport_box, DEBUG_BOX_PLAYER);
#endif
        return;
    }

    if (game_state->world_transition_active) {
        // 过渡期锁定控制且无敌：这是目标世界做延迟资源初始化的预算窗口。
        update_world_transition(game_state, dt);
        camera_follow(&game_state->camera, game_state->player_x, game_state->player_y,
                      &game_state->level.bounds, viewport_width, viewport_height, dt,
                      camera_profile_for());
#if MONO_DEBUG_VIS
        Rect2D player_box = {};
        player_collider_box(game_state, &player_box);
        debug_vis_box(&player_box, DEBUG_BOX_PLAYER);
        if (game_state->movers.count > 0) {
            for (u32 i = 0; i < game_state->movers.count; ++i) {
                debug_vis_box(&game_state->movers.items[i].rect, DEBUG_BOX_MOVING_PLATFORM);
            }
        }
        if (game_state->monster.active) {
            debug_vis_box(&game_state->monster.rect, DEBUG_BOX_MONSTER);
        }
        for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
            if (game_state->projectiles.items[i].active) {
                debug_vis_box(&game_state->projectiles.items[i].rect, DEBUG_BOX_PROJECTILE);
            }
        }
#endif
        return;
    }

    PlayerInput *controller = &game_input->player;
    // 可消失平台的相位要在拼碰撞数组之前更新（它决定这一帧算不算碰撞体）
    update_vanish_platforms(game_state, dt);
    Platform collision_platforms[MAX_COLLISION_PLATFORMS] = {};
    const u32 platform_count = build_collision_platforms(game_state, collision_platforms);
    const Platform *platforms = collision_platforms;

    if (controller->pressed[GA_SHOOT]) {
        try_fire_projectile(game_state);
    }
    update_projectiles(game_state, platforms, platform_count, dt);

    // ---------------------------------------------------------------------
    // 计时器
    // ---------------------------------------------------------------------
    if (game_state->dash_cooldown > 0.0f) {
        game_state->dash_cooldown -= dt;
    }
    if (game_state->coyote_timer > 0.0f) {
        game_state->coyote_timer -= dt;
    }
    if (game_state->jump_buffer_timer > 0.0f) {
        game_state->jump_buffer_timer -= dt;
    }
    if (game_state->drop_through_timer > 0.0f) {
        game_state->drop_through_timer -= dt;
    }

    // ---------------------------------------------------------------------
    // 水平输入：前后移动，摇杆优先
    // ---------------------------------------------------------------------
    f32 move_axis = 0.0f;
    if (controller->current[GA_LEFT]) {
        move_axis -= 1.0f;
    }
    if (controller->current[GA_RIGHT]) {
        move_axis += 1.0f;
    }

    v2 stick = stick_to_dir(controller->left_stick_x, controller->left_stick_y, STICK_DEADZONE);
    if (stick.x != 0.0f) {
        move_axis = stick.x;
    }

    // 「下」有两个入口：
    //   1. 键盘的 ↓ / S、手柄的十字键下（一个动作可以绑多个键，所以用边沿之外的 current）
    //   2. 左摇杆**压到底**（STICK_DOWN_PRESS）—— 手柄上更自然的下压手势，否则得去按不称手的十字键
    //
    // 为什么这里不复用死区（STICK_DEADZONE）：死区解决的是“静止时不要有输入”（漂移），
    // 而“下”是一个**确定、单目的**的输入（下穿单向平台）—— 左右移动时拇指很容易带出向下的轻微偏移，
    // 用 20% 当阀值会经常误触发。两个阀值语义不同，所以分开两个常量：
    //   死区 = “低于它我不认”（抗漂移，参考：XInput 官方常量 ≈ 24%）
    //   意图阀 = “要明确地压到底”（参考：Unity 的 deadzone processor 也是 min/max 两个参数，
    //            min 忽略噪声、max 视为满偏）
    // 摇杆竖直轴的约定是 +1 = 上，所以“向下”是负值（见 include/input.h）。
    bool down_held = controller->current[GA_DOWN] || (controller->left_stick_y <= -STICK_DOWN_PRESS);

    if (move_axis != 0.0f) {
        game_state->facing = (move_axis > 0.0f) ? FACE_RIGHT : FACE_LEFT;
    }

    // 跳跃缓冲：落地前按下也能生效
    if (controller->pressed[GA_JUMP]) {
        game_state->jump_buffer_timer = PLAYER_JUMP_BUFFER_TIME;
    }

    bool ignore_one_way = (game_state->drop_through_timer > 0.0f);

    // ---------------------------------------------------------------------
    // 冲刺：上升沿触发，方向在启动瞬间按当前朝向锁定
    // ---------------------------------------------------------------------
    bool dashing = (game_state->state == PSTATE_DASH);
    if (controller->pressed[GA_DASH] && !dashing && game_state->dash_cooldown <= 0.0f) {
        dashing = true;
        game_state->dash_timer = PLAYER_DASH_TIME;
        game_state->dash_cooldown = PLAYER_DASH_TIME + PLAYER_DASH_COOLDOWN;
        game_state->dash_dir_x = (game_state->facing == FACE_RIGHT) ? 1.0f : -1.0f;
        game_audio_play_dash();
    }

    if (dashing) {
        game_state->dash_timer -= dt;
        if (game_state->dash_timer <= 0.0f) {
            dashing = false;
        }
    }

    // ---------------------------------------------------------------------
    // 跳跃 / 下穿单向平台
    //
    // 两段共用一个初速：地面（含土狼时间）那一下不消耗次数，空中那一下才消耗，
    // 所以「地面起跳 + 空中再按一次」= 二段跳；从平台边缘走出去只算空中那一次。
    // ---------------------------------------------------------------------
    bool can_ground_jump = (game_state->coyote_timer > 0.0f);
    if (!dashing && game_state->jump_buffer_timer > 0.0f && (can_ground_jump || game_state->air_jumps_left > 0)) {
        if (can_ground_jump) {
            Rect2D box = {};
            player_collider_box(game_state, &box);
            const Platform *support = probe_ground_platform(&box, platforms, platform_count, ignore_one_way);

            if (down_held && support && support->kind == COLLIDER_ONE_WAY) {
                // 佳住下 + 跳：不跳跃，改为短暂忽略单向平台，从平台上落下去
                game_state->drop_through_timer = PLAYER_DROP_THROUGH_TIME;
                ignore_one_way = true;
                game_state->grounded = false;
            } else {
                game_state->velocity.y = PLAYER_JUMP_SPEED;
            }

            game_state->coyote_timer = 0.0f;
        } else {
            // 二段跳：只在这里扣次数，扣到 0 后空中再按也不会有第三段
            game_state->velocity.y = PLAYER_JUMP_SPEED;
            --game_state->air_jumps_left;
        }

        game_state->jump_buffer_timer = 0.0f;
    }

    // ---------------------------------------------------------------------
    // 速度
    // ---------------------------------------------------------------------
    if (dashing) {
        game_state->velocity.x = game_state->dash_dir_x * PLAYER_DASH_SPEED;
        game_state->velocity.y = 0.0f; // 冲刺期间不受重力影响
    } else {
        game_state->velocity.x = move_axis * PLAYER_MOVE_SPEED;
        game_state->velocity.y -= PLAYER_GRAVITY * dt;
        game_state->velocity.y = MAX(game_state->velocity.y, -PLAYER_MAX_FALL_SPEED);
    }

    // ---------------------------------------------------------------------
    // 移动与碰撞
    // ---------------------------------------------------------------------
    Rect2D box = {};
    player_collider_box(game_state, &box);
    move_and_collide(&box, &game_state->velocity, dt, platforms, platform_count, ignore_one_way);
    game_state->player_x = box.center_x - game_state->player_collider.offset_x;
    game_state->player_y = box.center_y - game_state->player_collider.offset_y;

    // 地面判定用探针，避免速度被清零后仍然贴地时的浮点抖动
    player_collider_box(game_state, &box);
    game_state->grounded = (probe_ground_platform(&box, platforms, platform_count, ignore_one_way) != nullptr);
    if (game_state->grounded) {
        game_state->coyote_timer = PLAYER_COYOTE_TIME;
        game_state->air_jumps_left = PLAYER_AIR_JUMP_COUNT; // 落地即恢复二段跳
    }

    // 上报物理盒（不是插值后的渲染盒）给调试层，保证看到的盒就是算碰撞的盒。
    // 同上：ground_probe 是纯调试量，整块包起来免得关宏后留下一段死计算
#if MONO_DEBUG_VIS
    debug_vis_box(&box, DEBUG_BOX_PLAYER);
    if (game_state->movers.count > 0) {
        for (u32 i = 0; i < game_state->movers.count; ++i) {
            debug_vis_box(&game_state->movers.items[i].rect, DEBUG_BOX_MOVING_PLATFORM);
        }
    }
    if (game_state->monster.active) {
        debug_vis_box(&game_state->monster.rect, DEBUG_BOX_MONSTER);
    }
    for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
        if (game_state->projectiles.items[i].active) {
            debug_vis_box(&game_state->projectiles.items[i].rect, DEBUG_BOX_PROJECTILE);
        }
    }
    Rect2D ground_probe = box;
    ground_probe.center_y -= GROUND_PROBE_DEPTH;
    debug_vis_box(&ground_probe, DEBUG_BOX_GROUND_PROBE);
#endif

    // ---------------------------------------------------------------------
    // 状态机
    // ---------------------------------------------------------------------
    if (dashing) {
        game_state->state = PSTATE_DASH;
    } else if (game_state->grounded) {
        game_state->state = (move_axis != 0.0f) ? PSTATE_RUN : PSTATE_IDLE;
    } else {
        game_state->state = (game_state->velocity.y > 0.0f) ? PSTATE_JUMP : PSTATE_FALL;
    }

    // 掉出关卡底部：直接回到出生点（暂时没有死亡表现）
    f32 level_bottom = game_state->level.bounds.center_y - game_state->level.bounds.half_h;
    if (game_state->player_y < level_bottom) {
        player_respawn(game_state);
    }

    // 传送门：站在门里按上键 → 送到同色号的另一扇（见 update_portals 的注释）
    update_portals(game_state, controller);

    // 普通连接：玩家的碰撞盒完全越过门线才切图，避免半个身位已经消失在目标世界。
    f32 world_right = game_state->level.bounds.center_x + game_state->level.bounds.half_w;
    if (game_state->level.has_right_connection &&
        game_state->player_x - game_state->player_collider.width * 0.5f > world_right) {
        WorldConnection connection = game_state->level.right_connection;
        switch_world(game_state, connection.target_world, &connection, viewport_width, viewport_height);
        return;
    }
    f32 world_left = game_state->level.bounds.center_x - game_state->level.bounds.half_w;
    if (game_state->level.has_left_connection &&
        game_state->player_x + game_state->player_collider.width * 0.5f < world_left) {
        WorldConnection connection = game_state->level.left_connection;
        switch_world(game_state, connection.target_world, &connection, viewport_width, viewport_height);
        return;
    }

    // 地刺 / 会伤害的移动组件（方形被挤住、圆形碰到） / 怪物：任一命中就重生。
    // 命中一次就跳出去：重生之后角色位置已经变了，再用同一个盒去比会得到假命中
    player_collider_box(game_state, &box);
    bool damaged = false;
    for (u32 i = 0; i < game_state->level.spike_count && !damaged; ++i) {
        damaged = test_rect_overlap(&box, &game_state->level.spikes[i]);
    }
    if (!damaged) {
        // 移动组件：方形只在“被挤住 / 压住”时伤害（damaging 开着）；圆形恒定伤害（碰到就伤）。
        // 圆形**不走 test_rect_overlap** —— 它是实心的，解算器会把玩家推到贴住的位置，
        // 严格相交测试会漏掉（那是“被挤住”的判据，不是“被碰到”的判据）。
        for (u32 i = 0; i < game_state->movers.count && !damaged; ++i) {
            const Mover *mover = &game_state->movers.items[i];
            if (mover->shape == LEVEL_MOVER_CIRCLE) {
                damaged = mover_circle_touches_box(mover, &box);
            } else if (mover->damaging) {
                damaged = test_rect_overlap(&box, &mover->rect);
            }
        }
    }
    if (!damaged && game_state->monster.active) {
        damaged = test_rect_overlap(&box, &game_state->monster.rect);
    }
    if (damaged) {
        player_respawn(game_state);
    }

    // ---------------------------------------------------------------------
    // 交互音效
    // ---------------------------------------------------------------------
    if (controller->pressed[GA_COIN]) {
        game_audio_play_coin();
    }

    // ---------------------------------------------------------------------
    // 相机
    // ---------------------------------------------------------------------
    // 平滑跟随（dt 传步长：平滑量与帧率无关）。
    //
    // 传送判定必须用「角色这一帧实际走了多远」（prev_* 是本步开始时的位置）——
    // 不能用“相机离角色多远”：那个距离里混着跟随滞距，会把正常跑动误判成传送，
    // 相机就会周期性瞬移（实测：滞距涨到 64px 就跳 65px，一趟跑出 29 次，画面卡顿）。
    f32 player_step = MAX(fabsf(game_state->player_x - game_state->prev_player_x),
                          fabsf(game_state->player_y - game_state->prev_player_y));
    bool teleported = (player_step > CAMERA_TELEPORT_MARGIN);

    game_state->camera.zoom = controller->current[GA_CAMERA_ZOOM] ? 2.0f : 1.0f;
    camera_follow(&game_state->camera, game_state->player_x, game_state->player_y,
                  &game_state->level.bounds, viewport_width, viewport_height,
                  teleported ? 0.0f : dt, camera_profile_for());
}
