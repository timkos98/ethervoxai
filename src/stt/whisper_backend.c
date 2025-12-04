/**
 * @file whisper_backend.c
 * @brief Minimal Whisper.cpp backend implementation for STT
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

#define LOG_ERROR(...) ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  ethervox_log(ETHERVOX_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)  ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_DEBUG(...) ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

/**
 * Minimal Whisper context - streaming with overlap
 */
typedef struct {
  struct whisper_context* ctx;
  struct whisper_full_params params;
  float* audio_buffer;        // Main accumulation buffer
  size_t audio_buffer_size;
  size_t audio_buffer_capacity;
  
  // Overlap buffer (keeps last 200ms for context between chunks)
  float* overlap_buffer;
  size_t overlap_size;
  size_t overlap_capacity;
  
  // Duplicate detection
  char* last_transcript;
  int duplicate_count;
  
  // Multi-language support
  bool auto_detect_language;
  char detected_language[3];  // Detected language code
  bool language_detected;
  
  // Periodic language re-detection for mid-conversation language changes
  int segment_count;           // Number of segments processed
  int redetect_interval;       // Re-detect language every N segments (0 = disabled)
  
  // Speaker tracking across entire session
  int current_speaker;         // Current active speaker ID (0, 1, 2, ...)
  bool show_speaker_labels;    // Always show speaker labels (not just on turns)
} whisper_backend_context_t;

static void log_audio_stats(const float* data, size_t sample_count, const char* label) {
  if (!data || sample_count == 0) {
    LOG_INFO("%s: no samples", label ? label : "Audio");
    return;
  }
  double sum_sq = 0.0;
  float peak = 0.0f;
  for (size_t i = 0; i < sample_count; ++i) {
    float s = fabsf(data[i]);
    sum_sq += (double)s * (double)s;
    if (s > peak) peak = s;
  }
  float rms = (float)sqrt(sum_sq / (double)sample_count);
  LOG_INFO("%s: %zu samples, RMS=%.4f, peak=%.4f", label ? label : "Audio",
           sample_count, rms, peak);
}

/**
 * Suppress whisper.cpp internal logs
 */
static void whisper_log_suppress(enum ggml_log_level level, const char* text, void* user_data) {
  (void)level; (void)text; (void)user_data;
}

/**
 * Initialize Whisper backend - MINIMAL VERSION
 */
