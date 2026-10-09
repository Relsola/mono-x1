#include "core.h"
#include "shared/memory.h"
#include "shared/file.h"
#include "shared/level_asset.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

#include <stdarg.h> // va_list（校验消息格式化）
#include <stdio.h>  // vsnprintf
#include <string.h> // memcpy / memcmp

// 文件布局（全部 little-endian）：
// u32 magic/version/columns/rows, f32 tile_size, u32 四类实体数量，随后按顺序紧密排列各段。
// 逐字段读取而非 memcpy C++ 结构，避免 padding 成为跨版本或编辑器/游戏间的隐式格式契约。
struct LevelAssetReader
{
    const u8 *at;
    const u8 *end;
};

internal bool level_asset_read_bytes(LevelAssetReader *reader, void *out, u32 size)
{
    if ((u64)(reader->end - reader->at) < size) {
        return false;
    }
    memcpy(out, reader->at, size);
    reader->at += size;
    return true;
}

internal bool level_asset_read_u32(LevelAssetReader *reader, u32 *out)
{
    return level_asset_read_bytes(reader, out, sizeof(*out));
}

internal bool level_asset_read_f32(LevelAssetReader *reader, f32 *out)
{
    return level_asset_read_bytes(reader, out, sizeof(*out));
}

internal bool level_asset_read_rect(LevelAssetReader *reader, Rect2D *out)
{
    return level_asset_read_f32(reader, &out->center_x) &&
           level_asset_read_f32(reader, &out->center_y) &&
           level_asset_read_f32(reader, &out->half_w) &&
           level_asset_read_f32(reader, &out->half_h);
}

// 文件总长度的**唯一算法**：reader 用它做长度校验，writer 用它做缓冲分配。
// 两处各写一遍就会漂移 —— 那时表现是「写出来的文件自己读不回来」。
struct LevelAssetSize
{
    u32 tile_count;
    u64 total_bytes;
};

// header 里的 u32 个数按**版本**查一次：v1 = 8（多一个地刺数量）、v2/v3 = 7、v4 = 8（多一个传送门数量）、
// v5+ = 9（再多一个传送点数量）。它不是单调递增的，所以不能用“是不是 v1”那种布尔判；
// 同一个函数给 reader（校验长度）与 writer（分配缓冲）用，两边才不会各自算错。
internal u32 level_asset_header_u32_count(u32 version)
{
    if (version <= 1) {
        return 8;
    }
    if (version <= 3) {
        return 7;
    }
    if (version == 4) {
        return 8;
    }
    return 9;
}

// legacy_spike_count 只在 v1 布局下有意义（那一段是地刺矩形，v2 换成了地刺格）。
// **版本要传进来**：移动组件段跟着版本变过两次（v6：水平往返 → 两点 + 速度 + 伤害；
// v7：绝对中心 → 轴上的 t0 + 形状），所以“每个元素多少字节”也是跟着版本走的，不能当常量。
internal bool level_asset_compute_size(u32 columns, u32 rows, u32 version, u32 legacy_spike_count,
                                       u32 connection_count, u32 mover_count, u32 monster_count,
                                       u32 portal_count, u32 waypoint_count, LevelAssetSize *out)
{
    if (columns == 0 || rows == 0) {
        return false;
    }

    // 逐段求和。**这里不做 u64 溢出检查**：5 个 u32 数量各乘几十字节，最多约 6e11，离 u64 上限很远；
    // 真正的闸门是调用方那句「总长必须正好等于文件长度」—— 损坏 header 里的巨大 count 会让总长
    // 对不上（文件长度是 u32），照样被拒。在这里写溢出检查只会得到跑不到的死代码。
    u64 tile_count = (u64)columns * rows;
    if (tile_count > UINT32_MAX) {
        return false; // 光 tiles 就超过 4GB，不可能与文件长度相符
    }

    // v7 起：两点(4 f32) + t0 + 半宽半高(3 f32) + speed + shape(u32) + damaging(u32) = 8 f32 + 2 u32
    // v6：rect(4 f32) + 两点(4 f32) + speed + damaging(u32) = 9 f32 + 1 u32
    // v5 及更早：rect(4 f32) + min_x/max_x/velocity_x = 7 f32
    u32 mover_bytes = (u32)(sizeof(f32) * 7);
    if (version >= 7) {
        mover_bytes = (u32)(sizeof(f32) * 8 + sizeof(u32) * 2);
    } else if (version >= 6) {
        mover_bytes = (u32)(sizeof(f32) * 9 + sizeof(u32));
    }

    u64 total = (u64)level_asset_header_u32_count(version) * sizeof(u32) + sizeof(f32);
    total += tile_count * sizeof(LevelTile);
    total += (u64)legacy_spike_count * sizeof(f32) * 4;
    total += (u64)connection_count * (sizeof(u32) * 3 + sizeof(f32) * 2);
    total += (u64)mover_count * mover_bytes;
    total += (u64)monster_count * sizeof(f32) * 7;
    total += (u64)portal_count * (sizeof(f32) * 4 + sizeof(u32));
    total += (u64)waypoint_count * sizeof(f32) * 2;

    out->tile_count = (u32)tile_count;
    out->total_bytes = total;
    return true;
}

