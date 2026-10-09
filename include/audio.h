#pragma once

#include "core.h"
#include "shared/mono_math.h" // INV_SQRT_2（默认立体声增益）

// ============================================================================
// 音频系统 XAudio2
//
// 本头不含任何 XAudio2 / COM / Win32 类型：AudioState 只有前向声明，
// 调用方拿到的是指针，内部布局与那些平台类型全锁在 src/audio.cc 里。
// 因此 audio.h 可以安全地在任何编译单元包含，将来跨平台也只需替换实现文件。
//
// 线程模型：
//   游戏线程  产生音频事件，只往命令队列里写，从不直接调用 XAudio2 API
//   音频线程  唯一调用 XAudio2 API 的地方，消费命令 + 回收播完的 voice + 给流式播放补块
//   混音线程  XAudio2 自己创建，回调里只允许「置标志 + 唤醒音频线程」
//

// 音频线程：
//   创建音频线程并设为高优先级
//     阻塞等待事件触发唤醒（命令 / voice 播完 / 流式块播完 / 退出）
//     唤醒，消费命令，回收播放完的 voice，给流式播放补块
//     阻塞等待唤醒
//
// 音频图（bus 的音量是相对父级的，级联乘积由 XAudio2 自动完成）：
//   source voice ──► bus: sfx.imp  ──┐
//   source voice ──► bus: sfx.chr  ──┼──► bus: sfx ──┐
//   source voice ──► bus: sfx.amb  ──┘               │
//   source voice ──► bus: music ─────────────────────┼──► bus: master ──► 设备
//   source voice ──► bus: ui ────────────────────────┘
// ============================================================================

// 全图统一采样率 48000Hz，设备不支持时由 mastering voice 做一次固定速率 SRC
inline constexpr u32 AUDIO_SAMPLE_RATE = 48000;
// 总线统一 2 声道，由 mastering voice 负责向设备实际声道布局做最终混音
inline constexpr u32 AUDIO_BUS_CHANNELS = 2;

// 音频总线：数组顺序即创建顺序，父总线必须排在子总线之前（拓扑序）
enum BusId : u8
{
    BUS_MASTER,        // 根节点
    BUS_MUSIC,         // 背景音乐
    BUS_UI,            // 界面音效
    BUS_SFX,           // 音效分组节点（自身不挂音源时只做分组）
    BUS_SFX_AMBIENCE,  // 环境音
    BUS_SFX_CHARACTER, // 角色音
    BUS_SFX_IMPACT,    // 打击音
    BUS_COUNT
};

// voice 池：source voice 的输入格式在创建时固定，每个「声道数 + 采样率」组合需要一个池
enum AudioPoolId : u8
{
    AUDIO_POOL_MONO_48K,   // 单声道 48kHz
    AUDIO_POOL_STEREO_44K, // 立体声 44100Hz
    AUDIO_POOL_STEREO_32K, // 立体声 32000Hz
    AUDIO_POOL_COUNT
};

// ---------------------------------------------------------------------------
// 资源层：解码 / 生成后的 PCM 采样，创建后不可变，可被多个 voice 共享
// ---------------------------------------------------------------------------

struct SoundAsset
{
    void *pcm;
    u16 channels;
    u32 sample_count; // 单个声道采样点数
    u32 byte_count;   // sample_count * channels * sizeof(f32);
    u32 sample_rate;  // 采样率（保留素材原生率，靠 AUDIO_POOL_TABLE 精确匹配到对应池）
};

// 播放句柄：pool / slot 定位槽位，generation 让过期句柄安全失效
struct SoundHandle
{
    u32 pool;
    u32 slot;
    u32 generation; // 0 表示无效句柄
};

// 等功率声像的左右增益
struct StereoGains
{
    f32 left;
    f32 right;
};

// 播放参数
struct AudioPlayParams
{
    BusId bus = BUS_SFX;
    f32 volume = 1.0f; // 音源自身音量，线性幅度
    f32 pitch = 1.0f;  // 频率比，1.0 为原速（默认上限为 XAUDIO2_DEFAULT_FREQ_RATIO = 2.0）
    bool loop = false;
    // 等功率声像增益，只对单声道资产生效，默认等功率居中
    StereoGains gains = { INV_SQRT_2, INV_SQRT_2 };
};

