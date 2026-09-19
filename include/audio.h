#pragma once

#include "core.h"
#include <xaudio2.h>

// ============================================================================
// 音频系统 XAudio2
//
// 线程模型：
//   游戏线程  产生音频事件，只往命令队列里写，从不直接调用 XAudio2 API
//   音频线程  唯一调用 XAudio2 API 的地方，消费命令 + 回收播完的 voice + 给流式播放补块
//   混音线程  XAudio2 自己创建，回调里只允许「置标志 + 唤醒音频线程」
//
// AudioVoiceSlot 字段所有权：
//   游戏线程独占写     generation
//   音频线程独占读写   voice / asset / active_generation / retire_flag / volume / gain_l / gain_r
//   两线程共同访问     AudioVoicePool::in_use_bits（游戏线程置位，音频线程清位）
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

// 每池最多 64 个 voice，与 64 位占用位图一一对应
internal constexpr u32 AUDIO_MAX_VOICES_PER_POOL = 64;
// 流式播放槽位数（背景音乐 + 预留，可同时存在多条流）
internal constexpr u32 AUDIO_STREAM_COUNT = 2;
// 环形命令队列最大容量
internal constexpr u32 AUDIO_CMD_RING_CAP = 512;
// 全图统一采样率 48000Hz， 设备不支持时由 mastering voice 做一次固定速率 SRC
internal constexpr u32 AUDIO_SAMPLE_RATE = 48000;
// 总线统一 2 声道，由 mastering voice 负责向设备实际声道布局做最终混音
internal constexpr u32 AUDIO_BUS_CHANNELS = 2;
// 队列最大长度掩码
internal constexpr u32 AUDIO_CMD_RING_MASK = AUDIO_CMD_RING_CAP - 1;

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
    u32 sample_rate;  // 采样率（全图统一 48000Hz）
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
// 实例层：池中固定数组的槽位，地址恒定，可直接当回调上下文
// ---------------------------------------------------------------------------

struct AudioVoiceSlot
{
    IXAudio2SourceVoice *voice;
    SoundAsset *asset;
    volatile LONG retire_flag; // 混音线程置位，音频线程消费并清位
    u32 active_generation;     // 音频线程记录的当前播放世代，用于校验过期的 stop / set_gains
    u32 generation;            // 游戏线程在占位时自增写入
    f32 volume;                // 当前音量
    f32 gain_l;                // 左增益
    f32 gain_r;                // 右增益
};

struct AudioVoicePool
{
    AudioVoiceSlot slots[AUDIO_MAX_VOICES_PER_POOL];
    u32 count;
    u16 channels;
    u64 valid_mask;              // 低 count 位有效，用于在位图里做边界裁剪
    volatile LONG64 in_use_bits; // 1 = 已占用
};

// 流式 voice 的回调：与池化 voice 的回调分开，互不干扰
struct AudioStreamCallback : IXAudio2VoiceCallback
{
    volatile LONG *chunk_done_flag = nullptr; // 指向所属槽位的标志位
    HANDLE chunk_event = nullptr;

    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnStreamEnd() override {}
    void __stdcall OnBufferStart(void *) override {}
    void __stdcall OnLoopEnd(void *) override {}
    void __stdcall OnVoiceError(void *, HRESULT) override {}

    void __stdcall OnBufferEnd(void *) override
    {
        // 在混音线程上执行：只置位 + 唤醒音频线程补块
        InterlockedExchange(chunk_done_flag, 1);
        SetEvent(chunk_event);
    }
};

// 流式播放槽位：voice 在流开始时创建、结束时销毁。
// 之所以不池化：格式由数据源决定（不必受池格式表约束），且流是长时间存在的少数对象，
// 创建/销毁（含会阻塞的 DestroyVoice）都发生在音频线程上，不影响游戏线程
struct AudioStreamSlot
{
    AudioStreamCallback callback; // 每槽一个，地址稳定
    IXAudio2SourceVoice *voice;
    AudioStreamSource source;      // 从命令拷贝过来的数据源描述
    volatile LONG chunk_done_flag; // 混音线程置位：有块播完了
    BusId bus;
    u32 generation;        // 游戏线程在占位时写入
    u32 active_generation; // 音频线程记录的当前世代
    u32 write_chunk;       // 环形块缓冲的写入下标（音频线程独占）
    f32 volume;
    bool active;
    bool at_end; // 数据源已结束，等尾块播完即可回收
};

