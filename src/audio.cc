#include "audio.h"
#include "logger.h"


// ============================================================================
// 总线表：数组顺序即创建顺序，父总线必须排在子总线之前
// 各总线的音量是「相对父级」的，级联乘积由 XAudio2 音频图自动完成，不需要手算
// ============================================================================

struct BusDesc
{
    BusId parent;
    f32 default_gain;
};

internal constexpr BusDesc BUS_TABLE[BUS_COUNT] = {
    /* BUS_MASTER        */ { BUS_MASTER, 1.00f },
    /* BUS_MUSIC         */ { BUS_MASTER, 0.70f },
    /* BUS_UI            */ { BUS_MASTER, 1.00f },
    /* BUS_SFX           */ { BUS_MASTER, 0.70f },
    /* BUS_SFX_AMBIENCE  */ { BUS_SFX,    1.00f },
    /* BUS_SFX_CHARACTER */ { BUS_SFX,    1.00f },
    /* BUS_SFX_IMPACT    */ { BUS_SFX,    1.00f },
};

// ============================================================================
// 池表：voice_count 为 0 表示不创建该池
// 本轮所有资产都是程序生成的单声道，立体声池留待导入立体声音乐资产时启用：
//   /* AUDIO_POOL_STEREO */ { 2, 16 }
// ============================================================================
struct AudioPoolDesc
{
    u16 channels;
    u32 voice_count;
};

internal constexpr AudioPoolDesc AUDIO_POOL_TABLE[AUDIO_POOL_COUNT] = {
    /* AUDIO_POOL_MONO */   { 1, 32 },
    /* AUDIO_POOL_STEREO */ { 2, 0 }  // 暂时禁用双声道池
};

internal inline void audio_error_log(const char *what, HRESULT hr)
{
    LOG_ERROR("audio: %s failed (HRESULT 0x%08X)", what, (u32)hr);
}

StereoGains audio_spatial_gains(f32 rel_x, f32 rel_y, f32 pan_width, f32 rolloff_radius, f32 min_gain)
{
    // 水平偏移映射到 [-1, 1] 的 pan 值
    f32 pan = rel_x / pan_width;
    pan = MIN(MAX(pan, -1.0f), 1.0f);

    // pan ∈ [-1, 1] 映射到 theta ∈ [0, π/2]
    f32 theta = (pan + 1.0f) * 0.25f * PI;

    // 反距离衰减：距离为 0 时满增益，等于 rolloff_radius 时约一半，远处收敛到 min_gain
    f32 distance = sqrtf(rel_x * rel_x + rel_y * rel_y);
    f32 attenuation = 1.0f / (1.0f + distance / rolloff_radius);
    f32 gain = min_gain + (1.0f - min_gain) * attenuation;

    StereoGains result;
    result.left = cosf(theta) * gain;
    result.right = sinf(theta) * gain;
    return result;
}

// ============================================================================
// 音频线程：voice 的启停与回收
// ============================================================================

// 停止播放并归还槽位，只能由音频线程调用
internal void audio_release_slot(AudioVoicePool *pool, u32 slot_index)
{
    AudioVoiceSlot *slot = &pool->slots[slot_index];

    slot->voice->Stop(0, XAUDIO2_COMMIT_NOW);
    slot->voice->FlushSourceBuffers();

    // 清位必须放在 FlushSourceBuffers 之后：flush 会同步回调 OnBufferEnd，
    // 把 retire_flag 重新置起来，提前清位会让紧随其后的回收扫描再跑一遍
    InterlockedExchange(&slot->retire_flag, 0);

    slot->asset = nullptr;
    slot->active_generation = 0;
    InterlockedAnd64(&pool->in_use_bits, ~((LONG64)((u64)1 << slot_index)));
}