int ethervox_stt_whisper_init(ethervox_stt_runtime_t* runtime) {
  if (!runtime) return -1;
  
  // DON'T suppress Whisper logs - we want to see what it's doing
  // whisper_log_set(whisper_log_suppress, NULL);
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)calloc(1, sizeof(whisper_backend_context_t));
  if (!ctx) return -1;
  
  // Load model
  struct whisper_context_params cparams = whisper_context_default_params();
  ctx->ctx = whisper_init_from_file_with_params(runtime->config.model_path, cparams);
  if (!ctx->ctx) {
    LOG_ERROR("Failed to load Whisper model: %s", runtime->config.model_path);
    free(ctx);
    return -1;
  }
  
  LOG_INFO("Loaded Whisper model: %s", runtime->config.model_path);
  
  // Get default params
  ctx->params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  
  // Language configuration for multilingual model
  const char* lang = runtime->config.language;
  if (lang && strcmp(lang, "auto") == 0) {
    // Multi-language support: Enable two-pass approach
    // Pass 1: detect_language=true to identify language
    // Pass 2: detect_language=false with detected language to transcribe
    ctx->auto_detect_language = true;
    ctx->language_detected = false;
    ctx->params.language = "auto";
    ctx->params.detect_language = true;  // First pass: detect only
    LOG_INFO("Multi-language auto-detection enabled (two-pass mode)");
  } else if (lang && strlen(lang) >= 2) {
    // Extract language code (e.g., "en" from "en-US")
    static char lang_code[3];
    lang_code[0] = lang[0];
    lang_code[1] = lang[1];
    lang_code[2] = '\0';
    ctx->auto_detect_language = false;
    ctx->language_detected = true;
    strncpy(ctx->detected_language, lang_code, 2);
    ctx->detected_language[2] = '\0';
    ctx->params.language = ctx->detected_language;
    ctx->params.detect_language = false;
    LOG_INFO("Language set to: %s", lang_code);
  } else {
    ctx->auto_detect_language = false;
    ctx->language_detected = true;
    strncpy(ctx->detected_language, "en", 2);
    ctx->detected_language[2] = '\0';
    ctx->params.language = ctx->detected_language;
    ctx->params.detect_language = false;
    LOG_INFO("Language defaulted to: en");
  }
  
  // Disable debug output (but enable timestamps for data extraction)
  ctx->params.print_progress = false;
  ctx->params.print_realtime = false;
  ctx->params.print_timestamps = false;
  ctx->params.print_special = false;
  
  // Enable timestamps for segment timing
  ctx->params.no_timestamps = false;
  ctx->params.token_timestamps = true;  // Token-level timestamps
  ctx->params.thold_pt = 0.01f;         // Timestamp token probability threshold
  ctx->params.thold_ptsum = 0.01f;      // Timestamp token sum probability threshold
  
  // Enable speaker detection (tinydiarize)
  ctx->params.tdrz_enable = true;
  LOG_INFO("Timestamps and speaker detection enabled");
  
  // Translation setting (transcribe in original language by default)
  ctx->params.translate = runtime->config.translate_to_english;
  if (ctx->params.translate) {
    LOG_INFO("Translation enabled: will translate non-English speech to English");
  } else {
    LOG_INFO("Translation disabled: will transcribe in original language");
  }
  
  // Enable proper filtering for multilingual models
  ctx->params.suppress_blank = true;    // Filter blank/noise segments
  ctx->params.suppress_nst = true;      // Suppress non-speech tokens
  
  // Quality thresholds - balanced for good speech detection
  ctx->params.no_speech_thold = 0.4f;   // Moderate filtering (0.2-0.6 range)
  ctx->params.logprob_thold = -1.0f;    // Default confidence threshold
  ctx->params.entropy_thold = 2.4f;     // Default entropy threshold
  LOG_INFO("Whisper speech thresholds: no_speech=%.2f logprob=%.2f entropy=%.2f",
           ctx->params.no_speech_thold,
           ctx->params.logprob_thold,
           ctx->params.entropy_thold);
  
  // Temperature fallback for better decoding
  ctx->params.temperature = 0.0f;       // Start greedy (deterministic)
  ctx->params.temperature_inc = 0.2f;   // Increase if needed
  
  // Streaming optimization (based on stream.cpp example)
  ctx->params.no_context = false;       // Keep context between chunks for better accuracy
  ctx->params.single_segment = false;   // Allow multiple segments per chunk
  ctx->params.max_tokens = 0;           // No token limit - process full chunk
  
  // DISABLE experimental VAD - it requires a separate VAD model we don't have
  // We'll use time-based chunking instead (like stream.cpp default mode)
  ctx->params.vad = false;
  LOG_INFO("Using time-based chunking (3s steps) instead of experimental VAD");
  
  // Allocate main audio buffer (10 seconds @ 16kHz for chunk processing)
  ctx->audio_buffer_capacity = 16000 * 10;
  ctx->audio_buffer = (float*)calloc(ctx->audio_buffer_capacity, sizeof(float));
  if (!ctx->audio_buffer) {
    whisper_free(ctx->ctx);
    free(ctx);
    return -1;
  }
  
  // Allocate overlap buffer (200ms @ 16kHz for context between chunks)
  ctx->overlap_capacity = 16000 * 0.2;  // 200ms = 3200 samples
  ctx->overlap_buffer = (float*)calloc(ctx->overlap_capacity, sizeof(float));
  if (!ctx->overlap_buffer) {
    free(ctx->audio_buffer);
    whisper_free(ctx->ctx);
    free(ctx);
    return -1;
  }
  ctx->overlap_size = 0;
  
  // Initialize duplicate detection
  ctx->last_transcript = NULL;
  ctx->duplicate_count = 0;
  
  // Initialize language re-detection
  ctx->segment_count = 0;
  ctx->redetect_interval = 2;  // Re-detect language every 2 segments
  
  // Initialize speaker tracking
  ctx->current_speaker = 0;
  ctx->show_speaker_labels = true;  // Always show speaker labels for better transcript clarity
  
  runtime->backend_context = ctx;
  LOG_INFO("Whisper backend initialized with multi-language, timestamps, and speaker detection");
  return 0;
}

