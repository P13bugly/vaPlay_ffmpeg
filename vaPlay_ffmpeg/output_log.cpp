#include "output_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#define MAX_BUF_LEN 1024

// 这些全局开关控制不同级别的日志是否输出。
int g_log_debug_flag = 1;
int g_log_info_flag = 1;
int g_log_warnning_flag = 1;
int g_log_error_flag = 1;

void set_log_flag(int log_debug_flag, int log_info_flag, int log_warnning_flag,
    int log_error_flag)
{
    g_log_debug_flag = log_debug_flag;
    g_log_info_flag = log_info_flag;
    g_log_warnning_flag = log_warnning_flag;
    g_log_error_flag = log_error_flag;
}

void output_log(LOG_LEVEL log_level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    // 先把可变参数格式化成一段普通字符串，后面统一输出。
    char buf[MAX_BUF_LEN] = { 0 };
    vsnprintf(buf, MAX_BUF_LEN - 1, fmt, args);

    switch (log_level)
    {
    case LOG_DEBUG:
        if (g_log_debug_flag)
            printf("[Log-Debug]:%s\n", buf);
        break;
    case LOG_INFO:
        if (g_log_info_flag)
            printf("[Log-Info]:%s\n", buf);
        break;
    case LOG_WARNING:
        if (g_log_warnning_flag)
            printf("[Log-Warning]:%s\n", buf);
        break;
    case LOG_ERROR:
        if (g_log_error_flag)
            printf("[Log-Error]:%s\n", buf);
        break;
    default:
        break;
    }

    // 同时把日志落到文件里，方便“程序一闪而过”时排查。
    FILE* fp = nullptr;
    if (fopen_s(&fp, "D:\\develop\\vs2026\\code\\vaPlay_ffmpeg\\run.log", "a") == 0 && fp)
    {
        const char* level_str = "Unknown";
        switch (log_level)
        {
        case LOG_DEBUG:
            level_str = "Debug";
            break;
        case LOG_INFO:
            level_str = "Info";
            break;
        case LOG_WARNING:
            level_str = "Warning";
            break;
        case LOG_ERROR:
            level_str = "Error";
            break;
        default:
            break;
        }

        std::fprintf(fp, "[Log-%s]:%s\n", level_str, buf);
        std::fclose(fp);
    }

    va_end(args);
}
