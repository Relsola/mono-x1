#pragma once

#include "core.h"
#include "shared/memory.h"
#include "shared/level_asset.h"
#include "shared/level.h" // 诊断要把资产编译一遍，所以需要 Level（编辑器不链 game.cc，也不认识 GameState）

// ============================================================================
// 编辑器文档层
//
// 职责：持有 LevelAsset、增删改实体与 tile、打开/保存、校验、按需诊断、撤销/重做。
// 界面层（editor_ui）只调这里的函数，不直接改 asset —— 所以「谁改了文档」
// 只有一个入口，撤销栈就挂在这一个入口上（见下面「撤销」一节）。
//
// 与游戏的关系：只共享 include/shared/level_asset.h（格式）与 src/level*.cc / collision.cc
// （编译与几何），不认识 GameState / 输入 / 渲染。
// ============================================================================

inline constexpr u32 EDITOR_PATH_SIZE = 260;
inline constexpr u32 EDITOR_MESSAGE_COUNT = 8;
inline constexpr u32 EDITOR_MESSAGE_SIZE = 224;
inline constexpr u32 EDITOR_DIAGNOSTIC_LINE_COUNT = 24;
inline constexpr u32 EDITOR_DIAGNOSTIC_LINE_SIZE = 192;
inline constexpr u32 EDITOR_LEVEL_LIST_MAX = 16;
inline constexpr u32 EDITOR_LEVEL_NAME_SIZE = 64;

// 诊断用的哨兵：这一格不属于任何合并碰撞体
inline constexpr u32 EDITOR_NO_PLATFORM = 0xFFFFFFFFu;

// 有身份的物件（数组项）＝ `LevelAssetEntityKind`，定义在共享的 include/shared/level_asset.h 里，
// 连同它的注册表（名字 / 数组位置 / 元素大小 / 有没有矩形 / 数量上限）。
// 编辑器**不再自己维护一份并行枚举** —— 那是「加一种实体忘了改」的经典来源。
// 没身份的东西（墙、单向平台、出生点）是 tile，不在那儿。

// 选中的目标。前两个不是实体：出生点是**关卡的单例属性**（只是寄居在网格里）。
enum EditorSelectionKind : u32
{
    EDITOR_SEL_NONE = 0,
    EDITOR_SEL_SPAWN = 1,
    EDITOR_SEL_ENTITY_BASE = 2, // 实体选择 = EDITOR_SEL_ENTITY_BASE + LevelAssetEntityKind
};

// 两个枚举类型直接相加在 C++20 里是「已弃用」（C5054），所以显式转成 u32
inline constexpr u32 EDITOR_SEL_KIND_COUNT = (u32)EDITOR_SEL_ENTITY_BASE + (u32)LEVEL_ASSET_ENTITY_KIND_COUNT;

constexpr bool editor_sel_is_entity(u32 selection_kind) { return selection_kind >= EDITOR_SEL_ENTITY_BASE; }
constexpr u32 editor_sel_entity_kind(u32 selection_kind) { return selection_kind - EDITOR_SEL_ENTITY_BASE; }
constexpr u32 editor_sel_of_entity(u32 entity_kind) { return EDITOR_SEL_ENTITY_BASE + entity_kind; }

struct EditorSelection
{
    u32 kind;
    u32 index;
};

// ============================================================================
// 撤销：定长快照环
//
// 形态是**快照**而不是命令对象 / 增量 diff：整份文档才 ~1.7KB（tiles + 实体数组），
// 拷贝比维护「每条命令的逆操作」便宜得多，也没有「逆操作写错了」这个错误来源。
// （工业上的两种主流：Qt 的 QUndoStack 用命令对象、Unity 的 Undo.RecordObject 用临时拷贝；
// 文档小、操作简单时快照胜出。）
//
// 一格的语义是「第 k 次提交之后的文档」，当前状态就是 pos 那格 —— 所以撤销 =
// pos 往回走一格、把内容写回现场，重做 = 走回来。
//
// 内存：EDITOR_UNDO_STATES 格 × （tiles + 各类实体预留容量）≈ 700KB，**一次分配、之后永不分配**
// （arena 不能 free，所以宁可一次留足：实时增长会随着历史深度不断吃掉 arena）。
//
// 两条规则：
//   · **一次改动 = 一格**。连续手势（拖动、按住 DragFloat）用 action_begin/end 包起来，
//     期间不落格，松手时落一次 —— 否则一次拖动会产生几百格历史。
//   · **内容没变就不落格**（拿 level_asset_equal 与当前格比）。所以「打开菜单又关掉」
//     「输入框里改了又改回去」不会污染历史，也不会白白截断重做尾巴。
// ============================================================================

