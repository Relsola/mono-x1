
#pragma once

#include "core.h"

// ============================================================================
// 日志（游戏与编辑器共用这一层，所以头在 include/shared/）
//
// 一份实现（src/shared/logger.cc）编进两个程序，两边共用同一套级别与格式。
//
// **日志文件由调用方指定，而且每个程序一个文件**（这一步是 log_init 的参数）：
// 游戏 game.log、编辑器 editor.log。不要共用一个：游戏每次启动都会清空日志，
// 而编辑器跑「保存并冒烟验证」时起的正是游戏 —— 共用一个文件时，
// 编辑器刚写的记录会被子进程顺手抹掉（实测踩到过）。
// 没显式初始化时，第一次 log_write 用默认路径（game.log）：所以编辑器必须尽早调用。
//
// **每次 log_init 都是「重写」而不是「追加」**（覆盖写 0 字节）：
// 一个进程一次运行 = 一份完整日志，于是「日志里有什么」与「这次跑出了什么」是同一件事，
// runner 与人都可以只看当前文件，不用按时间戳区分历史（test\run_tests.bat 就靠这条）。
// 代价是上一次崩溃现场的日志会在下次启动时被清掉 —— 需要保留历史时
// 应当另做一个「每次运行一个文件」的方案，而不是给 log_init 加一个 append 开关
// （那会让「读日志时到底在看几次运行」变成调用方的问题）。
// ============================================================================

enum LogLevel : u8
{
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_COUNT
};

// 初始化：**清空** path 对应的文件（见上面那段契约），并把这份日志固定到它
void log_init(const wchar_t *path);

void log_write(LogLevel level, const char *filename, int line_no, const char *fmt, ...);
void log_flush();

void log_shutdown();

// LOG_DEBUG 服务于所有调试模块，而每个模块有自己的开关 —— 所以它跟着“任意调试功能开启”走。
// 用一个具体模块的宏去门控它，会在“只开另一个模块”的配置下把日志静默关掉
// （例：MONO_DEBUG_INPUT=1 / MONO_DEBUG_BUILD=0 时，输入脚本的结果就一行都打不出来）。
// 编辑器那边四个宏都没定义，所以 LOG_DEBUG 在编辑器里是空宏，这是想要的（INFO/WARN/ERROR 照常用）。
#if MONO_DEBUG_ANY
#define LOG_DEBUG(...) log_write(LOG_LEVEL_DEBUG, __builtin_FILE(), __LINE__ __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

// LOG_INFO 走内存缓冲，正常退出（或 log_flush / 任何 WARN 以上）才落盘 ——
// 所以强杀进程会丢掉最后几行 INFO（同 AGENTS「空日志 ≠ 崩在早期」）。
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO, nullptr, 0 __VA_OPT__(, ) __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN, nullptr, 0 __VA_OPT__(, ) __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, nullptr, 0 __VA_OPT__(, ) __VA_ARGS__)
