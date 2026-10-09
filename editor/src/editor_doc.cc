#include "editor_doc.h"
#include "shared/file.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

#include <stdarg.h> // va_list / va_start（消息区与诊断文本）
#include <stdio.h>  // vsnprintf / snprintf
#include <string.h> // memcpy / memset / memcmp / strcmp / strlen
#include <wchar.h>  // wcscpy_s / wcsncat_s（子进程命令行）

#include "win32_prefix.h" // 只有 editor_list_levels 的 FindFirstFileW 需要它

// ============================================================================
// 消息区
// ============================================================================

void editor_message(EditorMessages *messages, const char *fmt, ...)
{
    u32 slot = messages->count % EDITOR_MESSAGE_COUNT;
    va_list args;
    va_start(args, fmt);
    vsnprintf(messages->text[slot], EDITOR_MESSAGE_SIZE, fmt, args);
    va_end(args);
    ++messages->count;
}

// ============================================================================
// 内部工具
// ============================================================================

// 前向声明：撤销的实现住在文件后半（它要用实体数组工具），而 touch 与生命周期函数更靠前
internal void editor_undo_init(EditorDoc *doc);
internal void editor_undo_capture(const LevelAsset *src, EditorUndoSlot *slot);
internal bool editor_undo_commit(EditorDoc *doc);
internal void editor_doc_refresh_dirty(EditorDoc *doc);
internal u32 editor_tile_count(const LevelAsset *asset, LevelTile tile);

// 「文档变了」的**唯一出口**：诊断结果过期 + 落一格撤销 + 重算 dirty。
// 改文档的函数最后都调它 —— 所以撤销不需要在几十个改动点上各写一遍。
internal void editor_doc_touch(EditorDoc *doc)
{
    ++doc->revision;
    if (doc->undo.action_depth == 0) {
        editor_undo_commit(doc); // 手势期间不落格，由 editor_doc_action_end 收尾
    }
    editor_doc_refresh_dirty(doc);
}

internal bool editor_path_copy(wchar_t *dst, u32 dst_size, const wchar_t *src)
{
    u32 i = 0;
    while (src[i] != L'\0' && i + 1 < dst_size) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = L'\0';
    return src[i] == L'\0';
}

// 大小写不敏感的 ASCII 比较（只用于识别 first.bin / second.bin 这类固定文件名）
internal bool editor_path_equals_ascii(const wchar_t *a, const wchar_t *b)
{
    for (u32 i = 0;; ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca >= L'A' && ca <= L'Z') {
            ca = (wchar_t)(ca - L'A' + L'a');
        }
        if (cb >= L'A' && cb <= L'Z') {
            cb = (wchar_t)(cb - L'A' + L'a');
        }
        if (ca != cb) {
            return false;
        }
        if (ca == L'\0') {
            return true;
        }
    }
}

internal bool editor_cell_in_range(const EditorDoc *doc, u32 col, u32 row)
{
    return doc->asset.tiles && col < doc->asset.tile_columns && row < doc->asset.tile_rows;
}

// 一格的外沿矩形（与 level.cc 一致：第 0 行在最上面，世界 y 向上为正）
internal Rect2D editor_cell_rect(const LevelAsset *asset, u32 col, u32 row)
{
    f32 size = asset->tile_size;
    return Rect2D{ .center_x = level_cell_center_x(col, size),
                   .center_y = level_cell_center_y(row, size),
                   .half_w = size * 0.5f,
                   .half_h = size * 0.5f };
}

// 实体数组的统一视图。字段位置从共享注册表来，所以这里没有 4 个 switch。
struct EditorEntityArray
{
    void *data;
    u32 count;
    u32 element_size;
};

internal EditorEntityArray editor_entity_array(const EditorDoc *doc, u32 kind)
{
    EditorEntityArray array = {};
    if (kind >= LEVEL_ASSET_ENTITY_KIND_COUNT) {
        return array; // element_size = 0 即「非法 kind」
    }
    const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
    array.data = level_asset_entity_data(&doc->asset, kind);
    array.count = level_asset_entity_count(&doc->asset, kind);
    array.element_size = meta->element_size;
    return array;
}

internal void editor_entity_array_store(EditorDoc *doc, u32 kind, void *data, u32 count)
{
    if (kind >= LEVEL_ASSET_ENTITY_KIND_COUNT) {
        return;
    }
    // 注册表给的是字段偏移，直接按偏移写回（写的是指针或 u32，两者都是 4/8 字节对齐的 POD 成员）
    const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
    *(void **)((u8 *)&doc->asset + meta->array_offset) = data;
    *(u32 *)((u8 *)&doc->asset + meta->count_offset) = count;
}

// ============================================================================
// 文档生命周期
// ============================================================================

void editor_doc_new(EditorDoc *doc, u32 columns, u32 rows, f32 tile_size)
{
    *doc = {};

    if (columns == 0 || rows == 0) {
        return;
    }

    doc->asset.tile_columns = columns;
    doc->asset.tile_rows = rows;
    doc->asset.tile_size = tile_size;

    u64 tile_count = (u64)columns * rows;
    doc->asset.tiles = (LevelTile *)arena_push(tile_count);
    for (u64 i = 0; i < tile_count; ++i) {
        doc->asset.tiles[i] = LEVEL_TILE_EMPTY;
    }

    // 模板自带地面与出生点：新建出来的关卡立刻就能通过校验、立刻能跑
    if (rows >= 3) {
        for (u32 col = 0; col < columns; ++col) {
            doc->asset.tiles[(u64)(rows - 2) * columns + col] = LEVEL_TILE_SOLID;
            doc->asset.tiles[(u64)(rows - 1) * columns + col] = LEVEL_TILE_SOLID;
        }
    }
    if (columns > 2 && rows >= 3) {
        editor_doc_set_spawn(doc, 2, rows - 3);
    }

    doc->has_path = false;
    // 建立撤销环：模板状态就是第 0 格，也是 dirty 的基准（刚新建 = 没改过）
    editor_undo_init(doc);
    editor_doc_refresh_dirty(doc);
}

bool editor_doc_open(EditorDoc *doc, const wchar_t *path)
{
    char utf8[EDITOR_PATH_SIZE * 2] = {};
    wide_to_utf8(path, utf8, sizeof(utf8));

    LevelAsset loaded = {};
    if (!level_asset_load(path, &loaded)) {
        // level_asset_load 自己已经报了具体哪一项校验没过，这里补上路径
        LOG_WARN("editor: cannot open %s", utf8);
        return false;
    }

    *doc = {};
    doc->asset = loaded;
    editor_path_copy(doc->path, EDITOR_PATH_SIZE, path);
    doc->has_path = true;
    // 环随文档重建：刚打开的这一份就是第 0 格，也是 dirty 的基准
    editor_undo_init(doc);
    editor_doc_refresh_dirty(doc);

    LOG_INFO("editor: opened %s (%u x %u tiles @%.0fpx, spike cells %u, vanish cells %u, doors %u, platforms %u, monsters %u, portals %u, waypoints %u)",
             utf8,
             loaded.tile_columns, loaded.tile_rows, loaded.tile_size, editor_tile_count(&loaded, LEVEL_TILE_SPIKE),
             editor_tile_count(&loaded, LEVEL_TILE_VANISH),
             loaded.connection_count, loaded.mover_count, loaded.monster_count, loaded.portal_count,
             loaded.waypoint_count);
    return true;
}

bool editor_doc_reload(EditorDoc *doc)
{
    if (!doc->has_path) {
        return false;
    }
    return editor_doc_open(doc, doc->path);
}

