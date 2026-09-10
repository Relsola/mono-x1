#pragma once

#include "core.h"

enum LogLevel : u8
{
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_COUNT
};

// 初始化：创建日志目录并清空旧日志文件
void log_init();

void log_write(LogLevel level, const char *filename, i32 line_no, const char *fmt, ...);
void log_flush();

void log_shutdown();

#if MONO_DEBUG_BUILD
#define LOG_DEBUG(...) log_write(LOG_LEVEL_DEBUG, __builtin_FILE(), __LINE__ __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO, nullptr, 0 __VA_OPT__(, ) __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN, nullptr, 0 __VA_OPT__(, ) __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, nullptr, 0 __VA_OPT__(, ) __VA_ARGS__)
