#pragma once

#include "core.h"
#include "input.h"

// ============================================================================
// UI 层（现在只有「大地图」一屏）
//
// 四条约定：
//   1. **自研、不引 ImGui 到游戏运行时**：UI 逻辑跑在固定步长里，所以输入脚本/回放能驱动它 ——
//      「按 ESC 开图 → 方向键选项 → 确认传送」整条链路都能写成自动化用例；代价是文本、滚动、
//      输入框这些都要自己写（文字要等阶段 4 的点阵字体）。
//   2. **打开 = 世界完全暂停**：game_update 在处理完 UI 之后直接 return（不吃输入、不走物理、不动动画）。
//      地图界面的惯例就是暂停，而且这对确定性最省心 —— 只要把 UI 状态放进快照就够了。
//   3. 绘制只上报**纯数据**（屏幕空间矩形 + 语义标签），翻译成绘制项在装配层 main.cc ——
//      与 debug_vis 同一套分工：UI 层不认识渲染层，渲染层也不认识 UI。
//   4. 贴图与颜色都不在这一层决定：装配层按 kind 查表（与 DEBUG_BOX_COLORS 同一套做法）。
// ============================================================================

// 候选项数量上限。候选项 = **所有世界的传送点**（`LEVEL_ASSET_MAX_WAYPOINTS * WORLD_COUNT` 个，
// game.h 里有 static_assert 把这两个数字钉在一起），所以上限必须跟得上那个总量。
inline constexpr u32 UI_MAX_ITEMS = 8;

// 只要指针，所以不需要 game.h（game.h 反过来要包含本文件才能把 UiState 放进 GameState）
struct GameState;

// 一帧最多产出多少个矩形。现在里面含**文字**（每个字形一个矩形）：
//   1 个压暗 + 每项 1 底板 + 1 边框 + 标签（最多 ~12 个字形）+ 底部提示（~44 个字形）。
// 上限 8 项时算下来 ~157，取 192 留点余量。超了不会崩（ui_collect_rects 写满就停），
// 但文字会被截断 —— 所以标签字符串别写长。
inline constexpr u32 UI_MAX_RECTS = 192;

// UI 状态：跨帧会变、且影响后续演化（暂停世界、决定传送到哪），所以进存档快照。
struct UiState
{
    bool open = false; // 大地图是否打开（打开时整个世界暂停）
    u32 selected = 0;  // 选中项下标（开图时对齐到当前世界）
};

// UI 一帧要画的一个矩形：**屏幕空间，左上角 + 尺寸**，单位是客户区像素。
// 用左上角而不是中心：这是界面布局的自然表达，也正好是渲染层屏幕空间入口的入参。
enum UiRectKind : u8
{
    UI_RECT_DIM,              // 全屏压暗（表示世界已暂停）
    UI_RECT_ITEM,             // 一个候选项的底板
    UI_RECT_ITEM_SELECTED,    // 选中项的底板（与未选中的区别只在这一层语义上）
    UI_RECT_SELECTION_BORDER, // 选中项的边框（线框）
    UI_RECT_TEXT,             // 一个字形（选中项的文字，亮）
    UI_RECT_TEXT_DIM,         // 一个字形（未选中项与提示的文字，暗）
    UI_RECT_KIND_COUNT
};

struct UiRect
{
    f32 x;
    f32 y;
    f32 w;
    f32 h;
    UiRectKind kind;
    // 只有文字矩形有意义；给个默认值是因为绝大多数矩形（压暗/底板/边框）不写字 ——
    // 否则每个聚合初始化都得补一个 0（clang 的 -Wmissing-field-initializers 会报）
    u32 glyph = 0;
};

// 推进 UI 状态（在固定步长里、**先于**游戏逻辑调用）。
// 键位/鼠标：GA_MAP 开关（同一个键做双层语义：游戏内开图、图内关图）、上下左右选择、
// GA_JUMP 确认；鼠标**悬停即选项**（与方向键同一个含义，停在空白处不动选中项）、左键落在某项上 = 确认。
// 返回 true = 这一逻辑步的游戏逻辑应当跳过。除了「图还开着」之外，**刚开/刚关的那一步也跳过**：
// 开关用的键（GA_MAP / GA_JUMP）如果同一步漏给角色，就会多跳一下或多触发一次别的动作。
bool ui_update(GameState *game_state, const GameInput *input, u32 viewport_width, u32 viewport_height);

// 收集本帧要画的矩形（UI 关闭时不产出任何东西）。返回写入 out 的个数。
u32 ui_collect_rects(const GameState *game_state, u32 client_width, u32 client_height,
                     UiRect *out, u32 capacity);
