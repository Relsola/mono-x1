#include "shared/memory.h"
#include "shared/file.h"

#include <string.h> // memcpy（arena 扩容时的搬迁）
#include <wchar.h>  // wcsncpy_s / wcslen（父目录路径）

#include "win32_prefix.h"

// ============================================================================
// 平台层：内存与文件
//
// 接口按概念分成 memory.h（线性分配器 + Array）与 file.h（读写文件），
// 实现合在这一个编译单元：两件事都只是对 Win32 的一层薄包装
// （VirtualAlloc / CreateFileW / ReadFile / WriteFile），跨平台时替换这一个文件。
//
// 依赖方向：platform_win32.cc -> memory.h / file.h -> core.h
// ============================================================================

// ============================================================================
// 线性分配器
// ============================================================================

struct ArenaMemory
{
    u8 *base;
    u64 size;
    u64 used;
};

global_variable ArenaMemory global_arena = {};

bool arena_init(u64 size)
{
    global_arena.base = (u8 *)VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    global_arena.size = global_arena.base ? size : 0;
    global_arena.used = 0;
    return global_arena.base != nullptr;
}

void *arena_push(u64 size)
{
    // 8 字节对齐：保证返回值对 f32 / 指针 / 普通结构体都对齐，避免未对齐访问
    global_arena.used = (global_arena.used + 7) & ~7ull;
    assert((global_arena.used + size) <= global_arena.size);
    void *result = global_arena.base + global_arena.used;
    global_arena.used += size;
    return result;
}

void *arena_realloc(void *p, u64 oldsz, u64 newsz)
{
    assert(oldsz <= newsz);
    void *result = arena_push(newsz);
    memcpy(result, p, oldsz);
    return result;
}

void scratch_init(ScratchArena *arena, u64 size)
{
    arena->base = (u8 *)VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    arena->size = arena->base ? size : 0;
    arena->used = 0;
}

void scratch_shutdown(ScratchArena *arena)
{
    if (arena->base) {
        VirtualFree(arena->base, 0, MEM_RELEASE);
    }
    *arena = {};
}

void scratch_reset(ScratchArena *arena) { arena->used = 0; }

void *scratch_push(ScratchArena *arena, u64 size)
{
    arena->used = (arena->used + 7) & ~7ull;
    assert((arena->used + size) <= arena->size);
    void *result = arena->base + arena->used;
    arena->used += size;
    return result;
}

void *scratch_realloc(ScratchArena *arena, void *p, u64 oldsz, u64 newsz)
{
    assert(oldsz <= newsz);
    void *result = scratch_push(arena, newsz);
    memcpy(result, p, oldsz);
    return result;
}

// ============================================================================
// 文件 I/O
// ============================================================================

ReadFileRes read_file(const wchar_t *filename)
{
    HANDLE file_handle = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return ReadFileRes{};
    }

    LARGE_INTEGER file_size_info = {};
    if (!GetFileSizeEx(file_handle, &file_size_info)) {
        CloseHandle(file_handle);
        return ReadFileRes{};
    }

    ReadFileRes result = {};
    result.file_size = safe_cast_u64(file_size_info.QuadPart);
    result.contents = VirtualAlloc(0, file_size_info.QuadPart, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    DWORD bytes_read = 0; // 实际读取的字节数
    bool read_ok = ReadFile(file_handle, result.contents, result.file_size, &bytes_read, 0) &&
                   result.file_size == bytes_read;
    CloseHandle(file_handle);

    if (!read_ok) {
        return ReadFileRes{}; // result 的析构会把刚分配到的内存放掉
    }
    return result;
}

internal void create_parent_directories(const wchar_t *file_path)
{
    wchar_t path[512];
    wcsncpy_s(path, array_size(path), file_path, _TRUNCATE);
    const u32 len = (u32)wcslen(path);

    // 从第二个字符开始找分隔符并逐级创建，跳过盘符 "C:" / 根路径 "\\"
    for (u32 i = 1; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
            path[i] = L'\0';
            CreateDirectoryW(path, nullptr);
            path[i] = L'\\';
        }
    }
}

bool write_file(const wchar_t *filename, u32 size, void *memory, bool append)
{
    // 确保父目录存在
    create_parent_directories(filename);

    DWORD creation = append ? OPEN_ALWAYS : CREATE_ALWAYS;
    HANDLE file_handle = CreateFileW(filename, GENERIC_WRITE, 0, 0, creation, 0, 0);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (append) {
        LARGE_INTEGER distance = {}; // 相对文件末尾偏移 0，即定位到末尾
        SetFilePointerEx(file_handle, distance, nullptr, FILE_END);
    }

    DWORD bytes_written = 0;
    bool written_ok = WriteFile(file_handle, memory, size, &bytes_written, 0) && bytes_written == size;

    CloseHandle(file_handle);
    return written_ok;
}

void free_file_memory(void *memory)
{
    if (memory) {
        VirtualFree(memory, 0, MEM_RELEASE);
    }
}

bool file_move_replace(const wchar_t *from, const wchar_t *to)
{
    // MOVEFILE_REPLACE_EXISTING 覆盖目标；不用 MOVEFILE_COPY_ALLOWED ——
    // 临时文件与原文件一定同目录，跨卷语义不需要
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
}

bool file_remove(const wchar_t *path)
{
    if (DeleteFileW(path)) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

// ============================================================================
// 文本编码转换（UTF-8 ⇄ UTF-16）
// ============================================================================

bool utf8_to_wide(const char *text, wchar_t *out, u32 out_size)
{
    // 先问长度（不含结尾 '\0'），放不下就不写 —— 调用方拿到的永远是完整的串
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 0 || (u32)length > out_size) {
        return false;
    }

    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, length) == length;
}

bool wide_to_utf8(const wchar_t *text, char *out, u32 out_size)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0 || (u32)length > out_size) {
        return false;
    }

    return WideCharToMultiByte(CP_UTF8, 0, text, -1, out, length, nullptr, nullptr) == length;
}
