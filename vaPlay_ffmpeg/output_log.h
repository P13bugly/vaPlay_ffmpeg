#pragma once

// 日志级别：用于控制一条日志属于调试、信息、警告还是错误。
enum LOG_LEVEL
{
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
};

// 设置不同日志级别是否打印。
void set_log_flag(int log_debug_flag, int log_info_flag, int log_warnning_flag, int log_error_flag);

// 输出一条格式化日志，使用方式和 printf 类似。
void output_log(LOG_LEVEL log_level, const char* fmt, ...);
