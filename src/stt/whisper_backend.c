/**
 * @file whisper_backend.c
 * @brief Whisper.cpp backend implementation for STT
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * Licensed under CC BY-NC-SA 4.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ethervox/stt.h"
#include "ethervox/logging.h"

#ifdef WHISPER_CPP_AVAILABLE
#include "whisper.h"

// Logging helper macro
#define LOG_ERROR(...) ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  ethervox_log(ETHERVOX_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)  ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_DEBUG(...) ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

/**
 * Whisper backend context
 */
typedef struct {
  struct whisper_context* ctx;
  struct whisper_full_params params;
  
  // Accumulated audio buffer for streaming
  float* audio_buffer;
  size_t audio_buffer_size;
  size_t audio_buffer_capacity;
  
  // Segmentation for speaker detection
  int64_t last_segment_end_ms;
  
  // Speaker detection state
  float* energy_history;
  size_t energy_history_size;
  float avg_energy;
  
} whisper_backend_context_t;

/**
 * Calculate RMS energy of audio buffer
 */
static float calculate_rms_energy(const float* samples, size_t n_samples) {
  if (!samples || n_samples == 0) return 0.0f;
  
  double sum = 0.0;
  for (size_t i = 0; i < n_samples; i++) {
    sum += samples[i] * samples[i];
  }
  return (float)sqrt(sum / n_samples);
}

/**
 * Detect speaker change based on energy and pause analysis
 */
static bool detect_speaker_change(whisper_backend_context_t* ctx,
                                   float segment_energy,
                                   int64_t pause_duration_ms) {
  if (!ctx || ctx->energy_history_size == 0) return false;
  
  // Detect speaker change if:
  // 1. Long pause (>1s) between segments
  // 2. Significant energy shift (>30%)
  
  bool long_pause = pause_duration_ms > 1000;
  
  float energy_ratio = segment_energy / (ctx->avg_energy + 1e-6f);
  bool energy_shift = (energy_ratio < 0.7f) || (energy_ratio > 1.3f);
  
  return long_pause || energy_shift;
}

/**
 * Initialize Whisper backend
 */
int ethervox_stt_whisper_init(ethervox_stt_runtime_t* runtime) {
  if (!runtime) {
    LOG_ERROR( "whisper_backend", "Runtime is NULL");
    return -1;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)calloc(1, sizeof(whisper_backend_context_t));
  if (!ctx) {
    LOG_ERROR( "whisper_backend", "Failed to allocate context");
    return -1;
  }
  
  // Initialize Whisper context
  struct whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = false; // CPU-only for now
  
  const char* model_path = runtime->config.model_path;
  if (!model_path || strlen(model_path) == 0) {
    model_path = "models/whisper/base.en.bin";
  }
  
  LOG_INFO( "whisper_backend", "Loading model: %s", model_path);
  
  ctx->ctx = whisper_init_from_file_with_params(model_path, cparams);
  if (!ctx->ctx) {
    LOG_ERROR( "whisper_backend", "Failed to load Whisper model from %s", model_path);
    free(ctx);
    return -1;
  }
  
  // Initialize full params
  ctx->params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  
  // Configure for real-time streaming
  ctx->params.n_threads = 4;
  ctx->params.print_progress = false;
  ctx->params.print_realtime = false;
  ctx->params.print_timestamps = false;
  ctx->params.translate = false;
  
  // Set language from config (supports hot-switching)
  // Parse language code: "en-US" -> "en", "zh-CN" -> "zh", etc.
  const char* lang = runtime->config.language;
  if (lang && strlen(lang) >= 2) {
    // Extract first 2 chars as language code
    static char lang_code[3];
    lang_code[0] = lang[0];
    lang_code[1] = lang[1];
    lang_code[2] = '\0';
    ctx->params.language = lang_code;
    LOG_INFO( "whisper_backend", "Language set to: %s (from %s)", lang_code, lang);
  } else {
    ctx->params.language = "en";
    LOG_INFO( "whisper_backend", "Language defaulted to: en");
  }
  
  // Enable auto-detection if language is "auto"
  if (lang && (strcmp(lang, "auto") == 0 || strcmp(lang, "AUTO") == 0)) {
    ctx->params.detect_language = true;
    ctx->params.language = NULL;
    LOG_INFO( "whisper_backend", "Auto language detection enabled");
  } else {
    ctx->params.detect_language = false;
  }
  
  // Enable token-level timestamps for better segmentation
  ctx->params.token_timestamps = true;
  ctx->params.thold_pt = 0.01f;
  ctx->params.thold_ptsum = 0.01f;
  
  // Enable speaker turn detection (experimental)
  ctx->params.tdrz_enable = true;
  
  // No context carry-over for better speaker segmentation
  ctx->params.no_context = false;
  ctx->params.single_segment = false;
  
  // Suppress non-speech tokens
  ctx->params.suppress_blank = true;
  ctx->params.suppress_nst = true;
  
  // Temperature settings
  ctx->params.temperature = 0.0f;
  ctx->params.temperature_inc = 0.2f;
  
  // Allocate audio buffer (30 seconds at 16kHz)
  ctx->audio_buffer_capacity = 16000 * 30;
  ctx->audio_buffer = (float*)calloc(ctx->audio_buffer_capacity, sizeof(float));
  if (!ctx->audio_buffer) {
    whisper_free(ctx->ctx);
    free(ctx);
    LOG_ERROR( "whisper_backend", "Failed to allocate audio buffer");
    return -1;
  }
  
  // Allocate energy history (for speaker detection)
  ctx->energy_history = (float*)calloc(100, sizeof(float));
  ctx->energy_history_size = 0;
  ctx->avg_energy = 0.0f;
  
  runtime->backend_context = ctx;
  
  LOG_INFO( "whisper_backend", "Whisper.cpp backend initialized successfully");
  LOG_INFO( "whisper_backend", "Speaker detection: ENABLED (energy-based + pause detection)");
  
  return 0;
}