// v1 → v2 的迁移：一个地刺矩形写成它覆盖的格（按**格中心**落在矩形内判定）。
// 一格只能装一种东西，所以只写空格，其余计数返回（调用方会留一条 WARN）。
internal u32 level_asset_spikes_from_rect(LevelAsset *asset, const Rect2D *rect)
{
    f32 left = rect->center_x - rect->half_w;
    f32 right = rect->center_x + rect->half_w;
    f32 bottom = rect->center_y - rect->half_h;
    f32 top = rect->center_y + rect->half_h;

    u32 skipped = 0;
    for (u32 row = 0; row < asset->tile_rows; ++row) {
        f32 cell_y = level_cell_center_y(row, asset->tile_size);
        if (cell_y < bottom || cell_y > top) {
            continue;
        }
        for (u32 col = 0; col < asset->tile_columns; ++col) {
            f32 cell_x = level_cell_center_x(col, asset->tile_size);
            if (cell_x < left || cell_x > right) {
                continue;
            }
            LevelTile *cell = &asset->tiles[(u64)row * asset->tile_columns + col];
            if (*cell == LEVEL_TILE_EMPTY) {
                *cell = LEVEL_TILE_SPIKE;
            } else {
                ++skipped;
            }
        }
    }
    return skipped;
}

// 从「本体中心」推出轴上的参数 t0：投影到轴向量上，再夹到 [0,1]。
// 旧版本（v6 及更早）存的是绝对中心，迁移时用它换算 —— 旧图的本体中心本来就在轴上
// （旧运行时会把不在轴上的它拉上去），所以算出来的就是它本来会在的位置。
internal f32 level_asset_mover_t0_from_center(v2 a, v2 b, f32 center_x, f32 center_y)
{
    f32 axis_x = b.x - a.x;
    f32 axis_y = b.y - a.y;
    f32 length_sq = axis_x * axis_x + axis_y * axis_y;
    if (length_sq <= 0.0f) {
        return 0.0f;
    }
    f32 t = ((center_x - a.x) * axis_x + (center_y - a.y) * axis_y) / length_sq;
    return clamp(t, 0.0f, 1.0f);
}