bool editor_doc_save(EditorDoc *doc, const wchar_t *path, EditorMessages *messages)
{
    if (!path || path[0] == L'\0') {
        LOG_WARN("editor: save refused (no path yet)");
        editor_message(messages, "保存失败：还没有路径（用「另存为」给一个）");
        return false;
    }

    char utf8[EDITOR_PATH_SIZE * 2] = {};
    wide_to_utf8(path, utf8, sizeof(utf8));

    // 写入前先跑同一份语义校验：有 ERROR 就不写盘。
    // 「能读」不等于「能玩」，这一步是编辑器替游戏挡住的那些坑（见 docs/level-assets.md）。
    LevelAssetIssues issues = {};
    editor_doc_validate(doc, &issues);
    if (issues.error_count > 0) {
        LOG_WARN("editor: save refused: %s has %u validation error(s)", utf8, issues.error_count);
        for (u32 i = 0; i < issues.count; ++i) {
            if (issues.items[i].level == LEVEL_ASSET_ISSUE_ERROR) {
                LOG_WARN("editor:   %s", issues.items[i].text);
            }
        }
        editor_message(messages, "保存被拒绝：%u 条错误（右侧「检查」里逐条列了）", issues.error_count);
        return false;
    }

    if (!level_asset_save(path, &doc->asset)) {
        LOG_ERROR("editor: save failed: %s (write or read-back check)", utf8);
        editor_message(messages, "保存失败：写入或写完自读校验没通过（原文件没有被改动）");
        return false;
    }

    editor_path_copy(doc->path, EDITOR_PATH_SIZE, path);
    doc->has_path = true;
    // 保存成功 = 新的基准：dirty 从此拿它比，撤销回到这一刻会自己变回「干净」
    editor_undo_capture(&doc->asset, &doc->undo.saved);
    doc->undo.has_saved = true;
    editor_doc_refresh_dirty(doc);

    LOG_INFO("editor: saved %s (%u x %u tiles, spike cells %u, vanish cells %u, doors %u, platforms %u, monsters %u, %u warning(s))",
             utf8, doc->asset.tile_columns, doc->asset.tile_rows,
             editor_tile_count(&doc->asset, LEVEL_TILE_SPIKE),
             editor_tile_count(&doc->asset, LEVEL_TILE_VANISH),
             doc->asset.connection_count, doc->asset.mover_count, doc->asset.monster_count,
             issues.warn_count);

    if (issues.warn_count > 0) {
        editor_message(messages, "已保存 %s（自读校验通过；还有 %u 条警告）", utf8, issues.warn_count);
    } else {
        editor_message(messages, "已保存 %s（自读校验通过）", utf8);
    }
    return true;
}

// ============================================================================
// tile 与出生点
// ============================================================================

LevelTile editor_doc_tile(const EditorDoc *doc, u32 col, u32 row)
{
    if (!editor_cell_in_range(doc, col, row)) {
        return LEVEL_TILE_EMPTY;
    }
    return doc->asset.tiles[(u64)row * doc->asset.tile_columns + col];
}

// 数一种 tile 的格数（地刺格、出生点都用它）。零分配、O(格数)。
internal u32 editor_tile_count(const LevelAsset *asset, LevelTile tile)
{
    if (!asset->tiles) {
        return 0;
    }
    u32 count = 0;
    u64 tile_count = (u64)asset->tile_columns * asset->tile_rows;
    for (u64 i = 0; i < tile_count; ++i) {
        if (asset->tiles[i] == tile) {
            ++count;
        }
    }
    return count;
}

u32 editor_doc_tile_count(const EditorDoc *doc, LevelTile tile)
{
    return editor_tile_count(&doc->asset, tile);
}

u32 editor_doc_spawn_count(const EditorDoc *doc)
{
    return editor_tile_count(&doc->asset, LEVEL_TILE_SPAWN);
}

bool editor_doc_find_spawn(const EditorDoc *doc, u32 *col, u32 *row)
{
    const LevelAsset *asset = &doc->asset;
    if (!asset->tiles) {
        return false;
    }
    // 多个 P 时返回最后一个：level.cc 也是「最后一个赢」，界面要显示的就是游戏会用的那个
    bool found = false;
    for (u32 r = 0; r < asset->tile_rows; ++r) {
        for (u32 c = 0; c < asset->tile_columns; ++c) {
            if (asset->tiles[(u64)r * asset->tile_columns + c] == LEVEL_TILE_SPAWN) {
                *col = c;
                *row = r;
                found = true;
            }
        }
    }
    return found;
}

void editor_doc_set_spawn(EditorDoc *doc, u32 col, u32 row)
{
    if (!editor_cell_in_range(doc, col, row)) {
        return;
    }
    LevelAsset *asset = &doc->asset;

    // 已经是这一格、而且只有一个 → 什么都没变
    if (asset->tiles[(u64)row * asset->tile_columns + col] == LEVEL_TILE_SPAWN &&
        editor_doc_spawn_count(doc) == 1) {
        return;
    }

    // 出生点是**单例**：先把网格里所有 P 清掉，再放下新的
    u64 tile_count = (u64)asset->tile_columns * asset->tile_rows;
    for (u64 i = 0; i < tile_count; ++i) {
        if (asset->tiles[i] == LEVEL_TILE_SPAWN) {
            asset->tiles[i] = LEVEL_TILE_EMPTY;
        }
    }
    asset->tiles[(u64)row * asset->tile_columns + col] = LEVEL_TILE_SPAWN;
    LOG_INFO("editor: spawn moved to tile (%u, %u)", col, row);
    editor_doc_touch(doc);
}

void editor_doc_set_tile(EditorDoc *doc, u32 col, u32 row, LevelTile tile)
{
    if (!editor_cell_in_range(doc, col, row)) {
        return;
    }
    if (tile == LEVEL_TILE_SPAWN) {
        editor_doc_set_spawn(doc, col, row);
        return;
    }

    LevelTile *cell = &doc->asset.tiles[(u64)row * doc->asset.tile_columns + col];
    if (*cell == tile) {
        return; // 拖动时会反复写同一格，没变化就不置脏
    }
    *cell = tile;
    editor_doc_touch(doc);
}

void editor_doc_fill_tiles(EditorDoc *doc, LevelTile tile)
{
    LevelAsset *asset = &doc->asset;
    if (!asset->tiles) {
        return;
    }
    if (tile == LEVEL_TILE_SPAWN) {
        return; // 「整片涂成出生点」没有意义：它是单例
    }
    u64 tile_count = (u64)asset->tile_columns * asset->tile_rows;
    for (u64 i = 0; i < tile_count; ++i) {
        asset->tiles[i] = tile;
    }
    // 逐格涂不记日志（一次拖动几百次调用），整片操作才记
    LOG_INFO("editor: filled all %llu tiles with tile %u", (unsigned long long)tile_count, (u32)tile);
    editor_doc_touch(doc);
}

void editor_doc_clear_entities(EditorDoc *doc)
{
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        editor_entity_array_store(doc, kind, nullptr, 0);
    }
    LOG_INFO("editor: cleared all entities");
    editor_doc_touch(doc);
}

// ============================================================================
// 实体
// ============================================================================

u32 editor_doc_entity_count(const EditorDoc *doc, u32 kind)
{
    return editor_entity_array(doc, kind).count;
}

// 界面**拿着结构体指针**直接改了字段之后走这里。与内部 touch 是同一个动作 ——
// 两个名字的差别只在调用点读起来能看出「这次改动是谁做的」。
void editor_doc_mark_changed(EditorDoc *doc)
{
    editor_doc_touch(doc);
}

LevelConnectionAsset *editor_doc_connection(EditorDoc *doc, u32 index)
{
    return (LevelConnectionAsset *)level_asset_entity_at(&doc->asset, LEVEL_ASSET_ENTITY_CONNECTION, index);
}

LevelMoverAsset *editor_doc_mover(EditorDoc *doc, u32 index)
{
    return (LevelMoverAsset *)level_asset_entity_at(&doc->asset, LEVEL_ASSET_ENTITY_MOVER, index);
}

LevelMonsterAsset *editor_doc_monster(EditorDoc *doc, u32 index)
{
    return (LevelMonsterAsset *)level_asset_entity_at(&doc->asset, LEVEL_ASSET_ENTITY_MONSTER, index);
}

LevelPortalAsset *editor_doc_portal(EditorDoc *doc, u32 index)
{
    return (LevelPortalAsset *)level_asset_entity_at(&doc->asset, LEVEL_ASSET_ENTITY_PORTAL, index);
}

LevelWaypointAsset *editor_doc_waypoint(EditorDoc *doc, u32 index)
{
    return (LevelWaypointAsset *)level_asset_entity_at(&doc->asset, LEVEL_ASSET_ENTITY_WAYPOINT, index);
}

Rect2D *editor_doc_entity_rect_mut(EditorDoc *doc, u32 entity_kind, u32 index)
{
    if (entity_kind >= LEVEL_ASSET_ENTITY_KIND_COUNT) {
        return nullptr;
    }
    return level_asset_entity_rect_mut(&doc->asset, entity_kind, index);
}