// ---------------------------------------------------------------------------
// 命令层：游戏线程 -> 音频线程的无锁单生产者单消费者环形队列
// ---------------------------------------------------------------------------

enum AudioCmdKind : u8
{
    AUDIO_CMD_PLAY,
    AUDIO_CMD_STOP,
    AUDIO_CMD_SET_GAINS,
    AUDIO_CMD_SET_BUS_GAIN,
    AUDIO_CMD_PLAY_STREAM,
    AUDIO_CMD_STOP_STREAM,
};

struct AudioCommand
{
    AudioCmdKind kind;
    BusId bus;
    u32 pool;
    u32 slot;
    u32 generation;
    SoundAsset *asset;
    f32 volume;
    f32 pitch;
    f32 gain_l;
    f32 gain_r;
    bool loop;
    AudioStreamSource source; // 仅流式命令使用（按值拷贝，调用方不必保证描述结构的生命周期）
};

// XAudio2 混音线程回调：只允许「置标志 + 唤醒音频线程」，其余一律交给音频线程处理
// 回调里严禁调用 XAudio2 API、加锁、分配内存
struct AudioVoiceCallback : IXAudio2VoiceCallback
{
    HANDLE retire_event = nullptr; // 由 audio_init 在创建音频线程时填充

    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnStreamEnd() override {}
    void __stdcall OnBufferStart(void *) override {}
    void __stdcall OnLoopEnd(void *) override {}
    void __stdcall OnVoiceError(void *, HRESULT) override {}

    void __stdcall OnBufferEnd(void *buffer_context) override
    {
        AudioVoiceSlot *slot = (AudioVoiceSlot *)buffer_context;
        InterlockedExchange(&slot->retire_flag, 1);
        SetEvent(retire_event);
    }
};

struct AudioState
{
    IXAudio2 *engine;
    IXAudio2Voice *bus_voices[BUS_COUNT];
    f32 bus_gain[BUS_COUNT];                     // 各总线相对父级的音量
    AudioVoicePool pools[AUDIO_POOL_COUNT];      // voice 池（按格式划分）
    AudioStreamSlot streams[AUDIO_STREAM_COUNT]; // 流式槽位（BGM 等长音频）
    AudioVoiceCallback callback;                 // 必须常驻且地址稳定，XAudio2 不会替我们持有它

    u32 sample_rate; // 总线与 mastering 的采样率

    // 游戏线程侧：无锁命令队列
    AudioCommand cmd_ring[AUDIO_CMD_RING_CAP];
    volatile LONG cmd_head;          // 消费者（音频线程）读取位置
    volatile LONG cmd_tail;          // 生产者（游戏线程）写入位置
    volatile LONG dropped_cmd_count; // 丢弃的命令（诊断信号）
    u32 generation_counter;
    volatile LONG64 stream_in_use_bits; // 流式槽位占用位图（游戏线程置位，音频线程清位）
    u32 stream_generation_counter;      // 流式句柄的世代计数（只有游戏线程写）

    // 音频线程侧
    HANDLE audio_thread;
    HANDLE cmd_event;
    HANDLE retire_event;
    HANDLE chunk_event; // 流式：某一块播完了，需要补块
    HANDLE quit_event;
    volatile bool running;
};

// 2D 空间化：纯函数，策略参数由调用方传入，音频层不掺和游戏概念
//   rel_x / rel_y  声源相对听者（摄像机）的世界偏移
//   pan_width      水平偏移达到该值时完全偏向一侧
//   rolloff_radius 反距离衰减参考半径，距离等于它时衰减到约一半
//   min_gain       远场增益地板，避免远处完全静音
StereoGains audio_spatial_gains(f32 rel_x, f32 rel_y, f32 pan_width, f32 rolloff_radius, f32 min_gain);

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool audio_init(AudioState *audio);
void audio_shutdown(AudioState *audio);

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