internal void audio_start_voice(AudioState *audio, const AudioCommand *cmd)
{
    AudioVoicePool *pool = &audio->pools[cmd->pool];
    assert(cmd->slot < pool->count);

    AudioVoiceSlot *slot = &pool->slots[cmd->slot];
    IXAudio2SourceVoice *voice = slot->voice;
    SoundAsset *asset = cmd->asset;
    assert(asset && asset->sample_count > 0);

    // 复用槽位：先停流再改路由，SetOutputVoices 只能在 voice 未运行时调用
    voice->Stop(0, XAUDIO2_COMMIT_NOW);
    voice->FlushSourceBuffers();
    InterlockedExchange(&slot->retire_flag, 0);

    // 把源 voice 接到目标总线上（池里的 voice 创建时不指定 send list，这里按播放请求改路由）
    XAUDIO2_SEND_DESCRIPTOR send = { 0, audio->bus_voices[cmd->bus] };
    XAUDIO2_VOICE_SENDS sends = { 1, &send };
    voice->SetOutputVoices(&sends);

    // 单声道源 -> 2 声道总线的输出矩阵，一次调用同时完成声像与距离衰减
    assert(pool->channels == 1);
    f32 matrix[AUDIO_BUS_CHANNELS] = { cmd->gain_l, cmd->gain_r };
    voice->SetOutputMatrix(nullptr, pool->channels, AUDIO_BUS_CHANNELS, matrix);
    voice->SetVolume(cmd->volume);
    voice->SetFrequencyRatio(cmd->pitch);

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = asset->byte_count;
    buffer.pAudioData = (const BYTE *)asset->pcm;
    buffer.pContext = slot; // 回调靠它定位槽位；slots 位于 AudioState 内，地址恒定
    if (cmd->loop) {
        buffer.LoopBegin = 0;
        buffer.LoopLength = asset->sample_count;
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    } else {
        // 一次性播放：整段缓冲播完触发 OnBufferEnd，再由音频线程回收
        buffer.Flags = XAUDIO2_END_OF_STREAM;
    }

    slot->asset = asset;
    slot->volume = cmd->volume;
    slot->gain_l = cmd->gain_l;
    slot->gain_r = cmd->gain_r;
    slot->active_generation = cmd->generation;

    voice->SubmitSourceBuffer(&buffer);
    voice->Start(0, XAUDIO2_COMMIT_NOW);
}

internal void audio_stop_voice(AudioState *audio, AudioCommand *cmd)
{
    AudioVoicePool *pool = &audio->pools[cmd->pool];
    assert(cmd->slot < pool->count);

    AudioVoiceSlot *slot = &pool->slots[cmd->slot];
    // 世代不符说明句柄已过期（槽位已被别的音效复用），直接忽略
    if (slot->active_generation != cmd->generation) {
        return;
    }

    audio_release_slot(pool, cmd->slot);
}

internal void audio_apply_gains(AudioState *audio, AudioCommand *cmd)
{
    AudioVoicePool *pool = &audio->pools[cmd->pool];
    assert(cmd->slot < pool->count);

    AudioVoiceSlot *slot = &pool->slots[cmd->slot];
    if (slot->active_generation != cmd->generation) {
        return; // 已过期或已播完
    }

    assert(pool->channels == 1);
    f32 matrix[AUDIO_BUS_CHANNELS] = { cmd->gain_l, cmd->gain_r };
    slot->gain_l = cmd->gain_l;
    slot->gain_r = cmd->gain_r;
    slot->voice->SetOutputMatrix(nullptr, pool->channels, AUDIO_BUS_CHANNELS, matrix);
}

internal void audio_apply_bus_gain(AudioState *audio, AudioCommand *cmd)
{
    assert(cmd->bus < BUS_COUNT);
    audio->bus_gain[cmd->bus] = cmd->volume;
    audio->bus_voices[cmd->bus]->SetVolume(cmd->volume);
}

internal void audio_execute_command(AudioState *audio, AudioCommand *cmd)
{
    switch (cmd->kind) {
    case AUDIO_CMD_PLAY:
        audio_start_voice(audio, cmd);
        break;
    case AUDIO_CMD_STOP:
        audio_stop_voice(audio, cmd);
        break;
    case AUDIO_CMD_SET_GAINS:
        audio_apply_gains(audio, cmd);
        break;
    case AUDIO_CMD_SET_BUS_GAIN:
        audio_apply_bus_gain(audio, cmd);
        break;
    }
}

internal void audio_process_commands(AudioState *audio)
{
    for (;;) {
        LONG head = audio->cmd_head;
        LONG tail = audio->cmd_tail; // 每次重新读取，保证看到生产者刚发布的命令
        if (head == tail) {
            break;
        }

        AudioCommand cmd = audio->cmd_ring[head & AUDIO_CMD_RING_MASK];
        InterlockedIncrement(&audio->cmd_head);
        audio_execute_command(audio, &cmd);
    }
}