bool level_asset_load(const wchar_t *path, LevelAsset *out)
{
    ReadFileRes file = read_file(path);
    if (!file.contents) {
        return false;
    }

    LevelAssetReader reader = { (const u8 *)file.contents, (const u8 *)file.contents + file.file_size };
    u32 magic = 0;
    u32 version = 0;
    if (!level_asset_read_u32(&reader, &magic) || !level_asset_read_u32(&reader, &version) ||
        magic != LEVEL_ASSET_MAGIC || version < 1 || version > LEVEL_ASSET_VERSION) {
        return false;
    }

    // v1 的地刺是一段矩形（跟在 tiles 后面），v2 没有这一段 —— 只在读入时拿它换算成地刺格。
    // v3 的布局与 v2 完全一样（只是多了一个 tile 值），所以这里只判 v1。
    bool legacy_spikes = (version == 1);
    u32 columns = 0;
    u32 rows = 0;
    f32 tile_size = 0.0f;
    u32 legacy_spike_count = 0;
    u32 connection_count = 0;
    u32 mover_count = 0;
    u32 monster_count = 0;

    bool header_ok = level_asset_read_u32(&reader, &columns) &&
                     level_asset_read_u32(&reader, &rows) &&
                     level_asset_read_f32(&reader, &tile_size);
    if (header_ok && legacy_spikes) {
        header_ok = level_asset_read_u32(&reader, &legacy_spike_count);
    }
    header_ok = header_ok && level_asset_read_u32(&reader, &connection_count) &&
                level_asset_read_u32(&reader, &mover_count) &&
                level_asset_read_u32(&reader, &monster_count);
    // v4 起 header 多一个「传送门数量」（第四类实体）；v1..v3 里它当作 0
    u32 portal_count = 0;
    if (version >= 4) {
        header_ok = header_ok && level_asset_read_u32(&reader, &portal_count);
    }
    // v5 起再多一个「传送点数量」（第五类实体）；更早的版本当作 0
    u32 waypoint_count = 0;
    if (version >= 5) {
        header_ok = header_ok && level_asset_read_u32(&reader, &waypoint_count);
    }
    if (!header_ok || columns == 0 || rows == 0 || tile_size <= 0.0f) {
        return false;
    }

    u32 tile_count = 0;
    // 在分配前先验证整个布局：损坏 header 里的巨大 count 不能推动 arena 或让后续段错位。
    // v1 的尺寸含地刺段，所以传它自己的数量；v2 传 0。
    LevelAssetSize size = {};
    if (!level_asset_compute_size(columns, rows, version, legacy_spike_count, connection_count,
                                  mover_count, monster_count, portal_count, waypoint_count, &size) ||
        size.total_bytes != file.file_size) {
        return false;
    }
    tile_count = size.tile_count;

    LevelAsset loaded = {};
    loaded.tile_columns = columns;
    loaded.tile_rows = rows;
    loaded.tile_size = tile_size;
    loaded.tiles = (LevelTile *)arena_push(tile_count);
    if (!level_asset_read_bytes(&reader, loaded.tiles, tile_count)) {
        return false;
    }

    // v1 → v2 的迁移：地刺矩形覆盖到的格（按**格中心**落在矩形内判定）变成地刺格。
    // 一格只能装一种东西，所以实心/单向/出生点上的格不动（旧语义是那个位置照样致命，
    // 新表示做不到；实测过的两张图都没有这种重叠）
    for (u32 i = 0; i < legacy_spike_count; ++i) {
        Rect2D rect = {};
        if (!level_asset_read_rect(&reader, &rect)) {
            return false;
        }
        u32 skipped = level_asset_spikes_from_rect(&loaded, &rect);
        if (skipped > 0) {
            LOG_WARN("level: v1 spike rect covered %u non-empty cell(s); those cells keep their tile", skipped);
        }
    }
    if (connection_count > 0) {
        loaded.connections = (LevelConnectionAsset *)arena_push(sizeof(LevelConnectionAsset) * connection_count);
        for (u32 i = 0; i < connection_count; ++i) {
            LevelConnectionAsset *connection = &loaded.connections[i];
            u32 side = 0;
            u32 facing = 0;
            if (!level_asset_read_u32(&reader, &side) ||
                !level_asset_read_u32(&reader, &connection->target_level) ||
                !level_asset_read_f32(&reader, &connection->entry_x) ||
                !level_asset_read_f32(&reader, &connection->entry_y) ||
                !level_asset_read_u32(&reader, &facing) ||
                side >= LEVEL_CONNECTION_SIDE_COUNT ||
                facing >= LEVEL_CONNECTION_FACING_COUNT) {
                return false;
            }
            connection->side = (LevelConnectionSide)side;
            connection->facing = (LevelConnectionFacing)facing;
        }
    }
    loaded.connection_count = connection_count;

    if (mover_count > 0) {
        loaded.movers = (LevelMoverAsset *)arena_push(sizeof(LevelMoverAsset) * mover_count);
        for (u32 i = 0; i < mover_count; ++i) {
            LevelMoverAsset *mover = &loaded.movers[i];
            if (version >= 7) {
                // 轴上的点 + 轴参数 t0 + 半宽半高 + 沿轴速度 + 形状 + 伤害开关
                u32 shape = 0;
                u32 damaging = 0;
                if (!level_asset_read_f32(&reader, &mover->point_a.x) ||
                    !level_asset_read_f32(&reader, &mover->point_a.y) ||
                    !level_asset_read_f32(&reader, &mover->point_b.x) ||
                    !level_asset_read_f32(&reader, &mover->point_b.y) ||
                    !level_asset_read_f32(&reader, &mover->t0) ||
                    !level_asset_read_f32(&reader, &mover->half_w) ||
                    !level_asset_read_f32(&reader, &mover->half_h) ||
                    !level_asset_read_f32(&reader, &mover->speed) ||
                    !level_asset_read_u32(&reader, &shape) || shape >= LEVEL_MOVER_SHAPE_COUNT ||
                    !level_asset_read_u32(&reader, &damaging) || damaging > 1) {
                    return false;
                }
                mover->shape = shape;
                mover->damaging = damaging;
            } else {
                // v6 及更早：本体位置存的是**绝对中心**，迁移成轴上的 t0。
                // v6：rect(4 f32) + 两点 + speed + damaging；v5 及更早：rect + min_x/max_x/velocity_x
                Rect2D rect = {};
                if (!level_asset_read_rect(&reader, &rect)) {
                    return false;
                }
                if (version >= 6) {
                    u32 damaging = 0;
                    if (!level_asset_read_f32(&reader, &mover->point_a.x) ||
                        !level_asset_read_f32(&reader, &mover->point_a.y) ||
                        !level_asset_read_f32(&reader, &mover->point_b.x) ||
                        !level_asset_read_f32(&reader, &mover->point_b.y) ||
                        !level_asset_read_f32(&reader, &mover->speed) ||
                        !level_asset_read_u32(&reader, &damaging) || damaging > 1) {
                        return false;
                    }
                    mover->damaging = damaging;
                } else {
                    // 水平往返：端点 y 取 rect 的中心 y，**两个端点的先后按 velocity_x 的符号定** ——
                    // 这样它一开始朝哪边走与迁移前一致，旧图轨迹因此逐帧不变
                    f32 min_x = 0.0f;
                    f32 max_x = 0.0f;
                    f32 velocity_x = 0.0f;
                    if (!level_asset_read_f32(&reader, &min_x) ||
                        !level_asset_read_f32(&reader, &max_x) ||
                        !level_asset_read_f32(&reader, &velocity_x) || min_x > max_x) {
                        return false;
                    }
                    f32 center_y = rect.center_y;
                    mover->point_a = (velocity_x < 0.0f) ? v2{ max_x, center_y } : v2{ min_x, center_y };
                    mover->point_b = (velocity_x < 0.0f) ? v2{ min_x, center_y } : v2{ max_x, center_y };
                    mover->speed = fabsf(velocity_x);
                    mover->damaging = 0;
                }
                mover->half_w = rect.half_w;
                mover->half_h = rect.half_h;
                mover->shape = LEVEL_MOVER_SQUARE; // 旧图只有方形平台
                mover->t0 = level_asset_mover_t0_from_center(mover->point_a, mover->point_b, rect.center_x,
                                                            rect.center_y);
            }
        }
    }
    loaded.mover_count = mover_count;

    if (monster_count > 0) {
        loaded.monsters = (LevelMonsterAsset *)arena_push(sizeof(LevelMonsterAsset) * monster_count);
        for (u32 i = 0; i < monster_count; ++i) {
            LevelMonsterAsset *monster = &loaded.monsters[i];
            if (!level_asset_read_rect(&reader, &monster->rect) ||
                !level_asset_read_f32(&reader, &monster->spawn_x) ||
                !level_asset_read_f32(&reader, &monster->spawn_y) ||
                !level_asset_read_f32(&reader, &monster->velocity_x)) {
                return false;
            }
        }
    }
    loaded.monster_count = monster_count;

    if (portal_count > 0) {
        loaded.portals = (LevelPortalAsset *)arena_push(sizeof(LevelPortalAsset) * portal_count);
        for (u32 i = 0; i < portal_count; ++i) {
            LevelPortalAsset *portal = &loaded.portals[i];
            if (!level_asset_read_rect(&reader, &portal->rect) ||
                !level_asset_read_u32(&reader, &portal->pair_id)) {
                return false;
            }
        }
    }
    loaded.portal_count = portal_count;

    if (waypoint_count > 0) {
        loaded.waypoints = (LevelWaypointAsset *)arena_push(sizeof(LevelWaypointAsset) * waypoint_count);
        for (u32 i = 0; i < waypoint_count; ++i) {
            LevelWaypointAsset *waypoint = &loaded.waypoints[i];
            if (!level_asset_read_f32(&reader, &waypoint->x) || !level_asset_read_f32(&reader, &waypoint->y)) {
                return false;
            }
        }
    }
    loaded.waypoint_count = waypoint_count;

    bool consumed_all = reader.at == reader.end;
    if (!consumed_all) {
        return false;
    }

    *out = loaded;
    return true;
}