inline constexpr u32 EDITOR_UNDO_LAYERS = 128;                    // 可回退步数
inline constexpr u32 EDITOR_UNDO_STATES = EDITOR_UNDO_LAYERS + 1; // 每格 = 一次提交后的状态（当前状态也占一格）
inline constexpr u32 EDITOR_UNDO_ENTITY_CAPACITY = 32;            // 每类实体每格预留几个（不够就重建环）

struct EditorUndoSlot
{
    LevelAsset view; // 指针指向 storage；view 里的数量就是这一格的状态
    u8 *storage;     // 整块：tiles 之后按注册表顺序紧跟各类实体数组
};

struct EditorUndoRing
{
    EditorUndoSlot slots[EDITOR_UNDO_STATES];
    EditorUndoSlot saved; // 「上次打开/保存」的基准（dirty 就是拿它比出来的，所以它不能住在会绕回的环里）
    u32 capacity[LEVEL_ASSET_ENTITY_KIND_COUNT]; // 每类实体每格留了几个
    u32 storage_bytes;
    u32 pos;          // 当前状态所在的格
    u32 undo_count;   // 可回退步数（<= EDITOR_UNDO_LAYERS）
    u32 redo_count;   // 可重做步数
    u32 action_depth; // > 0 = 手势进行中（期间只置脏，不落格）
    bool enabled;     // 网格不成立时为 false（那时一切撤销入口都不响应）
    bool has_saved;   // 基准存不存在（新建/打开/保存之后都有）
};

struct EditorDoc
{
    LevelAsset asset;

    wchar_t path[EDITOR_PATH_SIZE];
    bool has_path;
    // 「与上次打开/保存的那份比，内容变没变」。是**推导**出来的，不是自己置位的标志：
    // 所以撤销回到保存点时会自己变回 false（见 editor_doc_refresh_dirty）
    bool dirty;

    // 每次改动 +1。用它判断诊断结果是否过期（诊断不必认识文档内部结构）
    u32 revision;

    EditorUndoRing undo;
};

// 消息区：编辑器界面上滚动的那几行「刚才发生了什么」。
// 与 logger.cc 的关系：两者都留下痕迹，但受众不同 —— 这里给人（实时、会滚掉），
// logger 给事后（`editor.log`，可 grep）。写自己的文件而不是 game.log：游戏每次启动都会
// 清空 game.log，而编辑器随时会起游戏（冒烟验证），共用一个文件会互相抹掉（实测踩到过）。
struct EditorMessages
{
    char text[EDITOR_MESSAGE_COUNT][EDITOR_MESSAGE_SIZE];
    u32 count; // 累计条数（环形覆盖最旧的）
};

void editor_message(EditorMessages *messages, const char *fmt, ...);

// 诊断：**按钮触发**的只读快照。它需要一份编译结果（成本见 .cc 里的注释），
// 所以不跟着文档每帧重算；revision 用来显示「结果已过期」。
struct EditorDiagnostics
{
    bool valid;
    u32 revision;
    bool y_from_bottom; // 生成这份文字时用的 y 轴显示方式（见 editor_ui.cc 的说明）

    Level level;                  // level_build_from_asset 的产物（arena 常驻）
    u32 *cell_platform;           // cols*rows：格 → Platform 索引（EDITOR_NO_PLATFORM = 无）
    u32 cell_columns;
    u32 cell_rows;

    char lines[EDITOR_DIAGNOSTIC_LINE_COUNT][EDITOR_DIAGNOSTIC_LINE_SIZE];
    u32 line_count;
};

