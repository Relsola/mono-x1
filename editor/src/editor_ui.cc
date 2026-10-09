#include "editor_ui.h"
#include "shared/file.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

#include <stdio.h> // snprintf（属性面板 / 路径框的文本）

#include "imgui.h"

// ============================================================================
// 界面层
//
// 一条硬约束：界面只通过 editor_doc_* 改文档（唯一的改动入口）。
// 唯一例外是属性面板里的输入控件 —— 它们拿的是结构体指针，改完必须调
// editor_doc_mark_changed() 置脏，三个 helper 已经把这个动作包在里面了。
//
// 「怎么判定、怎么解算」不在这里：命中测试 / 拖动 / 吸附都在 editor_edit.cc（纯函数，无窗口可跑）。
// 本层只负责屏幕坐标 → 世界坐标的换算与绘制。
// ============================================================================

// 配色（IM_COL32 是常量表达式，所以可以放 constexpr）
inline constexpr ImU32 EDITOR_COLOR_OUTSIDE = IM_COL32(14, 14, 17, 255);
inline constexpr ImU32 EDITOR_COLOR_BACKDROP = IM_COL32(26, 26, 31, 255);
inline constexpr ImU32 EDITOR_COLOR_GRID = IM_COL32(56, 56, 66, 255);
inline constexpr ImU32 EDITOR_COLOR_SOLID = IM_COL32(104, 116, 138, 255);
inline constexpr ImU32 EDITOR_COLOR_ONE_WAY = IM_COL32(86, 152, 106, 255);
inline constexpr ImU32 EDITOR_COLOR_SPAWN = IM_COL32(226, 168, 62, 255);
inline constexpr ImU32 EDITOR_COLOR_SPIKE = IM_COL32(214, 76, 76, 150);
inline constexpr ImU32 EDITOR_COLOR_SPIKE_EDGE = IM_COL32(214, 96, 96, 255);
inline constexpr ImU32 EDITOR_COLOR_VANISH = IM_COL32(118, 198, 224, 140);
inline constexpr ImU32 EDITOR_COLOR_VANISH_EDGE = IM_COL32(150, 220, 240, 255);
inline constexpr ImU32 EDITOR_COLOR_PLATFORM = IM_COL32(92, 132, 214, 150);
inline constexpr ImU32 EDITOR_COLOR_PLATFORM_EDGE = IM_COL32(120, 160, 240, 255);
inline constexpr ImU32 EDITOR_COLOR_MONSTER = IM_COL32(226, 132, 62, 170);
inline constexpr ImU32 EDITOR_COLOR_MONSTER_EDGE = IM_COL32(250, 160, 90, 255);
inline constexpr ImU32 EDITOR_COLOR_DOOR = IM_COL32(180, 96, 214, 190);
inline constexpr ImU32 EDITOR_COLOR_WAYPOINT = IM_COL32(226, 226, 120, 255);
inline constexpr ImU32 EDITOR_COLOR_SELECT = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 EDITOR_COLOR_HOVER = IM_COL32(255, 255, 255, 120);
inline constexpr ImU32 EDITOR_COLOR_CURSOR = IM_COL32(255, 255, 255, 200);
inline constexpr ImU32 EDITOR_COLOR_DIAGNOSTIC = IM_COL32(90, 226, 226, 255);
inline constexpr ImU32 EDITOR_COLOR_COLLIDER = IM_COL32(90, 226, 226, 90);

inline constexpr f32 EDITOR_ZOOM_MIN = 0.02f;
inline constexpr f32 EDITOR_ZOOM_MAX = 4.0f;

// ============================================================================
// 画布坐标换算
// ============================================================================

struct EditorCanvas
{
    ImVec2 origin; // 画布左上角的屏幕坐标
    ImVec2 size;
    f32 zoom;
    f32 scroll_x; // 画布左上角对应的世界坐标
    f32 scroll_y;
};

internal ImVec2 editor_world_to_screen(const EditorCanvas *canvas, f32 world_x, f32 world_y)
{
    // 世界 y 向上为正、屏幕 y 向下为正，所以 y 是减
    return ImVec2(canvas->origin.x + (world_x - canvas->scroll_x) * canvas->zoom,
                  canvas->origin.y + (canvas->scroll_y - world_y) * canvas->zoom);
}

internal void editor_screen_to_world(const EditorCanvas *canvas, ImVec2 screen, f32 *world_x, f32 *world_y)
{
    *world_x = canvas->scroll_x + (screen.x - canvas->origin.x) / canvas->zoom;
    *world_y = canvas->scroll_y - (screen.y - canvas->origin.y) / canvas->zoom;
}

// ============================================================================
// y 轴的显示方式
//
// 关卡数据与世界坐标永远是「顶部为 0、向下为负」（和游戏、和 docs 里的所有数字一致），
// 画布内部的一切（几何、命中测试、拖动、存盘）也一律是世界 y。
// 这里只换**给人看的数字**：默认「底部为 0、向上为正」——
// 人在编辑器里看坐标时想的是「这块平台在地面上方 300 像素」，而不是「y = -1xxx」。
// 两者只差一个平移：display = world + 关卡高度（平移量，没有缩放）。
//
// 保留「原始世界 y」这个开关的理由：所有**机器可读的产物**都是世界 y ——
// `--trace` 的 CSV、`test/*.txt` 里的 assert_pos、游戏日志、docs 里的常数（如「地面顶面 -1408」）。
// 要拿编辑器里的数字去对它们，就要切回去（视图 → Y 轴）。
// ============================================================================

internal f32 editor_world_y_to_display(const EditorUiState *ui, f32 world_y)
{
    if (!ui->view.y_from_bottom) {
        return world_y;
    }
    return world_y + (f32)ui->doc.asset.tile_rows * ui->doc.asset.tile_size;
}

internal f32 editor_display_y_to_world(const EditorUiState *ui, f32 display_y)
{
    if (!ui->view.y_from_bottom) {
        return display_y;
    }
    return display_y - (f32)ui->doc.asset.tile_rows * ui->doc.asset.tile_size;
}

// 世界坐标 → 格号的换算在 editor_edit.cc（editor_edit_world_to_cell）

internal void editor_canvas_fit(EditorView *view, const EditorCanvas *canvas, const LevelAsset *asset)
{
    f32 world_w = (f32)asset->tile_columns * asset->tile_size;
    f32 world_h = (f32)asset->tile_rows * asset->tile_size;
    if (world_w <= 0.0f || world_h <= 0.0f) {
        return;
    }

    f32 zoom = MIN(canvas->size.x / world_w, canvas->size.y / world_h) * 0.95f;
    view->zoom = clamp(zoom, EDITOR_ZOOM_MIN, EDITOR_ZOOM_MAX);

    f32 visible_w = canvas->size.x / view->zoom;
    f32 visible_h = canvas->size.y / view->zoom;
    view->scroll_x = (world_w - visible_w) * 0.5f;
    view->scroll_y = (visible_h - world_h) * 0.5f;
}

// 把某一格摆到画布正中（点问题列表跳过去时用）
internal void editor_canvas_center_on(EditorUiState *ui, const EditorCanvas *canvas, int col, int row)
{
    const LevelAsset *asset = &ui->doc.asset;
    f32 world_x = level_cell_center_x(col, asset->tile_size);
    f32 world_y = level_cell_center_y(row, asset->tile_size);
    ui->view.scroll_x = world_x - canvas->size.x / (2.0f * ui->view.zoom);
    ui->view.scroll_y = world_y + canvas->size.y / (2.0f * ui->view.zoom);
}

// ============================================================================
// 属性面板的三种控件形态
//
// 手写而不是做「字段反射」：现有字段（矩形 / 标量 / 枚举）都能落在这三个上，
// 加一个属性 = 加一行。等实体种类多到出现重复描述时再谈抽象。
// ============================================================================

// 连续控件（DragFloat）的一条撤销规则：按住期间算一次手势，松手时才落一格快照。
// 用 IsItemDeactivated（而不是 IsItemDeactivatedAfterEdit）：没改动也要收尾，否则手势关不上。
internal void editor_field_gesture(EditorDoc *doc)
{
    if (ImGui::IsItemActivated()) {
        editor_doc_action_begin(doc);
    }
    if (ImGui::IsItemDeactivated()) {
        editor_doc_action_end(doc);
    }
}

internal bool editor_field_f32(EditorDoc *doc, const char *label, f32 *value, f32 step, const char *hint)
{
    ImGui::SetNextItemWidth(140.0f);
    bool changed = ImGui::DragFloat(label, value, step, 0.0f, 0.0f, "%.1f");
    editor_field_gesture(doc);
    if (hint) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint);
    }
    if (changed) {
        editor_doc_mark_changed(doc);
    }
    return changed;
}

// y 坐标专用的输入框：显示的、填的、拖的都是「给人看的 y」，写回数据前换算成世界 y。
// 存进去的永远是世界 y —— 换显示方式不会改动关卡数据的任何一个字节。
internal bool editor_field_coord_y(EditorUiState *ui, EditorDoc *doc, const char *label, f32 *value, f32 step,
                                   const char *hint)
{
    f32 display = editor_world_y_to_display(ui, *value);
    ImGui::SetNextItemWidth(140.0f);
    bool changed = ImGui::DragFloat(label, &display, step, 0.0f, 0.0f, "%.1f");
    editor_field_gesture(doc);
    if (hint) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint);
    }
    if (changed) {
        *value = editor_display_y_to_world(ui, display);
        editor_doc_mark_changed(doc);
    }
    return changed;
}

