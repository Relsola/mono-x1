#pragma once

#include "core.h"
#include "game.h"

// ============================================================================
// 存档系统
//
// 这是**正式系统**，不是调试设施：它不带任何 MONO_DEBUG_* 门控，将来就是游戏的
// 存盘点 / 手动存档 / 读档入口。现在先被调试用的「磁带」（录制回放与输入脚本）当底座用。
//
// 职责只有一件：**把一份游戏状态变成磁盘上的一个文件，再原样读回来**。
//   - 状态语义（哪些字段属于状态、枚举与 bool 是否合法）归 game_state_save/load（include/game.h）；
//     这里只管文件格式、完整性和「这份存档还配得上现在的关卡吗」。
//   - 路径即存档槽：调用方给路径。槽位编号、存档菜单、自动存档时机将来都长在这层之上，
//     不要塞进这里 —— 那些是策略，这一层只认「路径 → 字节」。
//
// 文件布局：SaveHeader + GameStateSnapshot（结构体字段直接按字节写，不做重排）。
// 三重校验，任何一条不过都拒载 —— 宁可报错，也不拿错位的字节去还原现场：
//   1. magic / version / payload_size：格式换代或快照结构改动（连编译器对齐变化都能拦下）；
//   2. level_fingerprint：关卡几何变了（改了 .bin 资产或合并规则）。存档里记的是
//      「那个关卡里的位置与实体状态」，拿它去解释新关卡会得到卡在墙里、掉出世界这类
//      无从排查的怪现象，所以直接拒载；
//   3. checksum（FNV-1a 32）：文件被截断或损坏。
//
// 读档会丢什么：只恢复 GameStateSnapshot 覆盖的部分。渲染插值的历史位置、动画当前帧、
// 音频播放相位这类「每次运行都会重新长出来」的东西不属于状态，因此不进存档 ——
// 这正是「合理重现世界、允许丢一点」的位置：丢的必须是**不影响后续演化的量**。
// ============================================================================

// 写档（自动创建缺失的父目录）
bool save_write(const wchar_t *path, const GameState *game_state);

// 读档：只读回快照，不碰 game_state。
// game_state 只用来取「当前关卡的指纹」做第 2 重校验；校验不过时 *out 保持原样并返回 false，
// 于是调用方可以「读档失败就沿用当前状态」。
bool save_read(const wchar_t *path, const GameState *game_state, GameStateSnapshot *out);

// 读档并恢复现场（save_read + game_state_load）：脚本、回放与命令行 `--load` 都走这一条
bool save_restore(const wchar_t *path, GameState *game_state);
