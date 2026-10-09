#pragma once

#include "core.h"

// ============================================================================
// 文件 IO
//
// 一次性读整个文件 / 覆盖或追加写文件。路径用宽字符（UNICODE 构建），
// 写文件会自动创建缺失的父目录。
//
// 它在 include/shared/ 里：编辑器也用它（`--check` 报告与关卡列表都要写读文件），所以它只能依赖 core.h。
// 实现见 src/platform_win32.cc（CreateFileW / ReadFile / WriteFile）。
// 依赖方向：shared/file.h -> core.h
// ============================================================================

// 释放 read_file 分配的内存（由 ReadFileRes 的析构自动调用，一般不需要手写）
void free_file_memory(void *memory);

// 读取结果：**内存归它自己管** —— 析构时就释放，所以调用方不必在每条失败路径上手写释放。
// 拷贝被删掉（两个对象指向同一块内存就是双重释放），移动把所有权挪走并清空来源。
// 用后即弃：不要把它的 contents 存下来长期使用。
struct ReadFileRes
{
    u32 file_size = 0;
    void *contents = nullptr;

    ReadFileRes() = default;
    ReadFileRes(ReadFileRes &&other) : file_size(other.file_size), contents(other.contents)
    {
        other.file_size = 0;
        other.contents = nullptr;
    }
    ~ReadFileRes() { free_file_memory(contents); }
};

// 读失败时返回的 contents 为 nullptr
ReadFileRes read_file(const wchar_t *filename);

// append = false 时截断重写，true 时追加到末尾
bool write_file(const wchar_t *filename, u32 size, void *memory, bool append = false);

// 把 from 覆盖移动到 to（同目录改名，目标已存在时直接替换）。
// 「先写临时文件、校验通过再改名」是关卡资产保存的原子性来源：
// 任何一步失败都不会动到原来的文件。
bool file_move_replace(const wchar_t *from, const wchar_t *to);

// 删除文件；文件不存在也算成功（清理临时文件的失败路径不该再报一次错）
bool file_remove(const wchar_t *path);

// 文本编码转换：文本文件（脚本等）里写的是 UTF-8，而 Win32 的路径 API 要 UTF-16。
// 放不下（含结尾 '\0'）或遇到非法 UTF-8 序列时返回 false。
// 路径来自脚本文本时必须走这里，而不是把字节逐个「强转」成宽字符 ——
// 后者对非 ASCII 路径会得到一个看起来正常、实际打不开的路径。
bool utf8_to_wide(const char *text, wchar_t *out, u32 out_size);
bool wide_to_utf8(const wchar_t *text, char *out, u32 out_size);