/**
 * Start session - MINIMAL
 */
int ethervox_stt_whisper_start(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->backend_context) return -1;
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  ctx->audio_buffer_size = 0;
  ctx->overlap_size = 0;
  ctx->current_speaker = 0;  // Reset to Speaker 0 at session start
  
  LOG_INFO("Whisper session started (streaming mode with 200ms overlap)");
  return 0;
}

/**
 * Process audio - STREAMING WITH OVERLAP
 * Based on whisper.cpp stream.cpp example:
 * - Process every 3 seconds of audio
 * - Keep 200ms overlap for context continuity
 * - No experimental VAD, just time-based chunking
 */
int ethervox_stt_whisper_process(ethervox_stt_runtime_t* runtime,
                                  const ethervox_audio_buffer_t* audio_buffer,
                                  ethervox_stt_result_t* result) {
  if (!runtime || !runtime->backend_context || !audio_buffer || !result) return -1;
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  memset(result, 0, sizeof(ethervox_stt_result_t));
  
  const float* samples = (const float*)audio_buffer->data;
  uint32_t sample_count = audio_buffer->size;
  
  if (sample_count == 0) return 1;
  
  // Add samples to buffer
  for (uint32_t i = 0; i < sample_count; i++) {
    if (ctx->audio_buffer_size >= ctx->audio_buffer_capacity) {
      LOG_WARN("Audio buffer overflow, processing early");
      break;
    }
    ctx->audio_buffer[ctx->audio_buffer_size++] = samples[i];
  }
  
  // Process when we have 3 seconds (stream.cpp uses step_ms=3000)
  const size_t chunk_size = 16000 * 3;  // 3 seconds @ 16kHz
  
  if (ctx->audio_buffer_size < chunk_size) {
    // Not enough audio yet - keep accumulating
    return 1;
  }
  
  // Prepare processing buffer: overlap + new audio
  size_t total_samples = ctx->overlap_size + ctx->audio_buffer_size;
  float* process_buffer = (float*)malloc(total_samples * sizeof(float));
  if (!process_buffer) {
    LOG_ERROR("Failed to allocate processing buffer");
    return -1;
  }
  
  // Copy overlap from previous chunk (provides context)
  if (ctx->overlap_size > 0) {
    memcpy(process_buffer, ctx->overlap_buffer, ctx->overlap_size * sizeof(float));
  }
  
  // Copy new audio
  memcpy(process_buffer + ctx->overlap_size, ctx->audio_buffer, 
         ctx->audio_buffer_size * sizeof(float));
  
  LOG_INFO("Processing %.1f seconds (%.1f overlap + %.1f new)...", 
           (float)total_samples / 16000.0f,
           (float)ctx->overlap_size / 16000.0f,
           (float)ctx->audio_buffer_size / 16000.0f);
  
  // Log audio stats to verify data quality
  log_audio_stats(process_buffer, total_samples, "Before whisper_full");
  
  // Periodic language re-detection for mid-conversation language changes
  bool should_redetect = false;
  if (ctx->auto_detect_language && ctx->redetect_interval > 0) {
    ctx->segment_count++;
    if (ctx->segment_count % ctx->redetect_interval == 0) {
      should_redetect = true;
      LOG_INFO("⟳ Segment %d: Re-detecting language (interval=%d)...", 
               ctx->segment_count, ctx->redetect_interval);
    }
  }
  
  // Two-pass approach for multi-language support
  // Pass 1 runs on: (a) first chunk, OR (b) periodic re-detection
  if (ctx->auto_detect_language && (!ctx->language_detected || should_redetect)) {
    // PASS 1: Detect language
    if (!ctx->language_detected) {
      LOG_INFO("Pass 1: Detecting language with %zu samples...", total_samples);
    }
    
    // Temporarily enable language detection for this pass
    bool saved_detect = ctx->params.detect_language;
    ctx->params.detect_language = true;
    
    int ret_code = whisper_full(ctx->ctx, ctx->params, process_buffer, total_samples);
    
    // Restore detection setting
    ctx->params.detect_language = saved_detect;
    
    if (ret_code != 0) {
      LOG_ERROR("Language detection failed with code %d", ret_code);
      free(process_buffer);
      ctx->audio_buffer_size = 0;
      return -1;
    }
    
    // Get detected language
    int lang_id = whisper_full_lang_id(ctx->ctx);
    const char* detected_lang = whisper_lang_str(lang_id);
    if (detected_lang && strlen(detected_lang) >= 2) {
      // Check if language changed
      bool lang_changed = false;
      if (ctx->language_detected && strncmp(ctx->detected_language, detected_lang, 2) != 0) {
        lang_changed = true;
        LOG_INFO("🔄 Language changed: %s → %s", ctx->detected_language, detected_lang);
      }
      
      strncpy(ctx->detected_language, detected_lang, 2);
      ctx->detected_language[2] = '\0';
      ctx->language_detected = true;
      
      if (!lang_changed) {
        LOG_INFO("✓ Detected language: %s (will use for next %d segments)", 
                 ctx->detected_language, ctx->redetect_interval);
      }
      
      // Update params for ALL future transcription passes
      ctx->params.language = ctx->detected_language;
      ctx->params.detect_language = false;  // Never detect during transcription
    } else {
      // Fallback to English if detection fails
      if (!ctx->language_detected) {
        strncpy(ctx->detected_language, "en", 2);
        ctx->detected_language[2] = '\0';
        ctx->language_detected = true;
        ctx->params.language = ctx->detected_language;
        ctx->params.detect_language = false;
        LOG_WARN("Language detection failed, defaulting to English");
      }
    }
  }
  
  // PASS 2 (or single pass if language already known): Transcribe
  LOG_INFO("Transcribing %zu samples in language: %s...", total_samples, ctx->params.language);
  int ret_code = whisper_full(ctx->ctx, ctx->params, process_buffer, total_samples);
  
  if (ret_code != 0) {
    LOG_ERROR("whisper_full() failed with code %d", ret_code);
    free(process_buffer);
    ctx->audio_buffer_size = 0;
    return -1;
  }
  
  // Get segments
  const int n_segments = whisper_full_n_segments(ctx->ctx);
  LOG_INFO("Got %d segments from chunk", n_segments);
  
  if (n_segments == 0) {
    LOG_DEBUG("No segments detected - continuing to accumulate");
    goto save_overlap;
  }
  
  // Build transcript from all segments with speaker detection
  size_t total_len = 0;
  int64_t first_t0 = 0;
  int64_t last_t1 = 0;
  bool has_speaker_change = false;
  
  for (int i = 0; i < n_segments; i++) {
    const char* text = whisper_full_get_segment_text(ctx->ctx, i);
    int64_t t0 = whisper_full_get_segment_t0(ctx->ctx, i);
    int64_t t1 = whisper_full_get_segment_t1(ctx->ctx, i);
    bool speaker_turn = whisper_full_get_segment_speaker_turn_next(ctx->ctx, i);
    
    if (i == 0) first_t0 = t0;
    last_t1 = t1;
    if (speaker_turn) has_speaker_change = true;
    
    LOG_DEBUG("Segment %d: [%.2fs -> %.2fs] speaker_turn=%d text='%s'", 
              i, t0/100.0f, t1/100.0f, speaker_turn, text ? text : "NULL");
    
    if (text) {
      total_len += strlen(text) + 1;
      if (speaker_turn && i < n_segments - 1) {
        total_len += 20;  // Space for "[Speaker X: " markers
      }
    }
  }
  
  LOG_INFO("Total %d segments, length=%zu, time=[%.2fs -> %.2fs], speaker_changes=%d", 
           n_segments, total_len, first_t0/100.0f, last_t1/100.0f, has_speaker_change);
  
  if (total_len > 0) {
    char* transcript = (char*)calloc(total_len + 200, 1);  // Extra space for speaker markers
    if (!transcript) {
      free(process_buffer);
      ctx->audio_buffer_size = 0;
      return -1;
    }
    
    // Track speaker changes across this chunk
    for (int i = 0; i < n_segments; i++) {
      const char* text = whisper_full_get_segment_text(ctx->ctx, i);
      bool speaker_turn = whisper_full_get_segment_speaker_turn_next(ctx->ctx, i);
      
      if (text && strlen(text) > 0) {
        // Add space separator if not first segment in transcript
        if (strlen(transcript) > 0) {
          strcat(transcript, " ");
        }
        
        // Add speaker label (always show if enabled, or only on turns if disabled)
        if (ctx->show_speaker_labels || (speaker_turn && has_speaker_change)) {
          char speaker_marker[30];
          snprintf(speaker_marker, sizeof(speaker_marker), "[Speaker %d] ", ctx->current_speaker);
          strcat(transcript, speaker_marker);
        }
        
        // Add the actual text
        strcat(transcript, text);
        
        // Update speaker ID if there's a turn detected
        if (speaker_turn) {
          ctx->current_speaker++;  // Increment to next speaker ID
          LOG_DEBUG("Speaker turn detected → now Speaker %d", ctx->current_speaker);
        }
      }
    }
    
    // Duplicate detection: Check if this transcript matches the last one
    if (ctx->last_transcript && strcmp(ctx->last_transcript, transcript) == 0) {
      ctx->duplicate_count++;
      LOG_WARN("Duplicate transcript detected (count=%d): %s", ctx->duplicate_count, transcript);
      
      // If we've seen this 3+ times, it's likely silence/noise - skip it
      if (ctx->duplicate_count >= 3) {
        LOG_INFO("Skipping repeated duplicate (silence detected)");
        free(transcript);
        goto save_overlap;
      }
    } else {
      // New transcript - reset duplicate counter
      ctx->duplicate_count = 0;
      if (ctx->last_transcript) free(ctx->last_transcript);
      ctx->last_transcript = strdup(transcript);
    }
    
    // Set timestamps (convert from 10ms units to microseconds)
    result->start_time_us = first_t0 * 10000;
    result->end_time_us = last_t1 * 10000;
    
    // Return result
    result->text = transcript;
    result->confidence = 0.9f;
    result->is_final = true;
    result->language = ctx->detected_language;
    
    LOG_INFO("✅ Transcript (%s) [%.2fs-%.2fs]: %s", 
             result->language, 
             result->start_time_us/1000000.0f,
             result->end_time_us/1000000.0f,
             transcript);
  }
  
save_overlap:
  // Save last 200ms of audio as overlap for next chunk (context continuity)
  ctx->overlap_size = (ctx->audio_buffer_size > ctx->overlap_capacity) 
                      ? ctx->overlap_capacity 
                      : ctx->audio_buffer_size;
  
  if (ctx->overlap_size > 0) {
    memcpy(ctx->overlap_buffer, 
           ctx->audio_buffer + ctx->audio_buffer_size - ctx->overlap_size,
           ctx->overlap_size * sizeof(float));
  }
  
  // Clear main buffer for next chunk
  ctx->audio_buffer_size = 0;
  free(process_buffer);
  
  return (result->text && strlen(result->text) > 0) ? 0 : 1;
}