u32 editor_doc_add_entity(EditorDoc *doc, u32 kind)
{
    EditorEntityArray array = editor_entity_array(doc, kind);
    if (array.element_size == 0) {
        return EDITOR_NO_PLATFORM;
    }

    // arena 不能 free，所以「增一个」= 分配新数组 + 拷贝旧内容。
    // 结构性改动是用户动作、量级几百字节，注释在此交代清楚。
    u32 old_count = array.count;
    u32 new_count = old_count + 1;
    u8 *new_data = (u8 *)arena_push((u64)new_count * array.element_size);
    if (old_count > 0) {
        memcpy(new_data, array.data, (u64)old_count * array.element_size);
    }
    memset(new_data + (u64)old_count * array.element_size, 0, array.element_size);
    editor_entity_array_store(doc, kind, new_data, new_count);

    // 默认值放在关卡中央：新加的东西如果在屏幕外，很容易以为「按钮没反应」
    const LevelAsset *asset = &doc->asset;
    Rect2D grid = level_grid_bounds(asset->tile_columns, asset->tile_rows, asset->tile_size);
    f32 mid_x = grid.center_x;
    f32 mid_y = grid.center_y;
    f32 size = asset->tile_size;

    switch (kind) {
    case LEVEL_ASSET_ENTITY_CONNECTION: {
        LevelConnectionAsset *connection = editor_doc_connection(doc, old_count);
        connection->side = LEVEL_CONNECTION_RIGHT;
        connection->facing = LEVEL_CONNECTION_FACE_RIGHT;
        connection->target_level = 1; // 默认指向 second.bin；在属性面板里改
        connection->entry_x = 0.0f;
        connection->entry_y = -size * 2.0f;
        break;
    }
    case LEVEL_ASSET_ENTITY_MOVER: {
        LevelMoverAsset *mover = editor_doc_mover(doc, old_count);
        // 默认：水平轴（两端各离中心四格）+ 从轴中间出发 + 方形平台（可站可驮、不伤害）
        mover->point_a = v2{ mid_x - size * 4.0f, mid_y };
        mover->point_b = v2{ mid_x + size * 4.0f, mid_y };
        mover->t0 = 0.5f;
        mover->half_w = size * 2.0f;
        mover->half_h = size * 0.25f;
        mover->speed = size; // 每格 1 秒，和 tile_size 一起缩放
        mover->shape = LEVEL_MOVER_SQUARE;
        mover->damaging = 0;
        break;
    }
    case LEVEL_ASSET_ENTITY_MONSTER: {
        LevelMonsterAsset *monster = editor_doc_monster(doc, old_count);
        monster->rect.center_x = mid_x;
        monster->rect.center_y = mid_y;
        monster->rect.half_w = size * 0.35f; // 编辑器默认值，随手改
        monster->rect.half_h = size * 0.5f;
        monster->spawn_x = mid_x;
        monster->spawn_y = mid_y;
        monster->velocity_x = -size;
        break;
    }
    case LEVEL_ASSET_ENTITY_PORTAL: {
        LevelPortalAsset *portal = editor_doc_portal(doc, old_count);
        portal->rect.center_x = mid_x;
        // 默认就站在地面上（编辑器新建的图最后两行是实体）：门底边 = 地面顶面
        portal->rect.center_y = -((f32)asset->tile_rows - 3.0f) * size;
        portal->rect.half_w = size * 0.5f; // 一格宽
        portal->rect.half_h = size * 1.0f; // 两格高
        // 每加两扇门自动成一对（同色 = 配对），不这样新加的门会因为“色号只有 1 个”而报 ERROR
        portal->pair_id = (old_count / 2u) % LEVEL_PORTAL_PALETTE_COUNT;
        break;
    }
    case LEVEL_ASSET_ENTITY_WAYPOINT: {
        // 传送点是**脚底**坐标，所以默认落在两格厚的地面顶面上（编辑器新建的图最后两行是实体）
        LevelWaypointAsset *waypoint = editor_doc_waypoint(doc, old_count);
        waypoint->x = mid_x;
        waypoint->y = -((f32)asset->tile_rows - 2.0f) * size;
        break;
    }
    default:
        break;
    }

    LOG_INFO("editor: added %s #%u (%u now)", LEVEL_ASSET_ENTITY_TABLE[kind].name, old_count,
             editor_doc_entity_count(doc, kind));
    editor_doc_touch(doc);
    return old_count;
}

bool editor_doc_remove_entity(EditorDoc *doc, u32 kind, u32 index)
{
    EditorEntityArray array = editor_entity_array(doc, kind);
    if (array.element_size == 0 || index >= array.count) {
        return false;
    }

    LOG_INFO("editor: removed %s #%u (%u left)", LEVEL_ASSET_ENTITY_TABLE[kind].name, index,
             array.count - 1);

    u32 new_count = array.count - 1;
    if (new_count == 0) {
        editor_entity_array_store(doc, kind, nullptr, 0);
    } else {
        u8 *new_data = (u8 *)arena_push((u64)new_count * array.element_size);
        const u8 *src = (const u8 *)array.data;
        if (index > 0) {
            memcpy(new_data, src, (u64)index * array.element_size);
        }
        if (index + 1 < array.count) {
            u32 tail = array.count - index - 1;
            memcpy(new_data + (u64)index * array.element_size, src + (u64)(index + 1) * array.element_size,
                   (u64)tail * array.element_size);
        }
        editor_entity_array_store(doc, kind, new_data, new_count);
    }

    editor_doc_touch(doc);
    return true;
}

// ============================================================================
// 撤销：快照环（数据结构与取舍见 editor_doc.h）
// ============================================================================

// 一格要多大：tiles + 各类实体按 capacity 预留
internal u32 editor_undo_storage_bytes(const LevelAsset *shape, const u32 *capacity)
{
    u32 bytes = (u32)((u64)shape->tile_columns * shape->tile_rows * sizeof(LevelTile));
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        bytes += capacity[kind] * LEVEL_ASSET_ENTITY_TABLE[kind].element_size;
    }
    return bytes;
}

// 分配一格的存储，并把 view 里的指针指进去（数量由 capture 写）
internal void editor_undo_slot_alloc(EditorUndoSlot *slot, const LevelAsset *shape, const u32 *capacity,
                                    u32 storage_bytes)
{
    slot->view = {};
    slot->view.tile_columns = shape->tile_columns;
    slot->view.tile_rows = shape->tile_rows;
    slot->view.tile_size = shape->tile_size;

    u8 *cursor = (u8 *)arena_push(storage_bytes);
    slot->storage = cursor;
    slot->view.tiles = (LevelTile *)cursor;
    cursor += (u64)shape->tile_columns * shape->tile_rows * sizeof(LevelTile);
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
        *(void **)((u8 *)&slot->view + meta->array_offset) = cursor;
        cursor += (u64)capacity[kind] * meta->element_size;
    }
}

// 把 src（现场资产，或另一格）的内容拷进 slot
internal void editor_undo_capture(const LevelAsset *src, EditorUndoSlot *slot)
{
    memcpy(slot->view.tiles, src->tiles, (u64)src->tile_columns * src->tile_rows * sizeof(LevelTile));
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
        u32 count = level_asset_entity_count(src, kind);
        *(u32 *)((u8 *)&slot->view + meta->count_offset) = count;
        const void *from = level_asset_entity_data(src, kind);
        if (count > 0 && from) {
            memcpy(level_asset_entity_data(&slot->view, kind), from, (u64)count * meta->element_size);
        }
    }
}

// 把一格的内容写回现场。实体数组要重新分配（arena 不能 free）：量级几百字节，与增删实体同级。
internal void editor_undo_apply(EditorDoc *doc, const EditorUndoSlot *slot)
{
    LevelAsset *asset = &doc->asset;
    memcpy(asset->tiles, slot->view.tiles, (u64)asset->tile_columns * asset->tile_rows * sizeof(LevelTile));
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
        u32 count = level_asset_entity_count(&slot->view, kind);
        if (count == 0) {
            editor_entity_array_store(doc, kind, nullptr, 0);
            continue;
        }
        u8 *data = (u8 *)arena_push((u64)count * meta->element_size);
        memcpy(data, level_asset_entity_data(&slot->view, kind), (u64)count * meta->element_size);
        editor_entity_array_store(doc, kind, data, count);
    }
}

// 建立 / 重建整个环（新建、打开、以及实体数超容时）。历史随之作废，但基准要保住。
internal void editor_undo_init(EditorDoc *doc)
{
    EditorUndoRing *ring = &doc->undo;
    const LevelAsset *asset = &doc->asset;

    EditorUndoSlot old_saved = ring->saved; // 指针仍指向旧 arena 块，重建不会覆盖它
    bool keep_saved = ring->enabled && ring->has_saved;

    *ring = {};
    if (!asset->tiles || asset->tile_columns == 0 || asset->tile_rows == 0) {
        return; // enabled = false：网格不成立，没有可撤销的东西
    }

    // 预留容量：够现在用就行（含基准那一份）；不够了由 editor_undo_commit 重建，不静默降级
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        u32 need = level_asset_entity_count(asset, kind);
        if (keep_saved) {
            u32 saved_count = level_asset_entity_count(&old_saved.view, kind);
            if (saved_count > need) {
                need = saved_count;
            }
        }
        if (need < EDITOR_UNDO_ENTITY_CAPACITY) {
            need = EDITOR_UNDO_ENTITY_CAPACITY;
        }
        u32 limit = LEVEL_ASSET_ENTITY_TABLE[kind].max_count;
        ring->capacity[kind] = need < limit ? need : limit;
    }

    ring->storage_bytes = editor_undo_storage_bytes(asset, ring->capacity);
    for (u32 i = 0; i < EDITOR_UNDO_STATES; ++i) {
        editor_undo_slot_alloc(&ring->slots[i], asset, ring->capacity, ring->storage_bytes);
    }
    editor_undo_slot_alloc(&ring->saved, asset, ring->capacity, ring->storage_bytes);
    ring->enabled = true;
    ring->has_saved = true;

    editor_undo_capture(asset, &ring->slots[0]); // 当前状态 = 第 0 格
    // 基准：重建前有就搬过来（它可能正是「上次保存的那一份」），否则就是当前状态
    editor_undo_capture(keep_saved ? &old_saved.view : asset, &ring->saved);
}

