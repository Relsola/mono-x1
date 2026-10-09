#pragma once

#include "core.h"
#include "shared/memory.h" // InputTape 里的 Array<T> 要完整类型
#include "game.h"
#include "input.h"

// ============================================================================
// 磁带（Tape）：录制 / 回放 / 文本脚本共用的唯一内存表示 + 执行引擎
//
// 由 MONO_DEBUG_INPUT 控制，门控是自包含的（宏关闭时下面全部展开为空，
// 所以包含它的编译单元不需要、也不应该在 #include 外面再套一层 #if）。
//
// 三种用法只是同一盘磁带的三种来源与两种终点：
//   录制（F5）       从真实设备记录**输入变化点**，F8 顺手把断言打进磁带；结束即导出脚本
//   回放（F6）       载入磁带（本次会话录的那盘，否则最近导出的脚本）→ 循环播放，看画面
//   脚本（input_script） 文本载入 → 与回放走同一条执行路径，只是跑完就退出并给出退出码
//
// 为什么这样就能稳定复现：磁带的第一条永远是**存档点**（磁盘上的 .sav，见 include/save.h），
// 执行之前先读档把世界恢复成录制当时的样子。于是「世界一模一样」由存档系统负责、
// 「输入一模一样」由磁带负责，两边可以各自独立演进 —— 这正是旧设计撑不住的地方：
// 当时快照是以结构体字节的 hex 内嵌在脚本文本里的，与编译器绑定，也没有版本与关卡校验。
//
// 为什么不再逐帧存输入：磁带只记**变化点**（连续相同的帧折叠掉），内存与文本形式因此
// 是同一量级；文本脚本的 press/release 也只是「在某帧改了一位」。
//
// 时间轴：磁带帧号从 0 开始，是**相对时间轴**（与装配层的 logic_step 无关），
// 所以「什么时候按 F6」不影响输入与断言的帧号。
// ============================================================================

// 只前向声明：这一层只需要它们的地址，不需要完整类型（同 audio.h 的做法）
struct GameState;
struct GameInput;

#if MONO_DEBUG_INPUT

// 一帧的完整输入快照。不含边沿：边沿是「上一帧到这一帧的变化」，由引擎在喂给游戏时算。
//
// 它是 PlayerInput / MouseInput 的一份**子集**（只留 current 与轴值）：给输入结构加字段时，
// 这里与 src/replay.cc 的 tape_capture_input / tape_input_equal / tape_apply_input 要一起改
// —— 漏了不会报错，表现是那个输入在录制/回放里永远是默认值
// （确定性守卫也拓不出来：两遍跑漏得一模一样）。同类坑见 AGENTS「加一个动作要改四处」。
struct TapeInputState
{
    bool is_pad;
    bool current[GA_COUNT];
    f32 left_stick_x;
    f32 left_stick_y;
    f32 right_stick_x;
    f32 right_stick_y;
    f32 left_trigger;
    f32 right_trigger;
    bool mouse_buttons[MOUSE_BUTTON_COUNT];
    f32 mouse_x;
    f32 mouse_y;
    f32 mouse_wheel_delta;
};

// 变化点：从 frame 这一逻辑步起输入状态变成 state，一直保持到下一个变化点
struct TapeFrame
{
    u32 frame;
    TapeInputState state;
};

// 磁带上的操作，全部发生在「该逻辑步的 game_update 之后」：
// 断言要看这一步的结果，控制指令（探针 / 写存档 / 打点）也以这一步为准。
enum TapeOpKind : u8
{
    TAPE_ASSERT_POS,                 // a = x, b = y, c = 容差
    TAPE_ASSERT_STATE,               // a = PlayerState
    TAPE_ASSERT_GROUNDED,            // a = 0/1
    TAPE_ASSERT_WORLD,               // a = WorldId
    TAPE_ASSERT_TRANSITION,          // a = 0/1
    TAPE_ASSERT_TIME_STOP,           // a = 0/1
    TAPE_ASSERT_MONSTER,             // a = 0/1
    TAPE_ASSERT_MONSTER_TIME_SLOWED, // a = 0/1
    TAPE_PROBE_MONSTER_RESPAWNS,     // 记下当前刷新次数作为基准
    TAPE_ASSERT_MONSTER_RESPAWNS,    // a = 自基准以来的新增次数
    TAPE_ASSERT_MONSTER_RESPAWNS_TOTAL,
    TAPE_ASSERT_PROJECTILES, // a = 飞行中的能量波数量
    TAPE_ASSERT_AIR_JUMPS,   // a = 二段跳剩余次数
    TAPE_PROBE_RESET,        // 位移探针归零（断言标尺）
    TAPE_ASSERT_RISE,        // a = 最小上升高度, c = 容差
    TAPE_ASSERT_RUN_X,       // a = 最小水平位移, c = 容差
    TAPE_SAVE_STATE,         // path = 写档路径
    TAPE_LOAD_STATE,         // path = 读档路径：把世界恢复到那一刻（输入继续由磁带驱动）
    TAPE_LOG_STATE,          // 打一行状态（不计入断言）
};