/**
 * Start Whisper session
 */
int ethervox_stt_whisper_start(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->backend_context) {
    return -1;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  // Reset buffers
  ctx->audio_buffer_size = 0;
  ctx->last_segment_end_ms = 0;
  ctx->energy_history_size = 0;
  ctx->avg_energy = 0.0f;
  
  LOG_INFO( "whisper_backend", "Whisper session started");
  
  return 0;
}

/**
 * Process audio chunk with Whisper
 */
int ethervox_stt_whisper_process(ethervox_stt_runtime_t* runtime,
                                  const ethervox_audio_buffer_t* audio_buffer,
                                  ethervox_stt_result_t* result) {
  if (!runtime || !runtime->backend_context || !audio_buffer || !result) {
    return -1;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  memset(result, 0, sizeof(ethervox_stt_result_t));
  
  // Convert int16 to float and accumulate
  const int16_t* samples = (const int16_t*)audio_buffer->data;
  uint32_t sample_count = audio_buffer->size / sizeof(int16_t);
  
  for (uint32_t i = 0; i < sample_count && ctx->audio_buffer_size < ctx->audio_buffer_capacity; i++) {
    ctx->audio_buffer[ctx->audio_buffer_size++] = (float)samples[i] / 32768.0f;
  }
  
  // Process when we have at least 3 seconds of audio
  if (ctx->audio_buffer_size < 16000 * 3) {
    return 1; // No result yet
  }
  
  LOG_DEBUG( "whisper_backend", "Processing %zu samples (%.1f seconds)",
               ctx->audio_buffer_size, (float)ctx->audio_buffer_size / 16000.0f);
  
  // Run Whisper inference
  if (whisper_full(ctx->ctx, ctx->params, ctx->audio_buffer, ctx->audio_buffer_size) != 0) {
    LOG_ERROR( "whisper_backend", "Failed to process audio");
    return -1;
  }
  
  // Get number of segments
  const int n_segments = whisper_full_n_segments(ctx->ctx);
  if (n_segments == 0) {
    LOG_DEBUG( "whisper_backend", "No segments detected");
    return 1; // No result yet
  }
  
  LOG_DEBUG( "whisper_backend", "Detected %d segments", n_segments);
  
  // Build combined transcript with speaker detection
  size_t total_len = 0;
  for (int i = 0; i < n_segments; i++) {
    const char* text = whisper_full_get_segment_text(ctx->ctx, i);
    total_len += strlen(text) + 50; // Extra space for speaker labels and formatting
  }
  
  char* transcript = (char*)calloc(total_len + 1, sizeof(char));
  if (!transcript) {
    return -1;
  }
  
  int current_speaker = 0;
  int64_t last_end_time = 0;
  
  for (int i = 0; i < n_segments; i++) {
    const char* text = whisper_full_get_segment_text(ctx->ctx, i);
    int64_t t0 = whisper_full_get_segment_t0(ctx->ctx, i);
    int64_t t1 = whisper_full_get_segment_t1(ctx->ctx, i);
    
    // Calculate pause duration
    int64_t pause_ms = (i > 0) ? (t0 - last_end_time) / 10 : 0; // centiseconds to ms
    last_end_time = t1;
    
    // Calculate energy for this segment
    size_t seg_start = (t0 * 16) / 1000; // Convert cs to samples
    size_t seg_end = (t1 * 16) / 1000;
    if (seg_end > ctx->audio_buffer_size) seg_end = ctx->audio_buffer_size;
    
    float seg_energy = 0.0f;
    if (seg_end > seg_start) {
      seg_energy = calculate_rms_energy(ctx->audio_buffer + seg_start, seg_end - seg_start);
    }
    
    // Update energy history
    if (ctx->energy_history_size < 100) {
      ctx->energy_history[ctx->energy_history_size++] = seg_energy;
    }
    
    // Calculate average energy
    ctx->avg_energy = 0.0f;
    for (size_t j = 0; j < ctx->energy_history_size; j++) {
      ctx->avg_energy += ctx->energy_history[j];
    }
    ctx->avg_energy /= ctx->energy_history_size;
    
    // Detect speaker change
    bool speaker_change = detect_speaker_change(ctx, seg_energy, pause_ms);
    
    // Check Whisper's speaker turn detection
    if (whisper_full_get_segment_speaker_turn_next(ctx->ctx, i)) {
      speaker_change = true;
    }
    
    if (speaker_change && i > 0) {
      current_speaker = 1 - current_speaker; // Toggle between 0 and 1
      LOG_DEBUG( "whisper_backend", "Speaker change detected at segment %d (pause: %lldms, energy ratio: %.2f)",
                   i, pause_ms, seg_energy / (ctx->avg_energy + 1e-6f));
    }
    
    // Format with speaker label
    char seg_text[512];
    snprintf(seg_text, sizeof(seg_text), "[Speaker %d, %02lld:%02lld-%02lld:%02lld] %s\n",
             current_speaker,
             t0 / 6000, (t0 / 100) % 60,
             t1 / 6000, (t1 / 100) % 60,
             text);
    
    strcat(transcript, seg_text);
  }
  
  // Set result
  result->text = transcript;
  result->confidence = 0.9f; // TODO: Calculate actual confidence from Whisper token probabilities
  result->is_partial = false;
  result->is_final = true;
  result->start_time_us = audio_buffer->timestamp_us;
  result->end_time_us = audio_buffer->timestamp_us + (ctx->audio_buffer_size * 1000000ULL) / 16000;
  result->language = "en";
  
  LOG_INFO( "whisper_backend", "Transcription complete: %d segments, %zu chars",
               n_segments, strlen(transcript));
  
  // Reset buffer for next chunk (keep last 1 second for context)
  size_t keep_samples = 16000;
  if (ctx->audio_buffer_size > keep_samples) {
    memmove(ctx->audio_buffer, 
            ctx->audio_buffer + (ctx->audio_buffer_size - keep_samples),
            keep_samples * sizeof(float));
    ctx->audio_buffer_size = keep_samples;
  }
  
  return 0;
}

/**
 * Finalize Whisper session
 */
int ethervox_stt_whisper_finalize(ethervox_stt_runtime_t* runtime, ethervox_stt_result_t* result) {
  if (!runtime || !runtime->backend_context || !result) {
    return -1;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  memset(result, 0, sizeof(ethervox_stt_result_t));
  
  // Process any remaining audio
  if (ctx->audio_buffer_size > 0) {
    LOG_INFO( "whisper_backend", "Finalizing with %zu remaining samples", ctx->audio_buffer_size);
    
    if (whisper_full(ctx->ctx, ctx->params, ctx->audio_buffer, ctx->audio_buffer_size) == 0) {
      const int n_segments = whisper_full_n_segments(ctx->ctx);
      
      if (n_segments > 0) {
        size_t total_len = 0;
        for (int i = 0; i < n_segments; i++) {
          total_len += strlen(whisper_full_get_segment_text(ctx->ctx, i)) + 50;
        }
        
        result->text = (char*)calloc(total_len + 1, sizeof(char));
        if (result->text) {
          for (int i = 0; i < n_segments; i++) {
            strcat(result->text, whisper_full_get_segment_text(ctx->ctx, i));
            strcat(result->text, " ");
          }
        }
      }
    }
  }
  
  if (!result->text) {
    result->text = strdup("[Final] No additional transcription");
  }
  
  result->confidence = 0.9f;
  result->is_partial = false;
  result->is_final = true;
  result->language = "en";
  
  return 0;
}

/**
 * Stop Whisper session
 */
void ethervox_stt_whisper_stop(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->backend_context) {
    return;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  ctx->audio_buffer_size = 0;
  
  LOG_INFO( "whisper_backend", "Whisper session stopped");
}

/**
 * Cleanup Whisper backend
 */
void ethervox_stt_whisper_cleanup(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->backend_context) {
    return;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  if (ctx->ctx) {
    whisper_free(ctx->ctx);
  }
  
  if (ctx->audio_buffer) {
    free(ctx->audio_buffer);
  }
  
  if (ctx->energy_history) {
    free(ctx->energy_history);
  }
  
  free(ctx);
  runtime->backend_context = NULL;
  
  LOG_INFO( "whisper_backend", "Whisper backend cleaned up");
}

/**
 * Set language for Whisper (hot-switch)
 */
int ethervox_stt_whisper_set_language(ethervox_stt_runtime_t* runtime, const char* language) {
  if (!runtime || !runtime->backend_context) {
    LOG_ERROR( "whisper_backend", "Invalid runtime for language switch");
    return -1;
  }
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  if (!language) {
    LOG_ERROR( "whisper_backend", "Language is NULL");
    return -1;
  }
  
  // Handle auto-detection
  if (strcmp(language, "auto") == 0 || strcmp(language, "AUTO") == 0) {
    ctx->params.detect_language = true;
    ctx->params.language = NULL;
    LOG_INFO( "whisper_backend", "Switched to auto language detection");
    return 0;
  }
  
  // Extract language code (first 2 chars)
  static char lang_code[3];
  lang_code[0] = language[0];
  lang_code[1] = language[1];
  lang_code[2] = '\0';
  
  ctx->params.language = lang_code;
  ctx->params.detect_language = false;
  
  LOG_INFO( "whisper_backend", "Language switched to: %s", lang_code);
  return 0;
}

#else
// Stub implementations when whisper.cpp is not available

int ethervox_stt_whisper_init(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  printf("Error: Whisper.cpp not available (compiled without WHISPER_CPP_AVAILABLE)\n");
  return -1;
}

int ethervox_stt_whisper_start(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  return -1;
}

int ethervox_stt_whisper_process(ethervox_stt_runtime_t* runtime,
                                  const ethervox_audio_buffer_t* audio_buffer,
                                  ethervox_stt_result_t* result) {
  (void)runtime;
  (void)audio_buffer;
  (void)result;
  return -1;
}

int ethervox_stt_whisper_finalize(ethervox_stt_runtime_t* runtime, ethervox_stt_result_t* result) {
  (void)runtime;
  (void)result;
  return -1;
}

void ethervox_stt_whisper_stop(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
}

void ethervox_stt_whisper_cleanup(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
}

#endif // WHISPER_CPP_AVAILABLE
