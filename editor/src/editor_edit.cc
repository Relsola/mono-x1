#include "editor_edit.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

// ============================================================================
// 编辑语义（纯）：实现
//
// 这一层不认识 ImGui、不认识画布、不认识 UI 状态 —— 见 editor_edit.h 的说明。
// 别名提醒：世界 y 向上为正；出生点标记画在格子**底边**（y = -(row+1)*tile_size）。
// ============================================================================

f32 editor_edit_snap(f32 value, f32 step)
{
    return (step > 0.0f) ? roundf(value / step) * step : value;
}

bool editor_edit_point_in_rect(const Rect2D *rect, f32 x, f32 y)
{
    return x >= rect->center_x - rect->half_w && x <= rect->center_x + rect->half_w &&
           y >= rect->center_y - rect->half_h && y <= rect->center_y + rect->half_h;
}

void editor_edit_world_to_cell(const LevelAsset *asset, f32 world_x, f32 world_y, int *col, int *row)
{
    *col = (int)floorf(world_x / asset->tile_size);
    *row = (int)floorf(-world_y / asset->tile_size);
    if (*col < 0 || *row < 0 || (u32)*col >= asset->tile_columns || (u32)*row >= asset->tile_rows) {
        *col = -1;
        *row = -1;
    }
}

bool editor_edit_selection_is(const EditorSelection *selection, u32 entity_kind, u32 index)
{
    return selection->kind == editor_sel_of_entity(entity_kind) && selection->index == index;
}

// 出生点标记的命中框：以格子中心为水平中心、底边为下沿的一小块
internal bool editor_edit_spawn_marker(const EditorDoc *doc, f32 world_x, f32 world_y)
{
    const LevelAsset *asset = &doc->asset;
    u32 spawn_col = 0;
    u32 spawn_row = 0;
    if (!editor_doc_find_spawn(doc, &spawn_col, &spawn_row)) {
        return false;
    }

    f32 half = asset->tile_size * 0.25f;
    f32 center_x = level_cell_center_x((u32)spawn_col, asset->tile_size);
    f32 bottom = level_cell_bottom_y((u32)spawn_row, asset->tile_size);
    return world_x >= center_x - half && world_x <= center_x + half && world_y >= bottom &&
           world_y <= bottom + half * 2.0f;
}

// 传送点的命中框：它本身就是一个点（**脚底**坐标），所以命中框是以那个点为心的一小格
internal bool editor_edit_waypoint_marker(const LevelAsset *asset, const LevelWaypointAsset *waypoint, f32 world_x,
                                          f32 world_y)
{
    f32 half = asset->tile_size * 0.25f;
    return world_x >= waypoint->x - half && world_x <= waypoint->x + half &&
           world_y >= waypoint->y - half && world_y <= waypoint->y + half;
}

EditorSelection editor_edit_hit_body(const EditorDoc *doc, f32 world_x, f32 world_y)
{
    const LevelAsset *asset = &doc->asset;

    if (editor_edit_spawn_marker(doc, world_x, world_y)) {
        return EditorSelection{ EDITOR_SEL_SPAWN, 0 };
    }

    // 传送点是**点**：它没有矩形，所以命中要单独测（与出生点标记同一类，先测、且优先于矩形实体）
    for (u32 i = 0; i < asset->waypoint_count; ++i) {
        if (editor_edit_waypoint_marker(asset, &asset->waypoints[i], world_x, world_y)) {
            return EditorSelection{ editor_sel_of_entity(LEVEL_ASSET_ENTITY_WAYPOINT), i };
        }
    }

    // 种类倒序：小的东西画在上面 → 先被点到
    // 用 rect_now：移动组件的矩形是**派生**的（它没有存储矩形），但它的本体当然可点
    for (u32 kind = LEVEL_ASSET_ENTITY_KIND_COUNT; kind-- > 0;) {
        u32 count = level_asset_entity_count(asset, kind);
        for (u32 i = 0; i < count; ++i) {
            Rect2D rect = {};
            if (level_asset_entity_rect_now(asset, kind, i, &rect) &&
                editor_edit_point_in_rect(&rect, world_x, world_y)) {
                return EditorSelection{ editor_sel_of_entity(kind), i };
            }
        }
    }

    return EditorSelection{ EDITOR_SEL_NONE, 0 };
}