// 落一格快照。返回 true = 真的落了（内容有变化）
internal bool editor_undo_commit(EditorDoc *doc)
{
    EditorUndoRing *ring = &doc->undo;
    if (!ring->enabled) {
        return false;
    }

    // 实体数超过预留容量 → 重建环（历史作废，但撤销仍可用）。留 WARN，不静默降级。
    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        u32 count = level_asset_entity_count(&doc->asset, kind);
        if (count > ring->capacity[kind]) {
            LOG_WARN("editor: undo capacity exceeded (%s: %u > %u), rebuilding undo history",
                     LEVEL_ASSET_ENTITY_TABLE[kind].name, count, ring->capacity[kind]);
            editor_undo_init(doc);
            return true;
        }
    }

    // 内容与当前格相同 → 不落格（也就不会白白截断重做尾巴）
    if (level_asset_equal(&doc->asset, &ring->slots[ring->pos].view)) {
        return false;
    }

    ring->redo_count = 0; // 新动作作废重做尾巴
    if (ring->undo_count < EDITOR_UNDO_LAYERS) {
        ++ring->undo_count;
    }
    ring->pos = (ring->pos + 1) % EDITOR_UNDO_STATES; // 环满时这一步正好挤掉最旧的一格
    editor_undo_capture(&doc->asset, &ring->slots[ring->pos]);
    return true;
}

// 与「上次打开/保存」的那份逐字段比。撤销回到保存点时会自己变回干净（工业上叫 clean state）
internal void editor_doc_refresh_dirty(EditorDoc *doc)
{
    doc->dirty = !doc->undo.has_saved || !level_asset_equal(&doc->asset, &doc->undo.saved.view);
}

// 撤销/重做之后，选择里的编号可能已经不存在了（实体被删掉或加回来），这里夹一下
internal void editor_doc_clamp_selection(EditorDoc *doc, EditorSelection *selection)
{
    if (selection->kind == EDITOR_SEL_SPAWN) {
        u32 col = 0;
        u32 row = 0;
        if (!editor_doc_find_spawn(doc, &col, &row)) {
            *selection = {};
        }
        return;
    }
    if (editor_sel_is_entity(selection->kind)) {
        u32 kind = editor_sel_entity_kind(selection->kind);
        if (selection->index >= editor_doc_entity_count(doc, kind)) {
            *selection = {};
        }
    }
}

bool editor_doc_can_undo(const EditorDoc *doc)
{
    return doc->undo.enabled && doc->undo.undo_count > 0;
}

bool editor_doc_can_redo(const EditorDoc *doc)
{
    return doc->undo.enabled && doc->undo.redo_count > 0;
}

// 撤销 / 重做只有一处差别：游标往哪走、两个计数怎么动；写入现场与收尾是同一套
internal bool editor_doc_step(EditorDoc *doc, EditorSelection *selection, bool forward)
{
    editor_doc_action_end(doc); // 拖动途中被按了 Ctrl+Z：先把手势收尾，别把那一拖丢掉
    EditorUndoRing *ring = &doc->undo;
    if (!ring->enabled || (forward ? ring->redo_count == 0 : ring->undo_count == 0)) {
        return false;
    }

    if (forward) {
        ring->pos = (ring->pos + 1) % EDITOR_UNDO_STATES;
        ++ring->undo_count;
        --ring->redo_count;
    } else {
        ring->pos = (ring->pos + EDITOR_UNDO_STATES - 1) % EDITOR_UNDO_STATES;
        --ring->undo_count;
        ++ring->redo_count;
    }

    editor_undo_apply(doc, &ring->slots[ring->pos]);
    ++doc->revision; // 诊断结果随之过期（不自动重跑：那会每次分配一块，撤销几百次就白吃掉 arena）
    editor_doc_refresh_dirty(doc);
    editor_doc_clamp_selection(doc, selection);
    LOG_INFO("editor: %s (%u undo / %u redo left)", forward ? "redo" : "undo", ring->undo_count, ring->redo_count);
    return true;
}

bool editor_doc_undo(EditorDoc *doc, EditorSelection *selection)
{
    return editor_doc_step(doc, selection, false);
}

bool editor_doc_redo(EditorDoc *doc, EditorSelection *selection)
{
    return editor_doc_step(doc, selection, true);
}

void editor_doc_action_begin(EditorDoc *doc)
{
    ++doc->undo.action_depth;
}

void editor_doc_action_end(EditorDoc *doc)
{
    if (doc->undo.action_depth == 0) {
        return;
    }
    --doc->undo.action_depth;
    if (doc->undo.action_depth == 0) {
        editor_undo_commit(doc); // 「内容没变就不落格」的判据也在这里用，所以空手势不留痕迹
        editor_doc_refresh_dirty(doc);
    }
}

// ============================================================================
// 自检（--selftest）：撤销的用例表
// ============================================================================

global_variable u32 global_selftest_failed = 0;

void editor_selftest_begin(void)
{
    global_selftest_failed = 0;
}

