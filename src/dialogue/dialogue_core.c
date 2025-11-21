/**
 * @file dialogue_core.c
 * @brief Core dialogue processing functionality for EthervoxAI
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ETHERVOX_PLATFORM_ANDROID
#include <android/log.h>
#endif

#include "ethervox/dialogue.h"
#include "ethervox/llm.h"

#ifndef ETHERVOX_UNUSED
#if defined(__GNUC__)
#define ETHERVOX_UNUSED __attribute__((unused))
#else
#define ETHERVOX_UNUSED
#endif
#endif

static const float kEthervoxIntentMatchConfidence ETHERVOX_UNUSED = 0.8f;
static const float kEthervoxIntentUnknownConfidence ETHERVOX_UNUSED = 0.1f;
static const float kEthervoxResponseConfidence = 0.9f;
static const uint32_t kEthervoxResponseProcessingTimeMs = 50U;
static const uint32_t kEthervoxTokenEstimateDivisor = 4U;
static const uint32_t kEthervoxDefaultMaxContexts = 16U;
static const uint32_t kEthervoxDefaultMaxHistory = 20U;
static const float kEthervoxWakeConfidenceScale ETHERVOX_UNUSED = 2.0f;
static const size_t kEthervoxLanguageMatchPrefix = 2U;
static const size_t kEthervoxLanguageTrimIndex = 2U;

static void sanitize_language_code(const char* source, char* target, size_t target_len) {
  if (!target || target_len == 0) {
    return;
  }

  const char* cursor = source;
  size_t out_pos = 0;

  while (cursor && *cursor && out_pos < target_len - 1) {
    if (*cursor == '.' || *cursor == '@') {
      break;
    }
    if (*cursor == '_' || *cursor == '-') {
      target[out_pos++] = '-';
      cursor++;
      continue;
    }
    if (isalpha((unsigned char)*cursor)) {
      target[out_pos++] = (char)tolower((unsigned char)*cursor);
    }
    cursor++;
  }

  target[out_pos] = '\0';

  if (out_pos > kEthervoxLanguageTrimIndex) {
    target[kEthervoxLanguageTrimIndex] = '\0';
  }
}

const char* ethervox_dialogue_detect_system_language(void) {
  static char cached_language[ETHERVOX_LANG_CODE_LEN] = "";
  static bool initialized = false;

  if (initialized && cached_language[0] != '\0') {
    return cached_language;
  }

  const char* env_lang = getenv("ETHERVOX_LANG");
  if (!env_lang || !*env_lang) {
    env_lang = getenv("LANG");
  }

  if (env_lang && *env_lang) {
    sanitize_language_code(env_lang, cached_language, sizeof(cached_language));
  }

  if (cached_language[0] == '\0') {
    snprintf(cached_language, sizeof(cached_language), "%s", "en");
  }

  initialized = true;
  return cached_language;
}
#include <inttypes.h>
#include <time.h>

// Supported languages for MVP
static const char* SUPPORTED_LANGUAGES[] = {"en",  // English
                                            "es",  // Spanish
                                            "zh",  // Mandarin Chinese
                                            NULL};

// Simple intent patterns for demonstration
typedef struct {
  const char* pattern;
  ethervox_intent_type_t intent_type;
  const char* language;
  bool require_start;  // Pattern must be at start of sentence
} intent_pattern_t;

static const intent_pattern_t INTENT_PATTERNS[] = {
    // English patterns - greetings must be at start
    {"hello", ETHERVOX_INTENT_GREETING, "en", true},
    {"hi", ETHERVOX_INTENT_GREETING, "en", true},
    {"hey", ETHERVOX_INTENT_GREETING, "en", true},
    {"good morning", ETHERVOX_INTENT_GREETING, "en", true},
    {"good afternoon", ETHERVOX_INTENT_GREETING, "en", true},
    {"good evening", ETHERVOX_INTENT_GREETING, "en", true},
    
    // Questions can appear anywhere
    {"what is", ETHERVOX_INTENT_QUESTION, "en", false},
    {"what's", ETHERVOX_INTENT_QUESTION, "en", false},
    {"how to", ETHERVOX_INTENT_QUESTION, "en", false},
    {"how do", ETHERVOX_INTENT_QUESTION, "en", false},
    {"where is", ETHERVOX_INTENT_QUESTION, "en", false},
    {"when is", ETHERVOX_INTENT_QUESTION, "en", false},
    
    // Control commands - should be at start
    {"turn on", ETHERVOX_INTENT_CONTROL, "en", true},
    {"turn off", ETHERVOX_INTENT_CONTROL, "en", true},
    {"set", ETHERVOX_INTENT_COMMAND, "en", true},
    {"play", ETHERVOX_INTENT_COMMAND, "en", true},
    {"stop", ETHERVOX_INTENT_COMMAND, "en", true},
    
    // Goodbyes must be at start or standalone
    {"goodbye", ETHERVOX_INTENT_GOODBYE, "en", true},
    {"bye", ETHERVOX_INTENT_GOODBYE, "en", true},
    {"no", ETHERVOX_INTENT_GOODBYE, "en", true},
    {"cancel", ETHERVOX_INTENT_GOODBYE, "en", true},

    // Spanish patterns
    {"hola", ETHERVOX_INTENT_GREETING, "es", true},
    {"buenos días", ETHERVOX_INTENT_GREETING, "es", true},
    {"qué es", ETHERVOX_INTENT_QUESTION, "es", false},
    {"cómo", ETHERVOX_INTENT_QUESTION, "es", false},
    {"encender", ETHERVOX_INTENT_CONTROL, "es", true},
    {"apagar", ETHERVOX_INTENT_CONTROL, "es", true},
    {"reproducir", ETHERVOX_INTENT_COMMAND, "es", true},
    {"parar", ETHERVOX_INTENT_COMMAND, "es", true},
    {"adiós", ETHERVOX_INTENT_GOODBYE, "es", true},

    // Chinese patterns (simplified)
    {"你好", ETHERVOX_INTENT_GREETING, "zh", true},
    {"早上好", ETHERVOX_INTENT_GREETING, "zh", true},
    {"什么是", ETHERVOX_INTENT_QUESTION, "zh", false},
    {"怎么", ETHERVOX_INTENT_QUESTION, "zh", false},
    {"打开", ETHERVOX_INTENT_CONTROL, "zh", true},
    {"关闭", ETHERVOX_INTENT_CONTROL, "zh", true},
    {"播放", ETHERVOX_INTENT_COMMAND, "zh", true},
    {"停止", ETHERVOX_INTENT_COMMAND, "zh", true},
    {"再见", ETHERVOX_INTENT_GOODBYE, "zh", true},

    {NULL, ETHERVOX_INTENT_UNKNOWN, NULL, false}  // Sentinel
};

// Intent type to string mapping
const char* ethervox_intent_type_to_string(ethervox_intent_type_t type) {
  switch (type) {
    case ETHERVOX_INTENT_GREETING:
      return "greeting";
    case ETHERVOX_INTENT_QUESTION:
      return "question";
    case ETHERVOX_INTENT_COMMAND:
      return "command";
    case ETHERVOX_INTENT_REQUEST:
      return "request";
    case ETHERVOX_INTENT_INFORMATION:
      return "information";
    case ETHERVOX_INTENT_CONTROL:
      return "control";
    case ETHERVOX_INTENT_GOODBYE:
      return "goodbye";
    case ETHERVOX_INTENT_UNKNOWN:
    default:
      return "unknown";
  }
}

// Entity type to string mapping
const char* ethervox_entity_type_to_string(ethervox_entity_type_t type) {
  switch (type) {
    case ETHERVOX_ENTITY_PERSON:
      return "person";
    case ETHERVOX_ENTITY_LOCATION:
      return "location";
    case ETHERVOX_ENTITY_TIME:
      return "time";
    case ETHERVOX_ENTITY_NUMBER:
      return "number";
    case ETHERVOX_ENTITY_DEVICE:
      return "device";
    case ETHERVOX_ENTITY_ACTION:
      return "action";
    default:
      return "unknown";
  }
}

// Default LLM configuration
ethervox_llm_config_t ethervox_dialogue_get_default_llm_config(void) {
  ethervox_llm_config_t config = {.model_path = NULL,  // Will be set based on platform
                                    .model_name = ETHERVOX_LLM_MODEL_NAME,
                                    .max_tokens = ETHERVOX_LLM_MAX_TOKENS_DEFAULT,
                                    .context_length = ETHERVOX_LLM_CONTEXT_LENGTH_DEFAULT,
                                    .temperature = ETHERVOX_LLM_TEMPERATURE_DEFAULT,
                                    .top_p = ETHERVOX_LLM_TOP_P_DEFAULT,
                                    .seed = ETHERVOX_LLM_SEED_DEFAULT,
                                    .use_gpu = true,  // Enable GPU acceleration by default
                                    .gpu_layers = ETHERVOX_LLM_GPU_LAYERS_DEFAULT,
                                  .language_code = NULL};

#ifdef ETHERVOX_PLATFORM_DESKTOP
  config.max_tokens = ETHERVOX_LLM_MAX_TOKENS_DESKTOP;
  config.context_length = ETHERVOX_LLM_CONTEXT_LENGTH_DESKTOP;
  config.use_gpu = true;
  config.gpu_layers = ETHERVOX_LLM_GPU_LAYERS_DESKTOP;
#endif

  return config;
}

// Check if language is supported
bool ethervox_dialogue_is_language_supported(const char* language_code) {
  if (!language_code) {
    return false;
  }

  for (int i = 0; SUPPORTED_LANGUAGES[i] != NULL; i++) {
    if (strncmp(language_code, SUPPORTED_LANGUAGES[i], kEthervoxLanguageMatchPrefix) == 0) {
      return true;
    }
  }

  return false;
}

// Generate conversation ID
static char* generate_conversation_id(void) {
  static uint32_t counter = 0;
  char* id = (char*)malloc(32);
  snprintf(id, 32, "conv_%" PRIu32 "_%lu", ++counter, (unsigned long)time(NULL));
  return id;
}

// Initialize dialogue engine
int ethervox_dialogue_init(ethervox_dialogue_engine_t* engine,
                           const ethervox_llm_config_t* config) {
  if (!engine){
    return -1;
  }

  memset(engine, 0, sizeof(ethervox_dialogue_engine_t));

  // Copy configuration
  if (config) {
    engine->llm_config = *config;
    if (config->model_path) {
      engine->llm_config.model_path = strdup(config->model_path);
    }
    if (config->model_name) {
      engine->llm_config.model_name = strdup(config->model_name);
    }
  } else {
    engine->llm_config = ethervox_dialogue_get_default_llm_config();
  }

  const char* language_source = NULL;
  if (config && config->language_code) {
    language_source = config->language_code;
  } else if (engine->llm_config.language_code) {
    language_source = engine->llm_config.language_code;
  } else {
    language_source = ethervox_dialogue_detect_system_language();
  }

  engine->llm_config.language_code = NULL;
  if (language_source) {
    engine->llm_config.language_code = strdup(language_source);
  }

  // Allocate context storage
  engine->max_contexts = kEthervoxDefaultMaxContexts;  // Support up to 16 simultaneous conversations
  engine->contexts = (ethervox_dialogue_context_t*)calloc(engine->max_contexts,
                                                          sizeof(ethervox_dialogue_context_t));
  if (!engine->contexts) {
    return -1;
  }

  // Initialize intent patterns (simplified - in production would load from files)
  engine->intent_patterns = (void*)INTENT_PATTERNS;

  // Initialize LLM backend if model path provided
  engine->llm_backend = NULL;
  engine->use_llm_for_unknown = false;
  
#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
  if (config && config->model_path) {
#ifdef ETHERVOX_PLATFORM_ANDROID
    __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "Initializing LLM backend with model: %s", config->model_path);
#else
    printf("Initializing LLM backend with model: %s\n", config->model_path);
#endif
    
    // Create llama backend
    ethervox_llm_backend_t* backend = ethervox_llm_create_llama_backend();
    if (backend) {
      // Initialize backend
      int result = ethervox_llm_backend_init(backend, config);
      if (result == 0) {
        // Load model
        result = ethervox_llm_backend_load_model(backend, config->model_path);
        if (result == 0) {
          engine->llm_backend = backend;
          engine->use_llm_for_unknown = true;
#ifdef ETHERVOX_PLATFORM_ANDROID
          __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM backend initialized successfully");
#else
          printf("LLM backend initialized successfully\n");
#endif
        } else {
#ifdef ETHERVOX_PLATFORM_ANDROID
          __android_log_print(ANDROID_LOG_ERROR, "EthervoxDialogue", "Failed to load LLM model: %d", result);
#else
          printf("Failed to load LLM model: %d\n", result);
#endif
          ethervox_llm_backend_cleanup(backend);
          ethervox_llm_backend_free(backend);
        }
      } else {
#ifdef ETHERVOX_PLATFORM_ANDROID
        __android_log_print(ANDROID_LOG_ERROR, "EthervoxDialogue", "Failed to initialize LLM backend: %d", result);
#else
        printf("Failed to initialize LLM backend: %d\n", result);
#endif
        ethervox_llm_backend_free(backend);
      }
    } else {
#ifdef ETHERVOX_PLATFORM_ANDROID
      __android_log_print(ANDROID_LOG_ERROR, "EthervoxDialogue", "Failed to create LLM backend");
#else
      printf("Failed to create LLM backend\n");
#endif
    }
  }
#else
  if (config && config->model_path) {
#ifdef ETHERVOX_PLATFORM_ANDROID
    __android_log_print(ANDROID_LOG_WARN, "EthervoxDialogue", "LLM support not compiled in (LLAMA_CPP_AVAILABLE not defined)");
#else
    printf("LLM support not compiled in (LLAMA_CPP_AVAILABLE not defined)\n");
#endif
  }
#endif

  engine->is_initialized = true;
  printf("Dialogue engine initialized with %s model\n",
         engine->llm_config.model_name ? engine->llm_config.model_name : "default");

  return 0;
}

// Cleanup dialogue engine
void ethervox_dialogue_cleanup(ethervox_dialogue_engine_t* engine) {
  if (!engine) {
    return;
  }

  // Cleanup contexts
  if (engine->contexts) {
    for (uint32_t i = 0; i < engine->max_contexts; i++) {
      ethervox_dialogue_context_t* ctx = &engine->contexts[i];
      if (ctx->conversation_id) {
        free(ctx->conversation_id);
      }
      free(ctx->user_id);
      
      
      if (ctx->conversation_history) {
        for (uint32_t j = 0; j < ctx->history_count; j++) {
          ethervox_intent_free(&ctx->conversation_history[j]);
        }
        free(ctx->conversation_history);
      }
    }
    free(engine->contexts);
  }

  // Cleanup LLM config
  if (engine->llm_config.model_path) {
    free(engine->llm_config.model_path);
  }
  if (engine->llm_config.model_name) {
    free(engine->llm_config.model_name);
  }
  if (engine->llm_config.language_code) {
    free(engine->llm_config.language_code);
    engine->llm_config.language_code = NULL;
  }

  // Cleanup LLM backend
#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
  if (engine->llm_backend) {
    ethervox_llm_backend_t* backend = (ethervox_llm_backend_t*)engine->llm_backend;
    ethervox_llm_backend_cleanup(backend);
    ethervox_llm_backend_free(backend);
    engine->llm_backend = NULL;
  }
#else
  engine->llm_backend = NULL;
#endif

  engine->is_initialized = false;
  printf("Dialogue engine cleaned up\n");
}

// Parse intent from text
int ethervox_dialogue_parse_intent(ethervox_dialogue_engine_t* engine,
                                   const ethervox_dialogue_intent_request_t* request,
                                   ethervox_intent_t* intent) {
  if (!engine || !request || !request->text || !intent) {
    return -1;
  }

  const char* language_code = request->language_code;
  if (!language_code || language_code[0] == '\0') {
    language_code = engine->llm_config.language_code ? engine->llm_config.language_code : "en";
  }
  
  // Normalize language code to just first 2 characters (en-US -> en)
  char lang_normalized[8] = {0};
  size_t lang_copy_len = strlen(language_code);
  if (lang_copy_len > 2) {
    lang_copy_len = 2; // Only take first 2 chars
  }
  strncpy(lang_normalized, language_code, lang_copy_len);
  lang_normalized[lang_copy_len] = '\0';
  
  // Convert to lowercase
  for (size_t i = 0; i < lang_copy_len; i++) {
    lang_normalized[i] = (char)tolower((unsigned char)lang_normalized[i]);
  }

  const char* text = request->text;

  memset(intent, 0, sizeof(ethervox_intent_t));

  // Copy input text
  intent->raw_text = strdup(text);
  
  // Normalize text to lowercase for pattern matching
  size_t text_len = strlen(text);
  char* normalized = (char*)malloc(text_len + 1);
  for (size_t i = 0; i < text_len; i++) {
    normalized[i] = (char)tolower((unsigned char)text[i]);
  }
  normalized[text_len] = '\0';
  intent->normalized_text = normalized;
  
  snprintf(intent->language_code, sizeof(intent->language_code), "%s", lang_normalized);

  // Simple pattern matching for intent detection
  intent->type = ETHERVOX_INTENT_UNKNOWN;
  intent->confidence = 0.0f;

  const intent_pattern_t* patterns = (const intent_pattern_t*)engine->intent_patterns;

  for (int i = 0; patterns[i].pattern != NULL; i++) {
    // Check if pattern matches language (use normalized language code)
    if (strcmp(patterns[i].language, lang_normalized) != 0) {
      continue;
    }

    // Check pattern position requirement
    const char* match_pos = strstr(normalized, patterns[i].pattern);
    if (match_pos != NULL) {
      // If pattern requires start of sentence, check position
      if (patterns[i].require_start) {
        // Pattern must be at the very beginning (after trimming whitespace)
        if (match_pos != normalized) {
          continue;  // Not at start, skip this pattern
        }
      }
      
      // Pattern matched!
      intent->type = patterns[i].intent_type;
      intent->confidence = 0.8f;  // Fixed confidence for demo
      break;
    }
  }

  // If no pattern matched, use unknown intent with low confidence
  if (intent->type == ETHERVOX_INTENT_UNKNOWN) {
    intent->confidence = 0.1f;
  }

  printf("Intent parsed: %s (confidence: %.2f) for text: '%s' [lang: %s]\n", 
         ethervox_intent_type_to_string(intent->type),
         intent->confidence, text, lang_normalized);

  return 0;
}

// Process with LLM
int ethervox_dialogue_process_llm(ethervox_dialogue_engine_t* engine,
                                  const ethervox_intent_t* intent, const char* context_id,
                                  ethervox_llm_response_t* response) {
  if (!engine || !intent || !response) {
    return -1;
  }

  memset(response, 0, sizeof(ethervox_llm_response_t));

  // For demo purposes, generate simple responses based on intent type
  const char* response_text = NULL;
  bool conversation_ended = false;

  switch (intent->type) {
    case ETHERVOX_INTENT_GREETING:
      if (strcmp(intent->language_code, "es") == 0) {
        response_text = "¡Hola! ¿En qué puedo ayudarte?";
      } else if (strcmp(intent->language_code, "zh") == 0) {
        response_text = "你好！我能为您做些什么？";
      } else {
        response_text = "Hello! How can I help you today?";
      }
      break;

    case ETHERVOX_INTENT_QUESTION:
      if (strcmp(intent->language_code, "es") == 0) {
        response_text = "Déjame pensar en eso. ¿Puedes ser más específico?";
      } else if (strcmp(intent->language_code, "zh") == 0) {
        response_text = "让我想想。您能更具体一些吗？";
      } else {
        response_text = "Let me think about that. Can you be more specific?";
      }
      break;

    case ETHERVOX_INTENT_COMMAND:
    case ETHERVOX_INTENT_CONTROL:
      conversation_ended = true;  // Commands should not auto-restart microphone
      if (strcmp(intent->language_code, "es") == 0) {
        response_text = "Entendido. Ejecutando comando.";
      } else if (strcmp(intent->language_code, "zh") == 0) {
        response_text = "明白了。正在执行命令。";
      } else {
        response_text = "Understood. Executing command.";
      }
      break;

    case ETHERVOX_INTENT_GOODBYE:
      conversation_ended = true;  // Signal that conversation should end
      if (strcmp(intent->language_code, "es") == 0) {
        response_text = "¡Hasta luego! Que tengas un buen día.";
      } else if (strcmp(intent->language_code, "zh") == 0) {
        response_text = "再见！祝您有美好的一天。";
      } else {
        response_text = "Goodbye! Have a great day.";
      }
      break;

    default:
    case ETHERVOX_INTENT_UNKNOWN:
      // Try LLM backend if available
#ifdef ETHERVOX_PLATFORM_ANDROID
      __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "UNKNOWN intent received: %s", intent->raw_text);
#else
      printf("UNKNOWN intent received: %s\n", intent->raw_text);
#endif

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#ifdef ETHERVOX_PLATFORM_ANDROID
      __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM compile flags are defined");
      __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", 
                         "engine->llm_backend = %p, engine->use_llm_for_unknown = %d", 
                         engine->llm_backend, engine->use_llm_for_unknown);
#else
      printf("LLM compile flags are defined\n");
      printf("engine->llm_backend = %p, engine->use_llm_for_unknown = %d\n", 
             engine->llm_backend, engine->use_llm_for_unknown);
#endif
      
      if (engine->llm_backend && engine->use_llm_for_unknown) {
        ethervox_llm_backend_t* backend = (ethervox_llm_backend_t*)engine->llm_backend;
        
#ifdef ETHERVOX_PLATFORM_ANDROID
        __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM backend exists, checking if loaded...");
        __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "backend->is_loaded = %d", backend->is_loaded);
#else
        printf("LLM backend exists, checking if loaded...\n");
        printf("backend->is_loaded = %d\n", backend->is_loaded);
#endif
        
        // Check if backend is loaded
        if (backend->is_loaded) {
          // Create conversational prompt
          char prompt[2048];
          snprintf(prompt, sizeof(prompt),
                   "User: %s\nAssistant:",
                   intent->raw_text);
          
          // Generate response with LLM
          int llm_result = ethervox_llm_backend_generate(backend, prompt, 
                                                        intent->language_code, response);
          if (llm_result == 0 && response->text && response->text[0] != '\0') {
            // LLM generated response successfully
            response->confidence = 0.7f;  // LLM confidence
            response->processing_time_ms = kEthervoxResponseProcessingTimeMs;
            response->conversation_ended = false;  // Don't end conversation
#ifdef ETHERVOX_PLATFORM_ANDROID
            __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM backend generated response: %s", response->text);
#else
            printf("LLM backend generated response: %s\n", response->text);
#endif
            return 0;
          } else {
#ifdef ETHERVOX_PLATFORM_ANDROID
            __android_log_print(ANDROID_LOG_WARN, "EthervoxDialogue", "LLM backend generation failed or empty response: %d", llm_result);
#else
            printf("LLM backend generation failed or empty response: %d\n", llm_result);
#endif
          }
        } else {
#ifdef ETHERVOX_PLATFORM_ANDROID
          __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM backend not loaded (is_loaded=%d), using fallback response", backend->is_loaded);
#else
          printf("LLM backend not loaded (is_loaded=%d), using fallback response\n", backend->is_loaded);
#endif
        }
      } else {
#ifdef ETHERVOX_PLATFORM_ANDROID
        __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM backend not available: llm_backend=%p, use_llm_for_unknown=%d",
               engine->llm_backend, engine->use_llm_for_unknown);
#else
        printf("LLM backend not available: llm_backend=%p, use_llm_for_unknown=%d\n",
               engine->llm_backend, engine->use_llm_for_unknown);
#endif
      }
#else
#ifdef ETHERVOX_PLATFORM_ANDROID
      __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", "LLM compile flags NOT defined - ETHERVOX_WITH_LLAMA and/or LLAMA_CPP_AVAILABLE not set");
#else
      printf("LLM compile flags NOT defined - ETHERVOX_WITH_LLAMA and/or LLAMA_CPP_AVAILABLE not set\n");
#endif
#endif
      
      // Fallback to pattern-based response if LLM not available or failed
      response->requires_external_llm = true;
      response->external_llm_prompt = strdup(intent->raw_text);

      if (strcmp(intent->language_code, "es") == 0) {
        response_text = "Lo siento, no entiendo completamente. ¿Podrías reformular?";
      } else if (strcmp(intent->language_code, "zh") == 0) {
        response_text = "抱歉，我不太理解。您能重新表述一下吗？";
      } else {
        response_text = "I'm sorry, I don't fully understand. Could you rephrase?";
      }
      break;
  }

  response->text = strdup(response_text);
  snprintf(response->language_code, sizeof(response->language_code), "%s", intent->language_code);
  response->confidence = intent->confidence;  // Use actual intent confidence
  response->processing_time_ms = kEthervoxResponseProcessingTimeMs;  // Simulated processing time
  response->token_count = strlen(response_text) / kEthervoxTokenEstimateDivisor;  // Rough token estimate
  response->conversation_ended = conversation_ended;  // Set flag to disable follow-up

  printf("LLM response generated: %s (confidence: %.0f%%, conversation_ended: %s)\n", 
         response->text, response->confidence * 100.0f, conversation_ended ? "true" : "false");

  return 0;
}

// Process LLM with streaming token generation
int ethervox_dialogue_process_llm_stream(ethervox_dialogue_engine_t* engine,
                                         const ethervox_intent_t* intent,
                                         const ethervox_dialogue_context_t* context,
                                         void (*token_callback)(const char* token, void* user_data),
                                         void* user_data,
                                         bool* conversation_ended) {
  if (!engine || !intent || !token_callback) {
    return -1;
  }

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
  ethervox_llm_backend_t* backend = (ethervox_llm_backend_t*)engine->llm_backend;
  if (backend && backend->is_loaded && intent->type == ETHERVOX_INTENT_UNKNOWN) {
    
    // Use the backend's generate_stream function if available
    if (backend->generate_stream) {
      __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", 
                         "Using LLM backend streaming for: %s", intent->raw_text);
      
      return backend->generate_stream(backend, 
                                      intent->raw_text,
                                      intent->language_code,
                                      token_callback,
                                      user_data);
    }
  }
#endif
  
  // Fallback: process with regular dialogue engine and stream the result
  __android_log_print(ANDROID_LOG_INFO, "EthervoxDialogue", 
                     "LLM streaming not available, using regular dialogue processing");
  
  ethervox_llm_response_t response;
  memset(&response, 0, sizeof(ethervox_llm_response_t));
  
  int result = ethervox_dialogue_process_llm(engine, intent, NULL, &response);
  if (result == 0 && response.text) {
    // Stream the complete response as one token
    token_callback(response.text, user_data);
    
    // Set conversation_ended flag if provided
    if (conversation_ended) {
      *conversation_ended = response.conversation_ended;
    }
    
    ethervox_llm_response_free(&response);
    return 0;
  }
  
  ethervox_llm_response_free(&response);
  return result;
}

// Create dialogue context
int ethervox_dialogue_create_context(ethervox_dialogue_engine_t* engine,
                                     const ethervox_dialogue_context_request_t* request,
                                     char** context_id) {
  if (!engine || !request || !request->user_id || !context_id) {
    return -1;
  }

  const char* language_code = request->language_code;
  if (!language_code || language_code[0] == '\0') {
    language_code = engine->llm_config.language_code ? engine->llm_config.language_code : "en";
  }

  // Find available context slot
  for (uint32_t i = 0; i < engine->max_contexts; i++) {
    ethervox_dialogue_context_t* ctx = &engine->contexts[i];
    if (!ctx->conversation_id) {  // Empty slot
      ctx->conversation_id = generate_conversation_id();
      ctx->user_id = strdup(request->user_id);
      snprintf(ctx->current_language, sizeof(ctx->current_language), "%s", language_code);
      ctx->max_history = kEthervoxDefaultMaxHistory;
      ctx->conversation_history =
          (ethervox_intent_t*)calloc(ctx->max_history, sizeof(ethervox_intent_t));
      ctx->last_interaction_time = time(NULL);

      *context_id = strdup(ctx->conversation_id);
      engine->active_contexts++;

      printf("Created dialogue context: %s for user: %s\n", ctx->conversation_id,
             request->user_id);
      return 0;
    }
  }

  return -1;  // No available slots
}

int ethervox_dialogue_set_language(ethervox_dialogue_engine_t* engine, const char* language_code) {
  if (!engine || !language_code) {
    return -1;
  }

  char normalized[8] = {0};
  sanitize_language_code(language_code, normalized, sizeof(normalized));

  if (normalized[0] == '\0') {
    return -1;
  }

  if (!ethervox_dialogue_is_language_supported(normalized)) {
    fprintf(stderr, "Dialogue language '%s' not supported; keeping current setting\n", normalized);
    return -1;
  }

  if (engine->llm_config.language_code &&
      strcmp(engine->llm_config.language_code, normalized) == 0) {
    return 0;
  }

  if (engine->llm_config.language_code) {
    free(engine->llm_config.language_code);
    engine->llm_config.language_code = NULL;
  }

  engine->llm_config.language_code = strdup(normalized);
  if (!engine->llm_config.language_code) {
    return -1;
  }

  for (uint32_t i = 0; i < engine->max_contexts; i++) {
    ethervox_dialogue_context_t* ctx = &engine->contexts[i];
    if (ctx->conversation_id) {
      snprintf(ctx->current_language, sizeof(ctx->current_language), "%s", normalized);
      ctx->current_language[sizeof(ctx->current_language) - 1] = '\0';
    }
  }

  return 0;
}

const char* ethervox_dialogue_get_language(const ethervox_dialogue_engine_t* engine) {
  if (!engine || !engine->llm_config.language_code) {
    return NULL;
  }
  return engine->llm_config.language_code;
}

// Free intent structure
void ethervox_intent_free(ethervox_intent_t* intent) {
  if (!intent){
    return;
  }
  
  if (intent->raw_text) {
    free(intent->raw_text);
    intent->raw_text = NULL;
  }
  if (intent->normalized_text) {
    free(intent->normalized_text);
    intent->normalized_text = NULL;
  }
  if (intent->entities) {
    for (uint32_t i = 0; i < intent->entity_count; i++) {
      if (intent->entities[i].value) {
        free(intent->entities[i].value);
      }
      if (intent->entities[i].normalized_value) {
        free(intent->entities[i].normalized_value);
      }
    }
    free(intent->entities);
    intent->entities = NULL;
  }
}

// Free LLM response structure
void ethervox_llm_response_free(ethervox_llm_response_t* response) {
  if (!response) {
    return;
  }

  if (response->text) {
    free(response->text);
    response->text = NULL;
  }

  if (response->model_name) {
    free(response->model_name);
    response->model_name = NULL;
  }

  if (response->finish_reason) {
    free(response->finish_reason);
    response->finish_reason = NULL;
  }
}

// Set external LLM callback
void ethervox_dialogue_set_external_llm_callback(ethervox_dialogue_engine_t* engine,
                                                 ethervox_external_llm_callback_t callback,
                                                 void* user_data) {
  if (engine) {
    engine->external_llm_callback = callback;
    engine->external_llm_user_data = user_data;
  }
}