internal bool editor_field_rect(EditorUiState *ui, EditorDoc *doc, const char *label, Rect2D *rect)
{
    ImGui::Text("%s", label);

    bool changed = false;
    ImGui::SetNextItemWidth(140.0f);
    changed |= ImGui::DragFloat("中心 x", &rect->center_x, 1.0f, 0.0f, 0.0f, "%.1f");
    editor_field_gesture(doc);
    ImGui::SameLine();
    // 中心 y 走显示坐标（下面那个 helper）；半宽 / 半高是**尺寸**，不换算
    changed |= editor_field_coord_y(ui, doc, "中心 y", &rect->center_y, 1.0f, nullptr);

    // 下限 1：鼠标拖动做不出零尺寸的矩形（零尺寸是校验里的 ERROR）
    ImGui::SetNextItemWidth(140.0f);
    changed |= ImGui::DragFloat("半宽", &rect->half_w, 1.0f, 1.0f, 0.0f, "%.1f");
    editor_field_gesture(doc);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    changed |= ImGui::DragFloat("半高", &rect->half_h, 1.0f, 1.0f, 0.0f, "%.1f");
    editor_field_gesture(doc);

    if (changed) {
        editor_doc_mark_changed(doc);
    }
    return changed;
}

internal bool editor_field_enum(EditorDoc *doc, const char *label, u32 *value, const char *const *names, u32 count)
{
    int current = (int)*value;
    ImGui::SetNextItemWidth(140.0f);
    bool changed = false;
    if (ImGui::BeginCombo(label, (current >= 0 && (u32)current < count) ? names[current] : "?")) {
        for (u32 i = 0; i < count; ++i) {
            bool selected = (current == (int)i);
            if (ImGui::Selectable(names[i], selected)) {
                *value = i;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (changed) {
        editor_doc_mark_changed(doc);
    }
    return changed;
}

// ============================================================================
// 命中测试与拖动
//
// 拖动交互的规则（唯一的规则，避免歧义）：
//   · 左键按在**已选中**实体的手柄 / 内部 → 搬它（缩放 / 平移）
//   · 左键按在出生点标记上 → 搬出生点（它是关卡单例属性）
//   · 其它情况左键 = 涂格子；Ctrl + 左键 = 选中；右键 = 擦；中键 = 平移
//   所以「选中」是拖动的入口：Ctrl + 左键点一下（或在左栏列表里点），然后才能拖它。
//   这条规则的代价是「选中期间无法在它内部涂格子」，按 Esc 取消选中即可。
// 吸附：默认按格（tile_size），按住 Alt 自由像素。吸附作用在**结果**上而不是增量上。
//
// 规则的**实现**在 editor_edit.cc（纯逻辑那一层：没有 ImGui、没有窗口，所以
// `build\editor.exe --selftest` 能逐条断言它）；这里只留翻译：屏幕↔世界、像素半径→世界半径。
// ============================================================================

// 屏幕像素半径是界面的手感策略（缩放后手感一致），纯层只认世界单位 ——
// 这整除个 zoom 就是两个世界的全部关系
inline constexpr f32 EDITOR_HANDLE_RADIUS_PX = 6.0f;
inline constexpr f32 EDITOR_NEAR_RADIUS_PX = 8.0f;

internal EditorPointer editor_ui_pointer(const EditorCanvas *canvas, f32 world_x, f32 world_y)
{
    EditorPointer pointer = {};
    pointer.x = world_x;
    pointer.y = world_y;
    pointer.handle_radius = EDITOR_HANDLE_RADIUS_PX / canvas->zoom;
    pointer.near_radius = EDITOR_NEAR_RADIUS_PX / canvas->zoom;
    return pointer;
}

// 命中测试、手柄判定、位置换算都在 editor_edit.cc；界面只负责把屏幕鼠标换成世界坐标。

// 光标形状：让「这里能拖」看得见。判据与按下时用**同一份**（editor_edit_hit_grip）——
// 原实现把范围端点 / 手柄各算了一遍，两处会各自漂移
internal void editor_drag_cursor(const EditorUiState *ui, const EditorCanvas *canvas, bool hovered)
{
    if (ui->drag.active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return;
    }
    if (!hovered) {
        return;
    }

    f32 world_x = 0.0f;
    f32 world_y = 0.0f;
    editor_screen_to_world(canvas, ImGui::GetIO().MousePos, &world_x, &world_y);

    u32 edge_mask = 0;
    u32 grip = editor_edit_hit_grip(&ui->doc, &ui->selection, editor_ui_pointer(canvas, world_x, world_y), &edge_mask);

    if (grip == EDITOR_DRAG_RANGE_A || grip == EDITOR_DRAG_RANGE_B) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); // 轴可以任意方向，所以是全向箭头
        return;
    }
    if (grip == EDITOR_DRAG_RESIZE) {
        if (edge_mask == (EDITOR_EDGE_LEFT | EDITOR_EDGE_RIGHT) || edge_mask == EDITOR_EDGE_LEFT ||
            edge_mask == EDITOR_EDGE_RIGHT) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            return;
        }
        if (edge_mask == EDITOR_EDGE_TOP || edge_mask == EDITOR_EDGE_BOTTOM) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            return;
        }
        if (edge_mask == (EDITOR_EDGE_LEFT | EDITOR_EDGE_TOP) || edge_mask == (EDITOR_EDGE_RIGHT | EDITOR_EDGE_BOTTOM)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            return;
        }
        if (edge_mask == (EDITOR_EDGE_LEFT | EDITOR_EDGE_BOTTOM) || edge_mask == (EDITOR_EDGE_RIGHT | EDITOR_EDGE_TOP)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
        }
        return;
    }

    // 选中实体的内部 → 平移光标（与按下时 MOVE 的判据一致）
    if (editor_sel_is_entity(ui->selection.kind)) {
        Rect2D rect = {};
        if (level_asset_entity_rect_now(&ui->doc.asset, editor_sel_entity_kind(ui->selection.kind),
                                        ui->selection.index, &rect) &&
            editor_edit_point_in_rect(&rect, world_x, world_y)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }
}

// ============================================================================
// 左侧：地形笔刷 + 实体列表
// ============================================================================