void editor_selftest_check(bool ok, const char *fmt, ...)
{
    char detail[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    if (ok) {
        LOG_INFO("[selftest] ok      %s", detail);
    } else {
        ++global_selftest_failed;
        LOG_ERROR("[selftest] FAILED  %s", detail);
    }
}

u32 editor_selftest_failed(void)
{
    return global_selftest_failed;
}

u32 editor_doc_run_selftest(void)
{
    editor_selftest_begin();

    // 01 涂一格 → 撤销 → 重做；新建出来的文档是「干净」的
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 8, 6, 64.0f);
        bool clean_start = !editor_doc_can_undo(&doc) && !doc.dirty;
        editor_doc_set_tile(&doc, 1, 1, LEVEL_TILE_SOLID);
        bool painted = editor_doc_tile(&doc, 1, 1) == LEVEL_TILE_SOLID && doc.dirty;
        EditorSelection selection = {};
        bool undone = editor_doc_undo(&doc, &selection) && editor_doc_tile(&doc, 1, 1) == LEVEL_TILE_EMPTY;
        bool clean_again = !doc.dirty; // 回到基准 → 自动变干净
        bool redone = editor_doc_redo(&doc, &selection) && editor_doc_tile(&doc, 1, 1) == LEVEL_TILE_SOLID;
        editor_selftest_check(clean_start && painted && undone && clean_again && redone,
                              "undo paint: start=%d painted=%d undone=%d clean=%d redone=%d", clean_start ? 1 : 0,
                              painted ? 1 : 0, undone ? 1 : 0, clean_again ? 1 : 0, redone ? 1 : 0);
    }

    // 02 手势合并：一次拖动（begin/end 包住 6 格）只占一格历史
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 16, 8, 64.0f);
        editor_doc_action_begin(&doc);
        for (u32 i = 0; i < 6; ++i) {
            editor_doc_set_tile(&doc, i, 1, LEVEL_TILE_SOLID);
        }
        bool quiet_during = !editor_doc_can_undo(&doc);
        editor_doc_action_end(&doc);
        EditorSelection selection = {};
        bool one_step = editor_doc_undo(&doc, &selection);
        bool all_gone = true;
        for (u32 i = 0; i < 6; ++i) {
            if (editor_doc_tile(&doc, i, 1) != LEVEL_TILE_EMPTY) {
                all_gone = false;
            }
        }
        bool exhausted = !editor_doc_can_undo(&doc);
        editor_selftest_check(quiet_during && one_step && all_gone && exhausted,
                              "undo gesture: quiet_during=%d one_step=%d all_gone=%d exhausted=%d",
                              quiet_during ? 1 : 0, one_step ? 1 : 0, all_gone ? 1 : 0, exhausted ? 1 : 0);
    }

    // 03 内容没变就不落格（菜单开了又关、输入框改了又改回去）
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 8, 6, 64.0f);
        editor_doc_mark_changed(&doc);
        editor_doc_mark_changed(&doc);
        editor_selftest_check(!editor_doc_can_undo(&doc), "undo no-op: can_undo=%d (expected 0)",
                              editor_doc_can_undo(&doc) ? 1 : 0);
    }

    // 04 撤销之后的新动作截断重做尾巴
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 8, 6, 64.0f);
        editor_doc_set_tile(&doc, 1, 1, LEVEL_TILE_SOLID);
        EditorSelection selection = {};
        editor_doc_undo(&doc, &selection);
        bool can_redo = editor_doc_can_redo(&doc);
        editor_doc_set_tile(&doc, 2, 2, LEVEL_TILE_SOLID);
        editor_selftest_check(can_redo && !editor_doc_can_redo(&doc), "undo redo tail: before=%d after=%d",
                              can_redo ? 1 : 0, editor_doc_can_redo(&doc) ? 1 : 0);
    }

    // 05 实体增删也能撤销（数组是重新分配的，最容易出错的地方）；选择越界要被夹掉
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 8, 6, 64.0f);
        u32 index = editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_MONSTER);
        bool added = index == 0 && editor_doc_entity_count(&doc, LEVEL_ASSET_ENTITY_MONSTER) == 1;
        EditorSelection selection = {};
        selection.kind = editor_sel_of_entity(LEVEL_ASSET_ENTITY_MONSTER);
        selection.index = index;
        bool undone = editor_doc_undo(&doc, &selection) &&
                      editor_doc_entity_count(&doc, LEVEL_ASSET_ENTITY_MONSTER) == 0;
        bool clamped = selection.kind == EDITOR_SEL_NONE;
        bool redone = editor_doc_redo(&doc, &selection) &&
                      editor_doc_entity_count(&doc, LEVEL_ASSET_ENTITY_MONSTER) == 1;
        editor_selftest_check(added && undone && clamped && redone,
                              "undo entity: added=%d undone=%d clamped=%d redone=%d", added ? 1 : 0,
                              undone ? 1 : 0, clamped ? 1 : 0, redone ? 1 : 0);
    }

    // 06 字段改动（直接改 + mark_changed）也能撤销
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 8, 6, 64.0f);
        u32 index = editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_MOVER);
        LevelMoverAsset *mover = editor_doc_mover(&doc, index);
        mover->point_a.x = 1000.0f;
        editor_doc_mark_changed(&doc);
        f32 moved = mover->point_a.x;
        mover->point_a.x = 1500.0f;
        editor_doc_mark_changed(&doc);
        EditorSelection selection = {};
        editor_doc_undo(&doc, &selection);
        f32 back = editor_doc_mover(&doc, index)->point_a.x;
        editor_selftest_check(moved == 1000.0f && back == 1000.0f, "undo field: moved=%.1f back=%.1f", moved, back);
    }

    // 07 环满：只留最近 EDITOR_UNDO_LAYERS 步，最旧的被挤掉
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 16, 16, 64.0f);
        const u32 total = EDITOR_UNDO_LAYERS + 2;
        for (u32 i = 0; i < total; ++i) {
            editor_doc_set_tile(&doc, i % 16, i / 16, LEVEL_TILE_SOLID);
        }
        u32 depth = 0;
        EditorSelection selection = {};
        while (editor_doc_undo(&doc, &selection)) {
            ++depth;
        }
        bool capped = depth == EDITOR_UNDO_LAYERS;
        bool oldest_kept = editor_doc_tile(&doc, 0, 0) == LEVEL_TILE_SOLID &&
                           editor_doc_tile(&doc, 1, 0) == LEVEL_TILE_SOLID; // 最早的两格回不去了
        bool third_gone = editor_doc_tile(&doc, 2, 0) == LEVEL_TILE_EMPTY;
        editor_selftest_check(capped && oldest_kept && third_gone, "undo ring: depth=%u capped=%d oldest=%d third=%d",
                              depth, capped ? 1 : 0, oldest_kept ? 1 : 0, third_gone ? 1 : 0);
    }

    // 08 资产往返：写出去的文件必须能读回来，且逐字段相同。
    // 挡的是「写的和读的两边漂了」——v2 曾经把 header 长度算错 4 字节，那只在编辑器里手点保存时
    // 才会暴露（--check 只读；关卡文件当时也全是 v1）。路径固定在 build\ 下，所以从仓库根跑。
    {
        EditorDoc doc = {};
        editor_doc_new(&doc, 12, 8, 64.0f);
        editor_doc_set_tile(&doc, 1, 1, LEVEL_TILE_SOLID);
        editor_doc_set_tile(&doc, 2, 1, LEVEL_TILE_ONE_WAY);
        editor_doc_set_tile(&doc, 3, 1, LEVEL_TILE_SPIKE);
        editor_doc_set_tile(&doc, 4, 1, LEVEL_TILE_SPIKE);
        editor_doc_set_tile(&doc, 5, 1, LEVEL_TILE_VANISH);
        editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_CONNECTION);
        editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_MONSTER);
        editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_PORTAL);
        editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_PORTAL);
        editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_WAYPOINT);
        editor_doc_mark_changed(&doc);

        const wchar_t *path = L"build\\_selftest_asset.bin";
        bool saved = level_asset_save(path, &doc.asset);
        LevelAsset loaded = {};
        bool loaded_ok = saved && level_asset_load(path, &loaded);
        bool equal = loaded_ok && level_asset_equal(&doc.asset, &loaded);
        editor_selftest_check(equal, "asset roundtrip: save=%d load=%d equal=%d", saved ? 1 : 0,
                              loaded_ok ? 1 : 0, equal ? 1 : 0);
    }

    if (global_selftest_failed == 0) {
        LOG_INFO("[selftest] doc cases passed");
    } else {
        LOG_ERROR("[selftest] %u doc case(s) failed", global_selftest_failed);
    }
    return global_selftest_failed;
}

// ============================================================================
// 校验与诊断
// ============================================================================

int editor_doc_world_id(const EditorDoc *doc)
{
    if (!doc->has_path) {
        return -1;
    }
    const wchar_t *name = doc->path;
    for (const wchar_t *at = doc->path; *at != L'\0'; ++at) {
        if (*at == L'\\' || *at == L'/') {
            name = at + 1;
        }
    }
    if (editor_path_equals_ascii(name, L"first.bin")) {
        return 0;
    }
    if (editor_path_equals_ascii(name, L"second.bin")) {
        return 1;
    }
    return -1;
}

void editor_doc_validate(const EditorDoc *doc, LevelAssetIssues *issues)
{
    level_asset_validate(&doc->asset, editor_doc_world_id(doc), issues);
}

// 诊断文本里的 y 换算。两种显示方式差一个平移：display = world + 关卡高度。
// 尺寸（半高）不参与换算 —— 只有坐标才换。
internal f32 editor_diag_y(const LevelAsset *asset, const EditorDiagnostics *diag, f32 world_y)
{
    if (!diag->y_from_bottom) {
        return world_y;
    }
    return world_y + (f32)asset->tile_rows * asset->tile_size;
}

internal void editor_diag_line(EditorDiagnostics *diag, const char *fmt, ...)
{
    if (diag->line_count >= EDITOR_DIAGNOSTIC_LINE_COUNT) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(diag->lines[diag->line_count], EDITOR_DIAGNOSTIC_LINE_SIZE, fmt, args);
    va_end(args);
    ++diag->line_count;
}

internal const char *editor_tile_name(LevelTile tile)
{
    switch (tile) {
    case LEVEL_TILE_EMPTY:
        return "空";
    case LEVEL_TILE_SOLID:
        return "实体";
    case LEVEL_TILE_ONE_WAY:
        return "单向平台";
    case LEVEL_TILE_SPAWN:
        return "出生点";
    case LEVEL_TILE_SPIKE:
        return "地刺";
    case LEVEL_TILE_VANISH:
        return "可消失平台";
    default:
        return "未知";
    }
}

internal const char *editor_entity_name(u32 kind)
{
    if (kind >= LEVEL_ASSET_ENTITY_KIND_COUNT) {
        return "?";
    }
    return LEVEL_ASSET_ENTITY_TABLE[kind].name;
}

internal bool editor_entity_rect(const EditorDoc *doc, u32 kind, u32 index, Rect2D *out)
{
    *out = {};
    // 走 rect_now：移动组件的矩形是**派生**的（本体位置是轴上的参数），但诊断要看到的是同一份几何
    if (!level_asset_entity_rect_now(&doc->asset, kind, index, out)) {
        return false; // 越界，或者这种实体没有矩形（门 / 传送点）
    }
    return true;
}