// ---------------------------------------------------------------------------
// 流式播放：只常驻压缩数据，PCM 由音频线程按块解码补充（长音频用）
//
// 与常驻播放的差别：不预先解码整段，内存从「解码后 PCM」降到「压缩数据」，
// 代价是起播前要先解出第一块（毫秒级），以及每块的解码发生在音频线程上。
// ---------------------------------------------------------------------------

// 流式数据源：fill / close 都由[音频线程]调用，
// 因此实现方只允许碰自己的解码状态：不得调用 XAudio2 API、不得加锁、不得分配内存
struct AudioStreamSource
{
    void *user; // 实现方的解码状态，由 fill / close 解释
    // 调用方提供的环形块缓冲：容量必须 >= chunk_count * chunk_frames * channels 个 f32。
    // 音频线程按 chunk_count 块轮转写入（读走一块才写下一块），整条流播放期间必须保持有效
    f32 *chunk_buffer;
    u32 chunk_frames; // 每块帧数：越大越省 CPU（解码次数少），但缓冲更深
    u32 chunk_count;  // 最多同时排队的块数，2~4 即可
    u16 channels;
    u32 sample_rate;

    // 把下一块交错 f32 写进 dst（容量 frames_want 帧），返回实际写入帧数；
    // at_end 置 true 表示流已结束（循环流由实现方内部回绕，永不置 true）
    u32 (*fill)(void *user, f32 *dst, u32 frames_want, bool *at_end);
    // 流结束或被停止时调用一次，用于释放解码器状态
    void (*close)(void *user);
};

// 流式播放句柄
struct AudioStreamHandle
{
    u32 slot;
    u32 generation; // 0 表示无效句柄
};

// ---------------------------------------------------------------------------
// 实例
// ---------------------------------------------------------------------------

// 音频系统实例：定义在 src/audio.cc（内部含 IXAudio2* / voice / 事件句柄 / 命令队列）。
// 调用方只持有指针，不关心也不需要知道内部布局。
struct AudioState;

// 2D 空间化：纯函数，策略参数由调用方传入，音频层不掺和游戏概念
//   rel_x / rel_y  声源相对听者（摄像机）的世界偏移
//   pan_width      水平偏移达到该值时完全偏向一侧
//   rolloff_radius 反距离衰减参考半径，距离等于它时衰减到约一半
//   min_gain       远场增益地板，避免远处完全静音
StereoGains audio_spatial_gains(f32 rel_x, f32 rel_y, f32 pan_width, f32 rolloff_radius, f32 min_gain);

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

// 创建并初始化音频系统（内存取自 arena）。返回 nullptr 表示音频不可用
// （例如没有输出设备），此时游戏应降级为静音而不是退出。
AudioState *audio_create();

// 关闭音频线程与 XAudio2 并释放资源；传 nullptr 安全。
void audio_destroy(AudioState *audio);

// ---------------------------------------------------------------------------
// 播放控制，游戏线程调用，通过命令队列下发
// ---------------------------------------------------------------------------

SoundHandle audio_play(AudioState *audio, SoundAsset *asset, AudioPlayParams *params);
bool audio_is_playing(AudioState *audio, SoundHandle handle);
void audio_stop(AudioState *audio, SoundHandle handle);
void audio_set_voice_gains(AudioState *audio, SoundHandle handle, f32 gain_l, f32 gain_r);
void audio_set_bus_gain(AudioState *audio, BusId bus, f32 gain);

// ---------------------------------------------------------------------------
// 流式播放（游戏线程调用）
//
// 数据源描述会被按值拷贝，但 source->chunk_buffer 指向的内存与 source->user
// 指向的解码状态都必须比整条流活得更久（通常放在 arena）
// ---------------------------------------------------------------------------

AudioStreamHandle audio_play_stream(AudioState *audio, AudioStreamSource *source, AudioPlayParams *params);
bool audio_is_stream_playing(AudioState *audio, AudioStreamHandle handle);
void audio_stop_stream(AudioState *audio, AudioStreamHandle handle);