internal void editor_draw_brush_section(EditorUiState *ui)
{
    ImGui::TextDisabled("值语义：涂在格子里，没有身份和参数");
    ImGui::Separator();

    struct BrushEntry
    {
        LevelTile tile;
        const char *name;
        ImU32 color;
        const char *hint;
    };
    static const BrushEntry entries[] = {
        { LEVEL_TILE_SOLID, "实体墙  '#'", EDITOR_COLOR_SOLID, "上下左右都阻挡" },
        { LEVEL_TILE_ONE_WAY, "单向平台  '='", EDITOR_COLOR_ONE_WAY, "上方可站、可跳穿" },
        { LEVEL_TILE_SPAWN, "出生点  'P'", EDITOR_COLOR_SPAWN, "关卡单例，全图只能有一个" },
        { LEVEL_TILE_SPIKE, "地刺  '^'", EDITOR_COLOR_SPIKE, "只造成伤害、不阻挡；按住拖动可连续涂" },
        { LEVEL_TILE_VANISH, "可消失平台  'V'", EDITOR_COLOR_VANISH,
          "连续踩住约 0.17s 后变暗→消失（不可碰撞），再过约 2s 恢复；触发后不可回退；时长在 src/game.cc" },
        { LEVEL_TILE_EMPTY, "空  '.'", EDITOR_COLOR_BACKDROP, "右键擦除也是它" },
    };

    for (const auto &entry : entries) {
        // ID 用笔画名而不是下标：表重排后控件的 ID 仍然稳定
        ImGui::PushID(entry.name);

        ImVec2 cursor = ImGui::GetCursorScreenPos();
        f32 size = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(cursor, ImVec2(cursor.x + size, cursor.y + size), entry.color);
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine();

        bool selected = (ui->brush == entry.tile);
        if (ImGui::Selectable(entry.name, selected)) {
            ui->brush = entry.tile;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", entry.hint);
        ImGui::PopID();
    }
}

internal void editor_draw_entity_section(EditorUiState *ui)
{
    EditorDoc *doc = &ui->doc;

    ImGui::TextDisabled("身份语义：可单独选中、改参数、拖动、删除");
    ImGui::Separator();

    for (u32 kind = 0; kind < LEVEL_ASSET_ENTITY_KIND_COUNT; ++kind) {
        const LevelAssetEntityMeta *meta = &LEVEL_ASSET_ENTITY_TABLE[kind];
        u32 count = editor_doc_entity_count(doc, kind);
        ImGui::PushID((int)kind);

        ImGui::Text("%s (%u)", meta->name, count);
        ImGui::SameLine();
        if (ImGui::SmallButton("+ 新建")) {
            u32 index = editor_doc_add_entity(doc, kind);
            if (index != EDITOR_NO_PLATFORM) {
                ui->selection = EditorSelection{ editor_sel_of_entity(kind), index };
                editor_message(&ui->messages, "新建了 %s #%u（默认放在关卡中央）", meta->name, index);
            }
        }

        // 运行期限一个的那些（表里的 max_count）：多加了游戏会直接 abort，先在这里说清楚
        if (count > meta->max_count) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  运行时只支持 %u 个，保存会被拒绝",
                               meta->max_count);
        }

        for (u32 i = 0; i < count; ++i) {
            ImGui::PushID((int)i);
            char label[64] = {};
            snprintf(label, sizeof(label), "%s #%u##item", meta->name, i);

            bool selected = editor_edit_selection_is(&ui->selection, kind, i);
            // 这一行是整行宽的可选中项，X 按钮叠在它上面 —— 不声明「允许后面的控件叠上来」，
            // Selectable 会把点击全吃掉，X 就永远点不到（实测就是这么坏的）
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Selectable(label, selected)) {
                ui->selection = EditorSelection{ editor_sel_of_entity(kind), i };
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
            if (ImGui::SmallButton("x##del")) {
                if (editor_doc_remove_entity(doc, kind, i)) {
                    editor_message(&ui->messages, "删除了 %s #%u", meta->name, i);
                    // 下标会整体前移，选中的是同一个下标的话就当没选中
                    ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
                }
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::PopID();
    }
}

// ============================================================================
// 中间：网格画布
// ============================================================================

internal void editor_draw_canvas_window(EditorUiState *ui, ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("网格（左键涂 / 右键擦 / Ctrl+左键选中 / 中键平移 / 滚轮缩放）", nullptr, flags)) {
        ImGui::End();
        return;
    }

    EditorDoc *doc = &ui->doc;
    LevelAsset *asset = &doc->asset;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.x < 32.0f || canvas_size.y < 32.0f || !asset->tiles) {
        ImGui::End();
        return;
    }

    EditorCanvas canvas = { origin, canvas_size, ui->view.zoom, ui->view.scroll_x, ui->view.scroll_y };
    if (ui->view.fit_requested) {
        editor_canvas_fit(&ui->view, &canvas, asset);
        ui->view.fit_requested = false;
    }

    ImGui::InvisibleButton("##canvas", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiIO &io = ImGui::GetIO();

    // ---- 输入 ----
    if (hovered && io.MouseWheel != 0.0f) {
        f32 world_x = 0.0f;
        f32 world_y = 0.0f;
        editor_screen_to_world(&canvas, io.MousePos, &world_x, &world_y);
        f32 factor = (io.MouseWheel > 0.0f) ? 1.15f : 1.0f / 1.15f;
        ui->view.zoom = clamp(ui->view.zoom * factor, EDITOR_ZOOM_MIN, EDITOR_ZOOM_MAX);
        canvas.zoom = ui->view.zoom;
        // 缩放后让鼠标指着的世界点留在原地
        ui->view.scroll_x = world_x - (io.MousePos.x - canvas.origin.x) / ui->view.zoom;
        ui->view.scroll_y = world_y + (io.MousePos.y - canvas.origin.y) / ui->view.zoom;
        canvas.scroll_x = ui->view.scroll_x;
        canvas.scroll_y = ui->view.scroll_y;
    }

    f32 world_x = 0.0f;
    f32 world_y = 0.0f;
    editor_screen_to_world(&canvas, io.MousePos, &world_x, &world_y);
    int cursor_col = -1;
    int cursor_row = -1;
    editor_edit_world_to_cell(asset, world_x, world_y, &cursor_col, &cursor_row);
    ui->cursor_col = hovered ? cursor_col : -1;
    ui->cursor_row = hovered ? cursor_row : -1;

    // 吸附：默认按格（tile 尺寸），按住 Alt 自由像素
    f32 snap_step = io.KeyAlt ? 0.0f : asset->tile_size;

    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        // 中键：平移
        ui->view.scroll_x -= io.MouseDelta.x / ui->view.zoom;
        ui->view.scroll_y += io.MouseDelta.y / ui->view.zoom;
        canvas.scroll_x = ui->view.scroll_x;
        canvas.scroll_y = ui->view.scroll_y;
        ui->drag.active = false;
    } else {
        // 手势边界：一次「按住→松手」= 一条撤销。画布上拖几百帧也只落一格
        if (ImGui::IsItemActivated()) {
            editor_doc_action_begin(doc);
        }

        // 按下的那一瞬间决定这一拖是干什么的（规则实现在 editor_edit.cc）
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyCtrl) {
            EditorDragBegin begin =
                editor_edit_begin_drag(doc, &ui->selection, editor_ui_pointer(&canvas, world_x, world_y));
            ui->drag = begin.drag;
            if (begin.selection_changed) {
                ui->selection = begin.selection;
            }

            // 起手是个「用户动作 + 判定结果」：出一行日志。拖不动时第一个该看的就是它
            // （它同时也是屏幕坐标 ↔ 世界坐标这条接线的证人）
            LOG_INFO("editor: drag begin kind=%u at world (%.1f, %.1f), radius %.1f/%.1f, selection (%u,%u)",
                     begin.drag.kind, world_x, world_y, EDITOR_HANDLE_RADIUS_PX / canvas.zoom,
                     EDITOR_NEAR_RADIUS_PX / canvas.zoom, ui->selection.kind, ui->selection.index);
        }

        if (ui->drag.active) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                editor_edit_apply_drag(doc, &ui->selection, &ui->drag, world_x, world_y, snap_step);
            }
        } else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (cursor_col >= 0) {
                editor_doc_set_tile(doc, (u32)cursor_col, (u32)cursor_row, LEVEL_TILE_EMPTY);
            }
        } else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyCtrl) {
            ui->selection = editor_edit_hit_body(doc, world_x, world_y);
            LOG_INFO("editor: ctrl+click at world (%.1f, %.1f) -> selection (%u,%u)", world_x, world_y,
                     ui->selection.kind, ui->selection.index);
        } else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (cursor_col >= 0) {
                editor_doc_set_tile(doc, (u32)cursor_col, (u32)cursor_row, ui->brush);
            }
        }

        if (ImGui::IsItemDeactivated()) {
            ui->drag.active = false;
            editor_doc_action_end(doc);
        }
    }

    editor_drag_cursor(ui, &canvas, hovered);

    // 悬停高亮（不改选中，只是让人知道点什么会被选中）
    EditorSelection hover_hit = {};
    bool hover_entity = false;
    if (hovered && !io.KeyCtrl) {
        hover_hit = editor_edit_hit_body(doc, world_x, world_y);
        hover_entity = hover_hit.kind != EDITOR_SEL_NONE;
    }

    // ---- 绘制 ----
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImVec2 canvas_end(canvas.origin.x + canvas.size.x, canvas.origin.y + canvas.size.y);
    draw->PushClipRect(canvas.origin, canvas_end, true);
    draw->AddRectFilled(canvas.origin, canvas_end, EDITOR_COLOR_OUTSIDE);

    f32 tile_size = asset->tile_size;
    f32 world_w = (f32)asset->tile_columns * tile_size;
    f32 world_h = (f32)asset->tile_rows * tile_size;

    // 关卡范围底色：网格外沿（x ∈ [0, 列数*格]，y ∈ [-行数*格, 0]）
    draw->AddRectFilled(editor_world_to_screen(&canvas, 0.0f, 0.0f),
                        editor_world_to_screen(&canvas, world_w, -world_h), EDITOR_COLOR_BACKDROP);

    // 只画视野内的格子
    int view_col0 = 0;
    int view_row0 = 0;
    int view_col1 = 0;
    int view_row1 = 0;
    {
        f32 corner_x0 = 0.0f;
        f32 corner_y0 = 0.0f;
        f32 corner_x1 = 0.0f;
        f32 corner_y1 = 0.0f;
        editor_screen_to_world(&canvas, canvas.origin, &corner_x0, &corner_y0);
        editor_screen_to_world(&canvas, canvas_end, &corner_x1, &corner_y1);
        int c0 = (int)floorf(corner_x0 / tile_size);
        int c1 = (int)floorf(corner_x1 / tile_size);
        int r0 = (int)floorf(-corner_y0 / tile_size); // corner_y0 是画布顶部（世界 y 较大）
        int r1 = (int)floorf(-corner_y1 / tile_size);
        view_col0 = MAX(c0, 0);
        view_col1 = MIN(c1, (int)asset->tile_columns - 1);
        view_row0 = MAX(r0, 0);
        view_row1 = MIN(r1, (int)asset->tile_rows - 1);
    }

    for (int row = view_row0; row <= view_row1; ++row) {
        for (int col = view_col0; col <= view_col1; ++col) {
            LevelTile tile = editor_doc_tile(doc, (u32)col, (u32)row);
            ImU32 color = 0;
            if (tile == LEVEL_TILE_SOLID) {
                color = EDITOR_COLOR_SOLID;
            } else if (tile == LEVEL_TILE_ONE_WAY) {
                color = EDITOR_COLOR_ONE_WAY;
            } else if (tile == LEVEL_TILE_SPAWN) {
                color = EDITOR_COLOR_SPAWN;
            } else if (tile != LEVEL_TILE_SPIKE && tile != LEVEL_TILE_VANISH) {
                continue;
            }
            ImVec2 a = editor_world_to_screen(&canvas, (f32)col * tile_size, -(f32)row * tile_size);
            ImVec2 b = editor_world_to_screen(&canvas, (f32)(col + 1) * tile_size, -(f32)(row + 1) * tile_size);
            if (tile == LEVEL_TILE_SPIKE) {
                // 画成锯齿而不是红块：一眼就能看出「这一格是地刺」，不会和别的实体混起来。
                // 一格四颗齿（16px 一颗）、齿高半格且贴在格子下半格，与 main.cc 的贴图对得上
                // （视觉半格，伤害判定仍是整格）；整格外框就是那条伤害矩形的范围。
                f32 step = (b.x - a.x) * 0.25f;
                f32 tooth_top = (a.y + b.y) * 0.5f;
                for (int k = 0; k < 4; ++k) {
                    f32 x0 = a.x + step * (f32)k;
                    draw->AddTriangleFilled(ImVec2(x0, b.y), ImVec2(x0 + step, tooth_top), ImVec2(x0 + step * 2.0f, b.y),
                                            EDITOR_COLOR_SPIKE);
                }
                draw->AddRect(a, b, EDITOR_COLOR_SPIKE_EDGE);
            } else if (tile == LEVEL_TILE_VANISH) {
                // 半透明填充 + 亮边框：与实体墙/单向平台（实心）区分开 —— 它并不是一块稳定的地形
                draw->AddRectFilled(a, b, EDITOR_COLOR_VANISH);
                draw->AddRect(a, b, EDITOR_COLOR_VANISH_EDGE);
            } else {
                draw->AddRectFilled(a, b, color);
            }
        }
    }

    // 网格线：太小就不画（否则糊成一片）
    f32 cell_px = tile_size * canvas.zoom;
    if (ui->view.show_grid && cell_px >= 4.0f) {
        for (int col = view_col0; col <= view_col1 + 1; ++col) {
            f32 x = (f32)col * tile_size;
            draw->AddLine(editor_world_to_screen(&canvas, x, 0.0f),
                          editor_world_to_screen(&canvas, x, -world_h), EDITOR_COLOR_GRID);
        }
        for (int row = view_row0; row <= view_row1 + 1; ++row) {
            f32 y = -(f32)row * tile_size;
            draw->AddLine(editor_world_to_screen(&canvas, 0.0f, y),
                          editor_world_to_screen(&canvas, world_w, y), EDITOR_COLOR_GRID);
        }
    }

    // 门：整条左/右边缘都是触发线（玩家的碰撞盒完全越过去才切图），所以画成一条竖条
    for (u32 i = 0; i < asset->connection_count; ++i) {
        const LevelConnectionAsset *connection = &asset->connections[i];
        bool left = (connection->side == LEVEL_CONNECTION_LEFT);
        f32 x = left ? 0.0f : world_w;
        ImVec2 a = editor_world_to_screen(&canvas, x, 0.0f);
        ImVec2 b = editor_world_to_screen(&canvas, x, -world_h);
        draw->AddLine(a, b, EDITOR_COLOR_DOOR, MAX(3.0f, 6.0f * canvas.zoom));
    }

    // 移动组件：本体几何是**派生**的（轴上的参数 + 半宽半高），所以这里也算一遍。
    // 圆形画成圆的（游戏里是透明外圈的圆盘贴图），会伤害的用伤害色
    for (u32 i = 0; i < asset->mover_count; ++i) {
        const LevelMoverAsset *mover = &asset->movers[i];
        Rect2D rect = level_asset_mover_rect(mover);
        bool hazard = (mover->shape == LEVEL_MOVER_CIRCLE) || (mover->damaging != 0);
        ImU32 fill = hazard ? EDITOR_COLOR_SPIKE : EDITOR_COLOR_PLATFORM;
        ImU32 edge = hazard ? EDITOR_COLOR_SPIKE_EDGE : EDITOR_COLOR_PLATFORM_EDGE;
        if (mover->shape == LEVEL_MOVER_CIRCLE) {
            ImVec2 center = editor_world_to_screen(&canvas, rect.center_x, rect.center_y);
            f32 radius_px = MAX(2.0f, rect.half_w * canvas.zoom);
            draw->AddCircleFilled(center, radius_px, fill);
            draw->AddCircle(center, radius_px, edge, 0, 2.0f);
        } else {
            ImVec2 a = editor_world_to_screen(&canvas, rect.center_x - rect.half_w, rect.center_y + rect.half_h);
            ImVec2 b = editor_world_to_screen(&canvas, rect.center_x + rect.half_w, rect.center_y - rect.half_h);
            draw->AddRectFilled(a, b, fill);
            draw->AddRect(a, b, edge);
        }
        // 往返轴：A → B 的直线（本体一定在这条线上）
        draw->AddLine(editor_world_to_screen(&canvas, mover->point_a.x, mover->point_a.y),
                      editor_world_to_screen(&canvas, mover->point_b.x, mover->point_b.y), EDITOR_COLOR_PLATFORM_EDGE);
    }
    for (u32 i = 0; i < asset->monster_count; ++i) {
        const LevelMonsterAsset *monster = &asset->monsters[i];
        ImVec2 a = editor_world_to_screen(&canvas, monster->rect.center_x - monster->rect.half_w,
                                          monster->rect.center_y + monster->rect.half_h);
        ImVec2 b = editor_world_to_screen(&canvas, monster->rect.center_x + monster->rect.half_w,
                                          monster->rect.center_y - monster->rect.half_h);
        draw->AddRectFilled(a, b, EDITOR_COLOR_MONSTER);
        draw->AddRect(a, b, EDITOR_COLOR_MONSTER_EDGE);
        // 复位点（掉出世界或被打中后回到这里）
        ImVec2 spawn = editor_world_to_screen(&canvas, monster->spawn_x, monster->spawn_y);
        draw->AddCircle(spawn, MAX(3.0f, 5.0f * canvas.zoom), EDITOR_COLOR_MONSTER_EDGE, 0, 2.0f);
        draw->AddLine(ImVec2(spawn.x - 8.0f, spawn.y), ImVec2(spawn.x + 8.0f, spawn.y), EDITOR_COLOR_MONSTER_EDGE);
    }
    // 传送门：同色 = 配对。颜色取自共享调色板，所以编辑器里看到的就是游戏里看到的
    for (u32 i = 0; i < asset->portal_count; ++i) {
        const LevelPortalAsset *portal = &asset->portals[i];
        const LevelPortalColor *color = &LEVEL_PORTAL_PALETTE[portal->pair_id % LEVEL_PORTAL_PALETTE_COUNT];
        ImU32 edge = IM_COL32(color->r, color->g, color->b, 255);
        ImU32 fill = IM_COL32(color->r, color->g, color->b, 90);
        ImVec2 a = editor_world_to_screen(&canvas, portal->rect.center_x - portal->rect.half_w,
                                          portal->rect.center_y + portal->rect.half_h);
        ImVec2 b = editor_world_to_screen(&canvas, portal->rect.center_x + portal->rect.half_w,
                                          portal->rect.center_y - portal->rect.half_h);
        draw->AddRectFilled(a, b, fill);
        draw->AddRect(a, b, edge, 0.0f, 0, MAX(2.0f, 3.0f * canvas.zoom));
    }

    // 传送点：它存的是一**个点**（脚底坐标），所以画成一个贴地点的小方块 + 一条底边线 ——
    // 形状与出生点标记一样（两者语义确实是「要不要用大地图去」的区别），颜色不同
    for (u32 i = 0; i < asset->waypoint_count; ++i) {
        const LevelWaypointAsset *waypoint = &asset->waypoints[i];
        ImVec2 a = editor_world_to_screen(&canvas, waypoint->x - tile_size * 0.25f, waypoint->y + tile_size * 0.5f);
        ImVec2 b = editor_world_to_screen(&canvas, waypoint->x + tile_size * 0.25f, waypoint->y);
        draw->AddRectFilled(a, b, EDITOR_COLOR_WAYPOINT);
        draw->AddLine(editor_world_to_screen(&canvas, waypoint->x - tile_size * 0.4f, waypoint->y),
                      editor_world_to_screen(&canvas, waypoint->x + tile_size * 0.4f, waypoint->y),
                      EDITOR_COLOR_WAYPOINT, 2.0f);
    }

    // 出生点标记：脚底在格子底边，所以标记贴着底边画
    u32 spawn_col = 0;
    u32 spawn_row = 0;
    if (editor_doc_find_spawn(doc, &spawn_col, &spawn_row)) {
        f32 center_x = level_cell_center_x((u32)spawn_col, tile_size);
        f32 bottom = level_cell_bottom_y((u32)spawn_row, tile_size);
        ImVec2 a = editor_world_to_screen(&canvas, center_x - tile_size * 0.25f, bottom + tile_size * 0.5f);
        ImVec2 b = editor_world_to_screen(&canvas, center_x + tile_size * 0.25f, bottom);
        draw->AddRectFilled(a, b, EDITOR_COLOR_SPAWN);
        draw->AddLine(editor_world_to_screen(&canvas, center_x - tile_size * 0.5f, bottom),
                      editor_world_to_screen(&canvas, center_x + tile_size * 0.5f, bottom), EDITOR_COLOR_SELECT, 2.0f);
    }

    // 编译后的合并碰撞体（只在诊断结果是最新的时候画：过期数据不冒充真相）
    bool diagnostics_fresh = ui->diagnostics.valid && ui->diagnostics.revision == doc->revision;
    if (ui->view.show_colliders && diagnostics_fresh) {
        for (u32 i = 0; i < ui->diagnostics.level.platform_count; ++i) {
            const Rect2D *rect = &ui->diagnostics.level.platforms[i].rect;
            ImVec2 a = editor_world_to_screen(&canvas, rect->center_x - rect->half_w, rect->center_y + rect->half_h);
            ImVec2 b = editor_world_to_screen(&canvas, rect->center_x + rect->half_w, rect->center_y - rect->half_h);
            draw->AddRect(a, b, EDITOR_COLOR_COLLIDER);
        }
    }

    // 诊断高亮：光标格所属的那一块合并体
    if (ui->view.show_diagnostic_box && diagnostics_fresh && ui->cursor_col >= 0 && ui->cursor_row >= 0) {
        u32 index = ui->diagnostics.cell_platform[(u64)ui->cursor_row * ui->diagnostics.cell_columns + (u32)ui->cursor_col];
        if (index != EDITOR_NO_PLATFORM) {
            const Rect2D *rect = &ui->diagnostics.level.platforms[index].rect;
            ImVec2 a = editor_world_to_screen(&canvas, rect->center_x - rect->half_w, rect->center_y + rect->half_h);
            ImVec2 b = editor_world_to_screen(&canvas, rect->center_x + rect->half_w, rect->center_y - rect->half_h);
            draw->AddRect(a, b, EDITOR_COLOR_DIAGNOSTIC, 0.0f, 0, 2.0f);
        }
    }

    // 悬停描边：告诉人「点这里会选中谁」
    if (hover_entity && editor_sel_is_entity(hover_hit.kind)) {
        Rect2D rect = {};
        if (level_asset_entity_rect_now(asset, editor_sel_entity_kind(hover_hit.kind), hover_hit.index, &rect)) {
            ImVec2 a = editor_world_to_screen(&canvas, rect.center_x - rect.half_w, rect.center_y + rect.half_h);
            ImVec2 b = editor_world_to_screen(&canvas, rect.center_x + rect.half_w, rect.center_y - rect.half_h);
            draw->AddRect(a, b, EDITOR_COLOR_HOVER, 0.0f, 0, 2.0f);
        }
    }

    // 选中描边 + 可拖的手柄（让「能拖」看得见）
    {
        Rect2D rect = {};
        bool has_rect = false;
        if (editor_sel_is_entity(ui->selection.kind)) {
            has_rect = level_asset_entity_rect_now(asset, editor_sel_entity_kind(ui->selection.kind),
                                                   ui->selection.index, &rect);
        }
        if (has_rect) {
            ImVec2 a = editor_world_to_screen(&canvas, rect.center_x - rect.half_w, rect.center_y + rect.half_h);
            ImVec2 b = editor_world_to_screen(&canvas, rect.center_x + rect.half_w, rect.center_y - rect.half_h);
            draw->AddRect(a, b, EDITOR_COLOR_SELECT, 0.0f, 0, 2.0f);

            f32 mid_x = (a.x + b.x) * 0.5f;
            f32 mid_y = (a.y + b.y) * 0.5f;
            const ImVec2 handles[8] = { ImVec2(a.x, a.y),     ImVec2(mid_x, a.y), ImVec2(b.x, a.y),
                                        ImVec2(a.x, mid_y), ImVec2(b.x, mid_y), ImVec2(a.x, b.y),
                                        ImVec2(mid_x, b.y), ImVec2(b.x, b.y) };
            for (u32 i = 0; i < 8; ++i) {
                draw->AddRectFilled(ImVec2(handles[i].x - 3.0f, handles[i].y - 3.0f),
                                    ImVec2(handles[i].x + 3.0f, handles[i].y + 3.0f), EDITOR_COLOR_SELECT);
            }
        }

        // 移动组件的往返轴端点（可以拖）
        if (ui->selection.kind == editor_sel_of_entity(LEVEL_ASSET_ENTITY_MOVER) &&
            ui->selection.index < asset->mover_count) {
            const LevelMoverAsset *mover = &asset->movers[ui->selection.index];
            // 两个端点都是可拖的（二维），A 画成方块、B 画成菱形 —— 组件**先朝 B 走**，所以能一眼分出来
            ImVec2 ends[2] = { editor_world_to_screen(&canvas, mover->point_a.x, mover->point_a.y),
                               editor_world_to_screen(&canvas, mover->point_b.x, mover->point_b.y) };
            draw->AddRect(ImVec2(ends[0].x - 5.0f, ends[0].y - 5.0f), ImVec2(ends[0].x + 5.0f, ends[0].y + 5.0f),
                          EDITOR_COLOR_SELECT, 0.0f, 0, 2.0f);
            draw->AddQuadFilled(ImVec2(ends[1].x, ends[1].y - 6.0f), ImVec2(ends[1].x + 6.0f, ends[1].y),
                                ImVec2(ends[1].x, ends[1].y + 6.0f), ImVec2(ends[1].x - 6.0f, ends[1].y),
                                EDITOR_COLOR_SELECT);
        }

        // 怪物的复位点（可以拖）
        if (ui->selection.kind == editor_sel_of_entity(LEVEL_ASSET_ENTITY_MONSTER) &&
            ui->selection.index < asset->monster_count) {
            const LevelMonsterAsset *monster = &asset->monsters[ui->selection.index];
            ImVec2 respawn = editor_world_to_screen(&canvas, monster->spawn_x, monster->spawn_y);
            draw->AddCircle(respawn, 8.0f, EDITOR_COLOR_SELECT, 0, 2.0f);
            draw->AddLine(ImVec2(respawn.x - 6.0f, respawn.y), ImVec2(respawn.x + 6.0f, respawn.y), EDITOR_COLOR_SELECT);
            draw->AddLine(ImVec2(respawn.x, respawn.y - 6.0f), ImVec2(respawn.x, respawn.y + 6.0f), EDITOR_COLOR_SELECT);
        }
    }

    // 光标格
    if (hovered && cursor_col >= 0) {
        ImVec2 a = editor_world_to_screen(&canvas, (f32)cursor_col * tile_size, -(f32)cursor_row * tile_size);
        ImVec2 b = editor_world_to_screen(&canvas, (f32)(cursor_col + 1) * tile_size, -(f32)(cursor_row + 1) * tile_size);
        draw->AddRect(a, b, EDITOR_COLOR_CURSOR);
    }

    draw->PopClipRect();

    // 提示：诊断过期 / 没跑过
    if (ui->view.show_colliders && !diagnostics_fresh) {
        ImGui::SetCursorScreenPos(ImVec2(canvas.origin.x + 8.0f, canvas.origin.y + 8.0f));
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "合并碰撞体叠加层需要最新的检查结果（右侧点「检查」）");
    }

    ImGui::End();
}

