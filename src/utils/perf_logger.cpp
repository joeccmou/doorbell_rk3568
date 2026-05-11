#include "perf_logger.h"

#include <cstdio>
#include <mutex>

namespace {
std::mutex g_perf_log_mtx;
FILE *g_perf_log_fp = nullptr;
}

bool perf_logger_init(const std::string &log_path) {
    std::lock_guard<std::mutex> lock(g_perf_log_mtx);
    if (g_perf_log_fp) {
        std::fclose(g_perf_log_fp);
        g_perf_log_fp = nullptr;
    }
    g_perf_log_fp = std::fopen(log_path.c_str(), "w");
    if (!g_perf_log_fp) {
        std::fprintf(stderr, "[perf] failed to open log file: %s\n", log_path.c_str());
        return false;
    }
    std::fprintf(stdout, "[perf] log file: %s\n", log_path.c_str());
    return true;
}

void perf_logger_close() {
    std::lock_guard<std::mutex> lock(g_perf_log_mtx);
    if (g_perf_log_fp) {
        std::fclose(g_perf_log_fp);
        g_perf_log_fp = nullptr;
    }
}

void perf_logger_vlog(const char *fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_perf_log_mtx);
    if (!g_perf_log_fp) return;
    std::vfprintf(g_perf_log_fp, fmt, args);
    std::fflush(g_perf_log_fp);
}

void perf_logger_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    perf_logger_vlog(fmt, args);
    va_end(args);
}