// 边/角手柄命中 → 边掩码（0 = 没命中）。半径是世界单位（UI 用 6px / zoom 换算）
internal u32 editor_edit_handle_mask(const Rect2D *rect, EditorPointer pointer)
{
    f32 left = rect->center_x - rect->half_w;
    f32 right = rect->center_x + rect->half_w;
    f32 bottom = rect->center_y - rect->half_h;
    f32 top = rect->center_y + rect->half_h;

    if (pointer.x < left - pointer.handle_radius || pointer.x > right + pointer.handle_radius ||
        pointer.y < bottom - pointer.handle_radius || pointer.y > top + pointer.handle_radius) {
        return 0;
    }

    u32 mask = 0;
    if (fabsf(pointer.x - left) <= pointer.handle_radius) {
        mask |= EDITOR_EDGE_LEFT;
    }
    if (fabsf(pointer.x - right) <= pointer.handle_radius) {
        mask |= EDITOR_EDGE_RIGHT;
    }
    if (fabsf(pointer.y - top) <= pointer.handle_radius) {
        mask |= EDITOR_EDGE_TOP;
    }
    if (fabsf(pointer.y - bottom) <= pointer.handle_radius) {
        mask |= EDITOR_EDGE_BOTTOM;
    }
    return mask;
}

u32 editor_edit_hit_grip(const EditorDoc *doc, const EditorSelection *selection, EditorPointer pointer,
                         u32 *edge_mask_out)
{
    *edge_mask_out = 0;
    if (!editor_sel_is_entity(selection->kind)) {
        return EDITOR_DRAG_NONE;
    }

    u32 entity_kind = editor_sel_entity_kind(selection->kind);
    u32 index = selection->index;

    // 往返端点：通常在矩形之外，必须先测（二维 —— 轴可以是任意方向）
    if (entity_kind == LEVEL_ASSET_ENTITY_MOVER) {
        const LevelMoverAsset *mover =
            (const LevelMoverAsset *)level_asset_entity_at(&doc->asset, entity_kind, index);
        if (mover) {
            if (fabsf(pointer.x - mover->point_a.x) <= pointer.near_radius &&
                fabsf(pointer.y - mover->point_a.y) <= pointer.near_radius) {
                return EDITOR_DRAG_RANGE_A;
            }
            if (fabsf(pointer.x - mover->point_b.x) <= pointer.near_radius &&
                fabsf(pointer.y - mover->point_b.y) <= pointer.near_radius) {
                return EDITOR_DRAG_RANGE_B;
            }
        }
    }

    // 怪物复位点：通常落在矩形内部，所以也要先测
    if (entity_kind == LEVEL_ASSET_ENTITY_MONSTER) {
        const LevelMonsterAsset *monster =
            (const LevelMonsterAsset *)level_asset_entity_at(&doc->asset, entity_kind, index);
        if (monster && fabsf(pointer.x - monster->spawn_x) <= pointer.near_radius &&
            fabsf(pointer.y - monster->spawn_y) <= pointer.near_radius) {
            return EDITOR_DRAG_MONSTER_RESPAWN;
        }
    }

    // 传送点：它本身就是一个点（没有矩形），所以命中点就是它的唯一抓手
    if (entity_kind == LEVEL_ASSET_ENTITY_WAYPOINT) {
        const LevelWaypointAsset *waypoint =
            (const LevelWaypointAsset *)level_asset_entity_at(&doc->asset, entity_kind, index);
        if (waypoint && fabsf(pointer.x - waypoint->x) <= pointer.near_radius &&
            fabsf(pointer.y - waypoint->y) <= pointer.near_radius) {
            return EDITOR_DRAG_WAYPOINT;
        }
    }

    Rect2D rect = {};
    if (level_asset_entity_rect_now(&doc->asset, entity_kind, index, &rect)) {
        u32 mask = editor_edit_handle_mask(&rect, pointer);
        if (mask != 0) {
            *edge_mask_out = mask;
            return EDITOR_DRAG_RESIZE;
        }
    }

    return EDITOR_DRAG_NONE;
}