// ============================================================================
// 右侧：关卡属性 / 选中物属性 / 检查
// ============================================================================

internal void editor_draw_properties(EditorUiState *ui)
{
    EditorDoc *doc = &ui->doc;
    LevelAsset *asset = &doc->asset;

    ImGui::SeparatorText("关卡属性（单例，寄居在网格里）");
    ImGui::Text("网格 %u × %u 格，每格 %.0fpx", asset->tile_columns, asset->tile_rows, asset->tile_size);
    int world_id = editor_doc_world_id(doc);
    if (world_id >= 0) {
        ImGui::Text("世界 ID %d（由文件名决定：first.bin=0 / second.bin=1）", world_id);
    } else {
        ImGui::TextDisabled("世界 ID 未知（文件名不是 first.bin / second.bin）");
    }

    u32 spawn_count = editor_doc_spawn_count(doc);
    if (spawn_count == LEVEL_ASSET_REQUIRED_SPAWN_TILES) {
        u32 col = 0;
        u32 row = 0;
        if (editor_doc_find_spawn(doc, &col, &row)) {
            f32 spawn_y = level_cell_bottom_y(row, asset->tile_size);
            int edit_col = (int)col;
            int edit_row = (int)row;
            ImGui::SetNextItemWidth(120.0f);
            bool changed = ImGui::DragInt("出生点 列", &edit_col, 0.2f, 0, (int)asset->tile_columns - 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            changed |= ImGui::DragInt("行", &edit_row, 0.2f, 0, (int)asset->tile_rows - 1);
            if (changed) {
                editor_doc_set_spawn(doc, (u32)MAX(edit_col, 0), (u32)MAX(edit_row, 0));
            }
            ImGui::TextDisabled("脚底落点 (%.0f, %.0f)（格子底边，不是中心）",
                                level_cell_center_x(col, asset->tile_size), editor_world_y_to_display(ui, spawn_y));
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "出生点有 %u 个，必须是 1 个", spawn_count);
    }

    if (ui->cursor_col >= 0) {
        if (ImGui::Button("把出生点放到光标格")) {
            editor_doc_set_spawn(doc, (u32)ui->cursor_col, (u32)ui->cursor_row);
        }
    } else {
        ImGui::TextDisabled("把鼠标移到网格上才能「放到光标格」");
    }

    ImGui::SeparatorText("选中物的属性");
    switch (ui->selection.kind) {
    case EDITOR_SEL_NONE:
        ImGui::TextDisabled("未选中。左栏点列表项，或在网格里 Ctrl + 左键点物件。");
        break;
    case EDITOR_SEL_SPAWN:
        ImGui::Text("出生点（关卡单例）");
        ImGui::TextDisabled("它的位置就是网格里那个 'P'，参数在上面改。");
        break;
    case editor_sel_of_entity(LEVEL_ASSET_ENTITY_CONNECTION): {
        LevelConnectionAsset *connection = editor_doc_connection(doc, ui->selection.index);
        if (connection) {
            ImGui::Text("门 #%u", ui->selection.index);
            ImGui::TextDisabled("整条左/右边缘都是触发线，玩家完全越过去才切图");
            static const char *const side_names[] = { "左侧", "右侧" };
            static const char *const facing_names[] = { "朝左", "朝右" };
            u32 side = (u32)connection->side;
            u32 facing = (u32)connection->facing;
            editor_field_enum(doc, "在哪一侧", &side, side_names, array_size(side_names));
            editor_field_enum(doc, "进场朝向", &facing, facing_names, array_size(facing_names));
            connection->side = (LevelConnectionSide)side;
            connection->facing = (LevelConnectionFacing)facing;

            int target = (int)connection->target_level;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragInt("目标世界", &target, 0.2f, 0, (int)LEVEL_ASSET_WORLD_COUNT - 1)) {
                connection->target_level = (u32)MAX(target, 0);
                editor_doc_mark_changed(doc);
            }
            editor_field_f32(doc, "落点 x", &connection->entry_x, 1.0f, "目标世界的脚底坐标");
            editor_field_coord_y(ui, doc, "落点 y", &connection->entry_y, 1.0f, nullptr);

            if (ImGui::Button("删除这扇门")) {
                editor_doc_remove_entity(doc, LEVEL_ASSET_ENTITY_CONNECTION, ui->selection.index);
                ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            }
        }
        break;
    }
    case editor_sel_of_entity(LEVEL_ASSET_ENTITY_MOVER): {
        LevelMoverAsset *mover = editor_doc_mover(doc, ui->selection.index);
        if (mover) {
            ImGui::Text("移动组件 #%u", ui->selection.index);
            ImGui::TextDisabled("在 A、B 两点之间往返（轴可任意方向）；先朝 B（菱形那个）走");
            int shape = (int)mover->shape;
            const char *shape_names[] = { "方形平台（可站可驮）", "圆形（碰到就伤害）" };
            if (ImGui::Combo("形状", &shape, shape_names, IM_ARRAYSIZE(shape_names))) {
                mover->shape = (u32)MAX(shape, 0);
                if (mover->shape == LEVEL_MOVER_CIRCLE) {
                    mover->half_h = mover->half_w; // 圆：半径就是半宽，两个半轴一体
                }
                editor_doc_mark_changed(doc);
            }
            editor_field_f32(doc, "端点 A x", &mover->point_a.x, 1.0f, nullptr);
            editor_field_coord_y(ui, doc, "端点 A y", &mover->point_a.y, 1.0f, nullptr);
            editor_field_f32(doc, "端点 B x", &mover->point_b.x, 1.0f, nullptr);
            editor_field_coord_y(ui, doc, "端点 B y", &mover->point_b.y, 1.0f, nullptr);
            editor_field_f32(doc, "起点 t0", &mover->t0, 0.01f, "0 = A、1 = B（本体按这个参数贴在轴上）");
            if (mover->shape == LEVEL_MOVER_CIRCLE) {
                editor_field_f32(doc, "半径", &mover->half_w, 1.0f, "像素");
            } else {
                editor_field_f32(doc, "半宽", &mover->half_w, 1.0f, nullptr);
                editor_field_f32(doc, "半高", &mover->half_h, 1.0f, nullptr);
            }
            editor_field_f32(doc, "speed", &mover->speed, 5.0f, "px/s（沿轴，不是分量）");
            if (mover->shape == LEVEL_MOVER_CIRCLE) {
                ImGui::TextDisabled("圆形恒定伤害：碰到就重生（与地刺同一套）");
            } else {
                bool damaging = (mover->damaging != 0);
                if (ImGui::Checkbox("被挤住会伤害", &damaging)) {
                    mover->damaging = damaging ? 1u : 0u;
                    editor_doc_mark_changed(doc);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("（驮着走是安全的）");
            }
            if (ImGui::Button("删除这个移动组件")) {
                editor_doc_remove_entity(doc, LEVEL_ASSET_ENTITY_MOVER, ui->selection.index);
                ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            }
        }
        break;
    }
    case editor_sel_of_entity(LEVEL_ASSET_ENTITY_MONSTER): {
        LevelMonsterAsset *monster = editor_doc_monster(doc, ui->selection.index);
        if (monster) {
            ImGui::Text("怪物 #%u", ui->selection.index);
            ImGui::TextDisabled("恒定水平速度 + 重力；掉出世界或被打中就回到复位点");
            editor_field_rect(ui, doc, "矩形（尺寸 + 初始位置）", &monster->rect);
            editor_field_f32(doc, "复位点 x", &monster->spawn_x, 1.0f, nullptr);
            editor_field_coord_y(ui, doc, "复位点 y", &monster->spawn_y, 1.0f, nullptr);
            editor_field_f32(doc, "velocity_x", &monster->velocity_x, 5.0f, "px/s");
            if (ImGui::Button("把复位点对齐到当前位置")) {
                monster->spawn_x = monster->rect.center_x;
                monster->spawn_y = monster->rect.center_y;
                editor_doc_mark_changed(doc);
            }
            if (ImGui::Button("删除这个怪物")) {
                editor_doc_remove_entity(doc, LEVEL_ASSET_ENTITY_MONSTER, ui->selection.index);
                ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            }
        }
        break;
    }
    case editor_sel_of_entity(LEVEL_ASSET_ENTITY_WAYPOINT): {
        LevelWaypointAsset *waypoint = editor_doc_waypoint(doc, ui->selection.index);
        if (waypoint) {
            ImGui::Text("传送点 #%u", ui->selection.index);
            ImGui::TextDisabled("大地图上的目的地（地图之间的传送）：选中它 → 点某个地图项 → 确认");
            ImGui::TextDisabled("坐标是**脚底**位置：摆在站得住的地方即可（摆进墙里会顶上墙顶）");
            editor_field_f32(doc, "x", &waypoint->x, 1.0f, nullptr);
            editor_field_coord_y(ui, doc, "脚底 y", &waypoint->y, 1.0f, nullptr);
            if (ImGui::Button("删除这个传送点")) {
                editor_doc_remove_entity(doc, LEVEL_ASSET_ENTITY_WAYPOINT, ui->selection.index);
                ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            }
        }
        break;
    }
    case editor_sel_of_entity(LEVEL_ASSET_ENTITY_PORTAL): {
        LevelPortalAsset *portal = editor_doc_portal(doc, ui->selection.index);
        if (portal) {
            ImGui::Text("传送门 #%u", ui->selection.index);
            ImGui::TextDisabled("同一色号的两扇门互传；角色中心站在门里时按上键触发");
            ImGui::TextDisabled("落点 = 角色水平中心对齐门中心、脚底贴门底边（门埋进墙里时会顶上墙顶）");
            editor_field_rect(ui, doc, "矩形（尺寸 + 位置）", &portal->rect);
            int pair_id = (int)portal->pair_id;
            if (ImGui::DragInt("配对角标（颜色）", &pair_id, 0.1f, 0, (int)LEVEL_PORTAL_PALETTE_COUNT - 1)) {
                portal->pair_id = (u32)MAX(pair_id, 0);
                editor_doc_mark_changed(doc);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("同色 = 一对，必须恰好两扇");
            if (ImGui::Button("删除这个传送门")) {
                editor_doc_remove_entity(doc, LEVEL_ASSET_ENTITY_PORTAL, ui->selection.index);
                ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            }
        }
        break;
    }
    default:
        // 选择种类来自界面自身，越界说明界面写错了 —— switch 不写 default 会被 /W4 提示漏 case，
        // 所以这里保留 default 但只做兜底
        ImGui::TextDisabled("选中的东西已经不在了");
        break;
    }
}

internal void editor_draw_check_section(EditorUiState *ui)
{
    EditorDoc *doc = &ui->doc;
    ImGui::SeparatorText("检查");

    // 校验每帧实时跑：纯资产、零分配、不编译（规则见 include/shared/level_asset.h）
    LevelAssetIssues issues = {};
    editor_doc_validate(doc, &issues);

    bool diagnostics_fresh = ui->diagnostics.valid && ui->diagnostics.revision == doc->revision &&
                             ui->diagnostics.y_from_bottom == ui->view.y_from_bottom;
    if (ImGui::Button("检查（编译 + 诊断）")) {
        editor_doc_run_diagnostics(doc, &ui->diagnostics, &ui->selection, ui->cursor_col, ui->cursor_row,
                                   ui->view.y_from_bottom);
        editor_message(&ui->messages, "已重新检查：编译 %u 块碰撞体",
                       ui->diagnostics.valid ? ui->diagnostics.level.platform_count : 0u);
    }
    ImGui::SameLine();
    if (ui->diagnostics.valid && !diagnostics_fresh) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "结果已过期");
    } else if (ui->diagnostics.valid) {
        ImGui::TextDisabled("结果是最新的");
    } else {
        ImGui::TextDisabled("还没检查过（或校验有错误，那一层不编译）");
    }

    if (issues.error_count > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "错误 %u 条 —— 有错误就存不了盘", issues.error_count);
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "没有错误");
    }
    if (issues.warn_count > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "警告 %u 条（能存盘，但建议看一眼）", issues.warn_count);
    }

    ImGui::BeginChild("##issues", ImVec2(0.0f, 160.0f * ui->ui_scale), true);
    for (u32 i = 0; i < issues.count; ++i) {
        const LevelAssetIssue *issue = &issues.items[i];
        bool is_error = (issue->level == LEVEL_ASSET_ISSUE_ERROR);
        ImVec4 color = is_error ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
        ImGui::PushID((int)i);
        ImGui::TextColored(color, "%s", is_error ? "[错误]" : "[警告]");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", issue->text);
        // 带格坐标的问题可以点着跳过去
        if (issue->col >= 0 && issue->row >= 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("定位")) {
                EditorCanvas canvas = { ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), 0.0f, 0.0f, 0.0f };
                // 定位只需要画布尺寸，而这个函数在画布窗口之外 —— 用主视口尺寸近似
                canvas.size = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.6f);
                editor_canvas_center_on(ui, &canvas, issue->col, issue->row);
            }
        }
        ImGui::PopID();
    }
    if (issues.truncated) {
        ImGui::TextDisabled("（问题多到装不下，先修掉前面的）");
    }
    ImGui::EndChild();

    if (ui->diagnostics.valid) {
        ImGui::SeparatorText("诊断（编译结果 / 光标格）");
        ImGui::BeginChild("##diagnostics", ImVec2(0.0f, 200.0f * ui->ui_scale), true);
        for (u32 i = 0; i < ui->diagnostics.line_count; ++i) {
            ImGui::TextUnformatted(ui->diagnostics.lines[i]);
        }
        ImGui::EndChild();
    }
}

