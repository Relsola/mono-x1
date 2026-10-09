#pragma once

#include "core.h"
#include "shared/level_asset.h"
#include "shared/mono_math.h" // v2：拖动记录里的起点端点
#include "editor_doc.h"

// ============================================================================
// 编辑语义（纯）：命中测试 / 拖动 / 吸附 —— 不含 ImGui，也不含窗口
//
// 为什么单独一层：这些是编辑器的**逻辑**，但原来住在 editor_ui.cc 里，参数带着
// EditorUiState / EditorCanvas / ImVec2 —— 于是只有"人在界面上点一下"才能验证它。
// 现在这一层的输入是「世界坐标 + 世界半径」（UI 负责把屏幕鼠标换算过来），
// 所以可以没有窗口、没有焦点地逐条断言（`build\editor.exe --selftest`）。
//
// 屏幕像素半径（手柄 6px、近邻 8px）是**界面**的策略：UI 除以 zoom 换算成世界半径再传进来。
// 这样这一层不必知道 zoom，也不必知道 ImVec2 是什么。
//
// 与 editor_doc 的分工：doc 层管"文档怎么变"（增删实体、改字段、置脏、校验、存盘），
// 这一层管"鼠标的意思"（点到了什么、这一拖要干什么、拖完落在哪）。
// ============================================================================

inline constexpr u32 EDITOR_EDGE_LEFT = 1;
inline constexpr u32 EDITOR_EDGE_RIGHT = 2;
inline constexpr u32 EDITOR_EDGE_BOTTOM = 4;
inline constexpr u32 EDITOR_EDGE_TOP = 8;

// 光标：世界坐标 + 两个世界半径
struct EditorPointer
{
    f32 x;
    f32 y;
    f32 handle_radius; // 矩形边/角手柄的命中范围（UI 传 6px / zoom）
    f32 near_radius;   // 「某个点附近」的命中范围，用于往返端点与怪物复位点（UI 传 8px / zoom）
};

// 拖动：按下的那一瞬间决定这一拖是干什么的（move / 缩放 / 拖附属点），
// 之后每帧用「当前世界坐标 - 按下时的世界坐标」算偏移 —— 用起点快照而不是逐帧累加，
// 所以不会漂移，也能正确处理吸附（吸附作用在**结果**上，不作用在增量上）。
enum EditorDragKind : u32
{
    EDITOR_DRAG_NONE,
    EDITOR_DRAG_MOVE,            // 整体平移（rect + 附属位置点）
    EDITOR_DRAG_RESIZE,          // 拖边 / 拖角
    EDITOR_DRAG_SPAWN,           // 出生点（关卡单例属性）
    EDITOR_DRAG_MONSTER_RESPAWN, // 怪物的复位点
    EDITOR_DRAG_WAYPOINT,        // 传送点（它**本身**就是一个点，没有矩形可拖）
    EDITOR_DRAG_RANGE_A,         // 移动组件的往返端点 A（二维：轴可以任意方向）
    EDITOR_DRAG_RANGE_B,
};

struct EditorDragState
{
    u32 kind;      // EditorDragKind
    u32 edge_mask; // 缩放用：1=左 2=右 4=下 8=上
    f32 grab_x;    // 按下时的世界坐标
    f32 grab_y;
    Rect2D start_rect;
    f32 start_spawn_x;
    f32 start_spawn_y;
    v2 start_point_a;
    v2 start_point_b;
    bool active;
};

// 起手的结果：拖动状态 + 需要替换成什么选择
// （点在出生点标记上时选择要变成"出生点"，而选择是界面状态，所以由调用方套用）
struct EditorDragBegin
{
    EditorDragState drag;
    EditorSelection selection;
    bool selection_changed;
};

f32 editor_edit_snap(f32 value, f32 step);
bool editor_edit_point_in_rect(const Rect2D *rect, f32 x, f32 y);
// 世界坐标 → 格号；不在网格里时 *col/*row 都是 -1（第 0 行在最上面，与 level.cc 一致）
void editor_edit_world_to_cell(const LevelAsset *asset, f32 world_x, f32 world_y, int *col, int *row);
bool editor_edit_selection_is(const EditorSelection *selection, u32 entity_kind, u32 index);

// 光标下的「实体体」：出生点标记 + 所有实体的矩形。优先级 = 出生点 → 实体种类倒序
// （小的画在上面就先被点到；门没有矩形，自动跳过）。没命中时返回 EDITOR_SEL_NONE
EditorSelection editor_edit_hit_body(const EditorDoc *doc, f32 world_x, f32 world_y);

// 选中实体的「附属抓手」：边/角手柄、移动组件的往返端点、怪物的复位点。
// 返回 EditorDragKind（EDITOR_DRAG_NONE = 没命中）；*edge_mask_out 只在 RESIZE 时有意义。
// 界面用它做悬停反馈（光标形状），拖动用它决定这一拖干什么 —— 同一份判据
u32 editor_edit_hit_grip(const EditorDoc *doc, const EditorSelection *selection, EditorPointer pointer,
                         u32 *edge_mask_out);

// 按下的那一刻决定这一拖干什么；返回的 drag.active == false 表示"这一下不是拖动"，
// 调用方照旧走涂格子 / Ctrl 选中。顺序与原实现一致：
// 出生点 → 往返端点 / 复位点 → 边角手柄 → 选中实体的矩形内部
EditorDragBegin editor_edit_begin_drag(const EditorDoc *doc, const EditorSelection *selection,
                                       EditorPointer pointer);

// 每帧应用拖动（写进文档并置脏）。返回 true = 这一帧内容变了
bool editor_edit_apply_drag(EditorDoc *doc, const EditorSelection *selection, const EditorDragState *drag, f32 world_x,
                            f32 world_y, f32 snap_step);

// 自证：在内存里造文档，跑一张固定用例表（`build\editor.exe --selftest`）。
// 用例全是纯数值断言（吸附、夹取、命中优先级、附属点跟随），不依赖关卡资产与窗口。
// 返回 true = 全部通过；每条结果同时写进 editor.log
bool editor_edit_run_selftest();