// 回收已播完的 voice：混音线程只置了标志，真正的 Stop / Flush 都在这里做
internal void audio_collect_retired(AudioState *audio)
{
    for (u32 p = 0; p < AUDIO_POOL_COUNT; ++p) {
        AudioVoicePool *pool = &audio->pools[p];
        for (u32 i = 0; i < pool->count; ++i) {
            AudioVoiceSlot *slot = &pool->slots[i];
            if (InterlockedCompareExchange(&slot->retire_flag, 0, 1) == 1) {
                audio_release_slot(pool, i);
            }
        }
    }
}

internal DWORD WINAPI audio_thread_proc(void *param)
{
    AudioState *audio = (AudioState *)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // 三条唤醒源：命令到达 / voice 播完 / 退出。
    constexpr u32 WAIT_COUNT = 3;
    HANDLE wait_handles[WAIT_COUNT] = { audio->cmd_event, audio->retire_event, audio->quit_event };

    while (audio->running) {
        WaitForMultipleObjects(WAIT_COUNT, wait_handles, FALSE, INFINITE); // 无限阻塞
        audio_process_commands(audio);
        audio_collect_retired(audio);
    }

    return 0;
}

// ============================================================================
// 初始化 / 关闭
// ============================================================================

internal bool audio_create_buses(AudioState *audio)
{
    HRESULT result = audio->engine->CreateMasteringVoice(
        (IXAudio2MasteringVoice **)&audio->bus_voices[BUS_MASTER],
        AUDIO_BUS_CHANNELS,
        audio->sample_rate);
    if (FAILED(result)) {
        audio_error_log("CreateMasteringVoice", result);
        return false;
    }

    // NOTE 编译期常量
    // 反向遍历算出每层的深度
    u32 bus_stage[BUS_COUNT] = {};
    for (u32 i = BUS_COUNT - 1; i > BUS_MASTER; --i) {
        BusId parent = BUS_TABLE[i].parent;
        bus_stage[parent] = MAX(bus_stage[parent], bus_stage[i] + 1);
    }

    for (u32 i = BUS_MASTER + 1; i < BUS_COUNT; ++i) {
        assert(BUS_TABLE[i].parent < i);                       // 父索引必须小于自己
        assert(bus_stage[BUS_TABLE[i].parent] > bus_stage[i]); // 父 stage 必须大于自己

        IXAudio2SubmixVoice *submix = nullptr;
        XAUDIO2_SEND_DESCRIPTOR send = { 0, audio->bus_voices[BUS_TABLE[i].parent] };
        XAUDIO2_VOICE_SENDS sends = { 1, &send };

        result = audio->engine->CreateSubmixVoice(&submix,
                                                  AUDIO_BUS_CHANNELS,
                                                  audio->sample_rate,
                                                  0,
                                                  bus_stage[i],
                                                  &sends);
        if (FAILED(result)) {
            audio_error_log("CreateSubmixVoice", result);
            return false;
        }

        audio->bus_voices[i] = submix;
        audio->bus_gain[i] = BUS_TABLE[i].default_gain;
        submix->SetVolume(BUS_TABLE[i].default_gain);
    }

    // 单独设置 master 音量
    audio->bus_gain[BUS_MASTER] = BUS_TABLE[BUS_MASTER].default_gain;
    audio->bus_voices[BUS_MASTER]->SetVolume(BUS_TABLE[BUS_MASTER].default_gain);
    return true;
}

internal bool audio_create_pools(AudioState *audio)
{
    for (u32 p = 0; p < AUDIO_POOL_COUNT; ++p) {
        const AudioPoolDesc *desc = &AUDIO_POOL_TABLE[p];
        if (desc->voice_count == 0) {
            continue;
        }
        assert(desc->voice_count <= AUDIO_MAX_VOICES_PER_POOL);

        AudioVoicePool *pool = &audio->pools[p];
        pool->count = desc->voice_count;
        pool->channels = desc->channels;
        pool->valid_mask = ~0ull >> (AUDIO_MAX_VOICES_PER_POOL - desc->voice_count);
        pool->in_use_bits = 0;

        // f32 交错采样格式；创建时不指定 send list，播放时再按目标总线改路由
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = (WORD)desc->channels;
        format.nSamplesPerSec = audio->sample_rate;
        format.wBitsPerSample = sizeof(f32) * 8;
        format.nBlockAlign = (WORD)(format.nChannels * sizeof(f32));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        // 创建 voice
        for (u32 i = 0; i < desc->voice_count; ++i) {
            AudioVoiceSlot *slot = &pool->slots[i];
            HRESULT result = audio->engine->CreateSourceVoice(&slot->voice, &format, 0,
                                                              XAUDIO2_DEFAULT_FREQ_RATIO,
                                                              &audio->callback);
            if (FAILED(result)) {
                audio_error_log("CreateSourceVoice", result);
                return false;
            }
            slot->voice->SetVolume(1.0f);
            slot->generation = 0;
        }
    }

    return true;
}

