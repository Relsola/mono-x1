#include "logger.h"

constexpr wchar_t LOG_FILE_NAME[] = L"build/log/game.log";

// 内存缓冲 64KB
constexpr u32 LOG_BUFFER_SIZE = KB(64);
// 单条栈日志最大长度
constexpr u32 MESSAGE_STACK_SIZE = KB(1);

struct LogBuffer
{
    char data[LOG_BUFFER_SIZE];
    u32 used;
};

global_variable LogBuffer global_log_buffer = {};
global_variable bool global_log_initialized = false;
global_variable constexpr const char *LEVEL_STRINGS[LOG_LEVEL_COUNT] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

// 只保留路径中的文件名部分
internal const char *log_file_basename(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    return name;
}

internal void log_buffer_append(const char *data, u32 len)
{
    if (global_log_buffer.used + len > LOG_BUFFER_SIZE) {
        log_flush();
    }

    memcpy(global_log_buffer.data + global_log_buffer.used, data, len);
    global_log_buffer.used += len;
}

void log_init()
{
    if (global_log_initialized) {
        return;
    }

    // 覆盖写 0 字节：清空上次运行的旧日志，保证每次运行只保留最新一次日志
    u8 dummy = 0;
    write_file(LOG_FILE_NAME, 0, &dummy, false);

    global_log_buffer.used = 0;
    global_log_initialized = true;
}

void log_write(LogLevel level, const char *filename, i32 line_no, const char *fmt, ...)
{
    if (!global_log_initialized) {
        log_init();
    }

    // 前缀
    char line[MESSAGE_STACK_SIZE];
    const char *tag = LEVEL_STRINGS[level];
    i32 prefix_len;
    if (filename) {
        prefix_len = snprintf(line, sizeof(line), "[%s] %s:%d ", tag, log_file_basename(filename), line_no);
    } else {
        prefix_len = snprintf(line, sizeof(line), "[%s] ", tag);
    }
    u32 head = (prefix_len < 0) ? 0 : (u32)prefix_len;
    if (head >= sizeof(line)) {
        head = sizeof(line) - 1;
    }

    // 打印日志
    va_list args;
    va_start(args, fmt);
    i32 written = vsnprintf(line + head, sizeof(line) - head, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }

    u32 msg_len = (u32)written;
    u32 cap = sizeof(line) - head - 1; // 给末尾换行保留 1 字节
    if (msg_len >= cap) {
        msg_len = cap; // 超长日志安全截断
    }
    head += msg_len;

    // 补换行并整体追加进缓冲
    line[head++] = '\n';
    log_buffer_append(line, head);
}

void log_flush()
{
    if (global_log_buffer.used == 0) {
        return;
    }

    write_file(LOG_FILE_NAME, global_log_buffer.used, global_log_buffer.data, true);
    global_log_buffer.used = 0;
}

void log_shutdown()
{
    log_flush();
    global_log_initialized = false;
}