EditorDragBegin editor_edit_begin_drag(const EditorDoc *doc, const EditorSelection *selection, EditorPointer pointer)
{
    EditorDragBegin result = {};
    result.selection = *selection;
    result.drag.grab_x = pointer.x;
    result.drag.grab_y = pointer.y;

    // 1) 出生点标记（关卡单例属性，位置上和 tile 一样按格吸附）
    if (editor_edit_spawn_marker(doc, pointer.x, pointer.y)) {
        result.drag.kind = EDITOR_DRAG_SPAWN;
        result.drag.active = true;
        result.selection = EditorSelection{ EDITOR_SEL_SPAWN, 0 };
        result.selection_changed = true;
        return result;
    }

    if (!editor_sel_is_entity(selection->kind)) {
        return result;
    }

    u32 entity_kind = editor_sel_entity_kind(selection->kind);
    u32 index = selection->index;

    // 2) 附属抓手（往返端点 / 复位点 / 边角手柄）
    u32 edge_mask = 0;
    u32 grip = editor_edit_hit_grip(doc, selection, pointer, &edge_mask);
    if (grip == EDITOR_DRAG_RANGE_A || grip == EDITOR_DRAG_RANGE_B) {
        const LevelMoverAsset *mover =
            (const LevelMoverAsset *)level_asset_entity_at(&doc->asset, entity_kind, index);
        if (mover) {
            result.drag.kind = grip;
            result.drag.start_point_a = mover->point_a;
            result.drag.start_point_b = mover->point_b;
            result.drag.active = true;
        }
        return result;
    }
    if (grip == EDITOR_DRAG_MONSTER_RESPAWN) {
        result.drag.kind = EDITOR_DRAG_MONSTER_RESPAWN;
        result.drag.active = true;
        return result;
    }
    if (grip == EDITOR_DRAG_WAYPOINT) {
        result.drag.kind = EDITOR_DRAG_WAYPOINT;
        result.drag.active = true;
        return result;
    }
    if (grip == EDITOR_DRAG_RESIZE) {
        Rect2D rect = {};
        if (level_asset_entity_rect_now(&doc->asset, entity_kind, index, &rect)) {
            result.drag.kind = EDITOR_DRAG_RESIZE;
            result.drag.edge_mask = edge_mask;
            result.drag.start_rect = rect;
            result.drag.active = true;
        }
        return result;
    }

    // 3) 矩形内部 → 整体平移（附属位置点一起走）
    Rect2D rect = {};
    if (level_asset_entity_rect_now(&doc->asset, entity_kind, index, &rect) &&
        editor_edit_point_in_rect(&rect, pointer.x, pointer.y)) {
        result.drag.kind = EDITOR_DRAG_MOVE;
        result.drag.start_rect = rect;
        if (entity_kind == LEVEL_ASSET_ENTITY_MONSTER) {
            const LevelMonsterAsset *monster =
                (const LevelMonsterAsset *)level_asset_entity_at(&doc->asset, entity_kind, index);
            if (monster) {
                result.drag.start_spawn_x = monster->spawn_x;
                result.drag.start_spawn_y = monster->spawn_y;
            }
        } else if (entity_kind == LEVEL_ASSET_ENTITY_MOVER) {
            const LevelMoverAsset *mover =
                (const LevelMoverAsset *)level_asset_entity_at(&doc->asset, entity_kind, index);
            if (mover) {
                result.drag.start_point_a = mover->point_a;
                result.drag.start_point_b = mover->point_b;
            }
        }
        result.drag.active = true;
    }

    return result;
}

