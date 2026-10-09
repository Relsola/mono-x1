#pragma once

#include "core.h"
#include "shared/level_asset.h"
#include "editor_doc.h"
#include "editor_edit.h" // 拖动状态（EditorDragState）与纯编辑语义都在那一层

// ============================================================================
// 编辑器界面层（ImGui）
//
// 三栏 + 顶菜单 + 底状态栏：
//   左 = 地形笔刷 + 实体列表（值语义 vs 身份语义分开摆）
//   中 = 网格画布（ImDrawList 直画，不走游戏的渲染器）
//   右 = 关卡属性 / 选中物的属性 / 检查（校验问题 + 诊断）
//
// 界面只调 editor_doc 的函数改文档；属性面板是「结构体参数的直译」（见 .cc 里的三个 helper）。
// ============================================================================

// 画布视图：世界坐标平移 + 缩放。
// scroll_* 是画布左上角对应的世界坐标（世界 y 向上为正，所以画布顶部对应较大的 y）。
struct EditorView
{
    f32 scroll_x;
    f32 scroll_y;
    f32 zoom;              // 屏幕像素 / 世界像素
    bool y_from_bottom;    // y 的显示方式：true = 关卡底部为 0（默认），false = 原始世界 y
    bool show_grid;
    bool show_colliders;   // 叠加显示编译后的合并碰撞体（需要最新的诊断结果）
    bool show_diagnostic_box;
    bool fit_requested;    // 下一帧把整幅关卡缩放进视野
};

// 拖动：按下的那一瞬间决定这一拖是干什么的（move / 缩放 / 拖附属点），
// 之后每帧用「当前世界坐标 - 按下时的世界坐标」算偏移 —— 用起点快照而不是逐帧累加，
// 所以不会漂移，也能正确处理吸附（吸附作用在结果上，不作用在增量上）。
// 定义与实现都在 editor_edit.h/.cc（纯逻辑那一层）；界面只负责把鼠标喂给它。

struct EditorUiState
{
    EditorDoc doc;
    EditorView view;
    EditorSelection selection;
    EditorDragState drag;
    LevelTile brush;
    EditorMessages messages;
    EditorDiagnostics diagnostics;

    f32 ui_scale;      // DPI 缩放（150% 机器上界面的实际观感靠它）
    bool request_quit; // 菜单「退出」置位，主循环据此结束

    int cursor_col;    // 当前光标格（-1 = 鼠标不在网格上）
    int cursor_row;

    // 两个辅助窗口：用普通窗口而不是 modal（省掉 OpenPopup 的 ID 栈约束，够用）
    bool save_as_visible;
    bool about_visible;

    // 路径输入缓冲（UTF-8：ImGui 的文本控件的编码约定）
    char path_input[EDITOR_PATH_SIZE * 2];
};

void editor_ui_initialize(EditorUiState *ui);

// 画一帧界面。所有窗口都是绝对定位（不用 docking 分支），布局由 DisplaySize 算出。
void editor_ui_frame(EditorUiState *ui);