// ============================================================================
// 写入：与 reader 逐字段对称
//
// 布局只有一个实现者（本文件），reader 与 writer 共用 level_asset_compute_size，
// 所以「写出来的文件自己读不回来」这一类漂移没有生存空间。
// ============================================================================

struct LevelAssetWriter
{
    u8 *at;
    u8 *end;
};

internal bool level_asset_write_bytes(LevelAssetWriter *writer, const void *data, u32 size)
{
    if ((u64)(writer->end - writer->at) < size) {
        return false;
    }
    memcpy(writer->at, data, size);
    writer->at += size;
    return true;
}

internal bool level_asset_write_u32(LevelAssetWriter *writer, u32 value)
{
    return level_asset_write_bytes(writer, &value, sizeof(value));
}

internal bool level_asset_write_f32(LevelAssetWriter *writer, f32 value)
{
    return level_asset_write_bytes(writer, &value, sizeof(value));
}

internal bool level_asset_write_rect(LevelAssetWriter *writer, const Rect2D *rect)
{
    return level_asset_write_f32(writer, rect->center_x) &&
           level_asset_write_f32(writer, rect->center_y) &&
           level_asset_write_f32(writer, rect->half_w) &&
           level_asset_write_f32(writer, rect->half_h);
}

internal bool level_asset_rect_equal(const Rect2D *a, const Rect2D *b)
{
    return a->center_x == b->center_x && a->center_y == b->center_y &&
           a->half_w == b->half_w && a->half_h == b->half_h;
}

// 逐字段比较。**不能 memcmp 结构体**：填充字节未初始化，逐位比较会随机失败。
// 非 static：编辑器要拿它做「有没有改过」与撤销栈的「内容没变就不落格」判据（头里有声明）。
bool level_asset_equal(const LevelAsset *a, const LevelAsset *b)
{
    if (a->tile_columns != b->tile_columns || a->tile_rows != b->tile_rows || a->tile_size != b->tile_size) {
        return false;
    }
    if (memcmp(a->tiles, b->tiles, (u64)a->tile_columns * a->tile_rows * sizeof(LevelTile)) != 0) {
        return false;
    }

    if (a->connection_count != b->connection_count ||
        a->mover_count != b->mover_count ||
        a->monster_count != b->monster_count ||
        a->portal_count != b->portal_count ||
        a->waypoint_count != b->waypoint_count) {
        return false;
    }

    for (u32 i = 0; i < a->connection_count; ++i) {
        const LevelConnectionAsset *ca = &a->connections[i];
        const LevelConnectionAsset *cb = &b->connections[i];
        if (ca->side != cb->side || ca->target_level != cb->target_level || ca->facing != cb->facing ||
            ca->entry_x != cb->entry_x || ca->entry_y != cb->entry_y) {
            return false;
        }
    }
    for (u32 i = 0; i < a->mover_count; ++i) {
        const LevelMoverAsset *pa = &a->movers[i];
        const LevelMoverAsset *pb = &b->movers[i];
        if (pa->t0 != pb->t0 || pa->half_w != pb->half_w || pa->half_h != pb->half_h || pa->speed != pb->speed ||
            pa->shape != pb->shape || pa->damaging != pb->damaging ||
            pa->point_a.x != pb->point_a.x || pa->point_a.y != pb->point_a.y ||
            pa->point_b.x != pb->point_b.x || pa->point_b.y != pb->point_b.y) {
            return false;
        }
    }
    for (u32 i = 0; i < a->monster_count; ++i) {
        const LevelMonsterAsset *ma = &a->monsters[i];
        const LevelMonsterAsset *mb = &b->monsters[i];
        if (ma->spawn_x != mb->spawn_x || ma->spawn_y != mb->spawn_y || ma->velocity_x != mb->velocity_x ||
            !level_asset_rect_equal(&ma->rect, &mb->rect)) {
            return false;
        }
    }
    for (u32 i = 0; i < a->portal_count; ++i) {
        const LevelPortalAsset *pa = &a->portals[i];
        const LevelPortalAsset *pb = &b->portals[i];
        if (pa->pair_id != pb->pair_id || !level_asset_rect_equal(&pa->rect, &pb->rect)) {
            return false;
        }
    }
    for (u32 i = 0; i < a->waypoint_count; ++i) {
        const LevelWaypointAsset *wa = &a->waypoints[i];
        const LevelWaypointAsset *wb = &b->waypoints[i];
        if (wa->x != wb->x || wa->y != wb->y) {
            return false;
        }
    }
    return true;
}

// 临时文件路径 = 原路径 + ".tmp"（同目录，所以最后的改名是同卷操作）
internal bool level_asset_make_temp_path(const wchar_t *path, wchar_t *out, u32 out_size)
{
    u32 len = 0;
    while (path[len] != L'\0' && len + 5 < out_size) {
        out[len] = path[len];
        ++len;
    }
    if (path[len] != L'\0') {
        return false;
    }
    const wchar_t *suffix = L".tmp"; // 含结尾 '\0'，共 5 个元素
    for (u32 i = 0; i < 5; ++i) {
        out[len + i] = suffix[i];
    }
    return true;
}