// ============================================================================
// 菜单栏 / 状态栏 / 弹窗
// ============================================================================

internal bool editor_open_level_from_list(EditorUiState *ui, const char *name)
{
    char path[EDITOR_PATH_SIZE * 2] = {};
    snprintf(path, sizeof(path), "data/map/%s", name);

    wchar_t wide[EDITOR_PATH_SIZE] = {};
    if (!utf8_to_wide(path, wide, EDITOR_PATH_SIZE)) {
        editor_message(&ui->messages, "路径不合法或太长：%s", path);
        return false;
    }
    if (!editor_doc_open(&ui->doc, wide)) {
        editor_message(&ui->messages, "打不开 %s（文件不存在，或 magic/版本/长度校验没通过）", path);
        return false;
    }

    ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
    ui->diagnostics = EditorDiagnostics{};
    ui->view.fit_requested = true;
    editor_message(&ui->messages, "已打开 %s", path);
    return true;
}

internal void editor_do_save_as(EditorUiState *ui)
{
    wchar_t wide[EDITOR_PATH_SIZE] = {};
    if (!utf8_to_wide(ui->path_input, wide, EDITOR_PATH_SIZE)) {
        editor_message(&ui->messages, "路径不合法或太长");
        return;
    }
    if (editor_doc_save(&ui->doc, wide, &ui->messages)) {
        ui->view.fit_requested = true;
    }
}

