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
// 池表：池 = 允许的 voice 输入格式清单（声道数 + 采样率），voice_count 为 0 表示不创建
// 资产采样率与池不一致时由 XAudio2 的 SRC 在源 voice 上完成转换，因此素材无需预处理
// ============================================================================
struct AudioPoolDesc
{
    u16 channels;
    u32 sample_rate;
    u32 voice_count;
};

internal constexpr AudioPoolDesc AUDIO_POOL_TABLE[AUDIO_POOL_COUNT] = {
    /* AUDIO_POOL_MONO_48K   */ { 1, 48000, 8 },
    /* AUDIO_POOL_STEREO_44K */ { 2, 44100, 8 },
    /* AUDIO_POOL_STEREO_32K */ { 2, 32000, 4 },
};

internal inline void audio_error_log(const char *what, HRESULT hr)
{
    LOG_ERROR("audio: %s failed (HRESULT 0x%08X)", what, (u32)hr);
}

// 本 SDK 的 IXAudio2SourceVoice::GetState 没有「非阻塞」标志；
// 我们只需要 BuffersQueued，用这个标志省掉 SamplesPlayed 的统计开销
internal constexpr UINT32 VOICE_STATE_FLAGS = XAUDIO2_VOICE_NOSAMPLESPLAYED;

// 流式槽位的有效位掩码（槽位数远小于 64）
internal constexpr u64 STREAM_VALID_MASK = (1ull << AUDIO_STREAM_COUNT) - 1;

// ---------------------------------------------------------------------------
// 共用小工具：池化播放与流式播放两条路径都复用
// ---------------------------------------------------------------------------

