/**
 * @file config.h
 * @brief Configuration definitions and platform detection for EthervoxAI
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 *
 * This file is part of EthervoxAI, licensed under CC BY-NC-SA 4.0.
 * You are free to share and adapt this work under the following terms:
 * - Attribution: Credit the original authors
 * - NonCommercial: Not for commercial use
 * - ShareAlike: Distribute under same license
 *
 * For full license terms, see: https://creativecommons.org/licenses/by-nc-sa/4.0/
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#ifndef ETHERVOX_CONFIG_H
#define ETHERVOX_CONFIG_H

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform detection macros
#if defined(TARGET_ESP32)
#define ETHERVOX_PLATFORM_ESP32 1
#define ETHERVOX_PLATFORM_EMBEDDED 1
#elif defined(TARGET_RPI)
#define ETHERVOX_PLATFORM_RPI 1
#define ETHERVOX_PLATFORM_EMBEDDED 1
#elif defined(TARGET_WINDOWS)
#define ETHERVOX_PLATFORM_WINDOWS 1
#define ETHERVOX_PLATFORM_DESKTOP 1
#elif defined(TARGET_LINUX)
#define ETHERVOX_PLATFORM_LINUX 1
#define ETHERVOX_PLATFORM_DESKTOP 1
#elif defined(TARGET_MACOS)
#define ETHERVOX_PLATFORM_MACOS 1
#define ETHERVOX_PLATFORM_DESKTOP 1
#endif

// Runtime directory structure
// Base directory is defined by CMake as ETHERVOX_RUNTIME_DIR_BASE
// Subdirectories are defined here

#ifndef ETHERVOX_RUNTIME_DIR_BASE
// Fallback if not defined by CMake
#if defined(ETHERVOX_PLATFORM_WINDOWS)
#define ETHERVOX_RUNTIME_DIR_BASE "%USERPROFILE%\\.ethervox"
#elif defined(ETHERVOX_PLATFORM_ANDROID)
#define ETHERVOX_RUNTIME_DIR_BASE NULL  // Handled at runtime via Java
#elif defined(ETHERVOX_PLATFORM_ESP32)
#define ETHERVOX_RUNTIME_DIR_BASE "/spiffs/ethervox"
#else
#define ETHERVOX_RUNTIME_DIR_BASE "~/.ethervox"
#endif
#endif

// Subdirectory paths (appended to ETHERVOX_RUNTIME_DIR_BASE)
#define ETHERVOX_MODELS_SUBDIR "/models"
#define ETHERVOX_MEMORY_SUBDIR "/memory"
#define ETHERVOX_TESTS_SUBDIR "/tests"
#define ETHERVOX_STARTUP_PROMPT_FILE "/startup_prompt.txt"

// Helper to construct full paths at runtime
// Example: get_ethervox_path(ETHERVOX_MODELS_SUBDIR) -> "~/.ethervox/models"

// Feature configuration
#define ETHERVOX_LANG_CODE_LEN 8
#define ETHERVOX_PLATFORM_NAME_LEN 32
#define ETHERVOX_PLATFORM_HW_REV_LEN 16
#define ETHERVOX_PLATFORM_CPU_MODEL_LEN 64
#define ETHERVOX_PLATFORM_ERROR_LEN 256
#define ETHERVOX_PLATFORM_ID_LEN 32
#define ETHERVOX_PLATFORM_DEFAULT_TEMP_C 45.0f
#define ETHERVOX_PLATFORM_MIC_COUNT 8
#define ETHERVOX_PLATFORM_MIC_SELECT_PINS 3
#define ETHERVOX_PLATFORM_US_PER_SEC 1000000ULL
#define ETHERVOX_PLATFORM_US_PER_MS 1000ULL

#ifndef ETHERVOX_MAX_LANGUAGES
#ifdef ETHERVOX_PLATFORM_EMBEDDED
#define ETHERVOX_MAX_LANGUAGES 3
#else
#define ETHERVOX_MAX_LANGUAGES 15
#endif
#endif

#ifndef ETHERVOX_AUDIO_SAMPLE_RATE
#define ETHERVOX_AUDIO_SAMPLE_RATE 16000
#endif

#ifndef ETHERVOX_AUDIO_CHANNELS_DEFAULT
#define ETHERVOX_AUDIO_CHANNELS_DEFAULT 1
#endif

#ifndef ETHERVOX_AUDIO_BITS_PER_SAMPLE
#define ETHERVOX_AUDIO_BITS_PER_SAMPLE 16
#endif

#ifndef ETHERVOX_AUDIO_BUFFER_SIZE
#ifdef ETHERVOX_PLATFORM_EMBEDDED
#define ETHERVOX_AUDIO_BUFFER_SIZE 1024
#else
#define ETHERVOX_AUDIO_BUFFER_SIZE 4096
#endif
#endif

#ifndef ETHERVOX_MAX_PLUGINS
#ifdef ETHERVOX_PLATFORM_EMBEDDED
#define ETHERVOX_MAX_PLUGINS 8
#else
#define ETHERVOX_MAX_PLUGINS 32
#endif
#endif

#ifndef ETHERVOX_LLM_MODEL_NAME
#define ETHERVOX_LLM_MODEL_NAME "ethervox-lite"
#endif

#ifndef ETHERVOX_LLM_MAX_TOKENS_DEFAULT
#define ETHERVOX_LLM_MAX_TOKENS_DEFAULT 512U
#endif

#ifndef ETHERVOX_LLM_CONTEXT_LENGTH_DEFAULT
#define ETHERVOX_LLM_CONTEXT_LENGTH_DEFAULT 2048U
#endif

#ifndef ETHERVOX_LLM_TEMPERATURE_DEFAULT
#define ETHERVOX_LLM_TEMPERATURE_DEFAULT 0.7f
#endif

#ifndef ETHERVOX_LLM_TOP_P_DEFAULT
#define ETHERVOX_LLM_TOP_P_DEFAULT 0.9f
#endif

#ifndef ETHERVOX_LLM_SEED_DEFAULT
#define ETHERVOX_LLM_SEED_DEFAULT 42U
#endif

#ifndef ETHERVOX_LLM_GPU_LAYERS_DEFAULT
#define ETHERVOX_LLM_GPU_LAYERS_DEFAULT 99U
#endif

#ifdef ETHERVOX_PLATFORM_DESKTOP
#ifndef ETHERVOX_LLM_MAX_TOKENS_DESKTOP
#define ETHERVOX_LLM_MAX_TOKENS_DESKTOP 1024U
#endif
#ifndef ETHERVOX_LLM_CONTEXT_LENGTH_DESKTOP
#define ETHERVOX_LLM_CONTEXT_LENGTH_DESKTOP 4096U
#endif
#ifndef ETHERVOX_LLM_GPU_LAYERS_DESKTOP
#define ETHERVOX_LLM_GPU_LAYERS_DESKTOP 10U
#endif
#else
#ifndef ETHERVOX_LLM_MAX_TOKENS_DESKTOP
#define ETHERVOX_LLM_MAX_TOKENS_DESKTOP ETHERVOX_LLM_MAX_TOKENS_DEFAULT
#endif
#ifndef ETHERVOX_LLM_CONTEXT_LENGTH_DESKTOP
#define ETHERVOX_LLM_CONTEXT_LENGTH_DESKTOP ETHERVOX_LLM_CONTEXT_LENGTH_DEFAULT
#endif
#ifndef ETHERVOX_LLM_GPU_LAYERS_DESKTOP
#define ETHERVOX_LLM_GPU_LAYERS_DESKTOP ETHERVOX_LLM_GPU_LAYERS_DEFAULT
#endif
#endif

// Android-specific LLM configuration (high performance mobile)
#ifdef ETHERVOX_PLATFORM_ANDROID
#ifndef ETHERVOX_LLM_MAX_TOKENS_ANDROID
#define ETHERVOX_LLM_MAX_TOKENS_ANDROID 200  // Longer responses for voice
#endif
#ifndef ETHERVOX_LLM_CONTEXT_LENGTH_ANDROID
#define ETHERVOX_LLM_CONTEXT_LENGTH_ANDROID 2048U  // Balance context vs memory
#endif
#ifndef ETHERVOX_LLM_GPU_LAYERS_ANDROID
#define ETHERVOX_LLM_GPU_LAYERS_ANDROID 99U  // Offload everything to GPU
#endif
#ifndef ETHERVOX_LLM_BATCH_SIZE_ANDROID
#define ETHERVOX_LLM_BATCH_SIZE_ANDROID 512U  // Batch size for generation (reduced for lower latency)
#endif
#ifndef ETHERVOX_LLM_PROMPT_BATCH_SIZE_ANDROID
#define ETHERVOX_LLM_PROMPT_BATCH_SIZE_ANDROID 64U  // Small batches for low latency (voice queries are short)
#endif
#ifndef ETHERVOX_LLM_MAX_RESPONSE_LENGTH_ANDROID
#define ETHERVOX_LLM_MAX_RESPONSE_LENGTH_ANDROID 4096U  // Maximum response buffer
#endif
#endif

// ===========================================================================
// Governor (Qwen2.5-3B-Instruct Quantized Tool Orchestration) Configuration
// ===========================================================================

#ifndef ETHERVOX_GOVERNOR_CONFIDENCE_THRESHOLD
#define ETHERVOX_GOVERNOR_CONFIDENCE_THRESHOLD 0.85f  // 85% confidence required
#endif

// ===========================================================================
// Whisper STT Configuration
// ===========================================================================

// Beam search settings
#ifndef ETHERVOX_WHISPER_BEAM_SIZE
#define ETHERVOX_WHISPER_BEAM_SIZE 5  // Number of beams for beam search (higher = more accurate but slower)
#endif

// Quality thresholds
#ifndef ETHERVOX_WHISPER_NO_SPEECH_THRESHOLD
#define ETHERVOX_WHISPER_NO_SPEECH_THRESHOLD 0.6f  // Threshold for filtering noise/silence (0.0-1.0)
#endif

#ifndef ETHERVOX_WHISPER_LOGPROB_THRESHOLD
#define ETHERVOX_WHISPER_LOGPROB_THRESHOLD -1.0f  // Confidence threshold for segments
#endif

#ifndef ETHERVOX_WHISPER_ENTROPY_THRESHOLD
#define ETHERVOX_WHISPER_ENTROPY_THRESHOLD 2.4f  // Entropy threshold for filtering uncertain predictions
#endif

// Temperature settings for decoding fallback
#ifndef ETHERVOX_WHISPER_TEMPERATURE_START
#define ETHERVOX_WHISPER_TEMPERATURE_START 0.0f  // Start with greedy decoding (deterministic)
#endif

#ifndef ETHERVOX_WHISPER_TEMPERATURE_INCREMENT
#define ETHERVOX_WHISPER_TEMPERATURE_INCREMENT 0.2f  // Increase temperature if decoding fails
#endif

// Streaming chunk size (in samples at 16kHz)
#ifndef ETHERVOX_WHISPER_CHUNK_SIZE
#define ETHERVOX_WHISPER_CHUNK_SIZE 480000  // 30 seconds at 16kHz
#endif

// Overlap buffer for context continuity (in samples at 16kHz)
#ifndef ETHERVOX_WHISPER_OVERLAP_SIZE
#define ETHERVOX_WHISPER_OVERLAP_SIZE 3200  // 200ms at 16kHz
#endif

// ===========================================================================
// Speaker Detection Configuration
// ===========================================================================

// Acoustic feature thresholds for speaker change detection
#ifndef ETHERVOX_SPEAKER_ENERGY_CHANGE_THRESHOLD
#define ETHERVOX_SPEAKER_ENERGY_CHANGE_THRESHOLD 0.3f  // 30% change in RMS energy
#endif

#ifndef ETHERVOX_SPEAKER_PITCH_CHANGE_THRESHOLD
#define ETHERVOX_SPEAKER_PITCH_CHANGE_THRESHOLD 0.12f  // 12% change in estimated pitch
#endif

#ifndef ETHERVOX_SPEAKER_PAUSE_THRESHOLD
#define ETHERVOX_SPEAKER_PAUSE_THRESHOLD 50  // 500ms pause (in 10ms units)
#endif

#ifndef ETHERVOX_SPEAKER_CHANGE_MIN_FACTORS
#define ETHERVOX_SPEAKER_CHANGE_MIN_FACTORS 2  // Minimum factors required to confirm speaker change
#endif

// Maximum speakers to track in a session
#ifndef ETHERVOX_SPEAKER_MAX_SPEAKERS
#define ETHERVOX_SPEAKER_MAX_SPEAKERS 10
#endif

// Number of example quotes to show per speaker during identification
#ifndef ETHERVOX_SPEAKER_EXAMPLE_QUOTES
#define ETHERVOX_SPEAKER_EXAMPLE_QUOTES 3
#endif

#ifndef ETHERVOX_GOVERNOR_MAX_ITERATIONS
#define ETHERVOX_GOVERNOR_MAX_ITERATIONS 10  // Maximum reasoning iterations
#endif

#ifndef ETHERVOX_GOVERNOR_TIMEOUT_SECONDS
#define ETHERVOX_GOVERNOR_TIMEOUT_SECONDS 300  // Maximum execution time
#endif

#ifndef ETHERVOX_GOVERNOR_GPU_LAYERS
#define ETHERVOX_GOVERNOR_GPU_LAYERS 999  // Full GPU offloading
#endif

#ifndef ETHERVOX_GOVERNOR_CONTEXT_SIZE
#define ETHERVOX_GOVERNOR_CONTEXT_SIZE 8192  // Larger context for tool orchestration
#endif

#ifndef ETHERVOX_GOVERNOR_BATCH_SIZE
#define ETHERVOX_GOVERNOR_BATCH_SIZE 1024  // Larger batch for faster inference
#endif

#ifndef ETHERVOX_GOVERNOR_THREADS
#define ETHERVOX_GOVERNOR_THREADS 8  // More threads for small model
#endif

#ifndef ETHERVOX_GOVERNOR_TEMPERATURE
#define ETHERVOX_GOVERNOR_TEMPERATURE 0.3f  // Low temperature for deterministic tool use
#endif

#ifndef ETHERVOX_GOVERNOR_MAX_TOKENS_PER_ITERATION
#define ETHERVOX_GOVERNOR_MAX_TOKENS_PER_ITERATION 64  // Very concise responses - most tool calls are <20 tokens
#endif

#ifndef ETHERVOX_GOVERNOR_USE_MMAP
#define ETHERVOX_GOVERNOR_USE_MMAP false  // Original: load into RAM
#endif

#ifndef ETHERVOX_GOVERNOR_KV_CACHE_TYPE
#define ETHERVOX_GOVERNOR_KV_CACHE_TYPE GGML_TYPE_Q8_0  // Q8_0 quantized for memory savings (requires flash attention)
#endif

#ifndef ETHERVOX_GOVERNOR_FLASH_ATTN_TYPE
#define ETHERVOX_GOVERNOR_FLASH_ATTN_TYPE -1  // -1=AUTO, 0=DISABLED, 1=ENABLED (required for quantized V cache)
#endif

#ifndef ETHERVOX_GOVERNOR_REPETITION_PENALTY
#define ETHERVOX_GOVERNOR_REPETITION_PENALTY 1.1f  // Penalty for repeating tokens (1.0 = no penalty)
#endif

#ifndef ETHERVOX_GOVERNOR_FREQUENCY_PENALTY
#define ETHERVOX_GOVERNOR_FREQUENCY_PENALTY 0.0f  // Penalty based on token frequency
#endif

#ifndef ETHERVOX_GOVERNOR_PRESENCE_PENALTY
#define ETHERVOX_GOVERNOR_PRESENCE_PENALTY 0.0f  // Penalty for tokens already present
#endif

#ifndef ETHERVOX_GOVERNOR_PENALTY_LAST_N
#define ETHERVOX_GOVERNOR_PENALTY_LAST_N 64  // Apply penalties to last N tokens
#endif

// Debug configuration
#ifdef DEBUG_ENABLED
#define ETHERVOX_DEBUG 1
#define ETHERVOX_LOG_LEVEL 0  // Verbose
#else
#define ETHERVOX_DEBUG 0
#define ETHERVOX_LOG_LEVEL 2  // Error only
#endif

// Runtime debug mode control (can be toggled at runtime)
extern int g_ethervox_debug_enabled;

// C log callback for sending logs to Java/Kotlin debug window
typedef void (*ethervox_log_callback_t)(int level, const char* tag, const char* message);
extern ethervox_log_callback_t g_ethervox_log_callback;

// Forward declare logging helper  
void ethervox_log_with_callback(int level, const char* tag, const char* fmt, ...);

// Logging macros that respect runtime debug flag
#if defined(__ANDROID__)
  #include <android/log.h>
  #define ETHERVOX_LOG_TAG "EthervoxCore"
  #define ETHERVOX_LOGD(...) ethervox_log_with_callback(ANDROID_LOG_DEBUG, ETHERVOX_LOG_TAG, __VA_ARGS__)
  #define ETHERVOX_LOGI(...) ethervox_log_with_callback(ANDROID_LOG_INFO, ETHERVOX_LOG_TAG, __VA_ARGS__)
  #define ETHERVOX_LOGW(...) ethervox_log_with_callback(ANDROID_LOG_WARN, ETHERVOX_LOG_TAG, __VA_ARGS__)
  #define ETHERVOX_LOGE(...) ethervox_log_with_callback(ANDROID_LOG_ERROR, ETHERVOX_LOG_TAG, __VA_ARGS__)
#else
  #include <stdio.h>
  // Helper to get timestamp for legacy logging (platform-agnostic)
  static inline void ethervox_log_timestamp(char* buf, size_t size) {
    time_t now = time(NULL);
    struct tm tm_info;
    #ifdef _WIN32
      localtime_s(&tm_info, &now);
    #else
      localtime_r(&now, &tm_info);
    #endif
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_info);
  }
  // Legacy logging macros - use stderr with timestamps to match new logging system
  #define ETHERVOX_LOGD(...) do { if (g_ethervox_debug_enabled) { \
    char ts[20]; ethervox_log_timestamp(ts, sizeof(ts)); \
    fprintf(stderr, "[%s] [DEBUG] ", ts); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); \
  } } while(0)
  #define ETHERVOX_LOGI(...) do { if (g_ethervox_debug_enabled) { \
    char ts[20]; ethervox_log_timestamp(ts, sizeof(ts)); \
    fprintf(stderr, "[%s] [INFO ] ", ts); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); \
  } } while(0)
  #define ETHERVOX_LOGW(...) do { if (g_ethervox_debug_enabled) { \
    char ts[20]; ethervox_log_timestamp(ts, sizeof(ts)); \
    fprintf(stderr, "[%s] [WARN ] ", ts); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); \
  } } while(0)
  #define ETHERVOX_LOGE(...) do { if (g_ethervox_debug_enabled) { \
    char ts[20]; ethervox_log_timestamp(ts, sizeof(ts)); \
    fprintf(stderr, "[%s] [ERROR] ", ts); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); \
  } } while(0)
#endif

// Version information (single source of truth)
#define ETHERVOX_VERSION_MAJOR 0
#define ETHERVOX_VERSION_MINOR 0
#define ETHERVOX_VERSION_PATCH 4
#define ETHERVOX_BUILD_TYPE "Engineering"

// Build version string from components
#define ETHERVOX_STRINGIFY(x) #x
#define ETHERVOX_TOSTRING(x) ETHERVOX_STRINGIFY(x)
#define ETHERVOX_VERSION_STRING \
  ETHERVOX_TOSTRING(ETHERVOX_VERSION_MAJOR) "." \
  ETHERVOX_TOSTRING(ETHERVOX_VERSION_MINOR) "." \
  ETHERVOX_TOSTRING(ETHERVOX_VERSION_PATCH)

/**
 * @brief Get runtime directory path
 * @param subdir Subdirectory constant (e.g., ETHERVOX_MODELS_SUBDIR) or NULL for base
 * @param out Output buffer
 * @param out_size Size of output buffer
 * @return 0 on success, -1 on failure
 * 
 * Example:
 *   char path[512];
 *   ethervox_get_runtime_path(ETHERVOX_MODELS_SUBDIR, path, sizeof(path));
 *   // Result: "~/.ethervox/models" or "$HOME/.ethervox/models" (expanded)
 */
static inline int ethervox_get_runtime_path(const char* subdir, char* out, size_t out_size);

static inline int ethervox_get_runtime_path(const char* subdir, char* out, size_t out_size) {
    if (!out || out_size == 0) { return -1; }
    
    const char* base = ETHERVOX_RUNTIME_DIR_BASE;
    
#if defined(ETHERVOX_PLATFORM_ANDROID)
    // Android: should be set at runtime, not compile time
    return -1;  // Caller must handle Android paths via JNI
#else
    // Expand ~ or %USERPROFILE% if needed
    if (base && base[0] == '~') {
        const char* home = getenv("HOME");
        if (home) {
            if (subdir) {
                snprintf(out, out_size, "%s%s%s", home, base + 1, subdir);
            } else {
                snprintf(out, out_size, "%s%s", home, base + 1);
            }
        } else {
            return -1;
        }
    } else if (base) {
        if (subdir) {
            snprintf(out, out_size, "%s%s", base, subdir);
        } else {
            snprintf(out, out_size, "%s", base);
        }
    } else {
        return -1;
    }
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif

#endif  // ETHERVOX_CONFIG_H