bool audio_init(AudioState *audio)
{
    HRESULT result = XAudio2Create(&audio->engine);
    if (FAILED(result)) {
        audio_error_log("XAudio2Create", result);
        return false;
    }

#if MONO_DEBUG_BUILD
    // 调试构建下把 XAudio2 内部告警输出到调试器
    XAUDIO2_DEBUG_CONFIGURATION debug_config = {};
    debug_config.TraceMask = XAUDIO2_LOG_ERRORS | XAUDIO2_LOG_WARNINGS;
    debug_config.BreakMask = 0;
    debug_config.LogThreadID = TRUE;
    debug_config.LogFileline = TRUE;
    debug_config.LogTiming = TRUE;
    audio->engine->SetDebugConfiguration(&debug_config, nullptr);
#endif

    audio->sample_rate = AUDIO_SAMPLE_RATE;
    if (!audio_create_buses(audio)) {
        return false;
    }
    if (!audio_create_pools(audio)) {
        return false;
    }

    // 命令队列与唤醒事件
    audio->cmd_head = 0;
    audio->cmd_tail = 0;
    audio->dropped_cmd_count = 0;
    audio->generation_counter = 0;

    audio->cmd_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audio->retire_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audio->quit_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audio->cmd_event || !audio->retire_event || !audio->quit_event) {
        LOG_ERROR("audio: CreateEventW failed");
        return false;
    }
    audio->callback.retire_event = audio->retire_event;

    audio->running = true;
    audio->audio_thread = CreateThread(nullptr, 0, audio_thread_proc, audio, 0, nullptr);
    if (!audio->audio_thread) {
        audio->running = false;
        LOG_ERROR("audio: CreateThread failed");
        return false;
    }

    return true;
}

void audio_shutdown(AudioState *audio)
{
    if (!audio || !audio->engine) {
        return;
    }

    // 先让音频线程退出，之后所有 XAudio2 调用都回到主线程
    if (audio->audio_thread) {
        audio->running = false;
        SetEvent(audio->quit_event);
        WaitForSingleObject(audio->audio_thread, INFINITE);
        CloseHandle(audio->audio_thread);
        audio->audio_thread = nullptr;
    }

    // 销毁源 voice 时仍可能触发 OnBufferEnd，所以 retire_event 必须等这里做完再关
    for (u32 p = 0; p < AUDIO_POOL_COUNT; ++p) {
        AudioVoicePool *pool = &audio->pools[p];
        for (u32 i = 0; i < pool->count; ++i) {
            if (pool->slots[i].voice) {
                pool->slots[i].voice->DestroyVoice();
                pool->slots[i].voice = nullptr;
            }
        }
        pool->count = 0;
        pool->in_use_bits = 0;
    }

    for (u32 i = BUS_MASTER; i < BUS_COUNT; ++i) {
        if (audio->bus_voices[i]) {
            audio->bus_voices[i]->DestroyVoice();
            audio->bus_voices[i] = nullptr;
        }
    }

    if (audio->cmd_event) {
        CloseHandle(audio->cmd_event);
        audio->cmd_event = nullptr;
    }
    if (audio->retire_event) {
        CloseHandle(audio->retire_event);
        audio->retire_event = nullptr;
    }
    if (audio->quit_event) {
        CloseHandle(audio->quit_event);
        audio->quit_event = nullptr;
    }

    audio->callback.retire_event = nullptr;

    audio->engine->Release();
    audio->engine = nullptr;

    LOG_DEBUG("Audio: 已关闭");
}

// ============================================================================
// 播放控制（游戏线程）
// ============================================================================