// 统一的源 voice 输入格式：交错 f32
internal WAVEFORMATEX audio_make_float_format(u16 channels, u32 sample_rate)
{
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = channels;
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = sizeof(f32) * 8;
    format.nBlockAlign = (WORD)(format.nChannels * sizeof(f32));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

// 把源 voice 接到目标总线并设置输出矩阵：
//   单声道源 -> 1×2 矩阵承载声像与距离衰减
//   立体声源 -> 2×2 恒等矩阵（立体声素材无法真正摆位，增益统一走 SetVolume）
// 矩阵索引公式为 pLevelMatrix[源声道 + 源声道数 * 目标声道]
internal void audio_bind_output(IXAudio2Voice *voice, IXAudio2Voice *bus, u16 channels, f32 gain_l, f32 gain_r)
{
    XAUDIO2_SEND_DESCRIPTOR send = { 0, bus };
    XAUDIO2_VOICE_SENDS sends = { 1, &send };
    voice->SetOutputVoices(&sends);

    if (channels == 1) {
        f32 matrix[AUDIO_BUS_CHANNELS] = { gain_l, gain_r };
        voice->SetOutputMatrix(nullptr, channels, AUDIO_BUS_CHANNELS, matrix);
    } else {
        assert(channels == AUDIO_BUS_CHANNELS);
        constexpr f32 IDENTITY_MATRIX[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        voice->SetOutputMatrix(nullptr, channels, AUDIO_BUS_CHANNELS, IDENTITY_MATRIX);
    }
}

// 免锁位图占位：取最低空闲位并 CAS 置位；失败（位图已满）返回 false
internal bool audio_bitmap_reserve(volatile LONG64 *bits_ptr, u64 valid_mask, u32 *out_index)
{
    LONG64 bits = *bits_ptr;
    for (;;) {
        u64 free_bits = (u64)~bits & valid_mask;
        if (free_bits == 0) {
            return false;
        }

        unsigned long bit_index = 0;
        _BitScanForward64(&bit_index, free_bits);

        LONG64 new_bits = bits | (LONG64)((u64)1 << bit_index);
        LONG64 observed = InterlockedCompareExchange64(bits_ptr, new_bits, bits);
        if (observed != bits) {
            bits = observed; // 竞争失败，用最新值重试
            continue;
        }

        *out_index = (u32)bit_index;
        return true;
    }
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

    // 把源 voice 接到目标总线并设置输出矩阵（池里的 voice 创建时不指定 send list）
    audio_bind_output(voice, audio->bus_voices[cmd->bus], pool->channels, cmd->gain_l, cmd->gain_r);
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

    // 立体声资产的增益不参与声像（矩阵恒等），音量请用播放参数里的 volume
    if (pool->channels != 1) {
        return;
    }

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

// ============================================================================
// 流式播放（音频线程侧）：按块填充，一块播完由回调唤醒后再补下一块
// ============================================================================

// 结束一条流：停播、销毁 voice、关闭解码器、归还槽位
// 顺序要求：close 必须在清位图之前，这样游戏线程看到槽位空闲时解码器一定已经关闭
internal void audio_stream_release(AudioState *audio, u32 slot_index)
{
    AudioStreamSlot *slot = &audio->streams[slot_index];

    if (slot->voice) {
        slot->voice->Stop(0, XAUDIO2_COMMIT_NOW);
        slot->voice->FlushSourceBuffers();
        slot->voice->DestroyVoice();
        slot->voice = nullptr;
    }

    if (slot->source.close) {
        slot->source.close(slot->source.user);
    }

    slot->source = {};
    slot->active = false;
    slot->at_end = false;
    slot->active_generation = 0;
    InterlockedAnd64(&audio->stream_in_use_bits, ~((LONG64)((u64)1 << slot_index)));
}

// 把排队的数据块补到上限；返回 false 表示数据源已结束（没有更多数据）
internal bool audio_stream_fill_queue(AudioStreamSlot *slot)
{
    XAUDIO2_VOICE_STATE state = {};
    slot->voice->GetState(&state, VOICE_STATE_FLAGS);

    // BuffersQueued 包含正在播放的那一块，所以「按完成数量轮转写块」绝不会覆盖正在读的块
    u32 queued = state.BuffersQueued;
    u32 chunk_floats = slot->source.chunk_frames * slot->source.channels;

    while (queued < slot->source.chunk_count) {
        f32 *dst = slot->source.chunk_buffer + (u64)slot->write_chunk * chunk_floats;

        bool at_end = false;
        u32 frames = slot->source.fill(slot->source.user, dst, slot->source.chunk_frames, &at_end);

        if (frames > 0) {
            XAUDIO2_BUFFER buffer = {};
            buffer.AudioBytes = frames * slot->source.channels * sizeof(f32);
            buffer.pAudioData = (const BYTE *)dst;
            if (at_end) {
                buffer.Flags = XAUDIO2_END_OF_STREAM;
            }
            slot->voice->SubmitSourceBuffer(&buffer);
            slot->write_chunk = (slot->write_chunk + 1) % slot->source.chunk_count;
            queued++;
        }

        if (at_end || frames == 0) {
            return false;
        }
    }

    return true;
}

internal void audio_start_stream(AudioState *audio, AudioCommand *cmd)
{
    AudioStreamSlot *slot = &audio->streams[cmd->slot];
    const AudioStreamSource *source = &cmd->source;
    assert(!slot->active);

    // 流式 voice 按数据源格式临时创建（不池化，因此不受池格式表限制）
    WAVEFORMATEX format = audio_make_float_format(source->channels, source->sample_rate);

    HRESULT result = audio->engine->CreateSourceVoice(&slot->voice, &format, 0,
                                                      XAUDIO2_DEFAULT_FREQ_RATIO, &slot->callback);
    if (FAILED(result)) {
        audio_error_log("CreateSourceVoice(stream)", result);
        InterlockedAnd64(&audio->stream_in_use_bits, ~((LONG64)((u64)1 << cmd->slot)));
        return;
    }

    audio_bind_output(slot->voice, audio->bus_voices[cmd->bus], source->channels, cmd->gain_l, cmd->gain_r);
    slot->voice->SetVolume(cmd->volume);
    slot->voice->SetFrequencyRatio(cmd->pitch);

    slot->source = *source;
    slot->bus = cmd->bus;
    slot->volume = cmd->volume;
    slot->write_chunk = 0;
    slot->active = true;
    slot->active_generation = cmd->generation;
    InterlockedExchange(&slot->chunk_done_flag, 0);

    slot->at_end = !audio_stream_fill_queue(slot);

    XAUDIO2_VOICE_STATE state = {};
    slot->voice->GetState(&state, VOICE_STATE_FLAGS);
    if (state.BuffersQueued == 0) {
        // 数据源一块数据都没给出（例如解码失败）：直接收摊
        LOG_WARN("Audio: 流式数据源没有可播放数据，放弃本次流式播放");
        audio_stream_release(audio, cmd->slot);
        return;
    }

    slot->voice->Start(0, XAUDIO2_COMMIT_NOW);
}

internal void audio_stop_stream_now(AudioState *audio, AudioCommand *cmd)
{
    if (cmd->slot >= AUDIO_STREAM_COUNT) {
        return;
    }

    AudioStreamSlot *slot = &audio->streams[cmd->slot];
    // 世代不符说明句柄已过期（该槽位已被别的流复用），直接忽略
    if (!slot->active || slot->active_generation != cmd->generation) {
        return;
    }

    audio_stream_release(audio, cmd->slot);
}

// 流式槽位维护：补块，以及在流结束后回收
internal void audio_service_streams(AudioState *audio)
{
    for (u32 i = 0; i < AUDIO_STREAM_COUNT; ++i) {
        AudioStreamSlot *slot = &audio->streams[i];
        if (!slot->active) {
            continue;
        }

        // 回调置位只是为了唤醒；真实进度以 BuffersQueued 为准，
        // 这样即使一次醒来时已经播完两块也不会算错
        InterlockedExchange(&slot->chunk_done_flag, 0);

        XAUDIO2_VOICE_STATE state = {};
        slot->voice->GetState(&state, VOICE_STATE_FLAGS);

        if (slot->at_end) {
            if (state.BuffersQueued == 0) {
                audio_stream_release(audio, i); // 尾块也播完了
            }
            continue;
        }

        if (state.BuffersQueued == 0) {
            // 队列断流：一块有几十毫秒，正常不该发生；续上时通知 XAudio2 时间线不连续
            slot->voice->Discontinuity();
        }

        if (!audio_stream_fill_queue(slot)) {
            slot->at_end = true;
        }
    }
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
    case AUDIO_CMD_PLAY_STREAM:
        audio_start_stream(audio, cmd);
        break;
    case AUDIO_CMD_STOP_STREAM:
        audio_stop_stream_now(audio, cmd);
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

    // 四条唤醒源：命令到达 / voice 播完 / 流式块播完 / 退出
    constexpr u32 WAIT_COUNT = 4;
    HANDLE wait_handles[WAIT_COUNT] = { audio->cmd_event, audio->retire_event, audio->chunk_event, audio->quit_event };

    while (audio->running) {
        WaitForMultipleObjects(WAIT_COUNT, wait_handles, FALSE, INFINITE); // 无限阻塞
        audio_process_commands(audio);
        audio_collect_retired(audio);
        audio_service_streams(audio);
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

        // 采样率取自池表（资产的原生采样率），与总线不同时由 XAudio2 在源 voice 内做 SRC；
        // 创建时不指定 send list，播放时再按目标总线改路由
        WAVEFORMATEX format = audio_make_float_format(desc->channels, desc->sample_rate);

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

        LOG_DEBUG("Audio: voice pool create (%u channels %u Hz, %u voice count)", desc->channels, desc->sample_rate, desc->voice_count);
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
    audio->chunk_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audio->quit_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audio->cmd_event || !audio->retire_event || !audio->chunk_event || !audio->quit_event) {
        LOG_ERROR("audio: CreateEventW failed");
        return false;
    }

    audio->callback.retire_event = audio->retire_event;
    audio->stream_in_use_bits = 0;
    audio->stream_generation_counter = 0;
    for (u32 i = 0; i < AUDIO_STREAM_COUNT; ++i) {
        // 每个流式槽位一个回调对象（地址稳定），回调只需知道自己的标志位
        audio->streams[i].callback.chunk_done_flag = &audio->streams[i].chunk_done_flag;
        audio->streams[i].callback.chunk_event = audio->chunk_event;
    }

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

    // 流式 voice 与解码器状态：必须在关事件句柄之前清理（销毁 voice 仍可能触发回调）
    for (u32 i = 0; i < AUDIO_STREAM_COUNT; ++i) {
        if (audio->streams[i].active) {
            audio_stream_release(audio, i);
        }
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

#define SAFE_CLOSE(_event) if (_event) { CloseHandle(_event); _event = nullptr; }

    SAFE_CLOSE(audio->cmd_event);
    SAFE_CLOSE(audio->retire_event);
    SAFE_CLOSE(audio->chunk_event);
    SAFE_CLOSE(audio->quit_event);

#undef SAFE_CLOSE

    audio->callback.retire_event = nullptr;
    for (u32 i = 0; i < AUDIO_STREAM_COUNT; ++i) {
        audio->streams[i].callback.chunk_event = nullptr;
        audio->streams[i].callback.chunk_done_flag = nullptr;
    }

    audio->engine->Release();
    audio->engine = nullptr;
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

// 按资产格式查池：voice 的输入格式创建后固定，所以必须精确匹配声道数与采样率
internal i32 audio_find_pool(AudioState *audio, u16 channels, u32 sample_rate)
{
    for (u32 i = 0; i < AUDIO_POOL_COUNT; ++i) {
        AudioVoicePool *pool = &audio->pools[i];
        if (pool->count > 0 && pool->channels == channels && AUDIO_POOL_TABLE[i].sample_rate == sample_rate) {
            return (i32)i;
        }
    }
    return -1;
}

// 用原子位图占位：免锁的 free list，游戏线程拿到槽位后立刻就能返回有效句柄
internal bool audio_pool_reserve(AudioState *audio, u32 pool_index, SoundHandle *out_handle)
{
    AudioVoicePool *pool = &audio->pools[pool_index];

    u32 slot_index = 0;
    if (!audio_bitmap_reserve(&pool->in_use_bits, pool->valid_mask, &slot_index)) {
        return false; // 池已满
    }

    // generation 只有游戏线程会写，因此这里不需要任何同步
    u32 generation = ++audio->generation_counter;
    pool->slots[slot_index].generation = generation;

    out_handle->pool = pool_index;
    out_handle->slot = slot_index;
    out_handle->generation = generation;
    return true;
}

SoundHandle audio_play(AudioState *audio, SoundAsset *asset, AudioPlayParams *params)
{
    SoundHandle handle = {};
    if (!audio || !audio->engine || !asset || asset->sample_count == 0) {
        return handle;
    }

    // 按资产格式选池：voice 的输入格式创建时固定，所以必须精确匹配（声道数 + 采样率）
    i32 pool_index = audio_find_pool(audio, asset->channels, asset->sample_rate);
    if (pool_index < 0) {
        LOG_ERROR("Audio: 没有匹配 (%u 声道 %u Hz) 的 voice 池，请在 AUDIO_POOL_TABLE 中新增一行",
                  (u32)asset->channels, asset->sample_rate);
        return handle;
    }

    if (!audio_pool_reserve(audio, (u32)pool_index, &handle)) {
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
    if (!audio || !audio->engine || handle.generation == 0) {
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
    if (!audio || !audio->engine || handle.generation == 0) {
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
    if (!audio || !audio->engine || handle.generation == 0) {
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
    if (!audio || !audio->engine) {
        return;
    }

    assert(bus < BUS_COUNT);
    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_SET_BUS_GAIN;
    cmd.bus = bus;
    cmd.volume = gain;
    audio_push_command(audio, &cmd);
}

// ============================================================================
// 流式播放（游戏线程）
// ============================================================================

// 与 voice 池同样的免锁占位，只是规模小得多
internal bool audio_stream_reserve(AudioState *audio, AudioStreamHandle *out_handle)
{
    u32 slot_index = 0;
    if (!audio_bitmap_reserve(&audio->stream_in_use_bits, STREAM_VALID_MASK, &slot_index)) {
        return false;
    }

    u32 generation = ++audio->stream_generation_counter;
    audio->streams[slot_index].generation = generation;

    out_handle->slot = slot_index;
    out_handle->generation = generation;
    return true;
}

AudioStreamHandle audio_play_stream(AudioState *audio, AudioStreamSource *source, AudioPlayParams *params)
{
    AudioStreamHandle handle = {};
    if (!audio || !audio->engine || !source || !source->fill || !source->chunk_buffer) {
        return handle;
    }
    if (source->chunk_frames == 0 || source->chunk_count == 0 || source->channels == 0) {
        assert(false); // 数据源描述不合法
        return handle;
    }

    if (!audio_stream_reserve(audio, &handle)) {
        LOG_WARN("Audio: 流式槽位已满，丢弃本次流式播放请求");
        return handle;
    }

    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_PLAY_STREAM;
    cmd.slot = handle.slot;
    cmd.generation = handle.generation;
    cmd.bus = params->bus;
    cmd.volume = params->volume;
    cmd.pitch = params->pitch;
    cmd.gain_l = params->gains.left;
    cmd.gain_r = params->gains.right;
    cmd.source = *source; // 按值拷贝：调用方不必保证描述结构的生命周期
    audio_push_command(audio, &cmd);

    return handle;
}

bool audio_is_stream_playing(AudioState *audio, AudioStreamHandle handle)
{
    if (!audio || !audio->engine || handle.generation == 0) {
        return false;
    }

    u64 bits = (u64)InterlockedOr64(&audio->stream_in_use_bits, 0);
    if ((bits & ((u64)1 << handle.slot)) == 0) {
        return false;
    }

    return audio->streams[handle.slot].generation == handle.generation;
}

void audio_stop_stream(AudioState *audio, AudioStreamHandle handle)
{
    if (!audio || !audio->engine || handle.generation == 0) {
        return;
    }

    AudioCommand cmd = {};
    cmd.kind = AUDIO_CMD_STOP_STREAM;
    cmd.slot = handle.slot;
    cmd.generation = handle.generation;
    audio_push_command(audio, &cmd);
}