void editor_doc_run_diagnostics(EditorDoc *doc, EditorDiagnostics *diag, const EditorSelection *selection,
                                int cursor_col, int cursor_row, bool y_from_bottom)
{
    // 旧的 Level / 反查表留在 arena 里不再引用（arena 只增不减）。
    // 每次检查约 7KB（编译产物）+ cols*rows*4（反查表）——
    // 「按钮触发」正是为了让它有界：不是每帧、也不是每次改动。
    *diag = {};
    diag->revision = doc->revision;
    diag->y_from_bottom = y_from_bottom;
    diag->valid = false;

    // 校验不过就不编译：游戏自己的 tile → 碰撞体编译器是按「这些规则已经成立」写的
    // —— src/level.cc 里那一串 assert 就是那些规则，拿一张有 ERROR 的图去编译会当场 assert
    // （实测：两个出生点 → `assert(!has_spawn && "关卡只能有一个出生点")`，
    //   调试构建弹断言框，命令行下则 abort（退出码 3））。
    // 这里刻意用与保存同一份校验，所以「能存盘」与「能编译」是同一个门槛
    LevelAssetIssues validation = {};
    editor_doc_validate(doc, &validation);
    if (validation.error_count > 0) {
        editor_diag_line(diag, "校验有 %u 条错误：不编译（游戏的关卡编译器会在非法图上 assert）",
                         validation.error_count);
        editor_diag_line(diag, "先按右侧「检查」里列出的问题改掉，诊断会自动能跑");
        LOG_WARN("editor: diagnostics skipped: %u validation error(s)", validation.error_count);
        return;
    }

    const LevelAsset *asset = &doc->asset;
    if (!asset->tiles || asset->tile_columns == 0 || asset->tile_rows == 0) {
        editor_diag_line(diag, "没有可诊断的网格");
        return;
    }

    level_build_from_asset(&diag->level, asset);

    // 反查表：格 → 合并后的碰撞体。判据是「**格中心点**落在哪个矩形里」，
    // 而不是「两个矩形相交」—— 相邻合并体共享边，相交判断会把邻块也算进来。
    u64 cell_count = (u64)asset->tile_columns * asset->tile_rows;
    diag->cell_platform = (u32 *)arena_push(cell_count * sizeof(u32));
    diag->cell_columns = asset->tile_columns;
    diag->cell_rows = asset->tile_rows;

    u32 solid_platforms = 0;
    u32 one_way_platforms = 0;
    for (u32 i = 0; i < diag->level.platform_count; ++i) {
        if (diag->level.platforms[i].kind == COLLIDER_ONE_WAY) {
            ++one_way_platforms;
        } else {
            ++solid_platforms;
        }
    }

    for (u32 row = 0; row < asset->tile_rows; ++row) {
        for (u32 col = 0; col < asset->tile_columns; ++col) {
            Rect2D cell = editor_cell_rect(asset, col, row);
            u32 index = EDITOR_NO_PLATFORM;
            for (u32 i = 0; i < diag->level.platform_count; ++i) {
                const Rect2D *rect = &diag->level.platforms[i].rect;
                if (cell.center_x >= rect->center_x - rect->half_w && cell.center_x <= rect->center_x + rect->half_w &&
                    cell.center_y >= rect->center_y - rect->half_h && cell.center_y <= rect->center_y + rect->half_h) {
                    index = i;
                    break;
                }
            }
            diag->cell_platform[(u64)row * asset->tile_columns + col] = index;
        }
    }

    // ---- 关卡概况 ----
    editor_diag_line(diag, "关卡 %u × %u 格，每格 %.0fpx，世界 ID %d",
                     asset->tile_columns, asset->tile_rows, asset->tile_size, editor_doc_world_id(doc));
    if (y_from_bottom) {
        editor_diag_line(diag, "y 轴：底部为 0、向上为正（关卡高 %.0f；原始世界 y = 此值 - %.0f）",
                         (f32)asset->tile_rows * asset->tile_size, (f32)asset->tile_rows * asset->tile_size);
    } else {
        editor_diag_line(diag, "y 轴：原始世界 y（关卡顶部为 0、向下为负）");
    }
    editor_diag_line(diag, "边界 x [%.0f, %.0f]  y [%.0f, %.0f]",
                     diag->level.bounds.center_x - diag->level.bounds.half_w,
                     diag->level.bounds.center_x + diag->level.bounds.half_w,
                     editor_diag_y(asset, diag, diag->level.bounds.center_y - diag->level.bounds.half_h),
                     editor_diag_y(asset, diag, diag->level.bounds.center_y + diag->level.bounds.half_h));
    editor_diag_line(diag, "编译后碰撞体 %u 块（实体 %u / 单向 %u）—— tile 被横向合并后的真实数量",
                     diag->level.platform_count, solid_platforms, one_way_platforms);
    editor_diag_line(diag, "实体：门 %u、移动组件 %u、怪物 %u、传送门 %u、传送点 %u；地刺格 %u、可消失格 %u",
                     asset->connection_count,
                     asset->mover_count, asset->monster_count, asset->portal_count,
                     asset->waypoint_count,
                     editor_tile_count(asset, LEVEL_TILE_SPIKE),
                     editor_tile_count(asset, LEVEL_TILE_VANISH));

    u32 spawn_count = editor_doc_spawn_count(doc);
    u32 spawn_col = 0;
    u32 spawn_row = 0;
    if (spawn_count == LEVEL_ASSET_REQUIRED_SPAWN_TILES && editor_doc_find_spawn(doc, &spawn_col, &spawn_row)) {
        f32 spawn_x = level_cell_center_x((u32)spawn_col, asset->tile_size);
        f32 spawn_y = level_cell_bottom_y((u32)spawn_row, asset->tile_size); // 脚底在格子底边
        editor_diag_line(diag, "出生点：格 (%u, %u) → 脚底落点 (%.1f, %.1f)",
                         spawn_col, spawn_row, spawn_x, editor_diag_y(asset, diag, spawn_y));
    } else {
        editor_diag_line(diag, "出生点：%u 个（应为 %u 个）", spawn_count, LEVEL_ASSET_REQUIRED_SPAWN_TILES);
    }

    // ---- 光标格 ----
    editor_diag_line(diag, " ");
    if (cursor_col < 0 || cursor_row < 0) {
        editor_diag_line(diag, "— 光标格：鼠标不在网格上 —");
    } else {
        u32 col = (u32)cursor_col;
        u32 row = (u32)cursor_row;
        LevelTile tile = editor_doc_tile(doc, col, row);
        Rect2D cell = editor_cell_rect(asset, col, row);
        editor_diag_line(diag, "— 光标格 (%u, %u) —", col, row);
        editor_diag_line(diag, "世界矩形 x [%.0f, %.0f] y [%.0f, %.0f]（中心 %.1f, %.1f）",
                         cell.center_x - cell.half_w, cell.center_x + cell.half_w,
                         editor_diag_y(asset, diag, cell.center_y - cell.half_h),
                         editor_diag_y(asset, diag, cell.center_y + cell.half_h),
                         cell.center_x, editor_diag_y(asset, diag, cell.center_y));
        editor_diag_line(diag, "tile = '%c'（%s）", (char)tile, editor_tile_name(tile));

        if (tile == LEVEL_TILE_SPAWN) {
            editor_diag_line(diag, "这是出生点：脚底落点 y = %.1f（格子底边，不是中心）",
                             editor_diag_y(asset, diag, level_cell_bottom_y(row, asset->tile_size)));
        }

        u32 platform_index = diag->cell_platform[(u64)row * asset->tile_columns + col];
        if (platform_index == EDITOR_NO_PLATFORM) {
            editor_diag_line(diag, "不属于任何碰撞体（空 / 出生点 / 可消失平台格）");
        } else {
            const Platform *platform = &diag->level.platforms[platform_index];
            u32 cell_total = 0;
            for (u64 i = 0; i < cell_count; ++i) {
                if (diag->cell_platform[i] == platform_index) {
                    ++cell_total;
                }
            }
            editor_diag_line(diag, "属于碰撞体 #%u（%s），合并矩形 x [%.0f, %.0f] y [%.0f, %.0f]，共 %u 格",
                             platform_index, (platform->kind == COLLIDER_ONE_WAY) ? "单向" : "实体",
                             platform->rect.center_x - platform->rect.half_w,
                             platform->rect.center_x + platform->rect.half_w,
                             editor_diag_y(asset, diag, platform->rect.center_y - platform->rect.half_h),
                             editor_diag_y(asset, diag, platform->rect.center_y + platform->rect.half_h),
                             cell_total);
        }

        bool covered = false;
        if (tile == LEVEL_TILE_SPIKE) {
            editor_diag_line(diag, "这一格是地刺（伤害，不阻挡）");
            covered = true;
        }
        for (u32 i = 0; i < asset->mover_count; ++i) {
            Rect2D mover_rect = level_asset_mover_rect(&asset->movers[i]);
            if (test_rect_overlap(&cell, &mover_rect)) {
                editor_diag_line(diag, "覆盖这一格：移动组件 #%u", i);
                covered = true;
            }
        }
        for (u32 i = 0; i < asset->monster_count; ++i) {
            if (test_rect_overlap(&cell, &asset->monsters[i].rect)) {
                editor_diag_line(diag, "覆盖这一格：怪物 #%u", i);
                covered = true;
            }
        }
        if (!covered) {
            editor_diag_line(diag, "没有实体覆盖这一格");
        }
    }

    // ---- 选中项 ----
    editor_diag_line(diag, " ");
    if (!selection || selection->kind == EDITOR_SEL_NONE) {
        editor_diag_line(diag, "— 未选中任何东西 —");
    } else if (selection->kind == EDITOR_SEL_SPAWN) {
        editor_diag_line(diag, "— 选中：出生点（关卡单例属性）—");
        editor_diag_line(diag, "网格里有 %u 个 P（必须是 1 个）", spawn_count);
    } else {
        u32 kind = editor_sel_entity_kind(selection->kind);
        const char *name = editor_entity_name(kind);
        editor_diag_line(diag, "— 选中：%s #%u —", name, selection->index);

        Rect2D rect = {};
        if (editor_entity_rect(doc, kind, selection->index, &rect)) {
            editor_diag_line(diag, "矩形 x [%.0f, %.0f] y [%.0f, %.0f]（%.0f × %.0f）",
                             rect.center_x - rect.half_w, rect.center_x + rect.half_w,
                             editor_diag_y(asset, diag, rect.center_y - rect.half_h),
                             editor_diag_y(asset, diag, rect.center_y + rect.half_h),
                             rect.half_w * 2.0f, rect.half_h * 2.0f);
        }

        if (kind == LEVEL_ASSET_ENTITY_MOVER) {
            const LevelMoverAsset *mover = editor_doc_mover(doc, selection->index);
            if (mover) {
                editor_diag_line(diag, "轴 A (%.0f, %.0f) → B (%.0f, %.0f)：本体按参数 %.2f 贴在轴上，速度 %.0f px/s（沿轴）",
                                 mover->point_a.x, editor_diag_y(asset, diag, mover->point_a.y), mover->point_b.x,
                                 editor_diag_y(asset, diag, mover->point_b.y), mover->t0, mover->speed);
                editor_diag_line(diag, "形状：%s",
                                 (mover->shape == LEVEL_MOVER_CIRCLE)
                                     ? "圆形（实心不可穿过；碰到就伤害）"
                                     : (mover->damaging ? "方形（可站可驮；被挤住会伤害）"
                                                        : "方形（可站可驮；不伤害）"));
            }
        } else if (kind == LEVEL_ASSET_ENTITY_MONSTER) {
            const LevelMonsterAsset *monster = editor_doc_monster(doc, selection->index);
            if (monster) {
                editor_diag_line(diag, "复位点 (%.0f, %.0f)，速度 %.0f px/s（掉出世界或被打中后回到复位点）",
                                 monster->spawn_x, editor_diag_y(asset, diag, monster->spawn_y),
                                 monster->velocity_x);
            }
        } else if (kind == LEVEL_ASSET_ENTITY_CONNECTION) {
            const LevelConnectionAsset *connection = editor_doc_connection(doc, selection->index);
            if (connection) {
                const char *target = "未知关卡";
                if (connection->target_level == 0) {
                    target = "first.bin（世界 0）";
                } else if (connection->target_level == 1) {
                    target = "second.bin（世界 1）";
                }
                editor_diag_line(diag, "在%s边缘，通向 %s，落点 (%.0f, %.0f)（脚底坐标）",
                                 (connection->side == LEVEL_CONNECTION_LEFT) ? "左侧" : "右侧",
                                 target, connection->entry_x, editor_diag_y(asset, diag, connection->entry_y));
            }
        }
    }

    diag->valid = true;

    LOG_INFO("editor: diagnostics rebuilt (revision %u, %u colliders: %u solid / %u one-way)", diag->revision,
             diag->level.platform_count, solid_platforms, one_way_platforms);
}