/**
 * Finalize - process remaining audio
 */
int ethervox_stt_whisper_finalize(ethervox_stt_runtime_t* runtime, ethervox_stt_result_t* result) {
  if (!runtime || !runtime->backend_context || !result) return -1;
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  memset(result, 0, sizeof(ethervox_stt_result_t));
  
  // Process any remaining audio (even if less than 3 seconds)
  if (ctx->audio_buffer_size > 16000) { // At least 1 second
    LOG_INFO("Finalizing with %.1f seconds", (float)ctx->audio_buffer_size / 16000.0f);
    log_audio_stats(ctx->audio_buffer, ctx->audio_buffer_size, "Finalize stats");
    
    if (whisper_full(ctx->ctx, ctx->params, ctx->audio_buffer, ctx->audio_buffer_size) == 0) {
      const int n_segments = whisper_full_n_segments(ctx->ctx);
      
      if (n_segments > 0) {
        size_t total_len = 0;
        for (int i = 0; i < n_segments; i++) {
          total_len += strlen(whisper_full_get_segment_text(ctx->ctx, i)) + 1;
        }
        
        result->text = (char*)calloc(total_len + 10, 1);
        if (result->text) {
          for (int i = 0; i < n_segments; i++) {
            if (i > 0) strcat(result->text, " ");
            strcat(result->text, whisper_full_get_segment_text(ctx->ctx, i));
          }
        }
      }
    }
  }
  
  if (!result->text) {
    result->text = strdup("");
  }
  
  result->is_final = true;
  return 0;
}

