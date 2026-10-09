#include "core.h"
#include "shared/memory.h"
#include "shared/file.h"
#include "audio.h"
#include "game_audio.h"
#include "shared/logger.h"
#include "shared/mono_math.h"

#include <string.h> // memcpy（音频文件常驻解码）

// 第三方解码器的实现集中在这一个编译单元，避免污染其他文件；
// 用 warning(push, 0) 屏蔽第三方代码的告警。
// **它不是万能的**：C4701 这类由代码生成阶段发出的告警不吃 pragma 里的 warning state（实测），
// 所以 dr_wav.h 与 stb_vorbis.c 各有 1 条 C4701 仍然会出现在构建输出里 ——
// 只有命令行 /W0 或 /wd4701 压得住。项目选择留着那两条，而不是为第三方代码关掉整条告警。
#pragma warning(push, 0)
#define DR_WAV_IMPLEMENTATION
#include "lib/dr_wav.h"
#include "lib/stb_vorbis.c"
#pragma warning(pop)

// ============================================================================
// 素材清单与调音参数
//
// 路径相对于进程工作目录
//
// 加载约定（单人项目，一开始就定死，省掉运行时的判断）：
//   短音效 -> 常驻解码后的 PCM，起播零延迟
//   长音频 -> 只常驻压缩数据，播放时由音频线程流式解码
//             （140 秒立体声 44.1kHz：47MB PCM 降到 3.6MB 压缩数据）
// ============================================================================
constexpr wchar_t COIN_PATH[] = L"data/music/coin recieved.wav";
constexpr wchar_t DASH_PATH[] = L"data/music/freesound button.wav";
constexpr wchar_t BGM_PATH[] = L"data/music/Memories of the School.ogg";

constexpr f32 COIN_VOLUME = 0.9f;
constexpr f32 DASH_VOLUME = 0.8f;
constexpr f32 BGM_VOLUME = 0.7f;

// 背景音乐的流式分块：4096 帧 ≈ 93ms/块，3 块 ≈ 280ms 缓冲；
// 每块解码约 0.2ms，占音频线程不到 0.3%
constexpr u32 BGM_CHUNK_FRAMES = 4096;
constexpr u32 BGM_CHUNK_COUNT = 3;

// ============================================================================
// 解码器
//
// 两者的目标格式一致：交错 f32，保留源声道数与源采样率（采样率不一致时由
// XAudio2 在源 voice 内做 SRC，因此这里完全不需要重采样/降混）。
// 解码后的 PCM 一律写入 arena 常驻，加载期不做堆分配；
// 唯一例外是 stb_vorbis 内部的临时结构（几百 KB，close 时由它自己释放）。
// ============================================================================

// WAV：dr_wav 的 init_memory 不做堆分配，PCM16 -> f32 也是直接转换
internal SoundAsset load_wav(const wchar_t *path, const void *data, u32 data_size)
{
    SoundAsset asset = {};

    drwav wav = {};
    if (!drwav_init_memory(&wav, data, data_size, nullptr)) {
        LOG_ERROR("Audio: WAV 解析失败: %ls", path);
        return asset;
    }

    u32 channels = (u32)wav.channels;
    u32 sample_rate = wav.sampleRate;
    u64 frames = wav.totalPCMFrameCount;

    // 头部已给出总帧数，可以一次性精确分配目标缓冲
    f32 *samples = (f32 *)arena_push(frames * channels * sizeof(f32));
    u64 frames_read = drwav_read_pcm_frames_f32(&wav, frames, samples);
    drwav_uninit(&wav);

    if (frames_read != frames) {
        LOG_WARN("Audio: WAV 实际解码 %llu 帧，头部声明 %llu 帧: %ls", frames_read, frames, path);
    }

    asset.pcm = samples;
    asset.channels = (u16)channels;
    asset.sample_count = (u32)frames_read;
    asset.byte_count = (u32)(frames_read * channels * sizeof(f32));
    asset.sample_rate = sample_rate;
    return asset;
}

