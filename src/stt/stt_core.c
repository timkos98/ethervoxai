/**
 * @file stt_core.c
 * @brief Core Speech-to-Text implementation
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * Licensed under CC BY-NC-SA 4.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ethervox/stt.h"
#include "ethervox/logging.h"

// Placeholder for Vosk integration (will be implemented with actual Vosk lib)
typedef struct {
  void* model;
  void* recognizer;
  char* last_result;
} vosk_context_t;

// Default configuration - use Whisper as default since it's implemented
ethervox_stt_config_t ethervox_stt_get_default_config(void) {
  ethervox_stt_config_t config = {.backend = ETHERVOX_STT_BACKEND_WHISPER,
                                  .model_path = NULL,
                                  .language = "auto",
                                  .sample_rate = 16000,
                                  .enable_partial_results = true,
                                  .enable_punctuation = true,
                                  .vad_threshold = 0.5f,
                                  .translate_to_english = false};  // Transcribe in original language by default
  return config;
}

// Initialize STT engine
int ethervox_stt_init(ethervox_stt_runtime_t* runtime, const ethervox_stt_config_t* config) {
  if (!runtime) {
    return -1;
  } 

  memset(runtime, 0, sizeof(ethervox_stt_runtime_t));

  // Copy configuration
  if (config) {
    runtime->config = *config;
    if (config->model_path) {
      runtime->config.model_path = strdup(config->model_path);
    }
    if (config->language) {
      runtime->config.language = strdup(config->language);
    }
  } else {
    runtime->config = ethervox_stt_get_default_config();
  }

  // Allocate audio accumulator (5 seconds)
  runtime->accumulator_size = runtime->config.sample_rate * 5;
  runtime->audio_accumulator = (float*)calloc(runtime->accumulator_size, sizeof(float));
  if (!runtime->audio_accumulator) {
    return -1;
  }

  // Initialize backend
  switch (runtime->config.backend) {
    case ETHERVOX_STT_BACKEND_VOSK: {
      // TODO: Initialize actual Vosk library
      // For now, placeholder
      vosk_context_t* ctx = (vosk_context_t*)calloc(1, sizeof(vosk_context_t));
      if (!ctx) {
        free(runtime->audio_accumulator);
        return -1;
      }

      ETHERVOX_LOG_INFO("STT: Vosk backend initialized (placeholder - requires vosk library)");
      ETHERVOX_LOG_INFO("STT: Model path: %s", runtime->config.model_path);
      ETHERVOX_LOG_INFO("STT: Language: %s", runtime->config.language);

      runtime->backend_context = ctx;
      break;
    }

    case ETHERVOX_STT_BACKEND_WHISPER:
      if (ethervox_stt_whisper_init(runtime) != 0) {
        free(runtime->audio_accumulator);
        return -1;
      }
      break;

    default:
      free(runtime->audio_accumulator);
      return -1;
  }

  runtime->is_initialized = true;
  return 0;
}

// Start STT session
int ethervox_stt_start(ethervox_stt_runtime_t* runtime) {
  if (!runtime || !runtime->is_initialized) {
    return -1;
  }

  runtime->is_processing = true;
  runtime->accumulator_write_pos = 0;

  // Delegate to backend-specific start
  switch (runtime->config.backend) {
    case ETHERVOX_STT_BACKEND_WHISPER:
      return ethervox_stt_whisper_start(runtime);
    
    case ETHERVOX_STT_BACKEND_VOSK:
      // TODO: Implement Vosk backend
      ETHERVOX_LOG_ERROR("Vosk backend not yet implemented");
      return -1;
    
    default:
      ETHERVOX_LOG_ERROR("Unknown STT backend: %d", runtime->config.backend);
      return -1;
  }
}

// Process audio
int ethervox_stt_process(ethervox_stt_runtime_t* runtime,
                         const ethervox_audio_buffer_t* audio_buffer,
                         ethervox_stt_result_t* result) {
  if (!runtime || !runtime->is_initialized || !runtime->is_processing || !audio_buffer || !result) {
    return -1;
  }

  memset(result, 0, sizeof(ethervox_stt_result_t));

  // Delegate to backend-specific processing
  switch (runtime->config.backend) {
    case ETHERVOX_STT_BACKEND_WHISPER:
      return ethervox_stt_whisper_process(runtime, audio_buffer, result);
    
    case ETHERVOX_STT_BACKEND_VOSK:
      // TODO: Implement Vosk backend
      ETHERVOX_LOG_ERROR("Vosk backend not yet implemented - use ETHERVOX_STT_BACKEND_WHISPER");
      return -1;
    
    default:
      ETHERVOX_LOG_ERROR("Unknown STT backend: %d", runtime->config.backend);
      return -1;
  }
}

// Finalize and get final result
int ethervox_stt_finalize(ethervox_stt_runtime_t* runtime, ethervox_stt_result_t* result) {
  if (!runtime || !result) {
    return -1;
  }

  memset(result, 0, sizeof(ethervox_stt_result_t));

  // Delegate to backend-specific finalize
  switch (runtime->config.backend) {
    case ETHERVOX_STT_BACKEND_WHISPER:
      return ethervox_stt_whisper_finalize(runtime, result);
    
    case ETHERVOX_STT_BACKEND_VOSK:
      // TODO: Implement Vosk backend
      ETHERVOX_LOG_ERROR("Vosk backend not yet implemented");
      return -1;
    
    default:
      ETHERVOX_LOG_ERROR("Unknown STT backend: %d", runtime->config.backend);
      return -1;
  }
}

// Stop STT session
void ethervox_stt_stop(ethervox_stt_runtime_t* runtime) {
  if (!runtime) {
    return;
  }

  runtime->is_processing = false;
  runtime->accumulator_write_pos = 0;

  // Delegate to backend-specific stop
  switch (runtime->config.backend) {
    case ETHERVOX_STT_BACKEND_WHISPER:
      ethervox_stt_whisper_stop(runtime);
      break;
    
    case ETHERVOX_STT_BACKEND_VOSK:
      // TODO: Implement Vosk backend
      break;
    
    default:
      break;
  }

  ETHERVOX_LOG_INFO("STT: Processing stopped");
}

// Free result
void ethervox_stt_result_free(ethervox_stt_result_t* result) {
  if (!result) {
    return;
  }

  if (result->text) {
    free(result->text);
    result->text = NULL;
  }
}

// Cleanup
void ethervox_stt_cleanup(ethervox_stt_runtime_t* runtime) {
  if (!runtime) {
    return;
  }

  if (runtime->audio_accumulator) {
    free(runtime->audio_accumulator);
  }

  // Delegate to backend-specific cleanup
  if (runtime->config.backend == ETHERVOX_STT_BACKEND_WHISPER) {
    ethervox_stt_whisper_cleanup(runtime);
  } else if (runtime->backend_context) {
    // Vosk or other backend cleanup
    free(runtime->backend_context);
    runtime->backend_context = NULL;
  }

  if (runtime->config.model_path) {
    free((void*)runtime->config.model_path);
  }

  if (runtime->config.language) {
    free((void*)runtime->config.language);
  }

  if (runtime->backend_context) {
    if (runtime->config.backend == ETHERVOX_STT_BACKEND_VOSK) {
      vosk_context_t* ctx = (vosk_context_t*)runtime->backend_context;
      if (ctx->last_result) {
        free(ctx->last_result);
      }
    }
    free(runtime->backend_context);
  }

  runtime->is_initialized = false;
  ETHERVOX_LOG_INFO("STT engine cleaned up");
}
/**
 * Set language (hot-switch without re-init)
 */
int ethervox_stt_set_language(ethervox_stt_runtime_t* runtime, const char* language) {
  if (!runtime || !runtime->is_initialized) {
    ETHERVOX_LOG_ERROR("STT runtime not initialized");
    return -1;
  }
  
  if (!language) {
    ETHERVOX_LOG_ERROR("Language is NULL");
    return -1;
  }
  
  // Update config
  runtime->config.language = language;
  
  // Delegate to backend-specific language switching
  switch (runtime->config.backend) {
    case ETHERVOX_STT_BACKEND_WHISPER: {
      extern int ethervox_stt_whisper_set_language(ethervox_stt_runtime_t* runtime, const char* language);
      return ethervox_stt_whisper_set_language(runtime, language);
    }
    
    case ETHERVOX_STT_BACKEND_VOSK:
      // TODO: Implement Vosk language switching
      ETHERVOX_LOG_ERROR("Language hot-switching not yet implemented for Vosk backend");
      return -1;
    
    default:
      ETHERVOX_LOG_ERROR("Unknown STT backend: %d", runtime->config.backend);
      return -1;
  }
}