bool level_asset_save(const wchar_t *path, const LevelAsset *asset)
{
    if (!path || !asset || !asset->tiles || asset->tile_columns == 0 || asset->tile_rows == 0 ||
        asset->tile_size <= 0.0f) {
        return false;
    }
    // 数量与指针的自洽性：编辑器增删实体时最容易漏成「count 加了、指针还是旧的」
    if ((asset->connection_count > 0 && !asset->connections) ||
        (asset->mover_count > 0 && !asset->movers) ||
        (asset->monster_count > 0 && !asset->monsters) ||
        (asset->portal_count > 0 && !asset->portals) ||
        (asset->waypoint_count > 0 && !asset->waypoints)) {
        return false;
    }

    LevelAssetSize size = {};
    // 写出去的一律是当前版本：header 个数与各段的长度都按 LEVEL_ASSET_VERSION 算
    if (!level_asset_compute_size(asset->tile_columns, asset->tile_rows, LEVEL_ASSET_VERSION, 0,
                                  asset->connection_count, asset->mover_count,
                                  asset->monster_count, asset->portal_count, asset->waypoint_count, &size) ||
        size.total_bytes > UINT32_MAX) {
        return false;
    }

    // 写出缓冲从 arena 拿：一次保存增长一次（量级 = 文件大小）。
    // 保存是用户动作、不是每帧动作，所以这不是增长律，注释在此说明来源。
    u8 *buffer = (u8 *)arena_push(size.total_bytes);
    LevelAssetWriter writer = { buffer, buffer + size.total_bytes };

    bool ok = level_asset_write_u32(&writer, LEVEL_ASSET_MAGIC) &&
              level_asset_write_u32(&writer, LEVEL_ASSET_VERSION) &&
              level_asset_write_u32(&writer, asset->tile_columns) &&
              level_asset_write_u32(&writer, asset->tile_rows) &&
              level_asset_write_f32(&writer, asset->tile_size) &&
              level_asset_write_u32(&writer, asset->connection_count) &&
              level_asset_write_u32(&writer, asset->mover_count) &&
              level_asset_write_u32(&writer, asset->monster_count) &&
              level_asset_write_u32(&writer, asset->portal_count) &&
              level_asset_write_u32(&writer, asset->waypoint_count) &&
              level_asset_write_bytes(&writer, asset->tiles, size.tile_count * sizeof(LevelTile));

    for (u32 i = 0; ok && i < asset->connection_count; ++i) {
        const LevelConnectionAsset *connection = &asset->connections[i];
        ok = level_asset_write_u32(&writer, (u32)connection->side) &&
             level_asset_write_u32(&writer, connection->target_level) &&
             level_asset_write_f32(&writer, connection->entry_x) &&
             level_asset_write_f32(&writer, connection->entry_y) &&
             level_asset_write_u32(&writer, (u32)connection->facing);
    }
    for (u32 i = 0; ok && i < asset->mover_count; ++i) {
        const LevelMoverAsset *mover = &asset->movers[i];
        ok = level_asset_write_f32(&writer, mover->point_a.x) &&
             level_asset_write_f32(&writer, mover->point_a.y) &&
             level_asset_write_f32(&writer, mover->point_b.x) &&
             level_asset_write_f32(&writer, mover->point_b.y) &&
             level_asset_write_f32(&writer, mover->t0) &&
             level_asset_write_f32(&writer, mover->half_w) &&
             level_asset_write_f32(&writer, mover->half_h) &&
             level_asset_write_f32(&writer, mover->speed) &&
             level_asset_write_u32(&writer, mover->shape) &&
             level_asset_write_u32(&writer, mover->damaging);
    }
    for (u32 i = 0; ok && i < asset->monster_count; ++i) {
        const LevelMonsterAsset *monster = &asset->monsters[i];
        ok = level_asset_write_rect(&writer, &monster->rect) &&
             level_asset_write_f32(&writer, monster->spawn_x) &&
             level_asset_write_f32(&writer, monster->spawn_y) &&
             level_asset_write_f32(&writer, monster->velocity_x);
    }
    for (u32 i = 0; ok && i < asset->portal_count; ++i) {
        const LevelPortalAsset *portal = &asset->portals[i];
        ok = level_asset_write_rect(&writer, &portal->rect) &&
             level_asset_write_u32(&writer, portal->pair_id);
    }
    for (u32 i = 0; ok && i < asset->waypoint_count; ++i) {
        const LevelWaypointAsset *waypoint = &asset->waypoints[i];
        ok = level_asset_write_f32(&writer, waypoint->x) && level_asset_write_f32(&writer, waypoint->y);
    }
    if (!ok || writer.at != writer.end) {
        return false;
    }

    wchar_t temp_path[LEVEL_ASSET_PATH_SIZE];
    if (!level_asset_make_temp_path(path, temp_path, LEVEL_ASSET_PATH_SIZE)) {
        return false;
    }

    if (!write_file(temp_path, (u32)size.total_bytes, buffer, false)) {
        file_remove(temp_path);
        return false;
    }

    // 闭环第一步：读回来，并逐字段比对「写出去的 == 交进来的」。
    // 只验证「读取器能接受」还不够 —— 写错一个字段一样是个合法文件。
    LevelAsset reloaded = {};
    bool verified = level_asset_load(temp_path, &reloaded) && level_asset_equal(asset, &reloaded);
    if (!verified) {
        file_remove(temp_path);
        return false;
    }

    // 闭环第二步：原子替换（失败时原文件全程未被动过）
    if (!file_move_replace(temp_path, path)) {
        file_remove(temp_path);
        return false;
    }
    return true;
}

// ============================================================================
// 语义校验（规则表见 include/shared/level_asset.h 的 LevelAssetIssueKind）
//
// 这里只做**能用资产本身判断**的事。枚举范围、长度这类格式约束由 reader 负责；
// 「门的两端是否都在对方关卡里」需要在编辑器里同时打开两张图，暂不在共享层。
// ============================================================================

struct LevelBounds
{
    f32 min_x;
    f32 max_x;
    f32 min_y;
    f32 max_y;
};

