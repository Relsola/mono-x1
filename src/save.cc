#include "save.h"
#include "shared/file.h"
#include "shared/logger.h"

#include <string.h> // memcpy（头部 + 负载拼一次写）

// ============================================================================
// 存档系统（文件格式与校验都在这个 TU）
//
// 格式定义刻意留在这里而不是公共头：调用方只该知道「写一份 / 读一份」，
// 不该知道头部有哪些字段 —— 否则格式一改就有多处要跟着改。
// ============================================================================

// 魔数 "MSAV"（小端字节序写成文件后正好是 M S A V）
internal constexpr u32 SAVE_MAGIC = 0x5641534Du;
// 格式版本：1 = SaveHeader + GameStateSnapshot。
// 改动负载结构、增删字段、甚至换编译器导致对齐变化时，payload_size 也会变 ——
// 两者是两道独立的闸门（版本表达「语义变了」，大小表达「布局变了」）。
internal constexpr u32 SAVE_VERSION = 1;

struct SaveHeader
{
    u32 magic;
    u32 version;
    u32 payload_size;
    u32 level_fingerprint;
    u32 checksum;
};

bool save_write(const wchar_t *path, const GameState *game_state)
{
    GameStateSnapshot snapshot = {};
    game_state_save(game_state, &snapshot);

    SaveHeader header = { .magic = SAVE_MAGIC,
                          .version = SAVE_VERSION,
                          .payload_size = (u32)sizeof(GameStateSnapshot),
                          .level_fingerprint = game_level_fingerprint(game_state),
                          .checksum = fnv1a_32(FNV1A_OFFSET_BASIS, &snapshot, sizeof(snapshot)) };

    // 两份固定大小的 POD 拼一次写：栈上缓冲足够（几十到几百字节），不必过 arena
    u8 buffer[sizeof(SaveHeader) + sizeof(GameStateSnapshot)];
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), &snapshot, sizeof(snapshot));

    if (!write_file(path, safe_cast_u64(sizeof(buffer)), buffer)) {
        LOG_ERROR("save: cannot write %ls", path);
        return false;
    }

    LOG_DEBUG("save: wrote %ls (%llu bytes, level fingerprint %08x)",
              path, (u64)sizeof(buffer), header.level_fingerprint);
    return true;
}

bool save_read(const wchar_t *path, const GameState *game_state, GameStateSnapshot *out)
{
    ReadFileRes file = read_file(path);
    if (!file.contents) {
        LOG_WARN("save: cannot open %ls", path);
        return false;
    }

    constexpr u64 EXPECTED_SIZE = sizeof(SaveHeader) + sizeof(GameStateSnapshot);
    if ((u64)file.file_size < EXPECTED_SIZE) {
        LOG_ERROR("save: %ls is truncated (%u bytes, need %llu)", path, file.file_size, EXPECTED_SIZE);
        return false;
    }

    SaveHeader header = {};
    memcpy(&header, file.contents, sizeof(header));

    if (header.magic != SAVE_MAGIC) {
        LOG_ERROR("save: %ls is not a save file", path);
        return false;
    }
    if (header.version != SAVE_VERSION || header.payload_size != sizeof(GameStateSnapshot)) {
        LOG_ERROR("save: %ls is incompatible (v%u payload %u, this build writes v%u payload %llu) — please save again",
                  path, header.version, header.payload_size,
                  SAVE_VERSION, (u64)sizeof(GameStateSnapshot));
        return false;
    }

    // 关卡指纹：几何变了，存档里的坐标与实体状态就不再指向同一处
    u32 current_fingerprint = game_level_fingerprint(game_state);
    if (header.level_fingerprint != current_fingerprint) {
        LOG_ERROR("save: %ls was written for another level layout (%08x, now %08x) — regenerate it",
                  path, header.level_fingerprint, current_fingerprint);
        return false;
    }

    const u8 *payload = (const u8 *)file.contents + sizeof(SaveHeader);
    if (fnv1a_32(FNV1A_OFFSET_BASIS, payload, sizeof(GameStateSnapshot)) != header.checksum) {
        LOG_ERROR("save: %ls is corrupted (checksum mismatch)", path);
        return false;
    }

    memcpy(out, payload, sizeof(GameStateSnapshot));
    return true;
}

bool save_restore(const wchar_t *path, GameState *game_state)
{
    GameStateSnapshot snapshot = {};
    if (!save_read(path, game_state, &snapshot)) {
        return false;
    }

    if (!game_state_load(game_state, &snapshot)) {
        // 校验和与指纹都过了，却仍不是合法状态：说明存档是用「另一套运行时不变量」写的
        LOG_ERROR("save: %ls holds an invalid state for this build", path);
        return false;
    }

    LOG_DEBUG("save: restored %ls (x=%.1f y=%.1f state=%s world=%u ground=%d air_jumps=%u)",
              path, game_state->player_x, game_state->player_y, player_state_name(game_state->state),
              (u32)game_state->world, game_state->grounded ? 1 : 0, game_state->air_jumps_left);
    return true;
}
