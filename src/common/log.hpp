#pragma once
// 极简日志：带时间戳 + 级别前缀，输出到 stdout
#include <cstdio>
#include <cstdarg>
#include <ctime>

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR };

inline void log_print(LogLevel lv, const char* fmt, ...) {
    char ts[32] = {0};
    time_t t = time(nullptr);
    struct tm* tm_ = localtime(&t);
    if (tm_) strftime(ts, sizeof(ts), "%H:%M:%S", tm_);

    const char* tag = (lv >= LogLevel::ERROR) ? "E"
                    : (lv >= LogLevel::WARN)  ? "W"
                    : (lv >= LogLevel::INFO)  ? "I" : "D";
    printf("[%s][%s] ", ts, tag);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

#define LOG_D(...) log_print(LogLevel::DEBUG, __VA_ARGS__)
#define LOG_I(...) log_print(LogLevel::INFO,  __VA_ARGS__)
#define LOG_W(...) log_print(LogLevel::WARN,  __VA_ARGS__)
#define LOG_E(...) log_print(LogLevel::ERROR, __VA_ARGS__)