// 不能用 isfinite()：本工程用 /fp:fast 编译，编译器被允许假定没有 NaN/Inf。
// 位模式判断（指数位全 1）与优化等级无关。
internal bool level_asset_f32_finite(f32 value)
{
    u32 bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

internal LevelBounds level_asset_bounds(const LevelAsset *asset)
{
    // 与 level.cc 同一处公式（level_grid_bounds）：边界取网格外沿
    Rect2D grid = level_grid_bounds(asset->tile_columns, asset->tile_rows, asset->tile_size);
    LevelBounds bounds = {};
    bounds.min_x = grid.center_x - grid.half_w;
    bounds.max_x = grid.center_x + grid.half_w;
    bounds.min_y = grid.center_y - grid.half_h;
    bounds.max_y = grid.center_y + grid.half_h;
    return bounds;
}

internal void level_asset_issue_add(LevelAssetIssues *out, LevelAssetIssueLevel level, LevelAssetIssueKind kind,
                                    int entity_kind, int entity_index, int col, int row, const char *fmt, ...)
{
    if (out->count >= LEVEL_ASSET_MAX_ISSUES) {
        out->truncated = true;
        return;
    }

    LevelAssetIssue *issue = &out->items[out->count++];
    issue->level = level;
    issue->kind = kind;
    issue->entity_kind = entity_kind;
    issue->entity_index = entity_index;
    issue->col = col;
    issue->row = row;

    va_list args;
    va_start(args, fmt);
    vsnprintf(issue->text, LEVEL_ASSET_ISSUE_TEXT_SIZE, fmt, args);
    va_end(args);

    if (level == LEVEL_ASSET_ISSUE_ERROR) {
        ++out->error_count;
    } else {
        ++out->warn_count;
    }
}

internal bool level_asset_rect_finite(const Rect2D *rect)
{
    return level_asset_f32_finite(rect->center_x) && level_asset_f32_finite(rect->center_y) &&
           level_asset_f32_finite(rect->half_w) && level_asset_f32_finite(rect->half_h);
}

// 矩形类实体共用的三条检查：有限值、非退化、是否落在关卡内。
// 名字从实体注册表里取，所以没有「哪天改了名字这里忘了改」。
internal void level_asset_check_rect(LevelAssetIssues *out, u32 kind, u32 index, const Rect2D *rect,
                                     const LevelBounds *bounds)
{
    const char *name = LEVEL_ASSET_ENTITY_TABLE[kind].name;
    int entity_kind = (int)kind;
    int entity_index = (int)index;

    if (!level_asset_rect_finite(rect)) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_NON_FINITE, entity_kind, entity_index,
                              -1, -1, "%s #%u 的矩形含 NaN / Inf", name, index);
        return; // 后面的几何比较对 NaN 没有意义
    }
    if (rect->half_w <= 0.0f || rect->half_h <= 0.0f) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_DEGENERATE_RECT, entity_kind,
                              entity_index, -1, -1, "%s #%u 的尺寸为零或负（半宽 %.1f，半高 %.1f）", name, index,
                              rect->half_w, rect->half_h);
        return;
    }

    f32 left = rect->center_x - rect->half_w;
    f32 right = rect->center_x + rect->half_w;
    f32 bottom = rect->center_y - rect->half_h;
    f32 top = rect->center_y + rect->half_h;

    bool outside = right <= bounds->min_x || left >= bounds->max_x || top <= bounds->min_y || bottom >= bounds->max_y;
    if (outside) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_OUT_OF_BOUNDS, entity_kind, entity_index,
                              -1, -1, "%s #%u 整个在关卡外（关卡 x [%.0f, %.0f] y [%.0f, %.0f]）", name, index,
                              bounds->min_x, bounds->max_x, bounds->min_y, bounds->max_y);
        return;
    }

    bool crosses = left < bounds->min_x || right > bounds->max_x || bottom < bounds->min_y || top > bounds->max_y;
    if (crosses) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_OUT_OF_BOUNDS, entity_kind, entity_index,
                              -1, -1, "%s #%u 有一部分越出关卡边界", name, index);
    }
}

