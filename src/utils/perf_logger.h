#pragma once

#include <cstdarg>
#include <string>

bool perf_logger_init(const std::string &log_path);
void perf_logger_close();
void perf_logger_vlog(const char *fmt, va_list args);
void perf_logger_log(const char *fmt, ...);
