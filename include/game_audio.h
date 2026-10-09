#pragma once

#include "core.h"

// ============================================================================
// 游戏音频：素材解析与语义化播放
//
// 游戏逻辑只调用 game_audio_play_* 这类语义函数，不需要接触音频系统与资产细节。
// 触发点必须放在固定步长的 game_update 内，回放（include/debug/replay.h）才能重现同样的声音。
//
// 前向声明 AudioState：游戏逻辑不需要也不应该包含 xaudio2.h
// ============================================================================

struct AudioState;

// 解析全部音频素材并起播背景音乐（在 audio_create 返回非 nullptr 之后调用一次）
void game_audio_init(AudioState *audio);

// 拾取音效（键盘 E / 手柄 A）
void game_audio_play_coin();

// 冲刺音效（键盘 F / 手柄右肩）
void game_audio_play_dash();

// 背景音乐停止 / 重新起播（调试用）
void game_audio_toggle_bgm();