/**
 * Stop session
 */
void ethervox_stt_whisper_stop(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->backend_context) return;
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  ctx->audio_buffer_size = 0;
}

/**
 * Set language for hot-switching
 */
int ethervox_stt_whisper_set_language(ethervox_stt_runtime_t* runtime, const char* language) {
  if (!runtime || !runtime->backend_context) return -1;
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  if (!language || strcmp(language, "auto") == 0 || strcmp(language, "AUTO") == 0) {
    ctx->params.detect_language = true;
    ctx->params.language = "auto";
    LOG_INFO("Language set to auto-detect");
  } else if (strlen(language) >= 2) {
    ctx->params.detect_language = false;
    static char lang_code[3];
    lang_code[0] = language[0];
    lang_code[1] = language[1];
    lang_code[2] = '\0';
    ctx->params.language = lang_code;
    LOG_INFO("Language set to: %s", lang_code);
  } else {
    return -1;
  }
  
  return 0;
}

/**
 * Cleanup
 */
void ethervox_stt_whisper_cleanup(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->backend_context) return;
  
  whisper_backend_context_t* ctx = (whisper_backend_context_t*)runtime->backend_context;
  
  if (ctx->ctx) whisper_free(ctx->ctx);
  if (ctx->audio_buffer) free(ctx->audio_buffer);
  if (ctx->overlap_buffer) free(ctx->overlap_buffer);
  if (ctx->last_transcript) free(ctx->last_transcript);
  
  free(ctx);
  runtime->backend_context = NULL;
  LOG_INFO("Whisper cleaned up");
}

#else // !WHISPER_CPP_AVAILABLE

int ethervox_stt_whisper_init(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  return -1;
}

int ethervox_stt_whisper_start(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  return -1;
}

int ethervox_stt_whisper_process(ethervox_stt_runtime_t* runtime,
                                  const ethervox_audio_buffer_t* audio_buffer,
                                  ethervox_stt_result_t* result) {
  (void)runtime; (void)audio_buffer; (void)result;
  return -1;
}

int ethervox_stt_whisper_finalize(ethervox_stt_runtime_t* runtime, ethervox_stt_result_t* result) {
  (void)runtime; (void)result;
  return -1;
}

void ethervox_stt_whisper_stop(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
}

void ethervox_stt_whisper_cleanup(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
}

#endif // WHISPER_CPP_AVAILABLE