// OGG：stb_vorbis 流式解码，分块直接写进 arena 目标缓冲，不产生中间缓冲
internal SoundAsset load_ogg(const wchar_t *path, const void *data, u32 data_size)
{
    SoundAsset asset = {};

    int error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_memory((const unsigned char *)data, (int)data_size, &error, nullptr);
    if (!vorbis) {
        LOG_ERROR("Audio: OGG 解析失败 (stb_vorbis error %d): %ls", error, path);
        return asset;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    u32 channels = (u32)info.channels;
    u32 sample_rate = (u32)info.sample_rate;
    // 从文件尾的 granule position 推算出总帧数（每声道），用于一次性精确分配
    u32 frames = stb_vorbis_stream_length_in_samples(vorbis);
    if (frames == 0) {
        stb_vorbis_close(vorbis);
        LOG_ERROR("Audio: OGG 总帧数为 0（文件可能截断）: %ls", path);
        return asset;
    }

    f32 *samples = (f32 *)arena_push((u64)frames * channels * sizeof(f32));

    constexpr u32 CHUNK_FRAMES = 4096;
    u32 frames_read = 0;
    while (frames_read < frames) {
        u32 want = MIN(CHUNK_FRAMES, frames - frames_read);
        f32 *dst = samples + (u64)frames_read * channels;
        int got = stb_vorbis_get_samples_float_interleaved(vorbis, (int)channels, dst, (int)(want * channels));
        if (got <= 0) {
            break;
        }
        frames_read += (u32)got;
    }
    stb_vorbis_close(vorbis);

    if (frames_read != frames) {
        LOG_WARN("Audio: OGG 实际解码 %u 帧，头部声明 %u 帧: %ls", frames_read, frames, path);
    }

    asset.pcm = samples;
    asset.channels = (u16)channels;
    asset.sample_count = frames_read;
    asset.byte_count = (u32)((u64)frames_read * channels * sizeof(f32));
    asset.sample_rate = sample_rate;
    return asset;
}

// 后缀判断：只用于在 wav / ogg 两种解码器之间分派
internal bool path_has_extension(const wchar_t *path, const wchar_t *extension)
{
    u32 path_len = 0;
    while (path[path_len] != L'\0') {
        ++path_len;
    }
    u32 ext_len = 0;
    while (extension[ext_len] != L'\0') {
        ++ext_len;
    }
    if (path_len < ext_len) {
        return false;
    }
    return _wcsicmp(path + path_len - ext_len, extension) == 0;
}

// 常驻素材：整个解码成 f32 PCM 写进 arena，起播零延迟（短音效）
// TODO 将来由独立加载线程预加载时，这里要改为「传入该线程自己的 arena」，
// 因为 arena_push 是单线程 bump 指针，跨线程并发 push 会数据竞争
internal SoundAsset load_asset(const wchar_t *path)
{
    SoundAsset asset = {};

    ReadFileRes file = read_file(path);
    if (!file.contents) {
        LOG_ERROR("Audio: Read File Failed: %ls", path);
        return asset;
    }

    asset = path_has_extension(path, L".ogg") ? load_ogg(path, file.contents, file.file_size)
                                              : load_wav(path, file.contents, file.file_size);
    return asset;
}

// 流式素材：只把压缩数据整块搬进 arena 常驻，解码推迟到播放时
// （这里只开一次解码器读头部信息用于日志，不解码任何采样）
internal void load_stream_asset(const wchar_t *path, void **out_data, u32 *out_bytes)
{
    ReadFileRes file = read_file(path);
    if (!file.contents) {
        LOG_ERROR("Audio: Read File Failed: %ls", path);
        return;
    }

    // 压缩数据搬进 arena 常驻（文件缓冲来自 VirtualAlloc，拷完立即释放）
    u8 *resident = (u8 *)arena_push(file.file_size);
    memcpy(resident, file.contents, file.file_size);

    *out_data = resident;
    *out_bytes = file.file_size;
}

// ============================================================================
// 游戏音频状态（本文件私有：外部只能通过语义函数访问）
// ============================================================================
internal AudioState *audio_state;
internal SoundAsset coin_asset;
internal SoundAsset dash_asset;

// 背景音乐：压缩数据常驻 + 播放时流式解码
internal void *bgm_compressed;
internal u32 bgm_compressed_bytes;
internal f32 *bgm_chunk_buffer; // 复用的环形块缓冲（每次起播复用同一块，避免 arena 只增不减）
internal u32 bgm_chunk_buffer_bytes;
internal AudioStreamHandle bgm_handle;

// 流式解码状态：由音频线程通过 AudioStreamSource 回调使用
struct BgmStream
{
    stb_vorbis *vorbis;
    u32 channels;
    bool loop;
};

// 音频线程调用：填满一块。循环流在文件末尾自动回到开头，因此块边界处音乐是连续的
internal u32 bgm_stream_fill(void *user, f32 *dst, u32 frames_want, bool *at_end)
{
    BgmStream *stream = (BgmStream *)user;
    u32 written = 0;
    u32 empty_reads = 0;

    while (written < frames_want) {
        u32 want = frames_want - written;
        f32 *out = dst + (u64)written * stream->channels;
        int got = stb_vorbis_get_samples_float_interleaved(stream->vorbis, (int)stream->channels,
                                                           out, (int)(want * stream->channels));

        if (got > 0) {
            written += (u32)got;
            empty_reads = 0;
        } else if (++empty_reads > 2) {
            *at_end = true; // 解码器连续读不出数据，避免死循环
            break;
        }

        if ((u32)got < want) {
            // 到达流末尾：循环流回到开头把本块填满，非循环流就此结束
            if (!stream->loop || !stb_vorbis_seek_start(stream->vorbis)) {
                *at_end = true;
                break;
            }
        }
    }

    return written;
}

// 音频线程调用：流结束或被停止时释放解码器
internal void bgm_stream_close(void *user)
{
    BgmStream *stream = (BgmStream *)user;
    if (stream && stream->vorbis) {
        stb_vorbis_close(stream->vorbis);
        stream->vorbis = nullptr;
    }
}

internal void game_audio_start_bgm()
{
    if (!audio_state || !bgm_compressed || bgm_compressed_bytes == 0) {
        return;
    }

    // 每次起播一份新的解码状态（arena 里十几字节，起停多次互不干扰）
    BgmStream *stream = (BgmStream *)arena_push(sizeof(BgmStream));

    int error = 0;
    stream->vorbis = stb_vorbis_open_memory((const unsigned char *)bgm_compressed, (int)bgm_compressed_bytes, &error, nullptr);
    if (!stream->vorbis) {
        LOG_ERROR("Audio: 背景音乐解码器打开失败 (stb_vorbis error %d)", error);
        return;
    }

    stb_vorbis_info info = stb_vorbis_get_info(stream->vorbis);
    stream->channels = (u32)info.channels;
    stream->loop = true;

    u32 need_bytes = BGM_CHUNK_FRAMES * BGM_CHUNK_COUNT * stream->channels * sizeof(f32);
    if (bgm_chunk_buffer_bytes < need_bytes) {
        bgm_chunk_buffer = (f32 *)arena_push(need_bytes);
        bgm_chunk_buffer_bytes = need_bytes;
    }

    AudioStreamSource source = {};
    source.user = stream;
    source.chunk_buffer = bgm_chunk_buffer;
    source.chunk_frames = BGM_CHUNK_FRAMES;
    source.chunk_count = BGM_CHUNK_COUNT;
    source.channels = (u16)stream->channels;
    source.sample_rate = (u32)info.sample_rate;
    source.fill = bgm_stream_fill;
    source.close = bgm_stream_close;

    AudioPlayParams params = {};
    params.bus = BUS_MUSIC;
    params.volume = BGM_VOLUME;

    bgm_handle = audio_play_stream(audio_state, &source, &params);
}

// 只在 audio_create 成功时调用（wWinMain 已经判过空指针：音频不是运行的必需条件，
// 失败只降级为静音，所以这里不需要再分一支「不可用」的路径）
void game_audio_init(AudioState *audio)
{
    audio_state = audio;

    coin_asset = load_asset(COIN_PATH);
    dash_asset = load_asset(DASH_PATH);
    load_stream_asset(BGM_PATH, &bgm_compressed, &bgm_compressed_bytes);

    game_audio_start_bgm();
}

void game_audio_play_coin()
{
    if (!audio_state || coin_asset.sample_count == 0) {
        return;
    }

    AudioPlayParams params = {};
    params.bus = BUS_SFX_CHARACTER;
    params.volume = COIN_VOLUME;

    audio_play(audio_state, &coin_asset, &params);
}

void game_audio_play_dash()
{
    if (!audio_state || dash_asset.sample_count == 0) {
        return;
    }

    AudioPlayParams params = {};
    params.bus = BUS_SFX_CHARACTER;
    params.volume = DASH_VOLUME;

    audio_play(audio_state, &dash_asset, &params);
}

void game_audio_toggle_bgm()
{
    if (!audio_state || !bgm_compressed) {
        return;
    }

    // 循环流不会自动结束，状态以实际查询为准（槽位空闲说明解码器已关闭）
    if (audio_is_stream_playing(audio_state, bgm_handle)) {
        audio_stop_stream(audio_state, bgm_handle);
        bgm_handle = {};
        LOG_DEBUG("Audio: 背景音乐停止（流式）");
    } else {
        game_audio_start_bgm();
    }
}