internal bool audio_push_command(AudioState *audio, const AudioCommand *cmd)
{
    LONG tail = audio->cmd_tail;
    LONG head = audio->cmd_head;
    if ((u32)(tail - head) >= AUDIO_CMD_RING_CAP) {
        // 队列满时丢弃新命令而不是阻塞游戏线程；正常情况下这个计数应该恒为 0
        InterlockedIncrement(&audio->dropped_cmd_count);
        return false;
    }

    audio->cmd_ring[tail & AUDIO_CMD_RING_MASK] = *cmd;
    InterlockedIncrement(&audio->cmd_tail);
    // 唤醒线程播放
    SetEvent(audio->cmd_event);
    return true;
}

// 用原子位图占位：免锁的 free list，游戏线程拿到槽位后立刻就能返回有效句柄
internal bool audio_pool_reserve(AudioState *audio, u32 pool_index, SoundHandle *out_handle)
{
    AudioVoicePool *pool = &audio->pools[pool_index];
    LONG64 bits = pool->in_use_bits;

    for (;;) {
        u64 free_bits = (u64)~bits & pool->valid_mask;
        if (free_bits == 0) {
            return false; // 池已满
        }

        unsigned long bit_index = 0;
        _BitScanForward64(&bit_index, free_bits);

        LONG64 new_bits = bits | (LONG64)((u64)1 << bit_index);
        LONG64 observed = InterlockedCompareExchange64(&pool->in_use_bits, new_bits, bits);
        if (observed != bits) {
            bits = observed; // 竞争失败，用最新值重试
            continue;
        }

        // generation 只有游戏线程会写，因此这里不需要任何同步
        u32 generation = ++audio->generation_counter;
        pool->slots[bit_index].generation = generation;

        out_handle->pool = pool_index;
        out_handle->slot = (u32)bit_index;
        out_handle->generation = generation;
        return true;
    }
}

SoundHandle audio_play(AudioState *audio, SoundAsset *asset, AudioPlayParams *params)
{
    SoundHandle handle = {};
    if (!asset || asset->sample_count == 0) {
        return handle;
    }
    assert(asset->sample_rate == audio->sample_rate);
    assert(asset->channels == 1); // 本轮只有单声道池

    if (!audio_pool_reserve(audio, AUDIO_POOL_MONO, &handle)) {
        LOG_WARN("Audio: voice 池已满，丢弃本次播放请求");
        handle = {};
        return handle;
    }

    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_PLAY;
    cmd.pool = handle.pool;
    cmd.slot = handle.slot;
    cmd.generation = handle.generation;
    cmd.bus = params->bus;
    cmd.asset = asset;
    cmd.volume = params->volume;
    cmd.pitch = params->pitch;
    cmd.gain_l = params->gains.left;
    cmd.gain_r = params->gains.right;
    cmd.loop = params->loop;
    audio_push_command(audio, &cmd);

    return handle;
}

bool audio_is_playing(AudioState *audio, SoundHandle handle)
{
    if (handle.generation == 0) {
        return false;
    }

    AudioVoicePool *pool = &audio->pools[handle.pool];
    // 原子读，防止编译器优化
    // 位图由音频线程清位，这里读到「还在播放」最多多持续一帧，无害
    u64 bits = (u64)InterlockedOr64(&pool->in_use_bits, 0);
    if ((bits & ((u64)1 << handle.slot)) == 0) {
        return false;
    }

    // 世代只有游戏线程会写，因此这个校验不存在竞态
    return pool->slots[handle.slot].generation == handle.generation;
}

void audio_stop(AudioState *audio, SoundHandle handle)
{
    if (handle.generation == 0) {
        return;
    }

    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_STOP;
    cmd.pool = handle.pool;
    cmd.slot = handle.slot;
    cmd.generation = handle.generation;
    audio_push_command(audio, &cmd);
}

void audio_set_voice_gains(AudioState *audio, SoundHandle handle, f32 gain_l, f32 gain_r)
{
    if (handle.generation == 0) {
        return;
    }

    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_SET_GAINS;
    cmd.pool = handle.pool;
    cmd.slot = handle.slot;
    cmd.generation = handle.generation;
    cmd.gain_l = gain_l;
    cmd.gain_r = gain_r;
    audio_push_command(audio, &cmd);
}

void audio_set_bus_gain(AudioState *audio, BusId bus, f32 gain)
{
    assert(bus < BUS_COUNT);
    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_SET_BUS_GAIN;
    cmd.bus = bus;
    cmd.volume = gain;
    audio_push_command(audio, &cmd);
}
