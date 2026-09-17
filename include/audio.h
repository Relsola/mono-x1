#pragma once

#include "core.h"
#include <xaudio2.h>

// ============================================================================
// 音频系统 XAudio2
//
// 线程模型：
//   游戏线程  产生音频事件，只往命令队列里写，从不直接调用 XAudio2 API
//   音频线程  唯一调用 XAudio2 API 的地方，消费命令 + 回收播完的 voice
//   混音线程  XAudio2 自己创建，回调里只允许「置标志 + 唤醒音频线程」
//
// AudioVoiceSlot 字段所有权：
//   游戏线程独占写     generation
//   音频线程独占读写   voice / asset / active_generation / retire_flag / volume / gain_l / gain_r
//   两线程共同访问     AudioVoicePool::in_use_bits（游戏线程置位，音频线程清位）
//
// 音频线程：
//   创建音频线程并设为高优先级
//     阻塞等待事件触发唤醒
//     唤醒，消费命令，回收播放完的 voice
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
static constexpr u32 AUDIO_MAX_VOICES_PER_POOL = 64;
// 环形命令队列最大容量
static constexpr u32 AUDIO_CMD_RING_CAP = 512;
// 全图统一采样率 48000Hz， 设备不支持时由 mastering voice 做一次固定速率 SRC
static constexpr u32 AUDIO_SAMPLE_RATE = 48000;
// 总线统一 2 声道，由 mastering voice 负责向设备实际声道布局做最终混音
static constexpr u32 AUDIO_BUS_CHANNELS = 2;
// 队列最大长度掩码
static constexpr u32 AUDIO_CMD_RING_MASK = AUDIO_CMD_RING_CAP - 1;

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

// voice 池：不同声道数的资产需要不同的池（source voice 的输入格式创建后即固定）
enum AudioPoolId : u8
{
    AUDIO_POOL_MONO,   // 单声道池
    AUDIO_POOL_STEREO, // 双声道池
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
    StereoGains gains = { INV_SQRT_2, INV_SQRT_2 }; // 初始声像增益，默认等功率居中
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

// ---------------------------------------------------------------------------
// 命令层：游戏线程 -> 音频线程的无锁单生产者单消费者环形队列
// ---------------------------------------------------------------------------

enum AudioCmdKind : u8
{
    AUDIO_CMD_PLAY,
    AUDIO_CMD_STOP,
    AUDIO_CMD_SET_GAINS,
    AUDIO_CMD_SET_BUS_GAIN,
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
    f32 bus_gain[BUS_COUNT];                // 各总线相对父级的音量
    AudioVoicePool pools[AUDIO_POOL_COUNT]; // voice 池（目前只有单声道池）
    AudioVoiceCallback callback;            // 必须常驻且地址稳定，XAudio2 不会替我们持有它

    u32 sample_rate; // 全图统一采样率 = 设备输出采样率，避免任何 SRC

    // 游戏线程侧：无锁命令队列
    AudioCommand cmd_ring[AUDIO_CMD_RING_CAP];
    volatile LONG cmd_head;          // 消费者（音频线程）读取位置
    volatile LONG cmd_tail;          // 生产者（游戏线程）写入位置
    volatile LONG dropped_cmd_count; // 丢弃的命令（诊断信号）
    u32 generation_counter;

    // 音频线程侧
    HANDLE audio_thread;
    HANDLE cmd_event;
    HANDLE retire_event;
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