void level_asset_validate(const LevelAsset *asset, int self_world_id, LevelAssetIssues *out)
{
    *out = {};

    bool grid_ok = asset && asset->tiles && asset->tile_columns > 0 && asset->tile_rows > 0 &&
                   asset->tile_size > 0.0f;
    for (u32 kind = 0; grid_ok && kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        grid_ok = level_asset_entity_count(asset, kind) == 0 || level_asset_entity_data(asset, kind) != nullptr;
    }
    if (!grid_ok) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_GRID_INVALID, -1, -1, -1, -1,
                              "资产或网格不成立（空指针 / 零尺寸网格 / count 与指针不一致）");
        return;
    }

    LevelBounds bounds = level_asset_bounds(asset);
    u32 columns = asset->tile_columns;
    u32 rows = asset->tile_rows;
    const f32 tile_size = asset->tile_size;

    // ---- 数量约束：上限来自实体注册表（游戏侧 assert 引用同一批常量）----
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
        u32 count = level_asset_entity_count(asset, kind);
        if (count > meta->max_count) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_ENTITY_COUNT, (int)kind, -1, -1,
                                  -1, "%s %u 个，运行时只支持 %u 个（多了游戏会直接 abort）", meta->name, count,
                                  meta->max_count);
        }
    }

    // ---- 实体矩形（表驱动：加一种带矩形的实体，这里自动覆盖到）----
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        if (LEVEL_ASSET_ENTITY_TABLE[kind].rect_offset == LEVEL_ASSET_NO_RECT) {
            continue;
        }
        u32 count = level_asset_entity_count(asset, kind);
        for (u32 i = 0; i < count; ++i) {
            level_asset_check_rect(out, kind, i, level_asset_entity_rect(asset, kind, i), &bounds);
        }
    }

    // ---- 出生点：关卡的单例属性，寄居在 tile 网格里 ----
    u32 spawn_count = 0;
    int spawn_col = -1;
    int spawn_row = -1;
    for (u32 row = 0; row < rows; ++row) {
        for (u32 col = 0; col < columns; ++col) {
            if (asset->tiles[row * columns + col] == LEVEL_TILE_SPAWN) {
                ++spawn_count;
                spawn_col = (int)col; // 多个时记最后一个：level.cc 也是「最后一个赢」
                spawn_row = (int)row;
            }
        }
    }
    if (spawn_count != LEVEL_ASSET_REQUIRED_SPAWN_TILES) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_SPAWN_COUNT, -1, -1, spawn_col, spawn_row,
                              "出生点（P）有 %u 个，必须恰好 %u 个；多个时游戏静默用最后一个", spawn_count,
                              LEVEL_ASSET_REQUIRED_SPAWN_TILES);
    }

    // ---- 各类实体自己的数值检查（运行字段不同，这里保持按类型写）----
    for (u32 i = 0; i < asset->mover_count; ++i) {
        const LevelMoverAsset *mover = &asset->movers[i];
        const char *name = LEVEL_ASSET_ENTITY_TABLE[LEVEL_ASSET_ENTITY_MOVER].name;
        int kind = (int)LEVEL_ASSET_ENTITY_MOVER;
        int index = (int)i;
        if (!level_asset_f32_finite(mover->point_a.x) || !level_asset_f32_finite(mover->point_a.y) ||
            !level_asset_f32_finite(mover->point_b.x) || !level_asset_f32_finite(mover->point_b.y) ||
            !level_asset_f32_finite(mover->t0) || !level_asset_f32_finite(mover->half_w) ||
            !level_asset_f32_finite(mover->half_h) || !level_asset_f32_finite(mover->speed)) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_NON_FINITE, kind, index, -1, -1,
                                  "%s #%u 的端点 / t0 / 尺寸 / 速度含 NaN 或 Inf", name, i);
            continue;
        }
        // 本体的矩形是**派生**的（表驱动的矩形检查会跳过它），所以在这里查尺寸与边界：
        // 它一直落在轴上，但轴本身可以伸到关卡外
        Rect2D rect = level_asset_mover_rect(mover);
        level_asset_check_rect(out, LEVEL_ASSET_ENTITY_MOVER, i, &rect, &bounds);
        if (mover->t0 < 0.0f || mover->t0 > 1.0f) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_PLATFORM_RANGE, kind, index, -1, -1,
                                  "%s #%u 的起始位置 t0 = %.2f 超出 [0,1]（运行时夹取，实际从端点开始）", name, i,
                                  mover->t0);
        }
        if (mover->point_a.x == mover->point_b.x && mover->point_a.y == mover->point_b.y) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_PLATFORM_RANGE, kind, index, -1, -1,
                                  "%s #%u 的两个端点重合（(%.0f, %.0f)）—— 没有可走的轴", name, i,
                                  mover->point_a.x, mover->point_a.y);
        } else if (mover->speed <= 0.0f) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_PLATFORM_RANGE, kind, index, -1, -1,
                                  "%s #%u 的速度是 %.1f：它永远不会动，不如直接摆一块静态地形", name, i,
                                  mover->speed);
        }
    }
    for (u32 i = 0; i < asset->monster_count; ++i) {
        const LevelMonsterAsset *monster = &asset->monsters[i];
        if (!level_asset_f32_finite(monster->spawn_x) || !level_asset_f32_finite(monster->spawn_y) ||
            !level_asset_f32_finite(monster->velocity_x)) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_NON_FINITE,
                                  (int)LEVEL_ASSET_ENTITY_MONSTER, (int)i, -1, -1,
                                  "怪物 #%u 的 spawn_x / spawn_y / velocity_x 含 NaN 或 Inf", i);
        }
    }
    // 传送点是**点**（没有矩形），所以上面那圈表驱动的矩形检查覆盖不到它：这里查有限值 + 在不在关卡里。
    // 不查「点下面有没有地面」：那要编译 tile，超出了「能读 ≠ 能玩」这类静态检查的范围 ——
    // 落点落在墙里由游戏侧的弹出保护兜底（与传送门同一条路）。
    for (u32 i = 0; i < asset->waypoint_count; ++i) {
        const LevelWaypointAsset *waypoint = &asset->waypoints[i];
        if (!level_asset_f32_finite(waypoint->x) || !level_asset_f32_finite(waypoint->y)) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_NON_FINITE,
                                  (int)LEVEL_ASSET_ENTITY_WAYPOINT, (int)i, -1, -1,
                                  "传送点 #%u 的坐标含 NaN 或 Inf", i);
            continue;
        }
        bool inside = waypoint->x >= bounds.min_x && waypoint->x <= bounds.max_x &&
                      waypoint->y >= bounds.min_y && waypoint->y <= bounds.max_y;
        if (!inside) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_OUT_OF_BOUNDS,
                                  (int)LEVEL_ASSET_ENTITY_WAYPOINT, (int)i, -1, -1,
                                  "传送点 #%u (%.0f, %.0f) 在关卡外（关卡 x [%.0f, %.0f] y [%.0f, %.0f]）", i,
                                  waypoint->x, waypoint->y, bounds.min_x, bounds.max_x, bounds.min_y,
                                  bounds.max_y);
        }
    }
    for (u32 i = 0; i < asset->connection_count; ++i) {
        const LevelConnectionAsset *connection = &asset->connections[i];
        if (!level_asset_f32_finite(connection->entry_x) || !level_asset_f32_finite(connection->entry_y)) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_NON_FINITE,
                                  (int)LEVEL_ASSET_ENTITY_CONNECTION, (int)i, -1, -1, "门 #%u 的落点含 NaN 或 Inf", i);
            continue;
        }
        if (connection->target_level >= LEVEL_ASSET_WORLD_COUNT) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_CONNECTION_TARGET,
                                  (int)LEVEL_ASSET_ENTITY_CONNECTION, (int)i, -1, -1,
                                  "门 #%u 指向世界 %u，但只有 %u 个世界（0..%u）", i, connection->target_level,
                                  LEVEL_ASSET_WORLD_COUNT, LEVEL_ASSET_WORLD_COUNT - 1);
            continue;
        }
        if (self_world_id >= 0 && connection->target_level == (u32)self_world_id) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_CONNECTION_TARGET,
                                  (int)LEVEL_ASSET_ENTITY_CONNECTION, (int)i, -1, -1,
                                  "门 #%u 指向自己（世界 %d），穿过去会原地循环", i, self_world_id);
        }
    }

    // ---- 传送门：配对角标要在调色板范围内，而且同一色号必须恰好 2 个（一对一互传）----
    for (u32 i = 0; i < asset->portal_count; ++i) {
        if (asset->portals[i].pair_id >= LEVEL_PORTAL_PALETTE_COUNT) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_PORTAL_PAIR,
                                  (int)LEVEL_ASSET_ENTITY_PORTAL, (int)i, -1, -1,
                                  "传送门 #%u 的配对角标 %u 超出调色板（0..%u）", i, asset->portals[i].pair_id,
                                  LEVEL_PORTAL_PALETTE_COUNT - 1);
        }
    }
    for (u32 pair_id = 0; pair_id < LEVEL_PORTAL_PALETTE_COUNT; ++pair_id) {
        u32 pair_count = 0;
        for (u32 i = 0; i < asset->portal_count; ++i) {
            if (asset->portals[i].pair_id == pair_id) {
                ++pair_count;
            }
        }
        if (pair_count != 0 && pair_count != LEVEL_ASSET_PORTALS_PER_PAIR) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_ERROR, LEVEL_ASSET_ISSUE_PORTAL_PAIR,
                                  (int)LEVEL_ASSET_ENTITY_PORTAL, -1, -1, -1,
                                  "色号 %u 的传送门有 %u 个，必须恰好 %u 个（一对一互传）", pair_id, pair_count,
                                  LEVEL_ASSET_PORTALS_PER_PAIR);
        }
    }

    // ---- 出生点的可玩性：脚下有没有地面、有没有和实体重叠 ----
    if (spawn_count == LEVEL_ASSET_REQUIRED_SPAWN_TILES && spawn_col >= 0) {
        f32 spawn_x = level_cell_center_x((u32)spawn_col, tile_size);
        f32 spawn_y = level_cell_bottom_y((u32)spawn_row, tile_size); // 脚底落在格子底边（与 level.cc 一致）

        // 用「一格宽的一半、站在落点上」的近似盒代表角色（共享层不知道角色真实尺寸）
        Rect2D stand_box = {};
        stand_box.center_x = spawn_x;
        stand_box.center_y = spawn_y + tile_size * 0.25f;
        stand_box.half_w = tile_size * 0.25f;
        stand_box.half_h = tile_size * 0.25f;

        bool has_ground = false;
        u32 below_row = (u32)spawn_row + 1;
        if (below_row < rows) {
            LevelTile below = asset->tiles[below_row * columns + (u32)spawn_col];
            has_ground = (below == LEVEL_TILE_SOLID || below == LEVEL_TILE_ONE_WAY);
        }
        if (!has_ground) {
            level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_SPAWN_NO_GROUND, -1, -1, spawn_col,
                                  spawn_row,
                                  "出生点正下方没有支撑（第 %u 行那一格不是实体或单向平台），"
                                  "角色一出现就会往下掉",
                                  (u32)spawn_row + 1);
        }

        for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
            if (LEVEL_ASSET_ENTITY_TABLE[kind].rect_offset == LEVEL_ASSET_NO_RECT) {
                continue;
            }
            u32 count = level_asset_entity_count(asset, kind);
            for (u32 i = 0; i < count; ++i) {
                const Rect2D *rect = level_asset_entity_rect(asset, kind, i);
                if (rect && test_rect_overlap(&stand_box, rect)) {
                    level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_SPAWN_IN_ENTITY, (int)kind,
                                          (int)i, spawn_col, spawn_row, "出生点落在%s #%u 里",
                                          LEVEL_ASSET_ENTITY_TABLE[kind].name, i);
                }
            }
        }
    }

    // ---- 过窄的竖直通道：行差 2 格 = 中间只空 1 格，角色（约 90px）挤不进去 ----
    // 行差 1 是实心块（墙、地面），那是正常的，只有「中间恰好空 1 格」才报。
    u32 tight_reported = 0;
    for (u32 col = 0; col < columns && tight_reported < 3; ++col) {
        int prev_row = -1;
        for (u32 row = 0; row < rows; ++row) {
            LevelTile tile = asset->tiles[row * columns + col];
            bool solid = (tile == LEVEL_TILE_SOLID || tile == LEVEL_TILE_ONE_WAY);
            if (!solid) {
                continue;
            }
            if (prev_row >= 0) {
                int delta = (int)row - prev_row;
                if (delta > 1 && delta < (int)LEVEL_ASSET_MIN_ROW_DELTA) {
                    level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_TIGHT_ROW_GAP, -1, -1,
                                          (int)col, (int)row,
                                          "第 %u 列：第 %d 行与第 %u 行之间只空 1 格（%.0fpx），"
                                          "角色挤不进去；上下平台的行差建议 ≥ %u 格",
                                          col, prev_row, row, tile_size, LEVEL_ASSET_MIN_ROW_DELTA);
                    ++tight_reported;
                    if (tight_reported >= 3) {
                        break;
                    }
                }
            }
            prev_row = (int)row;
        }
    }
    if (tight_reported >= 3) {
        level_asset_issue_add(out, LEVEL_ASSET_ISSUE_WARN, LEVEL_ASSET_ISSUE_TIGHT_ROW_GAP, -1, -1, -1, -1,
                              "还有更多过窄的竖直通道，不再逐条列出");
    }
}