bool editor_edit_apply_drag(EditorDoc *doc, const EditorSelection *selection, const EditorDragState *drag, f32 world_x,
                            f32 world_y, f32 snap_step)
{
    if (!drag->active) {
        return false;
    }

    u32 entity_kind = editor_sel_is_entity(selection->kind) ? editor_sel_entity_kind(selection->kind) : 0;
    u32 index = selection->index;
    bool changed = false;

    switch (drag->kind) {
    case EDITOR_DRAG_SPAWN: {
        int col = -1;
        int row = -1;
        editor_edit_world_to_cell(&doc->asset, world_x, world_y, &col, &row);
        if (col >= 0) {
            editor_doc_set_spawn(doc, (u32)col, (u32)row); // 内部已置脏
            changed = true;
        }
        break;
    }
    case EDITOR_DRAG_MOVE: {
        // 移动组件的矩形是派生的：平移它 = 平移两个端点（t0 不变，所以本体还是贴在轴上）。
        // 两个端点同时加同一个偏移，轴的方向不会变。
        if (entity_kind == LEVEL_ASSET_ENTITY_MOVER) {
            LevelMoverAsset *mover = editor_doc_mover(doc, index);
            if (!mover) {
                break;
            }
            f32 dx = editor_edit_snap(drag->start_point_a.x + (world_x - drag->grab_x), snap_step) -
                     drag->start_point_a.x;
            f32 dy = editor_edit_snap(drag->start_point_a.y + (world_y - drag->grab_y), snap_step) -
                     drag->start_point_a.y;
            mover->point_a = drag->start_point_a + v2{ dx, dy };
            mover->point_b = drag->start_point_b + v2{ dx, dy };
            changed = true;
            break;
        }
        Rect2D *rect = editor_doc_entity_rect_mut(doc, entity_kind, index);
        if (!rect) {
            break;
        }
        f32 new_center_x = editor_edit_snap(drag->start_rect.center_x + (world_x - drag->grab_x), snap_step);
        f32 new_center_y = editor_edit_snap(drag->start_rect.center_y + (world_y - drag->grab_y), snap_step);
        f32 dx = new_center_x - drag->start_rect.center_x;
        f32 dy = new_center_y - drag->start_rect.center_y;

        rect->center_x = new_center_x;
        rect->center_y = new_center_y;
        rect->half_w = drag->start_rect.half_w;
        rect->half_h = drag->start_rect.half_h;

        // 附属位置点跟着走：否则「把怪物挪个位置」会让它一出生就飞回去
        if (entity_kind == LEVEL_ASSET_ENTITY_MONSTER) {
            LevelMonsterAsset *monster = editor_doc_monster(doc, index);
            if (monster) {
                monster->spawn_x = drag->start_spawn_x + dx;
                monster->spawn_y = drag->start_spawn_y + dy;
            }
        }
        changed = true;
        break;
    }
    case EDITOR_DRAG_RESIZE: {
        // 移动组件：本体中心由 t0 决定（不能跟着边跑），所以拖边只改尺寸 ——
        // 从拖到的那条边算半宽半高，中心不动。圆形两个半轴一体（改一条边 = 改半径）。
        if (entity_kind == LEVEL_ASSET_ENTITY_MOVER) {
            LevelMoverAsset *mover = editor_doc_mover(doc, index);
            if (!mover) {
                break;
            }
            Rect2D now = level_asset_mover_rect(mover);
            f32 x = editor_edit_snap(world_x, snap_step);
            f32 y = editor_edit_snap(world_y, snap_step);
            f32 half_w = mover->half_w;
            f32 half_h = mover->half_h;
            if (drag->edge_mask & EDITOR_EDGE_LEFT) {
                half_w = now.center_x - x;
            }
            if (drag->edge_mask & EDITOR_EDGE_RIGHT) {
                half_w = x - now.center_x;
            }
            if (drag->edge_mask & EDITOR_EDGE_BOTTOM) {
                half_h = now.center_y - y;
            }
            if (drag->edge_mask & EDITOR_EDGE_TOP) {
                half_h = y - now.center_y;
            }
            // 别拖成零尺寸：那是校验里的 ERROR，会直接卡住保存
            constexpr f32 MIN_MOVER_HALF = 1.0f;
            mover->half_w = (half_w < MIN_MOVER_HALF) ? MIN_MOVER_HALF : half_w;
            if (mover->shape == LEVEL_MOVER_CIRCLE) {
                mover->half_h = mover->half_w;
            } else {
                mover->half_h = (half_h < MIN_MOVER_HALF) ? MIN_MOVER_HALF : half_h;
            }
            changed = true;
            break;
        }
        Rect2D *rect = editor_doc_entity_rect_mut(doc, entity_kind, index);
        if (!rect) {
            break;
        }
        f32 left = drag->start_rect.center_x - drag->start_rect.half_w;
        f32 right = drag->start_rect.center_x + drag->start_rect.half_w;
        f32 bottom = drag->start_rect.center_y - drag->start_rect.half_h;
        f32 top = drag->start_rect.center_y + drag->start_rect.half_h;

        f32 x = editor_edit_snap(world_x, snap_step);
        f32 y = editor_edit_snap(world_y, snap_step);
        if (drag->edge_mask & EDITOR_EDGE_LEFT) {
            left = x;
        }
        if (drag->edge_mask & EDITOR_EDGE_RIGHT) {
            right = x;
        }
        if (drag->edge_mask & EDITOR_EDGE_BOTTOM) {
            bottom = y;
        }
        if (drag->edge_mask & EDITOR_EDGE_TOP) {
            top = y;
        }

        // 别拖成零尺寸：那是校验里的 ERROR，会直接卡住保存
        f32 min_size = 2.0f;
        if (right - left < min_size) {
            if (drag->edge_mask & EDITOR_EDGE_LEFT) {
                left = right - min_size;
            } else {
                right = left + min_size;
            }
        }
        if (top - bottom < min_size) {
            if (drag->edge_mask & EDITOR_EDGE_BOTTOM) {
                bottom = top - min_size;
            } else {
                top = bottom + min_size;
            }
        }

        rect->center_x = (left + right) * 0.5f;
        rect->half_w = (right - left) * 0.5f;
        rect->center_y = (top + bottom) * 0.5f;
        rect->half_h = (top - bottom) * 0.5f;
        changed = true;
        break;
    }
    case EDITOR_DRAG_MONSTER_RESPAWN: {
        LevelMonsterAsset *monster = editor_doc_monster(doc, index);
        if (monster) {
            monster->spawn_x = editor_edit_snap(world_x, snap_step);
            monster->spawn_y = editor_edit_snap(world_y, snap_step);
            changed = true;
        }
        break;
    }
    case EDITOR_DRAG_WAYPOINT: {
        LevelWaypointAsset *waypoint = editor_doc_waypoint(doc, index);
        if (waypoint) {
            waypoint->x = editor_edit_snap(world_x, snap_step);
            waypoint->y = editor_edit_snap(world_y, snap_step);
            changed = true;
        }
        break;
    }
    case EDITOR_DRAG_RANGE_A: {
        LevelMoverAsset *mover = editor_doc_mover(doc, index);
        if (mover) {
            v2 point = v2{ editor_edit_snap(world_x, snap_step), editor_edit_snap(world_y, snap_step) };
            // 只要不跟另一端重合就行（重合了就没有可走的轴，校验会报 ERROR）。
            // t0 不动：轴线转开时本体按同一个参数跟过去，所以它永远贴在轴上。
            bool same = (point.x == mover->point_b.x && point.y == mover->point_b.y);
            mover->point_a = same ? mover->point_a : point;
            changed = true;
        }
        break;
    }
    case EDITOR_DRAG_RANGE_B: {
        LevelMoverAsset *mover = editor_doc_mover(doc, index);
        if (mover) {
            v2 point = v2{ editor_edit_snap(world_x, snap_step), editor_edit_snap(world_y, snap_step) };
            bool same = (point.x == mover->point_a.x && point.y == mover->point_a.y);
            mover->point_b = same ? mover->point_b : point;
            changed = true;
        }
        break;
    }
    default:
        break;
    }

    if (changed) {
        editor_doc_mark_changed(doc);
    }
    return changed;
}

