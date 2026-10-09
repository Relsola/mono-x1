#pragma once

#include "core.h"
#include "game.h"
#include "input.h"
#include "debug/replay.h" // 文本的产物就是磁带（InputTape / TapeOpKind）

// ============================================================================
// 输入脚本（文本形式）：磁带 ⇄ 文本
//
// 由 MONO_DEBUG_INPUT 控制，门控自包含（宏关闭时下面全部展开为空）。
//
// 这个模块只做一件事：**文本的读写**。输入怎么喂给游戏、断言怎么求值，全在
// include/debug/replay.h 的引擎里 —— 脚本、回放、录制共用同一条执行路径，所以
// 「录一段 → 导出成脚本 → 当回归用例跑」之间不会出现语义漂移。
//
// 脚本格式（帧号 = 磁带逻辑步，60Hz，从 0 开始）：
//   * press / release 的帧号**必须非递减**（输入变化点是按顺序追加的）
//   * 其它语句的先后不影响结果：tape_add_op 会把操作插到时间轴的正确位置，
//     所以同一帧的断言、`save_state`、`probe_reset` 写成什么顺序都是等价的
//   * 语句帧号允许乱序，但「同一帧里的多个操作」在文本里的先后就是它们的求值顺序
//   # 注释                                  '#' 之后到行尾都忽略
//   save build/checkpoint.sav               必须是第一条非注释语句：先读这个存档再跑
//   save spawn                              同上，但不读档，从关卡出生点开始
//   12    press   JUMP                      该逻辑步 game_update 之前按下
//   18    release JUMP
//   20    mouse_move 1920 1200              鼠标位置（客户区像素；UI 的悬停/命中靠它）
//   30    mouse_press left                  鼠标按钮：left / middle / right（缺省 left）
//   32    mouse_release left
//   30    assert_pos  1618.1  -961.3  2     game_update 之后校验（容差可省，默认 2）
//   30    assert_state RUN                  动作状态：IDLE / RUN / JUMP / FALL / DASH
//   30    assert_grounded 1                 是否有支撑
//   30    assert_world SECOND               当前世界：FIRST / SECOND
//   30    assert_transition 0               是否处于门过渡（锁定输入、无敌）
//   30    assert_time_stop 1                伪时停范围是否激活
//   30    assert_monster 1                  怪物是否存活
//   30    assert_monster_time_slowed 1      怪物是否被伪时停覆盖
//   30    probe_monster_respawns            记下刷新次数作为基准
//   40    assert_monster_respawns 1          自基准以来的新增刷新次数
//   40    assert_monster_respawns_total 3   累计刷新次数
//   40    assert_projectiles 1               飞行中的能量波数量
//   40    assert_air_jumps 1                 二段跳剩余次数
//   40    probe_reset                       位移探针基准（断言标尺）
//   55    assert_rise 220 8                 探针区间内最大上升高度 ≥ 220 - 8
//   55    assert_run_x 1300 30              探针区间内最大水平位移 ≥ 1300 - 30
//   40    save_state build/checkpoint.sav    在该帧写一份存档（造中途起点用例用）
//   40    log_state                         打一行状态（观察用，不计入断言）
//
// **第一条非注释语句必须是 `save`**：磁带先要回答「世界从哪来」，才谈得上复现。
// 脚本跑到最后一个帧号之后自动结束，退出码：全部断言通过 = 0，否则 = 1。
// ============================================================================

#if MONO_DEBUG_INPUT

// 文本 → 磁带（不执行）。F6 载入最近导出的脚本回放时也用它
bool input_script_load_tape(const wchar_t *path, InputTape *out);

// 文本 → 磁带 → 开始执行（input_script 的入口；跑完由引擎通知主循环退出）
bool input_script_run(const wchar_t *path, GameState *game_state, GameInput *input);

// 磁带 → 文本（F4 导出 / 录制结束写盘）
bool input_script_write(const InputTape *tape, const wchar_t *path);

#else

// 每个入口的空实现：名字与函数同名，所以调用点不需要写 #if
#define input_script_load_tape(path, out)         (false)
#define input_script_run(path, game_state, input) (false)
#define input_script_write(tape, path)            (false)

#endif
