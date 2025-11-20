// SPDX-License-Identifier: CC-BY-NC-SA-4.0
#include "ethervox/logging.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

static ethervox_log_level_t g_log_level = ETHERVOX_LOG_LEVEL_INFO;
int g_ethervox_debug_enabled = 1;

void ethervox_log_set_level(ethervox_log_level_t level) {
    g_log_level = level;
}

ethervox_log_level_t ethervox_log_get_level(void) {
    return g_log_level;
}

// ANSI color codes
#define COLOR_RESET    "\033[0m"
#define COLOR_TRACE    "\033[36m"   // Cyan
#define COLOR_DEBUG    "\033[34m"   // Blue
#define COLOR_INFO     "\033[32m"   // Green
#define COLOR_WARN     "\033[33m"   // Yellow
#define COLOR_ERROR    "\033[31m"   // Red
#define COLOR_FATAL    "\033[35m"   // Magenta

static const char* log_level_string(ethervox_log_level_t level) {
    switch (level) {
        case ETHERVOX_LOG_LEVEL_TRACE: return "TRACE";
        case ETHERVOX_LOG_LEVEL_DEBUG: return "DEBUG";
        case ETHERVOX_LOG_LEVEL_INFO:  return "INFO ";
        case ETHERVOX_LOG_LEVEL_WARN:  return "WARN ";
        case ETHERVOX_LOG_LEVEL_ERROR: return "ERROR";
        case ETHERVOX_LOG_LEVEL_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

static const char* log_level_color(ethervox_log_level_t level) {
    switch (level) {
        case ETHERVOX_LOG_LEVEL_TRACE: return COLOR_TRACE;
        case ETHERVOX_LOG_LEVEL_DEBUG: return COLOR_DEBUG;
        case ETHERVOX_LOG_LEVEL_INFO:  return COLOR_INFO;
        case ETHERVOX_LOG_LEVEL_WARN:  return COLOR_WARN;
        case ETHERVOX_LOG_LEVEL_ERROR: return COLOR_ERROR;
        case ETHERVOX_LOG_LEVEL_FATAL: return COLOR_FATAL;
        default: return COLOR_RESET;
    }
}

static const char* extract_filename(const char* path) {
    const char* filename = strrchr(path, '/');
    if (filename) {
        return filename + 1;
    }
    filename = strrchr(path, '\\');
    if (filename) {
        return filename + 1;
    }
    return path;
}

void ethervox_log(ethervox_log_level_t level, const char* file, int line, 
                  const char* func, const char* fmt, ...) {
    if (level < g_log_level) {
        return;
    }
    
    // Get timestamp
    time_t now = time(NULL);
    struct tm tm_info;
    char timestamp[20];
#ifdef _WIN32
    localtime_s(&tm_info, &now);  // Windows: different parameter order
#else
    localtime_r(&now, &tm_info);  // POSIX
#endif
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
    
    // Get color for this level
    const char* color = log_level_color(level);
    
    // Print log prefix with color
    fprintf(stderr, "%s[%s] [%s] [%s:%d %s] ", 
            color,
            timestamp,
            log_level_string(level),
            extract_filename(file),
            line,
            func);
    
    // Print message in color
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    // Reset color at end of line
    fprintf(stderr, "%s\n", COLOR_RESET);
    fflush(stderr);
}

void ethervox_log_error_context(const ethervox_error_context_t* ctx) {
    if (!ctx) {
        return;
    }
    
    fprintf(stderr, "%s[ERROR CONTEXT]\n", COLOR_ERROR);
    fprintf(stderr, "  Code: %d (%s)\n", ctx->code, ethervox_error_string(ctx->code));
    if (ctx->message) {
        fprintf(stderr, "  Message: %s\n", ctx->message);
    }
    fprintf(stderr, "  Location: %s:%d in %s()\n", 
            extract_filename(ctx->file), ctx->line, ctx->function);
    fprintf(stderr, "  Timestamp: %llu ms%s\n", (unsigned long long)ctx->timestamp_ms, COLOR_RESET);
    fflush(stderr);
}