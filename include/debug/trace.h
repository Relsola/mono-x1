#pragma once

#include "core.h"
#include "game.h"
#include "input.h"

// ============================================================================
// 逐逻辑步状态轨迹（CSV）
//
// 用途：把「游戏内部发生了什么」变成可离线分析的数值曲线。
// 人的用法：跑一段后把 csv 拖进表格软件看轨迹；
// 工具/AI 的用法：直接用脚本统计、对拍、定位「第 N 步为什么没起跳」。
//
// 由 MONO_DEBUG_BUILD 控制，关闭时 trace_step 是空宏。
//
// 列定义：
//   frame          逻辑步序号（60Hz，从 0 开始）
//   x, y           玩家中心世界坐标（像素）
//   vx, vy         速度（像素/秒）
//   state          IDLE / RUN / JUMP / FALL / DASH
//   grounded       1 = 有支撑
//   facing         1 = 朝右
//   coyote         土狼时间剩余（秒）
//   jump_buffer    跳跃缓冲剩余（秒）
//   air_jumps      二段跳剩余次数（落地恢复）
//   drop_through   下穿单向平台的忽略剩余（秒）
//   dash_timer     冲刺剩余（秒）
//   dash_cooldown  冲刺冷却剩余（秒）
//   cam_x, cam_y   相机中心（世界坐标）—— 夹取不越界、停下不漂移这类性质靠它事后验证
//   zoom           相机缩放（1 = 原比例）
//   held           本步按住的动作位掩码（十六进制）
//   pressed        本步按下沿位掩码
//   released       本步松开沿位掩码
//
//   held/pressed/released 的位序 = GameAction 枚举顺序：
//   bit0 LEFT, bit1 RIGHT, bit2 UP, bit3 DOWN, bit4 JUMP, bit5 DASH, bit6 SHOOT, bit7 TIME_STOP, bit8 COIN, bit9 CAMERA_ZOOM
// ============================================================================

#if MONO_DEBUG_BUILD

// 打开轨迹文件（覆盖写并写表头）；失败返回 false
bool trace_open(const wchar_t *path);
// 每个逻辑步（game_update 之后）调用一次
void trace_step(u32 frame, const GameInput *input, const GameState *state);
// 刷缓冲并关闭
void trace_close();

#else

#define trace_open(path)                (false)
#define trace_step(frame, input, state) ((void)0)
#define trace_close()                   ((void)0)

#endif