// ---------------------------------------------------------------------------
// 文档生命周期
// ---------------------------------------------------------------------------

// 空关卡模板：全空网格 + 底部两行地面 + 一个出生点（避免"新建就通不过校验"）
void editor_doc_new(EditorDoc *doc, u32 columns, u32 rows, f32 tile_size);

bool editor_doc_open(EditorDoc *doc, const wchar_t *path);
bool editor_doc_reload(EditorDoc *doc);

// 保存：先校验（有 ERROR 就不写）、再 level_asset_save（原子写 + 写完自读比对）
bool editor_doc_save(EditorDoc *doc, const wchar_t *path, EditorMessages *messages);

// ---------------------------------------------------------------------------
// 撤销 / 重做
// ---------------------------------------------------------------------------

bool editor_doc_can_undo(const EditorDoc *doc);
bool editor_doc_can_redo(const EditorDoc *doc);

// 撤销 / 重做一步。选择里的编号可能随之失效，所以顺手夹好（界面不用自己防越界）。
// 注意：**不**自动重跑诊断 —— 诊断是按钮触发、每次都会分配，撤销几百次会白吃掉 arena；
// 右上角仍会显示「结果已过期」，和任何其它改动一样。
bool editor_doc_undo(EditorDoc *doc, EditorSelection *selection);
bool editor_doc_redo(EditorDoc *doc, EditorSelection *selection);

// 连续手势：按下时 begin、松手时 end，期间的所有改动只落一格。
// 可以嵌套（用深度计数），所以画布拖动与属性面板控件即使有重叠也不会错开。
void editor_doc_action_begin(EditorDoc *doc);
void editor_doc_action_end(EditorDoc *doc);

// ---------------------------------------------------------------------------
// 自检（`--selftest`）
//
// editor_edit（编辑语义）与 editor_doc（文档与撤销）两份用例集共用这一份计数与输出格式。
// 放在这一层：editor_edit 依赖 editor_doc，反向不行。
// ---------------------------------------------------------------------------

void editor_selftest_begin(void);
void editor_selftest_check(bool ok, const char *fmt, ...);
u32 editor_selftest_failed(void);

// 在内存里造文档跑撤销的用例表（不读盘、不开窗口）。返回失败条数。
u32 editor_doc_run_selftest(void);

// ---------------------------------------------------------------------------
// tile 与出生点
// ---------------------------------------------------------------------------

u32 editor_doc_spawn_count(const EditorDoc *doc);
// 数一种 tile 的格数（地刺格、出生点都用它）
u32 editor_doc_tile_count(const EditorDoc *doc, LevelTile tile);
bool editor_doc_find_spawn(const EditorDoc *doc, u32 *col, u32 *row);
LevelTile editor_doc_tile(const EditorDoc *doc, u32 col, u32 row);

// 写入一个格子。tile == LEVEL_TILE_SPAWN 时走 set_spawn（保证唯一），越界忽略。
void editor_doc_set_tile(EditorDoc *doc, u32 col, u32 row, LevelTile tile);

// 把唯一的出生点搬到 (col, row)：先清掉网格里所有 P，再放下新的
void editor_doc_set_spawn(EditorDoc *doc, u32 col, u32 row);

void editor_doc_fill_tiles(EditorDoc *doc, LevelTile tile);
void editor_doc_clear_entities(EditorDoc *doc);

// ---------------------------------------------------------------------------
// 实体
// ---------------------------------------------------------------------------

u32 editor_doc_entity_count(const EditorDoc *doc, u32 kind);

// 新增一个实体（默认值放在关卡中央，返回新索引；kind 非法时返回 EDITOR_NO_PLATFORM）
u32 editor_doc_add_entity(EditorDoc *doc, u32 kind);
bool editor_doc_remove_entity(EditorDoc *doc, u32 kind, u32 index);