// ============================================================================
// 自证（--selftest）
//
// 用例全是纯数值断言，不依赖关卡资产、不依赖窗口 —— 这是把这一层拆出来的**理由**，
// 也是它唯一能自证的方式（界面只能靠人点）。数字来自实际手测过的那几次拖动。
// ============================================================================

// 自检的报告（计数与日志格式）在 editor_doc.cc，与文档层的用例集共用一份：
// 依赖方向是 editor_edit → editor_doc，反过来不行，所以工具放在那一边。

internal EditorPointer selftest_pointer(f32 x, f32 y)
{
    // 手柄 6px / 近邻 8px，按 zoom = 1 传成世界半径（用例都在世界单位上写数字）
    EditorPointer pointer = {};
    pointer.x = x;
    pointer.y = y;
    pointer.handle_radius = 6.0f;
    pointer.near_radius = 8.0f;
    return pointer;
}

internal EditorSelection selftest_select(u32 entity_kind, u32 index)
{
    return EditorSelection{ editor_sel_of_entity(entity_kind), index };
}

internal bool selftest_near(f32 a, f32 b)
{
    return fabsf(a - b) <= 0.001f;
}

bool editor_edit_run_selftest()
{
    editor_selftest_begin();

    EditorDoc doc = {};
    editor_doc_new(&doc, 64, 24, 64.0f);
    if (!doc.asset.tiles) {
        LOG_ERROR("[selftest] cannot build a document in memory");
        return false;
    }

    const LevelAsset *asset = &doc.asset;
    const f32 tile = asset->tile_size;

    // 01 世界坐标 → 格号（含边界外）
    {
        int col = 0;
        int row = 0;
        editor_edit_world_to_cell(asset, 100.0f, -100.0f, &col, &row);
        int out_col = 0;
        int out_row = 0;
        editor_edit_world_to_cell(asset, -1.0f, 5.0f, &out_col, &out_row);
        editor_selftest_check(col == 1 && row == 1 && out_col == -1 && out_row == -1,
                       "world_to_cell: (100,-100)=(%d,%d) out-of-grid=(-1,-1)", col, row);
    }

    // 02 吸附：作用在结果上，step = 0 表示自由像素
    {
        f32 snapped = editor_edit_snap(2085.0f, 64.0f);
        f32 free_value = editor_edit_snap(2085.0f, 0.0f);
        editor_selftest_check(selftest_near(snapped, 2112.0f) && selftest_near(free_value, 2085.0f),
                       "snap: 2085/64 -> %.1f, step 0 -> %.1f", snapped, free_value);
    }

    // ---- 造一个移动组件 + 一个怪物（下面几组用例都用它们）----
    // 本体位置是**轴上的参数 t0**：a / b 各离轴中心 320、t0 = 0.5 → 本体落在轴正中间 (2048, -768)
    u32 mover_index = editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_MOVER);
    LevelMoverAsset *mover = editor_doc_mover(&doc, mover_index);
    mover->point_a = v2{ 1728.0f, -768.0f }; // 端点离本体要够远，否则抓中心会先命中往返端点
    mover->point_b = v2{ 2368.0f, -768.0f };
    mover->t0 = 0.5f;
    mover->half_w = 80.0f;
    mover->half_h = 24.0f;
    mover->speed = 64.0f;
    mover->shape = LEVEL_MOVER_SQUARE;
    mover->damaging = 0;

    // 03 整体平移：两端同时加同一个偏移（t0 不变 → 本体还是贴在轴上）
    //    这组数字是 2026-09-22 手工在界面上拖出来并读 .bin 核对过的
    //    （两个分量都是 64 的整数倍 → 吸附对；半宽半高不变 → 是平移不是缩放）
    {
        EditorSelection sel = selftest_select(LEVEL_ASSET_ENTITY_MOVER, mover_index);
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(2048.0f, -768.0f));
        bool moved = editor_edit_apply_drag(&doc, &sel, &begin.drag, 2368.0f, -960.0f, tile);
        Rect2D now = level_asset_mover_rect(mover);
        editor_selftest_check(begin.drag.kind == EDITOR_DRAG_MOVE && begin.drag.active && moved &&
                           selftest_near(now.center_x, 2368.0f) && selftest_near(now.center_y, -960.0f) &&
                           selftest_near(now.half_w, 80.0f) && selftest_near(now.half_h, 24.0f),
                       "move: kind=%u center=(%.1f,%.1f) half=(%.1f,%.1f)", begin.drag.kind, now.center_x,
                       now.center_y, now.half_w, now.half_h);
    }

    // 04 拖左边的手柄 → 缩放。移动组件的中心由 t0 决定，所以**中心不动**，只有半宽跟着变
    {
        // 端点要摆得离本体边缘够远：抓手的判定顺序是「端点优先于边角手柄」，
        // 端点与左边界重合时抓到的是端点（那一下是转轴，不是缩放）
        mover->point_a = v2{ 256.0f, -768.0f };
        mover->point_b = v2{ 3840.0f, -768.0f };
        mover->t0 = 0.5f; // 中心回到 (2048, -768)
        mover->half_w = 320.0f; // left = 1728, right = 2368
        mover->half_h = 24.0f;

        EditorSelection sel = selftest_select(LEVEL_ASSET_ENTITY_MOVER, mover_index);
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(1731.0f, -768.0f));
        bool resized = editor_edit_apply_drag(&doc, &sel, &begin.drag, 1856.0f, -768.0f, tile);
        // 左边 1728 -> 1856（29 格）：半宽 = 中心 2048 - 1856 = 192
        Rect2D now = level_asset_mover_rect(mover);
        editor_selftest_check(begin.drag.kind == EDITOR_DRAG_RESIZE && begin.drag.edge_mask == EDITOR_EDGE_LEFT &&
                           resized && selftest_near(now.center_x, 2048.0f) && selftest_near(now.half_w, 192.0f),
                       "resize left: mask=%u center=%.1f half_w=%.1f", begin.drag.edge_mask, now.center_x,
                       now.half_w);
    }

    // 05 缩放到最小尺寸：夹到 2 像素宽（零尺寸是校验里的 ERROR）
    {
        EditorSelection sel = selftest_select(LEVEL_ASSET_ENTITY_MOVER, mover_index);
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(1859.0f, -768.0f));
        editor_edit_apply_drag(&doc, &sel, &begin.drag, 4000.0f, -768.0f, tile);
        f32 width = mover->half_w * 2.0f;
        editor_selftest_check(selftest_near(width, 2.0f), "resize clamp: width=%.1f (expected 2.0)", width);
    }

    // ---- 再加一个怪物，用来测命中优先级与附属点跟随 ----
    u32 monster_index = editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_MONSTER);
    LevelMonsterAsset *monster = editor_doc_monster(&doc, monster_index);
    monster->rect.center_x = 2048.0f;
    monster->rect.center_y = -768.0f; // 与平台重叠，用来验证倒序优先级
    monster->rect.half_w = 32.0f;
    monster->rect.half_h = 32.0f;
    monster->spawn_x = 1900.0f;
    monster->spawn_y = -700.0f;

    // 06 命中优先级：小的（怪物）盖在大的（地刺）上面，先被点到
    {
        EditorSelection hit = editor_edit_hit_body(&doc, 2048.0f, -768.0f);
        EditorSelection outside = editor_edit_hit_body(&doc, 0.0f, 0.0f);
        editor_selftest_check(editor_edit_selection_is(&hit, LEVEL_ASSET_ENTITY_MONSTER, monster_index) &&
                           outside.kind == EDITOR_SEL_NONE,
                       "hit_body priority: kind=%u index=%u, outside=%u", hit.kind, hit.index, outside.kind);
    }

    // 07 出生点标记：命中它要返回出生点选择（它是关卡单例属性）
    {
        u32 spawn_col = 0;
        u32 spawn_row = 0;
        bool found = editor_doc_find_spawn(&doc, &spawn_col, &spawn_row);
        f32 marker_x = level_cell_center_x((u32)spawn_col, tile);
        f32 marker_y = level_cell_bottom_y((u32)spawn_row, tile) + 1.0f; // 底边往上 1 像素，仍在标记框内
        EditorSelection hit = editor_edit_hit_body(&doc, marker_x, marker_y);
        bool marker_found = found && hit.kind == EDITOR_SEL_SPAWN;
        editor_selftest_check(marker_found, "hit_body spawn: found=%d kind=%u", found ? 1 : 0, hit.kind);

        if (found) {
            EditorDragBegin begin = editor_edit_begin_drag(&doc, &hit, selftest_pointer(marker_x, marker_y));
            editor_selftest_check(begin.drag.kind == EDITOR_DRAG_SPAWN && begin.selection_changed,
                           "drag spawn: kind=%u selection_changed=%d", begin.drag.kind,
                           begin.selection_changed ? 1 : 0);
        }
    }

    // 08 拖动怪物：整体平移时复位点必须跟着走（否则一启动就飞回旧位置）
    {
        EditorSelection sel = selftest_select(LEVEL_ASSET_ENTITY_MONSTER, monster_index);
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(2048.0f, -768.0f));
        editor_edit_apply_drag(&doc, &sel, &begin.drag, 2112.0f, -704.0f, tile);
        bool follow = selftest_near(monster->rect.center_x, 2112.0f) && selftest_near(monster->rect.center_y, -704.0f) &&
                      selftest_near(monster->spawn_x, 1964.0f) && selftest_near(monster->spawn_y, -636.0f);
        editor_selftest_check(begin.drag.kind == EDITOR_DRAG_MOVE && follow,
                       "move monster: center=(%.1f,%.1f) spawn=(%.1f,%.1f)", monster->rect.center_x,
                       monster->rect.center_y, monster->spawn_x, monster->spawn_y);
    }

    // 08b 传送点：它没有矩形，所以命中与拖动都是自己的那一条（点即抓手）
    {
        u32 waypoint_index = editor_doc_add_entity(&doc, LEVEL_ASSET_ENTITY_WAYPOINT);
        LevelWaypointAsset *waypoint = editor_doc_waypoint(&doc, waypoint_index);
        waypoint->x = 3000.0f;
        waypoint->y = -256.0f;

        EditorSelection hit = editor_edit_hit_body(&doc, 3000.0f, -256.0f);
        bool hit_ok = editor_edit_selection_is(&hit, LEVEL_ASSET_ENTITY_WAYPOINT, waypoint_index);

        EditorDragBegin begin = editor_edit_begin_drag(&doc, &hit, selftest_pointer(3000.0f, -256.0f));
        editor_edit_apply_drag(&doc, &hit, &begin.drag, 3128.0f, -320.0f, tile);
        bool moved = selftest_near(waypoint->x, 3136.0f) && selftest_near(waypoint->y, -320.0f);
        editor_selftest_check(hit_ok && begin.drag.kind == EDITOR_DRAG_WAYPOINT && moved,
                       "waypoint: hit=%d kind=%u pos=(%.1f,%.1f)", hit_ok ? 1 : 0, begin.drag.kind, waypoint->x,
                       waypoint->y);
    }

    // 09 怪物复位点抓手：命中它时是 RESPAWN 而不是 MOVE
    {
        EditorSelection sel = selftest_select(LEVEL_ASSET_ENTITY_MONSTER, monster_index);
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(monster->spawn_x + 2.0f, monster->spawn_y));
        editor_selftest_check(begin.drag.kind == EDITOR_DRAG_MONSTER_RESPAWN,
                       "respawn grip: kind=%u (expected %u)", begin.drag.kind, (u32)EDITOR_DRAG_MONSTER_RESPAWN);
    }

    // 10 移动组件往返端点：抓手优先于矩形内部；**二维** —— 把 B 拖到斜向的另一角，
    //    两个分量都要按吸附落到 64 的整数倍，而且**本体一直贴在轴上**（t0 不变 → 按同一个参数跟过去）
    {
        mover->point_a = v2{ 800.0f, -500.0f };
        mover->point_b = v2{ 1400.0f, -500.0f };
        mover->t0 = 0.5f;
        mover->half_w = 128.0f;
        mover->half_h = 32.0f;

        EditorSelection sel = selftest_select(LEVEL_ASSET_ENTITY_MOVER, mover_index);
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(1400.0f, -500.0f));
        editor_edit_apply_drag(&doc, &sel, &begin.drag, 1370.0f, -760.0f, tile); // 斜向拖
        bool moved = selftest_near(mover->point_b.x, 1344.0f) && selftest_near(mover->point_b.y, -768.0f);
        bool a_kept = selftest_near(mover->point_a.x, 800.0f) && selftest_near(mover->point_a.y, -500.0f);
        // 轴变成 (800,-500)→(1344,-768)，t0 = 0.5 → 本体中心应该正好在两点的中点上
        Rect2D now = level_asset_mover_rect(mover);
        bool on_axis = selftest_near(now.center_x, 1072.0f) && selftest_near(now.center_y, -634.0f);
        editor_selftest_check(begin.drag.kind == EDITOR_DRAG_RANGE_B && moved && a_kept && on_axis,
                       "range grip: kind=%u b=(%.1f,%.1f) (expected 1344,-768) a_kept=%d on_axis=%d",
                       begin.drag.kind, mover->point_b.x, mover->point_b.y, a_kept ? 1 : 0, on_axis ? 1 : 0);

        // B 拖到 A 上：不能重合（重合了就没有可走的轴）。A 取吸附网格上的点，
        // 这样「拖到 A 上」在开启吸附时也确实能落到同一个坐标
        mover->point_a = v2{ 832.0f, -512.0f };
        EditorDragBegin again = editor_edit_begin_drag(&doc, &sel, selftest_pointer(mover->point_b.x, mover->point_b.y));
        editor_edit_apply_drag(&doc, &sel, &again.drag, 800.0f, -500.0f, tile);
        bool kept = !selftest_near(mover->point_b.x, mover->point_a.x) ||
                    !selftest_near(mover->point_b.y, mover->point_a.y);
        editor_selftest_check(kept, "range grip coincide: b=(%.1f,%.1f) a=(%.1f,%.1f) (expected not equal)",
                       mover->point_b.x, mover->point_b.y, mover->point_a.x, mover->point_a.y);
    }

    // 11 拖空处 = 不拖（这一下留给涂格 / Ctrl 选中）
    {
        EditorSelection sel = {};
        EditorDragBegin begin = editor_edit_begin_drag(&doc, &sel, selftest_pointer(0.0f, -2000.0f));
        editor_selftest_check(begin.drag.kind == EDITOR_DRAG_NONE && !begin.drag.active, "drag empty: kind=%u active=%d",
                              begin.drag.kind, begin.drag.active ? 1 : 0);
    }

    if (editor_selftest_failed() == 0) {
        LOG_INFO("[selftest] edit cases passed");
        return true;
    }
    LOG_ERROR("[selftest] %u edit case(s) failed", editor_selftest_failed());
    return false;
}
