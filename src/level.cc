#include "core.h"
#include "shared/memory.h"
#include "shared/level.h"

// 关卡编译：把编辑资产的 tile 合并成尽量少的碰撞矩形。
// 编辑器只需要改 LevelAsset；运行时 Platform 指针、碰撞矩形和世界边界全由本文件生成。
struct LevelRun
{
    u32 col_begin;
    u32 col_end;
    ColliderKind kind;
    u32 platform_index;
};

internal bool level_tile_kind(LevelTile tile, ColliderKind *kind)
{
    if (tile == LEVEL_TILE_SOLID) {
        *kind = COLLIDER_SOLID;
        return true;
    }
    if (tile == LEVEL_TILE_ONE_WAY) {
        *kind = COLLIDER_ONE_WAY;
        return true;
    }
    return false;
}

void level_build_from_asset(Level *level, const LevelAsset *asset)
{
    assert(asset && asset->tiles && asset->tile_columns > 0 && asset->tile_rows > 0);

    *level = {};
    level->movers = asset->movers;
    level->mover_count = asset->mover_count;
    level->monsters = asset->monsters;
    level->monster_count = asset->monster_count;
    level->portals = asset->portals;
    level->portal_count = asset->portal_count;
    // 传送点同理：运行态只引用资产的数组（坐标是脚底语义，换算在 game.cc 落点时做）
    level->waypoints = asset->waypoints;
    level->waypoint_count = asset->waypoint_count;
    const u32 col_count = asset->tile_columns;
    const u32 row_count = asset->tile_rows;
    const f32 tile_size = asset->tile_size;
    Array<Platform> platforms = init<Platform>(32);
    LevelRun *prev_runs = (LevelRun *)arena_push(sizeof(LevelRun) * (col_count + 1));
    LevelRun *cur_runs = (LevelRun *)arena_push(sizeof(LevelRun) * (col_count + 1));
    u32 prev_count = 0;
    bool has_spawn = false;

    for (u32 row = 0; row < row_count; ++row) {
        u32 cur_count = 0;
        u32 col = 0;

        while (col < col_count) {
            LevelTile tile = asset->tiles[row * col_count + col];
            if (tile == LEVEL_TILE_SPAWN) {
                assert(!has_spawn && "关卡只能有一个出生点");
                level->spawn_x = level_cell_center_x(col, tile_size);
                level->spawn_y = level_cell_bottom_y(row, tile_size);
                has_spawn = true;
                ++col;
                continue;
            }

            ColliderKind kind = {};
            if (!level_tile_kind(tile, &kind)) {
                // 地刺与可消失平台都不参与静态碰撞（各自在下面被收集成伤害矩形 / 可消失块）
                assert((tile == LEVEL_TILE_EMPTY || tile == LEVEL_TILE_SPIKE || tile == LEVEL_TILE_VANISH) &&
                       "关卡含未知 tile");
                ++col;
                continue;
            }

            u32 col_begin = col;
            while (col < col_count && asset->tiles[row * col_count + col] == tile) {
                ++col;
            }
            u32 col_end = col - 1;

            u32 platform_index = UINT32_MAX;
            for (u32 i = 0; i < prev_count; ++i) {
                LevelRun *prev = &prev_runs[i];
                if (prev->col_begin != col_begin || prev->col_end != col_end || prev->kind != kind) {
                    continue;
                }

                Platform *platform = &platforms.data[prev->platform_index];
                platform->rect.center_y -= tile_size * 0.5f;
                platform->rect.half_h += tile_size * 0.5f;
                platform_index = prev->platform_index;
                break;
            }

            if (platform_index == UINT32_MAX) {
                Platform *platform = array_push_slot(&platforms);
                platform_index = platforms.size - 1;

                f32 cell_count = (f32)(col_end - col_begin + 1);
                platform->rect.center_x = level_row_span_center_x(col_begin, cell_count, tile_size);
                platform->rect.center_y = level_cell_center_y(row, tile_size);
                platform->rect.half_w = cell_count * tile_size * 0.5f;
                platform->rect.half_h = tile_size * 0.5f;
                platform->kind = kind;
            }

            cur_runs[cur_count].col_begin = col_begin;
            cur_runs[cur_count].col_end = col_end;
            cur_runs[cur_count].kind = kind;
            cur_runs[cur_count].platform_index = platform_index;
            ++cur_count;
        }

        for (u32 i = 0; i < cur_count; ++i) {
            prev_runs[i] = cur_runs[i];
        }
        prev_count = cur_count;
    }

    // 地刺：每行里相邻的地刺格合并成一条矩形。只按行、不跨行 —— 一行格就是一道地刺，
    // 跨行合并只会让「哪一段是地刺」更难对（碰撞体的逐行合并是为了少发平台，目的不同）。
    Array<Rect2D> spikes = init<Rect2D>(16);
    for (u32 row = 0; row < row_count; ++row) {
        u32 col = 0;
        while (col < col_count) {
            if (asset->tiles[(u64)row * col_count + col] != LEVEL_TILE_SPIKE) {
                ++col;
                continue;
            }
            u32 col_begin = col;
            while (col < col_count && asset->tiles[(u64)row * col_count + col] == LEVEL_TILE_SPIKE) {
                ++col;
            }
            f32 cell_count = (f32)(col - col_begin);
            Rect2D *rect = array_push_slot(&spikes);
            rect->center_x = level_row_span_center_x(col_begin, cell_count, tile_size);
            rect->center_y = level_cell_center_y(row, tile_size);
            rect->half_w = cell_count * tile_size * 0.5f;
            rect->half_h = tile_size * 0.5f;
        }
    }
    level->spikes = spikes.data;
    level->spike_count = spikes.size;

    // 可消失平台：与地刺同形（每行相邻的格合并成一块），但**不能和实体/单向合并** ——
    // 它的状态是按块走的，混进 platforms 会让它消失时把邻居一起带走。
    Array<Rect2D> vanish = init<Rect2D>(16);
    for (u32 row = 0; row < row_count; ++row) {
        u32 col = 0;
        while (col < col_count) {
            if (asset->tiles[(u64)row * col_count + col] != LEVEL_TILE_VANISH) {
                ++col;
                continue;
            }
            u32 col_begin = col;
            while (col < col_count && asset->tiles[(u64)row * col_count + col] == LEVEL_TILE_VANISH) {
                ++col;
            }
            f32 cell_count = (f32)(col - col_begin);
            Rect2D *rect = array_push_slot(&vanish);
            rect->center_x = level_row_span_center_x(col_begin, cell_count, tile_size);
            rect->center_y = level_cell_center_y(row, tile_size);
            rect->half_w = cell_count * tile_size * 0.5f;
            rect->half_h = tile_size * 0.5f;
        }
    }
    assert(vanish.size <= MAX_VANISH_BLOCKS && "可消失平台的块数超过 MAX_VANISH_BLOCKS");
    level->vanish_platforms = vanish.data;
    level->vanish_platform_count = vanish.size;

    assert(has_spawn && "关卡缺少出生点");
    level->platforms = platforms.data;
    level->platform_count = platforms.size;

    for (u32 i = 0; i < asset->connection_count; ++i) {
        const LevelConnectionAsset *asset_connection = &asset->connections[i];
        assert(asset_connection->target_level < WORLD_COUNT && "门目标世界不存在");

        assert(asset_connection->facing < LEVEL_CONNECTION_FACING_COUNT);
        PlayerFacing facing = (asset_connection->facing == LEVEL_CONNECTION_FACE_RIGHT) ? FACE_RIGHT : FACE_LEFT;
        WorldConnection connection = { .target_world = (WorldId)asset_connection->target_level,
                                       .entry_x = asset_connection->entry_x,
                                       .entry_y = asset_connection->entry_y,
                                       .facing = facing };

        if (asset_connection->side == LEVEL_CONNECTION_LEFT) {
            assert(!level->has_left_connection && "同一侧只能有一个门");
            level->has_left_connection = true;
            level->left_connection = connection;
        } else {
            assert(asset_connection->side == LEVEL_CONNECTION_RIGHT);
            assert(!level->has_right_connection && "同一侧只能有一个门");
            level->has_right_connection = true;
            level->right_connection = connection;
        }
    }

    // 边界取网格**外沿**（公式与校验共用 level_grid_bounds）：x ∈ [0, 列数*格]、y ∈ [-行数*格, 0]
    level->bounds = level_grid_bounds(col_count, row_count, tile_size);
}