// 类型化访问器：属性面板是「结构体参数的直译」，所以这里不做 void* 通用化 ——
// 加一个字段就是加一行控件。它们内部都走共享注册表，不是 4 个手写 switch。
LevelConnectionAsset *editor_doc_connection(EditorDoc *doc, u32 index);
LevelMoverAsset *editor_doc_mover(EditorDoc *doc, u32 index);
LevelMonsterAsset *editor_doc_monster(EditorDoc *doc, u32 index);
// 传送门 / 传送点：与其它实体一样是数组元素，直接给出可写指针（越界返回 nullptr）
LevelPortalAsset *editor_doc_portal(EditorDoc *doc, u32 index);
LevelWaypointAsset *editor_doc_waypoint(EditorDoc *doc, u32 index);

// 可写矩形（拖动用）：entity_kind 越界或这种实体没有矩形时返回 nullptr；
// 拿到指针就意味着「可以改」，改完记得 editor_doc_mark_changed()。
Rect2D *editor_doc_entity_rect_mut(EditorDoc *doc, u32 entity_kind, u32 index);

// 界面**直接**改了某个字段之后必须调它：置脏 + 让诊断结果过期。
// 走文档层函数（set_tile / add_entity / …）的改动不需要自己调，那些函数内部已经置脏了。
void editor_doc_mark_changed(EditorDoc *doc);

// ---------------------------------------------------------------------------
// 校验与诊断
// ---------------------------------------------------------------------------

// 这份资产自己的世界 ID：由文件名推出（first.bin = 0 / second.bin = 1），其它名字返回 -1。
// 世界 ID 是**位置决定**的，光看数字会懵，所以编辑器把它显示出来。
int editor_doc_world_id(const EditorDoc *doc);

// 语义校验（纯资产、零分配、不编译）→ 所以界面可以每帧调它做实时提示。
// save 时也调同一个函数，规则只有一份。
void editor_doc_validate(const EditorDoc *doc, LevelAssetIssues *issues);

// 编译 + 生成诊断文本。cursor_col/row 传 -1 表示没有光标格。
// y_from_bottom：y 的显示方式（true = 关卡底部为 0），文本会被逐字写成对应的坐标。
void editor_doc_run_diagnostics(EditorDoc *doc, EditorDiagnostics *diag, const EditorSelection *selection,
                                int cursor_col, int cursor_row, bool y_from_bottom);

// data/map/*.bin 列表（按名字排序，给「打开」菜单用）；返回写入的条数
u32 editor_list_levels(char names[][EDITOR_LEVEL_NAME_SIZE], u32 max_count);

// ---------------------------------------------------------------------------
// 一键闭环：保存之后让**主程序**去验证这张图
//
// 起的是 build\main.exe（编辑器与游戏的可执行文件同目录），子进程继承本进程的
// 工作目录 —— 所以编辑器必须从仓库根启动（与游戏一样，路径按工作目录解析）。
//   arguments：传给 main.exe 的参数（nullptr = 不带参数，即手动游玩）
//   wait：true 时等它跑完，并把退出码写回 exit_code（0 = 全部断言通过）
// 返回 false = 起不来（比如还没编译出 build\main.exe）。
// 注意：**不是热重载** —— 游戏启动时读一次 .bin，改完必须重起（理由见 README）。
bool editor_run_game(const wchar_t *arguments, bool wait, u32 *exit_code);

// ---------------------------------------------------------------------------
// 无界面自检（--check）：**不建窗口、不初始化 D3D/ImGui** 的一次性检查
//
// 回答的是「这批关卡资产还好吗」：加载 → 语义校验 → 编译诊断（可选再让主程序跑一遍冒烟）。
// 它用的是与「检查」面板同一份校验与编译代码，所以不会出现两套语义。
//
//   map_path：nullptr = 检查 data/map/ 下全部 .bin
//   smoke：检查完再跑一次 `build\main.exe --fast input_script test/smoke.txt`
//          （游戏固定从第一世界出发，所以它只回答「第一世界能不能跑」，见 README）
//   report_path：文本报告写到哪里（UTF-8）
// 返回 true = 每张图都是 0 error（且请求的冒烟通过）；报告里逐条列了原因。
// 同时会往父控制台回显一份（有的话）—— 人从终端跑时不用再去开报告文件。
bool editor_check_run(const wchar_t *map_path, bool smoke, const wchar_t *report_path);