internal bool editor_save_before_run(EditorUiState *ui)
{
    if (!ui->doc.has_path) {
        snprintf(ui->path_input, sizeof(ui->path_input), "%s", "data/map/new_map.bin");
        ui->save_as_visible = true;
        editor_message(&ui->messages, "先给这张图一个路径（另存为），存过盘才谈得上让主程序验证");
        return false;
    }
    // 保存会先跑语义校验：有 ERROR 就不写盘 —— 那也是「不许让主程序去撞」的信号
    return editor_doc_save(&ui->doc, ui->doc.path, &ui->messages);
}

internal void editor_draw_menu_bar(EditorUiState *ui)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建空关卡模板")) {
            editor_doc_new(&ui->doc, 64, 24, 64.0f);
            ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            ui->diagnostics = EditorDiagnostics{};
            ui->view.fit_requested = true;
            editor_message(&ui->messages, "新建了空关卡模板（还没存过盘，用「另存为」给个路径）");
        }

        if (ImGui::BeginMenu("打开 data/map")) {
            char names[EDITOR_LEVEL_LIST_MAX][EDITOR_LEVEL_NAME_SIZE] = {};
            u32 count = editor_list_levels(names, EDITOR_LEVEL_LIST_MAX);
            if (count == 0) {
                ImGui::MenuItem("（没有找到 .bin）", nullptr, false, false);
            }
            for (u32 i = 0; i < count; ++i) {
                if (ImGui::MenuItem(names[i])) {
                    editor_open_level_from_list(ui, names[i]);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("重新载入（丢弃未保存的改动）")) {
            if (editor_doc_reload(&ui->doc)) {
                ui->diagnostics = EditorDiagnostics{};
                ui->view.fit_requested = true;
                editor_message(&ui->messages, "已重新载入");
            } else {
                editor_message(&ui->messages, "重新载入失败");
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem("保存", "Ctrl+S", false, ui->doc.has_path)) {
            editor_doc_save(&ui->doc, ui->doc.path, &ui->messages);
        }
        if (ImGui::MenuItem("另存为…")) {
            char current[EDITOR_PATH_SIZE * 2] = {};
            wide_to_utf8(ui->doc.has_path ? ui->doc.path : L"data/map/new_map.bin", current, sizeof(current));
            snprintf(ui->path_input, sizeof(ui->path_input), "%s", current);
            ui->save_as_visible = true;
        }

        ImGui::Separator();
        if (ImGui::MenuItem("保存并试玩（起 build\\main.exe）")) {
            if (editor_save_before_run(ui)) {
                if (editor_run_game(nullptr, false, nullptr)) {
                    editor_message(&ui->messages, "已启动主程序：手动玩一遍");
                } else {
                    editor_message(&ui->messages, "起不来：先跑一次 build.bat，且编辑器要从仓库根启动");
                }
            }
        }
        if (ImGui::MenuItem("保存并冒烟验证（--fast input_script test\\smoke.txt）")) {
            if (editor_save_before_run(ui)) {
                u32 exit_code = 1;
                if (editor_run_game(L"--fast input_script test/smoke.txt", true, &exit_code)) {
                    if (exit_code == 0) {
                        editor_message(&ui->messages, "冒烟通过：主程序加载了这张图、出生点站得住、能走能跳");
                    } else {
                        editor_message(&ui->messages, "冒烟失败：退出码 %u（看 game.log 里的 FAIL 行）", exit_code);
                    }
                } else {
                    editor_message(&ui->messages, "起不来：先跑一次 build.bat，且编辑器要从仓库根启动");
                }
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem("退出")) {
            ui->request_quit = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("编辑")) {
        // 撤销 / 重做：菜单项与 Ctrl+Z / Ctrl+Y 走同一条路径。
        // 选择里的编号越不越界由 editor_doc_undo/redo 自己夹（界面不管）
        if (ImGui::MenuItem("撤销", "Ctrl+Z", false, editor_doc_can_undo(&ui->doc))) {
            editor_doc_undo(&ui->doc, &ui->selection);
            ui->drag.active = false;
        }
        if (ImGui::MenuItem("重做", "Ctrl+Y", false, editor_doc_can_redo(&ui->doc))) {
            editor_doc_redo(&ui->doc, &ui->selection);
            ui->drag.active = false;
        }
        ImGui::Separator();
        // 「打开一张图 → 另存为 → 清空」是「新建关卡」的替代路径
        // （真正的新建世界要改游戏侧：世界 ID 是位置决定的，见 editor/README.md）
        if (ImGui::MenuItem("清空网格（全部涂空）")) {
            editor_doc_fill_tiles(&ui->doc, LEVEL_TILE_EMPTY);
            editor_message(&ui->messages, "网格已清空：出生点（'P'）也跟着没了，记得再放一个");
        }
        if (ImGui::MenuItem("删除全部实体")) {
            editor_doc_clear_entities(&ui->doc);
            ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
            editor_message(&ui->messages, "所有实体已删除（地刺 / 门 / 移动组件 / 怪物）");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("显示网格", nullptr, &ui->view.show_grid);
        ImGui::MenuItem("显示合并碰撞体", nullptr, &ui->view.show_colliders);
        ImGui::MenuItem("高亮光标格的碰撞体", nullptr, &ui->view.show_diagnostic_box);
        ImGui::Separator();
        // 只影响「给人看的数字」：画布与关卡数据永远是世界 y（理由见文件上方那段注释）
        ImGui::MenuItem("Y 轴：底部为 0（向上为正）", nullptr, &ui->view.y_from_bottom);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("默认：关卡底部 = 0，往上变大（所以「顶部 1080」就是关卡高）\n"
                              "关掉：原始世界 y（顶部为 0、向下为负）\n"
                              "要跟 --trace / test\\*.txt 的 assert_pos / docs 里的数字对齐时用后者");
        }
        if (ImGui::MenuItem("全览（缩放到整幅关卡）")) {
            ui->view.fit_requested = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("快捷键与说明")) {
            ui->about_visible = true;
        }
        ImGui::EndMenu();
    }

    // 右侧显示当前文件与脏标记
    char utf8[EDITOR_PATH_SIZE * 2] = {};
    if (ui->doc.has_path) {
        wide_to_utf8(ui->doc.path, utf8, sizeof(utf8));
    } else {
        snprintf(utf8, sizeof(utf8), "（未保存过的新关卡）");
    }
    char right[EDITOR_PATH_SIZE * 2 + 32] = {};
    // 「未保存」标记：改过内容，或者根本还没存过（新建的关卡）
    snprintf(right, sizeof(right), "%s%s", utf8, (ui->doc.dirty || !ui->doc.has_path) ? "  *未保存" : "");
    f32 text_w = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(MAX(ImGui::GetWindowWidth() - text_w - 16.0f, 320.0f));
    ImGui::TextUnformatted(right);

    ImGui::EndMainMenuBar();
}

internal void editor_draw_status_bar(EditorUiState *ui, ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("##status", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const EditorDoc *doc = &ui->doc;
    const LevelAsset *asset = &doc->asset;

    char utf8[EDITOR_PATH_SIZE * 2] = {};
    if (doc->has_path) {
        wide_to_utf8(doc->path, utf8, sizeof(utf8));
    } else {
        snprintf(utf8, sizeof(utf8), "（未保存过）");
    }
    ImGui::Text("%s", utf8);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("v%u  %u×%u @%.0fpx", LEVEL_ASSET_VERSION, asset->tile_columns, asset->tile_rows, asset->tile_size);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ui->cursor_col >= 0) {
        f32 world_x = ((f32)ui->cursor_col + 0.5f) * asset->tile_size;
        f32 world_y = -((f32)ui->cursor_row + 0.5f) * asset->tile_size;
        ImGui::Text("格 (%d, %d)  坐标 (%.0f, %.0f)", ui->cursor_col, ui->cursor_row, world_x,
                    editor_world_y_to_display(ui, world_y));
        ImGui::SameLine();
        ImGui::TextDisabled(ui->view.y_from_bottom ? "（y 底部为 0）" : "（y 为世界坐标）");
    } else {
        ImGui::TextDisabled("格 (-, -)");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("地刺格 %u  可消失格 %u  门 %u  移动组件 %u  怪物 %u  传送门 %u  传送点 %u",
                editor_doc_tile_count(doc, LEVEL_TILE_SPIKE),
                editor_doc_tile_count(doc, LEVEL_TILE_VANISH),
                asset->connection_count,
                asset->mover_count, asset->monster_count, asset->portal_count,
                asset->waypoint_count);

    u32 spawn_count = editor_doc_spawn_count(doc);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (spawn_count == LEVEL_ASSET_REQUIRED_SPAWN_TILES) {
        ImGui::Text("出生点 %u", spawn_count);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "出生点 %u（应为 1）", spawn_count);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "改关卡会让旧存档与 test/baseline.txt 的断言失效");

    ImGui::End();
}

internal void editor_draw_side_windows(EditorUiState *ui)
{
    if (ui->save_as_visible) {
        ImGui::SetNextWindowSize(ImVec2(620.0f * ui->ui_scale, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("另存为", &ui->save_as_visible);
        ImGui::TextDisabled("相对进程工作目录（和游戏一样，从仓库根启动）。会自动建父目录。");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##path", ui->path_input, sizeof(ui->path_input));
        if (ImGui::Button("写入（原子 + 写完自读校验）")) {
            editor_do_save_as(ui);
        }
        ImGui::SameLine();
        if (ImGui::Button("取消")) {
            ui->save_as_visible = false;
        }
        ImGui::End();
    }

    if (ui->about_visible) {
        ImGui::SetNextWindowSize(ImVec2(560.0f * ui->ui_scale, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("快捷键与说明", &ui->about_visible);
        ImGui::BulletText("左键拖动：涂当前笔刷；右键拖动：擦成空；中键拖动：平移；滚轮：以鼠标为中心缩放");
        ImGui::BulletText("Ctrl + 左键：选中光标下的物件（从小往大找：出生点 → 怪物 → 平台 → 地刺）");
        ImGui::BulletText("选中之后可以拖它：拖内部 = 平移（附属的复位点 / 往返范围一起跟着走）；"
                          "拖边或角上的小白块 = 缩放");
        ImGui::BulletText("拖动默认按格吸附，按住 Alt = 自由像素；Esc = 取消选中（这样才能在它内部涂格子）");
        ImGui::BulletText("1 / 2 / 3 / 4 / 5：切换笔刷（5 = 地刺）；Delete：删除当前选中的物件；Ctrl + S：保存");
        ImGui::BulletText("Ctrl + Z：撤销；Ctrl + Y（或 Ctrl + Shift + Z）：重做 ——"
                          "最多 128 步，一次拖动 / 一次握住输入框只算一步");
        ImGui::BulletText("y 轴默认「底部为 0、向上为正」（视图菜单可切回原始世界 y）："
                          "只影响显示的数字，画布与关卡数据永远是世界坐标");
        ImGui::BulletText("G / C：切换网格 / 合并碰撞体叠加层；「视图 → 全览」回到整幅关卡");
        ImGui::Separator();
        ImGui::TextWrapped("出生点（'P'）是关卡的单例属性，全图只能有一个：涂第二个会自动把第一个搬走。"
                           "怪物运行时只支持 1 个、移动组件最多 8 个，多加了保存会被拒绝。");
        ImGui::TextWrapped("「检查」按钮才会编译（生成合并碰撞体与诊断）。校验是每帧实时的，因为它不编译、零分配。");
        ImGui::TextWrapped("「保存并试玩 / 保存并冒烟验证」会起一次 build\\main.exe —— 不是热重载："
                           "游戏启动时只读一次 .bin，改完必须重起。");
        ImGui::End();
    }
}

// ============================================================================
// 对外入口
// ============================================================================

void editor_ui_initialize(EditorUiState *ui)
{
    *ui = {};

    ui->view.scroll_x = 0.0f;
    ui->view.scroll_y = 0.0f;
    ui->view.zoom = 1.0f;
    ui->view.y_from_bottom = true; // 默认「底部为 0」，理由见文件上方那段注释
    ui->view.show_grid = true;
    ui->view.show_colliders = true;
    ui->view.show_diagnostic_box = true;
    ui->view.fit_requested = true;

    ui->brush = LEVEL_TILE_SOLID;
    ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
    ui->ui_scale = 1.0f;
    ui->cursor_col = -1;
    ui->cursor_row = -1;
}

void editor_ui_frame(EditorUiState *ui)
{
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 display = io.DisplaySize;
    f32 menu_h = ImGui::GetFrameHeight();
    f32 status_h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
    f32 left_w = 268.0f * ui->ui_scale;
    f32 right_w = 400.0f * ui->ui_scale;
    f32 middle_h = MAX(display.y - menu_h - status_h, 64.0f);

    // 快捷键（输入框有焦点时不要抢键）
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            ui->brush = LEVEL_TILE_SOLID;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
            ui->brush = LEVEL_TILE_ONE_WAY;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
            ui->brush = LEVEL_TILE_SPAWN;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) {
            ui->brush = LEVEL_TILE_EMPTY;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_5, false)) {
            ui->brush = LEVEL_TILE_SPIKE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_6, false)) {
            ui->brush = LEVEL_TILE_VANISH;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && editor_sel_is_entity(ui->selection.kind)) {
            u32 entity_kind = editor_sel_entity_kind(ui->selection.kind);
            editor_doc_remove_entity(&ui->doc, entity_kind, ui->selection.index);
            ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            // 取消选中 = 「又能在这块区域里涂格子了」
            ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && ui->doc.has_path) {
            editor_doc_save(&ui->doc, ui->doc.path, &ui->messages);
        }
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            // 拖动途中被按到也没关系：editor_doc_undo 先把手势收尾，不会把那一拖丢掉
            if (editor_doc_undo(&ui->doc, &ui->selection)) {
                ui->drag.active = false;
            }
        }
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
                           (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))) {
            if (editor_doc_redo(&ui->doc, &ui->selection)) {
                ui->drag.active = false;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            ui->view.show_grid = !ui->view.show_grid;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            ui->view.show_colliders = !ui->view.show_colliders;
        }
    }

    // 选中目标必须在合法范围内（界面自己只会写入合法值；这一条是廉价的不变量检查）
    if (ui->selection.kind >= EDITOR_SEL_KIND_COUNT) {
        ui->selection = EditorSelection{ EDITOR_SEL_NONE, 0 };
    }

    editor_draw_menu_bar(ui);
    editor_draw_side_windows(ui);

    // 左栏
    ImGui::SetNextWindowPos(ImVec2(0.0f, menu_h));
    ImGui::SetNextWindowSize(ImVec2(left_w, middle_h));
    ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("物件（笔刷 / 实体）", nullptr, panel_flags)) {
        editor_draw_brush_section(ui);
        editor_draw_entity_section(ui);

        ImGui::SeparatorText("消息");
        ImGui::BeginChild("##messages", ImVec2(0.0f, 120.0f * ui->ui_scale), true);
        u32 shown = MIN(ui->messages.count, (u32)EDITOR_MESSAGE_COUNT);
        u32 first = ui->messages.count - shown;
        for (u32 i = 0; i < shown; ++i) {
            ImGui::TextWrapped("%s", ui->messages.text[(first + i) % EDITOR_MESSAGE_COUNT]);
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // 中栏（画布）—— 放在右栏前面：诊断要用这一帧刚算出的光标格
    editor_draw_canvas_window(ui, ImVec2(left_w, menu_h),
                             ImVec2(MAX(display.x - left_w - right_w, 64.0f), middle_h));

    // 右栏
    ImGui::SetNextWindowPos(ImVec2(display.x - right_w, menu_h));
    ImGui::SetNextWindowSize(ImVec2(right_w, middle_h));
    if (ImGui::Begin("属性 / 检查", nullptr, panel_flags)) {
        editor_draw_properties(ui);
        editor_draw_check_section(ui);
    }
    ImGui::End();

    // 底栏
    editor_draw_status_bar(ui, ImVec2(0.0f, display.y - status_h), ImVec2(display.x, status_h));
}