u32 editor_list_levels(char names[][EDITOR_LEVEL_NAME_SIZE], u32 max_count)
{
    WIN32_FIND_DATAW find_data = {};
    HANDLE find = FindFirstFileW(L"data/map/*.bin", &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }

    u32 count = 0;
    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        char utf8[EDITOR_LEVEL_NAME_SIZE] = {};
        if (!wide_to_utf8(find_data.cFileName, utf8, sizeof(utf8))) {
            continue;
        }

        // 插入排序：数量是个位数，顺便把列表按键名排好，便于人工找
        u32 at = count;
        while (at > 0 && strcmp(names[at - 1], utf8) > 0) {
            --at;
        }
        if (at >= max_count) {
            continue;
        }
        u32 last = MIN(count, max_count - 1);
        for (u32 i = last; i > at; --i) {
            memcpy(names[i], names[i - 1], EDITOR_LEVEL_NAME_SIZE);
        }
        memcpy(names[at], utf8, EDITOR_LEVEL_NAME_SIZE);
        if (count < max_count) {
            ++count;
        }
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
    return count;
}

// ============================================================================
// 一键闭环：起主程序
// ============================================================================

bool editor_run_game(const wchar_t *arguments, bool wait, u32 *exit_code)
{
    // CreateProcessW 要求命令行缓冲可写。_TRUNCATE = 放不下就截断（与原来的手写循环一致，而不是报错）
    wchar_t command_line[512] = {};
    wcscpy_s(command_line, L"build\\main.exe");
    if (arguments && arguments[0] != L'\0') {
        wcsncat_s(command_line, array_size(command_line), L" ", _TRUNCATE);
        wcsncat_s(command_line, array_size(command_line), arguments, _TRUNCATE);
    }

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {};

    // lpCurrentDirectory = nullptr：子进程继承本进程的工作目录（也就是仓库根）
    LOG_INFO("editor: launching %ls", command_line);
    if (!CreateProcessW(nullptr, command_line, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup_info,
                        &process_info)) {
        LOG_ERROR("editor: cannot launch %ls (run build.bat first, and start the editor from the repo root)",
                  command_line);
        return false;
    }

    if (wait) {
        WaitForSingleObject(process_info.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process_info.hProcess, &code);
        if (exit_code) {
            *exit_code = (u32)code;
        }
        LOG_INFO("editor: %ls exited with %lu", command_line, (unsigned long)code);
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

// ============================================================================
// 无界面自检（--check）
//
// 为什么值得有：编辑器此前只有「人看着界面点」这一条验证路径，UI 又只有像素可比 ——
// 人看得累、AI 更没法用（截图在这台机器上连文字都读不出来，见 AGENTS 4.5）。
// 这个入口把「这批关卡资产还好吗」变成一条命令：**文本报告 + 退出码**，
// 并且它调的就是「检查」面板那两个函数，所以两边语义不会漂。
//
// 它不建窗口、不初始化 D3D/ImGui —— 因此可以在没有显示器/没有焦点的环境里跑。
// ============================================================================

// 报告缓冲：一张图的诊断最多 24 行 ×192B，加上校验问题（最多 64 条）与 game.log 摘录，
// 两张图 64KB 足够。arena 只增不减，但这是进程级一次性开销
internal constexpr u32 EDITOR_CHECK_REPORT_SIZE = KB(64);

struct EditorCheckReport
{
    char *data;
    u32 capacity;
    u32 used;
    bool truncated;
};

internal void check_printf(EditorCheckReport *report, const char *fmt, ...)
{
    if (report->used + 1 >= report->capacity) {
        report->truncated = true;
        return;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(report->data + report->used, report->capacity - report->used, fmt, args);
    va_end(args);
    if (written <= 0) {
        return;
    }

    u32 room = report->capacity - report->used - 1;
    if ((u32)written > room) {
        report->used += room;
        report->truncated = true;
        return;
    }
    report->used += (u32)written;
}

internal bool check_line_has(const char *line, u32 length, const char *needle)
{
    u32 needle_len = (u32)strlen(needle);
    if (needle_len == 0 || length < needle_len) {
        return false;
    }
    for (u32 i = 0; i + needle_len <= length; ++i) {
        if (memcmp(line + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

// 冒烟跑挂了就把 game.log 里的失败行抄进报告：报告要能自己回答「为什么」，
// 而不是只说「退出码 1，自己去看别的文件」
internal void check_append_log_failures(EditorCheckReport *report)
{
    ReadFileRes file = read_file(L"game.log");
    if (!file.contents || file.file_size == 0) {
        return;
    }

    const char *text = (const char *)file.contents;
    u32 shown = 0;
    u32 total = 0;
    u32 at = 0;
    while (at < file.file_size) {
        u32 end = at;
        while (end < file.file_size && text[end] != '\n') {
            ++end;
        }
        u32 length = end - at;
        if (check_line_has(text + at, length, "FAIL") || check_line_has(text + at, length, "TAPE:") ||
            check_line_has(text + at, length, "[ERROR]") || check_line_has(text + at, length, "[WARN ")) {
            ++total;
            if (shown == 0) {
                check_printf(report, "  --- game.log: failure lines ---\n");
            }
            if (shown < 12) {
                // 日志单行最长 1KB（见 logger.cc），照抄一行不会溢出
                check_printf(report, "  %.*s\n", (int)length, text + at);
                ++shown;
            }
        }
        at = end + 1;
    }
    if (total > shown) {
        check_printf(report, "  ... and %u more line(s), all in game.log\n", total - shown);
    }
}

// 有父控制台就顺手回显一份（人从终端跑时不用再去开报告文件）。
// 没有就算了 —— **报告文件才是唯一的真相来源**：GUI 子系统进程的控制台输出与
// 调用方的等待/顺序都不可靠（AGENTS 4.5 记过这条）
internal void check_echo_to_console(const char *text, u32 size)
{
    if (size == 0 || !AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }

    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(out, text, size, &written, nullptr);
        CloseHandle(out);
    }
    // 解绑：否则调用方的 shell 会拿不回控制权（也会让输出的顺序变得难解释）
    FreeConsole();
}

// 一张图：加载 → 语义校验 → 编译诊断。返回 false = 这张图有 ERROR
internal bool editor_check_one_map(EditorCheckReport *report, const wchar_t *path)
{
    char utf8[EDITOR_PATH_SIZE * 2] = {};
    wide_to_utf8(path, utf8, sizeof(utf8));
    check_printf(report, "\n--- %s\n", utf8);

    EditorDoc doc = {};
    if (!editor_doc_open(&doc, path)) {
        // 具体哪一项没过已由 level_asset_load 写进 editor.log
        check_printf(report, "load: FAILED (magic / version / length / counts — details in editor.log)\n");
        check_printf(report, "RESULT: FAILED\n");
        return false;
    }

    const LevelAsset *asset = &doc.asset;
    check_printf(report, "map: %u x %u tiles @%.0fpx, world id %d\n", asset->tile_columns, asset->tile_rows,
                 asset->tile_size, editor_doc_world_id(&doc));
    check_printf(report, "entities: doors %u  movers %u  monsters %u  portals %u  waypoints %u  spike cells %u  vanish cells %u\n",
                 asset->connection_count, asset->mover_count, asset->monster_count, asset->portal_count,
                 asset->waypoint_count, editor_tile_count(asset, LEVEL_TILE_SPIKE),
                 editor_tile_count(asset, LEVEL_TILE_VANISH));

    LevelAssetIssues issues = {};
    editor_doc_validate(&doc, &issues);
    check_printf(report, "validate: %u error(s), %u warning(s)%s\n", issues.error_count, issues.warn_count,
                 issues.truncated ? " (list truncated)" : "");
    for (u32 i = 0; i < issues.count; ++i) {
        check_printf(report, "%s: %s\n", (issues.items[i].level == LEVEL_ASSET_ISSUE_ERROR) ? "ERROR" : "WARN",
                     issues.items[i].text);
    }

    // 编译诊断：与界面上的「检查」按钮同一份代码（含游戏自己的 tile → 碰撞体编译器）。
    // y 轴按界面默认的「底部为 0」写，诊断文本第一行会自述用的是哪一种
    EditorDiagnostics diag = {};
    EditorSelection selection = { EDITOR_SEL_NONE, 0 };
    editor_doc_run_diagnostics(&doc, &diag, &selection, -1, -1, true);
    check_printf(report, "diagnostics:\n");
    for (u32 i = 0; i < diag.line_count; ++i) {
        check_printf(report, "  %s\n", diag.lines[i]);
    }

    bool ok = (issues.error_count == 0) && diag.valid;
    check_printf(report, "RESULT: %s\n", ok ? "OK" : "FAILED");
    return ok;
}

bool editor_check_run(const wchar_t *map_path, bool smoke, const wchar_t *report_path)
{
    // arena_push 不给空指针（容量不够时它自己 assert），所以这里没有空指针分支
    char *buffer = (char *)arena_push(EDITOR_CHECK_REPORT_SIZE);
    EditorCheckReport report = { buffer, EDITOR_CHECK_REPORT_SIZE, 0, false };
    const wchar_t *out_path = (report_path && report_path[0] != L'\0') ? report_path : L"build/editor_check.txt";

    check_printf(&report, "editor check: load + validate + compile diagnostics (no window)\n");

    // 先把表头落盘（覆盖写）：万一检查中途崩了，磁盘上留下的是一份**没有 RESULT 行**的残缺报告，
    // 而不是上一次运行那份「看着 OK」的旧报告 —— 少一种误判。（全文在结尾再写一遍覆盖）
    write_file(out_path, report.used, report.data, false);

    // 要检查哪些图：给了路径就只查它，否则查 data/map 下全部（和「打开」菜单同一份列表）
    wchar_t paths[EDITOR_LEVEL_LIST_MAX][EDITOR_PATH_SIZE] = {};
    u32 map_count = 0;
    if (map_path && map_path[0] != L'\0') {
        editor_path_copy(paths[0], EDITOR_PATH_SIZE, map_path);
        map_count = 1;
    } else {
        char names[EDITOR_LEVEL_LIST_MAX][EDITOR_LEVEL_NAME_SIZE] = {};
        u32 found = editor_list_levels(names, EDITOR_LEVEL_LIST_MAX);
        for (u32 i = 0; i < found && map_count < EDITOR_LEVEL_LIST_MAX; ++i) {
            char utf8[EDITOR_LEVEL_NAME_SIZE * 2] = {};
            snprintf(utf8, sizeof(utf8), "data/map/%s", names[i]);
            if (utf8_to_wide(utf8, paths[map_count], EDITOR_PATH_SIZE)) {
                ++map_count;
            }
        }
    }

    if (map_count == 0) {
        check_printf(&report, "\nno map found (looked in data/map/*.bin)\nRESULT: FAILED\n");
    }

    u32 failed_maps = 0;
    for (u32 i = 0; i < map_count; ++i) {
        if (!editor_check_one_map(&report, paths[i])) {
            ++failed_maps;
        }
    }
    check_printf(&report, "\nmaps: %u checked, %u with errors\n", map_count, failed_maps);

    // 冒烟：让**主程序**回答「这张图能不能加载、出生点站得住、能走能跳」。
    // 游戏固定从第一世界（data/map/first.bin）出发，所以它只回答第一世界 —— 这点写进报告
    int smoke_exit = -1;
    bool smoke_ok = true;
    if (smoke) {
        if (failed_maps > 0) {
            check_printf(&report, "smoke: skipped (a map above has errors)\n");
            smoke_ok = false;
        } else {
            u32 exit_code = 1;
            if (!editor_run_game(L"--fast input_script test/smoke.txt", true, &exit_code)) {
                check_printf(&report, "smoke: cannot launch build\\main.exe (run build.bat first)\n");
                smoke_ok = false;
            } else {
                smoke_exit = (int)exit_code;
                check_printf(&report, "smoke: build\\main.exe --fast input_script test/smoke.txt -> exit %d (%s)\n",
                             smoke_exit, (smoke_exit == 0) ? "PASS" : "FAIL");
                check_printf(&report, "  (the game always starts in world 0 = data/map/first.bin)\n");
                if (smoke_exit != 0) {
                    check_append_log_failures(&report);
                    smoke_ok = false;
                }
            }
        }
    }

    bool ok = (failed_maps == 0) && smoke_ok;
    check_printf(&report, "RESULT: %s\n", ok ? "OK" : "FAILED");
    if (report.truncated) {
        check_printf(&report, "(the report was truncated: buffer is %u bytes)\n", EDITOR_CHECK_REPORT_SIZE);
    }

    if (!write_file(out_path, report.used, report.data, false)) {
        LOG_ERROR("editor check: cannot write the report to %ls", out_path);
        ok = false;
    }

    LOG_INFO("editor: check done (%u map(s), %u with errors%s) -> %ls", map_count, failed_maps,
             smoke ? (ok ? ", smoke passed" : ", smoke failed") : "", out_path);

    check_echo_to_console(report.data, report.used);
    return ok;
}