struct TapeOp
{
    u32 frame;
    // 来自脚本的行号（从 1 起）；0 = 没有来源行（F8 标记 / 录制产物）。
    // 它是「FAIL 行能不能指回脚本文本」的全部依据 —— 没有它就只能靠用户自己数帧号
    u32 source_line;
    TapeOpKind kind;
    f32 a;
    f32 b;
    f32 c;
    const char *path; // 仅 SAVE_STATE / LOAD_STATE 用（UTF-8，内存由 arena 常驻）
};

// 磁带里的定长缓冲容量。写成常量而不是成员数组上的 array_size：
// 通过指针取成员不是常量表达式，consteval 的 array_size 在那里用不了
inline constexpr u32 SAVE_PATH_SIZE = 260;

struct InputTape
{
    // 存档点：开始这盘磁带之前先读档，把世界恢复到这里
    bool has_save_path;             // false = 不读档，从关卡出生点开始（脚本的 `save spawn`）
    char save_path[SAVE_PATH_SIZE]; // UTF-8：它主要来自脚本文本，也要原样写回文本

    bool loop;        // 回放（F6）= true（播完重读档再播）；脚本 = false（播完退出）
    u32 total_frames; // 磁带长度（录制结束时确定 / 脚本按最后一个帧号推出）

    Array<TapeFrame> frames; // 输入变化点：帧号非递减
    Array<TapeOp> ops;       // 操作：帧号升序
};

// ----------------------------------------------------------------------------
// 磁带构造（文本前端与引擎共用；纯数据操作，不碰引擎状态）
//
// 这几个入口的 #else 分支里没有空宏：它们只在同样被 MONO_DEBUG_INPUT 包住的 .cc 里使用，
// 装配层（main.cc）看不到 InputTape。带空宏的是下面那些「装配层会直接调」的入口。
// ----------------------------------------------------------------------------

InputTape tape_create(bool loop);
void tape_set_save_path(InputTape *tape, const char *utf8_path);
void tape_set_save_spawn(InputTape *tape);
void tape_push_frame(InputTape *tape, u32 frame, const TapeInputState *state);
void tape_add_op(InputTape *tape, u32 frame, TapeOpKind kind, f32 a, f32 b, f32 c, const char *path);
// 之后 tape_add_op 加进来的操作都标注成这个行号（0 = 不标注）。
// 由解析器设置：调用点近二十处，而「哪一行文本产生哪个操作」只有解析器知道
void tape_set_op_source_line(u32 source_line);
void tape_set_total_frames(InputTape *tape, u32 total_frames);
// 两个输入快照是否完全相同（录制靠它把连续相同的帧折叠成变化点）
bool tape_input_equal(const TapeInputState *a, const TapeInputState *b);

// ----------------------------------------------------------------------------
// 录制（F5）
// ----------------------------------------------------------------------------

bool replay_is_recording();
// 开始录制：先把存档点写盘（磁带的第一条），再清空磁带
void replay_record_start(const GameState *game_state, const GameInput *input);
// 每个逻辑步记一帧（在 input_step 之后、game_update 之前调用）
void replay_record_input(const GameInput *input);
// 结束录制：定下磁带长度并立即导出成本文脚本
void replay_record_stop();
// F8：把「最后已完成的那一步」的状态写成断言打进磁带（录制中与回放中都可用）
void replay_mark_assert(const GameState *game_state);

// ----------------------------------------------------------------------------
// 执行（回放 F6 与脚本 input_script 共用同一条路径）
// ----------------------------------------------------------------------------

bool replay_is_playing();
// 开始执行一盘磁带：按它的存档点读档，清空输入边沿后从第 0 帧开始
bool replay_start_tape(const InputTape *tape, GameState *game_state, GameInput *input);
// 回放：本次会话录过的磁带；没有就去载入最近导出的脚本（跨会话回放的那条路）
bool replay_start_last(GameState *game_state, GameInput *input);
// 每个逻辑步、game_update 之前：喂这一帧的输入
void replay_before_step(GameInput *input);
// 每个逻辑步、game_update 之后：求值本帧的操作；返回 true = 磁带跑完了且不循环（脚本该退出）
bool replay_after_step(GameState *game_state, GameInput *input);
void replay_end();

// ----------------------------------------------------------------------------
// 导出与收尾
// ----------------------------------------------------------------------------

// F4：把当前磁带写成文本脚本（录制结束时也会自动调一次）
bool replay_export_script();
// 脚本模式的退出码：有断言失败即 1（与旧版一致，用于自动化）
int replay_exit_code();
// 打一行状态（F7 与脚本的 log_state 操作共用）
void replay_log_state(u32 frame, const GameState *state);

#else

// 每个入口的空实现：名字与函数同名，所以调用点不需要写 #if
#define replay_is_recording()                      (false)
#define replay_is_playing()                        (false)
#define replay_record_start(game_state, input)     ((void)0)
#define replay_record_input(input)                 ((void)0)
#define replay_record_stop()                       ((void)0)
#define replay_mark_assert(game_state)             ((void)0)
#define replay_start_tape(tape, game_state, input) (false)
#define replay_start_last(game_state, input)       (false)
#define replay_before_step(input)                  ((void)0)
#define replay_after_step(game_state, input)       (false)
#define replay_end()                               ((void)0)
#define replay_export_script()                     (false)
#define replay_exit_code()                         (0)
#define replay_log_state(frame, state)             ((void)0)

#endif
