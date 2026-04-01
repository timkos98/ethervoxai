/**
 * @file ethervox_android_core.c
 * @brief Android JNI wrapper for EthervoxAI
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 *
 * This file is part of EthervoxAI, licensed under CC BY-NC-SA 4.0.
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ethervox/audio.h"
#include "ethervox/llm.h"
#include "ethervox/stt.h"
#include "ethervox/wake_word.h"
// NOTE: dialogue.h removed - using direct governor/registry architecture
#include "ethervox/compute_tools.h"
#include "ethervox/config.h"
#include "ethervox/governor.h"
#include "ethervox/integration_tests.h"
#include "ethervox/llm_tool_tests.h"
#include "ethervox/memory_tools.h"
#include "ethervox/platform.h"
#include "ethervox/timer_tools.h"
#include "ethervox/tool_manifest.h"
#include "ethervox/tool_prompt_optimizer.h"
#include "ethervox/voice_tools.h"

#if ETHERVOX_WITH_LLAMA && LLAMA_CPP_AVAILABLE
#include "llama.h"
#endif

#define LOGI(...) ETHERVOX_LOGI(__VA_ARGS__)
#define LOGE(...) ETHERVOX_LOGE(__VA_ARGS__)
#define LOGW(...) ETHERVOX_LOGE(__VA_ARGS__)

// Global debug mode flag (defined in logging.c, referenced here)
extern int g_ethervox_debug_enabled;

// Global log callback for sending C logs to Java/Kotlin debug window
ethervox_log_callback_t g_ethervox_log_callback = NULL;

// Logging helper that sends to both logcat and callback
void ethervox_log_with_callback(int level, const char* tag, const char* fmt, ...) {
  if (!g_ethervox_debug_enabled)
    return;

  // Guard against NULL format string
  if (!fmt) {
    fmt = "(null format string)";
  }
  if (!tag) {
    tag = "EthervoxCore";
  }

  char buffer[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  // Send to Android logcat
  __android_log_print(level, tag, "%s", buffer);

  // Send to callback (debug window) if registered
  if (g_ethervox_log_callback) {
    g_ethervox_log_callback(level, tag, buffer);
  }
}

// JNI callback storage for log forwarding
static JavaVM* g_jvm = NULL;
static jobject g_log_callback_obj = NULL;
static jmethodID g_log_callback_method = NULL;

// Global runtime instances (managed from Java layer)
static ethervox_audio_runtime_t* g_audio_runtime = NULL;
static ethervox_wake_runtime_t* g_wake_runtime = NULL;
static ethervox_stt_runtime_t* g_stt_runtime = NULL;
static ethervox_platform_t* g_platform = NULL;

// Direct Governor and Registry (mirrors desktop main.c architecture)
// REFACTORED 2025-12-09: Removed dialogue_engine wrapper to align with desktop
// Android now uses same initialization path: governor + tool_registry directly
static ethervox_governor_t* g_governor = NULL;
static ethervox_tool_registry_t* g_registry = NULL;
static tool_manifest_registry_t* g_manifest_registry = NULL;
static ethervox_memory_store_t* g_memory_store = NULL;

// Android-specific files directory
static char g_android_files_dir[512] = {0};

// Getter function for Android files directory (used by manifest system)
const char* ethervox_android_get_files_dir(void) {
  return g_android_files_dir;
}

// ===========================================================================
// Utility Functions
// ===========================================================================

// Default startup prompt (defined here for JNI access, same as main.c)
static const char* DEFAULT_STARTUP_PROMPT = "Greet the user with a short creative greeting.";

// Validate and truncate UTF-8 string at incomplete multi-byte sequences
// This is needed because llama.cpp token streaming can split emoji bytes
static bool validate_and_fix_utf8(const char* str, size_t* out_valid_len) {
  if (!str) {
    *out_valid_len = 0;
    return true;
  }

  size_t len = strlen(str);
  *out_valid_len = 0;
  bool is_valid = true;

  for (size_t i = 0; i < len;) {
    unsigned char c = (unsigned char)str[i];
    int expected_bytes = 0;

    if (c <= 0x7F) {
      // ASCII - 1 byte
      expected_bytes = 1;
    } else if ((c & 0xE0) == 0xC0) {
      // 2-byte sequence: 110xxxxx
      expected_bytes = 2;
    } else if ((c & 0xF0) == 0xE0) {
      // 3-byte sequence: 1110xxxx
      expected_bytes = 3;
    } else if ((c & 0xF8) == 0xF0) {
      // 4-byte sequence: 11110xxx (emojis!)
      expected_bytes = 4;
    } else {
      // Invalid start byte
      is_valid = false;
      break;
    }

    // Check if we have all the bytes for this character
    if (i + expected_bytes > len) {
      // Incomplete sequence at end - truncate here
      __android_log_print(ANDROID_LOG_DEBUG, "EthervoxJNI",
                          "Incomplete UTF-8 sequence at end: need %d bytes, have %zu",
                          expected_bytes, len - i);
      is_valid = false;
      break;
    }

    // Verify continuation bytes (should be 10xxxxxx)
    for (int j = 1; j < expected_bytes; j++) {
      if ((str[i + j] & 0xC0) != 0x80) {
        __android_log_print(ANDROID_LOG_WARN, "EthervoxJNI",
                            "Invalid continuation byte at position %zu", i + j);
        is_valid = false;
        break;
      }
    }

    if (!is_valid)
      break;

    i += expected_bytes;
    *out_valid_len = i;
  }

  return is_valid;
}

static jstring create_jstring(JNIEnv* env, const char* str) {
  if (!str) {
    return (*env)->NewStringUTF(env, "");
  }

  size_t valid_len = 0;
  bool is_valid = validate_and_fix_utf8(str, &valid_len);

  if (is_valid) {
    // String is completely valid UTF-8
    return (*env)->NewStringUTF(env, str);
  } else {
    // String has invalid or incomplete UTF-8
    // Create truncated string with only valid portion
    if (valid_len == 0) {
      __android_log_print(ANDROID_LOG_WARN, "EthervoxJNI",
                          "Completely invalid UTF-8 string, returning empty");
      return (*env)->NewStringUTF(env, "");
    }

    char* valid_str = malloc(valid_len + 1);
    if (!valid_str) {
      return (*env)->NewStringUTF(env, "");
    }

    memcpy(valid_str, str, valid_len);
    valid_str[valid_len] = '\0';

    __android_log_print(ANDROID_LOG_DEBUG, "EthervoxJNI",
                        "Truncated UTF-8 string from %zu to %zu bytes (incomplete sequence at end)",
                        strlen(str), valid_len);

    jstring result = (*env)->NewStringUTF(env, valid_str);
    free(valid_str);
    return result;
  }
}

// ===========================================================================
// Platform Initialization
// ===========================================================================

// Track last load error code for corruption detection
static int g_last_governor_load_error = 0;

JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_loadGovernorModel(
    JNIEnv* env, jobject thiz, jstring modelPath) {
  (void)thiz;

  if (!g_governor) {
    LOGE("Cannot load Governor model - governor not initialized");
    g_last_governor_load_error = -1;
    return JNI_FALSE;
  }

  const char* path = (*env)->GetStringUTFChars(env, modelPath, NULL);

  LOGI("[JNI] Loading Governor model from: %s", path);

  ethervox_result_t result = ethervox_governor_load_model(g_governor, path, NULL, NULL);

  (*env)->ReleaseStringUTFChars(env, modelPath, path);

  // Store error code for corruption detection
  g_last_governor_load_error = result;

  if (ethervox_is_success(result)) {
    LOGI("[JNI] Governor model loaded successfully");

    // Initialize Tool Manifest System (after Governor model is loaded)
    // This is optional - graceful fallback if it fails
    if (!g_manifest_registry) {
      LOGI("[JNI] Initializing manifest registry after model load");

      // Get the model path from JNI parameter (we need to get it again since we released it)
      const char* model_path_for_manifest = (*env)->GetStringUTFChars(env, modelPath, NULL);

      // Use the centralized manifest setup helper
      tool_manifest_registry_t* manifest = NULL;
      ethervox_result_t manifest_result =
          ethervox_governor_setup_manifest(g_governor, model_path_for_manifest, &manifest);

      if (ethervox_is_success(manifest_result) && manifest) {
        g_manifest_registry = manifest;
        LOGI("[JNI] Manifest initialized successfully");
      } else {
        LOGE("Manifest initialization failed - using runtime registry only");
      }

      (*env)->ReleaseStringUTFChars(env, modelPath, model_path_for_manifest);
    }

    return JNI_TRUE;
  } else {
    if (result == -2) {
      LOGE("Failed to load Governor model - likely corrupted");
    } else {
      LOGE("Failed to load Governor model");
    }
    return JNI_FALSE;
  }
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_wasLastLoadCorrupted(JNIEnv* env, jobject thiz) {
  (void)env;
  (void)thiz;

  // -2 indicates probable corruption
  return (g_last_governor_load_error == -2) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_unloadGovernorModel(JNIEnv* env, jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_governor) {
    LOGE("Cannot unload Governor model - Governor not initialized");
    return JNI_FALSE;
  }

  LOGI("[JNI] Unloading Governor model to free memory");

  ethervox_result_t result = ethervox_governor_unload_model(g_governor);

  if (ethervox_is_success(result)) {
    LOGI("[JNI] Governor model unloaded successfully");
    return JNI_TRUE;
  } else {
    LOGE("Failed to unload Governor model");
    return JNI_FALSE;
  }
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_reloadGovernorModel(JNIEnv* env, jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_governor) {
    LOGE("Cannot reload Governor model - Governor not initialized");
    return JNI_FALSE;
  }

  LOGI("[JNI] Reloading Governor model");

  ethervox_result_t result = ethervox_governor_reload_model(g_governor);

  if (ethervox_is_success(result)) {
    LOGI("[JNI] Governor model reloaded successfully");
    return JNI_TRUE;
  } else {
    LOGE("Failed to reload Governor model");
    return JNI_FALSE;
  }
}

JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_isGovernorLoaded(JNIEnv* env,
                                                                                    jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_governor) {
    return JNI_FALSE;
  }

  return ethervox_governor_is_loaded(g_governor) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_droid_ethervox_1core_NativeLib_getRegisteredPlugins(JNIEnv* env, jobject thiz) {
  if (!g_registry) {
    // Return empty array if not initialized
    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    return (*env)->NewObjectArray(env, 0, stringClass, NULL);
  }

  int tool_count = g_registry->tool_count;

  // Create Java string array
  jclass stringClass = (*env)->FindClass(env, "java/lang/String");
  jobjectArray result = (*env)->NewObjectArray(env, tool_count, stringClass, NULL);

  // Populate array with tool names
  for (int i = 0; i < tool_count; i++) {
    jstring toolName = (*env)->NewStringUTF(env, g_registry->tools[i].name);
    (*env)->SetObjectArrayElement(env, result, i, toolName);
    (*env)->DeleteLocalRef(env, toolName);
  }

  return result;
}

// Thread data for optimization (to avoid blocking UI)
typedef struct {
  ethervox_governor_t* governor;
  tool_manifest_registry_t* manifest_registry;
  char model_path[512];
  bool optimize_new_only;
  int result;
  bool completed;
  pthread_mutex_t mutex;
} optimization_thread_data_t;

// Optimization thread function
static void* optimization_thread_func(void* arg) {
  optimization_thread_data_t* data = (optimization_thread_data_t*)arg;

  LOGI("Optimization thread started for model: %s", data->model_path);

  // Run V2 optimizer (this can take 30-60 seconds)
  ethervox_result_t result = ethervox_optimize_tool_prompts_v2(
      data->governor, data->model_path, data->manifest_registry, data->optimize_new_only);

  // Update result atomically
  pthread_mutex_lock(&data->mutex);
  data->result = result;
  data->completed = true;
  pthread_mutex_unlock(&data->mutex);

  if (ethervox_is_success(result)) {
    LOGI("Optimization thread completed successfully");
  } else {
    LOGE("Optimization thread failed with code: %d", result);
  }

  return NULL;
}

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_optimizeToolPrompts(
    JNIEnv* env, jobject thiz, jstring modelPath) {
  (void)thiz;

  if (!g_governor) {
    LOGE("Governor not loaded - cannot optimize tool prompts");
    return -1;
  }

  if (!g_manifest_registry) {
    LOGE("Manifest registry not initialized - ensure model is loaded first");
    return -2;
  }

  const char* model_path = (*env)->GetStringUTFChars(env, modelPath, NULL);
  if (!model_path) {
    LOGE("Failed to get model path string");
    return -3;
  }

  LOGI("Starting tool prompt optimization (V2) for model: %s", model_path);
  LOGI("This may take 30-60 seconds and is memory-intensive");

  // Allocate thread data
  optimization_thread_data_t* thread_data = malloc(sizeof(optimization_thread_data_t));
  if (!thread_data) {
    LOGE("Failed to allocate optimization thread data");
    (*env)->ReleaseStringUTFChars(env, modelPath, model_path);
    return -4;
  }

  // Initialize thread data
  thread_data->governor = g_governor;
  thread_data->manifest_registry = g_manifest_registry;
  snprintf(thread_data->model_path, sizeof(thread_data->model_path), "%s", model_path);
  thread_data->optimize_new_only = false;  // Always optimize all tools
  thread_data->result = -999;              // Sentinel for "not completed"
  thread_data->completed = false;
  pthread_mutex_init(&thread_data->mutex, NULL);

  LOGI("Thread data initialized: optimize_new_only=%s",
       thread_data->optimize_new_only ? "true" : "false");

  (*env)->ReleaseStringUTFChars(env, modelPath, model_path);

  // Create optimization thread (to avoid blocking UI)
  pthread_t thread;
  if (pthread_create(&thread, NULL, optimization_thread_func, thread_data) != 0) {
    LOGE("Failed to create optimization thread");
    pthread_mutex_destroy(&thread_data->mutex);
    free(thread_data);
    return -5;
  }

  // Detach thread and wait for completion
  pthread_detach(thread);

  // Wait for completion with periodic logging
  while (!thread_data->completed) {
    sleep(5);  // Check every 5 seconds

    pthread_mutex_lock(&thread_data->mutex);
    bool is_completed = thread_data->completed;
    pthread_mutex_unlock(&thread_data->mutex);

    if (!is_completed) {
      static int wait_seconds = 0;
      wait_seconds += 5;
      LOGI("Optimization in progress... (%d seconds elapsed)", wait_seconds);
    }
  }

  // Get final result
  pthread_mutex_lock(&thread_data->mutex);
  int result = thread_data->result;
  pthread_mutex_unlock(&thread_data->mutex);

  // Cleanup
  pthread_mutex_destroy(&thread_data->mutex);
  free(thread_data);

  if (ethervox_is_success(result)) {
    LOGI("Tool prompt optimization completed successfully");
  } else {
    LOGE("Tool prompt optimization failed with code: %d", result);
  }

  return result;
}

JNIEXPORT jobject JNICALL Java_com_droid_ethervox_1core_NativeLib_loadOptimizedPrompts(
    JNIEnv* env, jobject thiz, jstring modelPath) {
  (void)thiz;

  const char* model_path = (*env)->GetStringUTFChars(env, modelPath, NULL);
  if (!model_path) {
    LOGE("Failed to get model path string");
    return NULL;
  }

  // Allocate buffers for instruction and examples
  char instruction[4096] = {0};
  char examples[8192] = {0};

  ethervox_result_t result = ethervox_load_optimized_prompts(
      model_path, instruction, sizeof(instruction), examples, sizeof(examples));

  (*env)->ReleaseStringUTFChars(env, modelPath, model_path);

  if (ethervox_is_error(result)) {
    // No optimized prompts found
    return NULL;
  }

  // Create OptimizedPrompts object
  jclass optimizedPromptsClass = (*env)->FindClass(env, "com/droid/ethervox_core/OptimizedPrompts");
  if (!optimizedPromptsClass) {
    LOGE("Failed to find OptimizedPrompts class");
    return NULL;
  }

  jmethodID constructor = (*env)->GetMethodID(env, optimizedPromptsClass, "<init>",
                                              "(Ljava/lang/String;Ljava/lang/String;)V");
  if (!constructor) {
    LOGE("Failed to find OptimizedPrompts constructor");
    return NULL;
  }

  jstring instructionStr = (*env)->NewStringUTF(env, instruction);
  jstring examplesStr = (*env)->NewStringUTF(env, examples);

  jobject optimizedPrompts =
      (*env)->NewObject(env, optimizedPromptsClass, constructor, instructionStr, examplesStr);

  (*env)->DeleteLocalRef(env, instructionStr);
  (*env)->DeleteLocalRef(env, examplesStr);
  (*env)->DeleteLocalRef(env, optimizedPromptsClass);

  return optimizedPrompts;
}

/**
 * Get manifest registry information
 * Returns JSON string with current manifest state
 */
JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_getManifestInfo(JNIEnv* env,
                                                                                  jobject thiz) {
  (void)thiz;

  if (!g_manifest_registry) {
    // No manifest available - return minimal JSON
    return (*env)->NewStringUTF(
        env,
        "{\"available\":false,\"tool_count\":0,\"fallback_level\":3,\"optimized_loaded\":false,"
        "\"tools_detected\":false,\"tools_loaded_count\":0}");
  }

  ETHERVOX_LOGI("DEBUG getManifestInfo: reading tools_loaded_count=%u",
                g_manifest_registry->tools_loaded_count);

  // Build JSON response with guard status
  char json[512];
  snprintf(json, sizeof(json),
           "{\"available\":%s,\"tool_count\":%u,\"fallback_level\":%u,\"optimized_loaded\":%s,"
           "\"tools_detected\":%s,\"tools_loaded_count\":%u}",
           g_manifest_registry->tools_available ? "true" : "false",
           g_manifest_registry->header.tool_count, g_manifest_registry->fallback_level,
           g_manifest_registry->optimization_loaded ? "true" : "false",
           g_manifest_registry->tools_detected ? "true" : "false",
           g_manifest_registry->tools_loaded_count);

  return (*env)->NewStringUTF(env, json);
}

/**
 * Get the optimized tool prompts directory path
 * Returns the absolute path where optimized JSON files are stored/expected
 */
JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_getOptimizedDir(JNIEnv* env,
                                                                                  jobject thiz) {
  (void)thiz;

  char optimized_dir[512];

  if (g_android_files_dir[0] != '\0') {
    // Android: Use app files directory
    snprintf(optimized_dir, sizeof(optimized_dir), "%s/tools/optimized", g_android_files_dir);
  } else {
    // Fallback (should not happen on Android)
    const char* home = getenv("HOME");
    snprintf(optimized_dir, sizeof(optimized_dir), "%s/.ethervox/tools/optimized",
             home ? home : ".");
  }

  return (*env)->NewStringUTF(env, optimized_dir);
}

/**
 * Check if optimization file exists for a given model
 * Returns true if the optimized JSON file exists on disk
 */
JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_optimizationFileExists(
    JNIEnv* env, jobject thiz, jstring modelPath) {
  (void)thiz;

  if (!modelPath) {
    return JNI_FALSE;
  }

  const char* model_path_str = (*env)->GetStringUTFChars(env, modelPath, NULL);
  if (!model_path_str) {
    return JNI_FALSE;
  }

  // Extract model name from path (e.g., /path/to/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf ->
  // Qwen2.5-0.5B-Instruct-Q4_K_M)
  const char* filename = strrchr(model_path_str, '/');
  if (!filename) {
    filename = model_path_str;
  } else {
    filename++;  // Skip the '/'
  }

  // Remove .gguf extension
  char model_name[256];
  size_t i = 0;
  while (filename[i] && filename[i] != '.' && i < sizeof(model_name) - 1) {
    model_name[i] = filename[i];
    i++;
  }
  model_name[i] = '\0';

  // Remove trailing dash if present
  if (i > 0 && model_name[i - 1] == '-') {
    model_name[i - 1] = '\0';
  }

  (*env)->ReleaseStringUTFChars(env, modelPath, model_path_str);

  // Build path to optimization file
  char optimized_path[512];
  if (g_android_files_dir[0] != '\0') {
    snprintf(optimized_path, sizeof(optimized_path), "%s/tools/optimized/%s.json",
             g_android_files_dir, model_name);
  } else {
    const char* home = getenv("HOME");
    snprintf(optimized_path, sizeof(optimized_path), "%s/.ethervox/tools/optimized/%s.json",
             home ? home : ".", model_name);
  }

  // Check if file exists
  struct stat st;
  jboolean exists = (stat(optimized_path, &st) == 0) ? JNI_TRUE : JNI_FALSE;

  ETHERVOX_LOGI("Checking optimization file: %s -> %s", optimized_path,
                exists ? "EXISTS" : "NOT FOUND");

  return exists;
}

/**
 * Run comprehensive integration tests
 */
JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_runIntegrationTests(JNIEnv* env,
                                                                                   jobject thiz) {
  (void)env;
  (void)thiz;

  LOGI("Running integration tests...");
  run_integration_tests(g_governor);

  // Log completion with test reports path
  if (g_android_files_dir[0] != '\0') {
    LOGI("Integration tests complete. Reports saved to: %s/tests/", g_android_files_dir);
  } else {
    LOGI("Integration tests complete");
  }
}

/**
 * Run LLM tool usage tests
 */
JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_runLlmToolTests(JNIEnv* env,
                                                                               jobject thiz,
                                                                               jstring modelPath,
                                                                               jboolean verbose) {
  (void)thiz;

  if (!g_governor) {
    LOGE("Governor not loaded - cannot run LLM tool tests");
    return;
  }

  if (!g_memory_store) {
    LOGE("Memory store not initialized - cannot run LLM tool tests");
    return;
  }

  const char* model_path_str = (*env)->GetStringUTFChars(env, modelPath, NULL);
  if (!model_path_str) {
    LOGE("Failed to get model path string");
    return;
  }

  LOGI("Running LLM tool tests (verbose=%d)...", verbose);
  run_llm_tool_tests(g_governor, g_memory_store, model_path_str, (bool)verbose, NULL);

  // Log completion with test reports path
  if (g_android_files_dir[0] != '\0') {
    LOGI("LLM tool tests complete. Reports saved to: %s/tests/", g_android_files_dir);
  } else {
    LOGI("LLM tool tests complete");
  }

  (*env)->ReleaseStringUTFChars(env, modelPath, model_path_str);
}

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_platformInit(JNIEnv* env,
                                                                            jobject thiz) {
  (void)thiz;

  if (g_platform) {
    LOGI("Platform already initialized");
    return 0;
  }

  g_platform = (ethervox_platform_t*)calloc(1, sizeof(ethervox_platform_t));
  if (!g_platform) {
    LOGE("Failed to allocate platform");
    return -1;
  }

  ethervox_result_t result = ethervox_platform_init(g_platform);
  if (ethervox_is_error(result)) {
    LOGE("Failed to initialize platform");
    free(g_platform);
    g_platform = NULL;
    return -1;
  }

  // === DIRECT GOVERNOR INITIALIZATION (matches desktop main.c) ===
  // REFACTORED 2025-12-09: Removed dialogue_engine wrapper
  // Android now mirrors desktop architecture for consistency

  // Initialize memory store BEFORE tools so they can register with it
  g_memory_store = (ethervox_memory_store_t*)calloc(1, sizeof(ethervox_memory_store_t));
  if (g_memory_store) {
    // Initialize with NULL session_id (auto-generated) and NULL storage_dir (set later via JNI)
    ethervox_result_t mem_result = ethervox_memory_init(g_memory_store, NULL, NULL);
    if (ethervox_is_error(mem_result)) {
      LOGE("Failed to initialize memory store - error code: %d", mem_result);
      free(g_memory_store);
      g_memory_store = NULL;
    } else {
      LOGI("Memory store initialized successfully (in-memory mode until storage dir set)");
    }
  } else {
    LOGE("Failed to allocate memory store");
  }

  // Create and initialize tool registry
  g_registry = (ethervox_tool_registry_t*)malloc(sizeof(ethervox_tool_registry_t));
  if (!g_registry) {
    LOGE("Failed to allocate tool registry");
    if (g_memory_store) {
      ethervox_memory_cleanup(g_memory_store);
      free(g_memory_store);
      g_memory_store = NULL;
    }
    ethervox_platform_cleanup(g_platform);
    free(g_platform);
    g_platform = NULL;
    return -1;
  }

  ethervox_result_t registry_result = ethervox_tool_registry_init(g_registry, 16);
  if (ethervox_is_error(registry_result)) {
    LOGE("Failed to initialize tool registry - error code: %d", registry_result);
    free(g_registry);
    g_registry = NULL;
    if (g_memory_store) {
      ethervox_memory_cleanup(g_memory_store);
      free(g_memory_store);
      g_memory_store = NULL;
    }
    ethervox_platform_cleanup(g_platform);
    free(g_platform);
    g_platform = NULL;
    return -1;
  }

  LOGI("Tool registry initialized successfully");

  // Register compute tools (math, logic, etc.)
  int tool_count = ethervox_compute_tools_register_all(g_registry);
  LOGI("Registered %d compute tools", tool_count);

  // Register timer/alarm tools
  ethervox_tool_registry_add(g_registry, ethervox_tool_timer_create());
  ethervox_tool_registry_add(g_registry, ethervox_tool_timer_cancel());
  ethervox_tool_registry_add(g_registry, ethervox_tool_timer_list());
  ethervox_tool_registry_add(g_registry, ethervox_tool_alarm_create());
  tool_count += 4;

  // Register memory tools if memory store is available
  if (g_memory_store) {
    ethervox_result_t mem_tools_result = ethervox_memory_tools_register(g_registry, g_memory_store);
    if (ethervox_is_success(mem_tools_result)) {
      tool_count += 6;  // 6 memory tools
      LOGI("Registered memory tools successfully");
    } else {
      LOGE("Failed to register memory tools - error code: %d", mem_tools_result);
    }
  } else {
    LOGI("Memory store not available, skipping memory tools registration");
  }

  LOGI("Total tools registered: %d", tool_count);

  // NOTE: Governor initialization moved to platformInitGovernor()
  // This allows user to choose minimal vs full mode before governor init

  LOGI(
      "Platform, memory store, and tool registry initialized successfully (Governor pending user "
      "choice)");

  return 0;
}

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_platformInitGovernor(
    JNIEnv* env, jobject thiz, jboolean minimal_mode) {
  (void)env;
  (void)thiz;

  if (g_governor) {
    LOGI("Governor already initialized");
    return 0;
  }

  if (!g_platform || !g_registry) {
    LOGE("Platform not initialized - call platformInit() first");
    return -1;
  }

  // Initialize Governor with chosen config (minimal or full)
  ethervox_governor_config_t gov_config = ethervox_governor_default_config();

  if (minimal_mode) {
    gov_config.system_prompt_mode = ETHERVOX_GOVERNOR_MODE_MINIMAL;
    LOGI("Initializing Governor in MINIMAL MODE (fast loading, tools disabled)");
  } else {
    gov_config.system_prompt_mode = ETHERVOX_GOVERNOR_MODE_FULL;
    LOGI("Initializing Governor in FULL MODE (all tools available)");
  }

  ethervox_result_t gov_result = ethervox_governor_init(&g_governor, &gov_config, g_registry);
  if (ethervox_is_error(gov_result)) {
    LOGE("Failed to initialize Governor - error code: %d", gov_result);
    return -1;
  }

  LOGI("Governor initialized successfully (mode: %s)", minimal_mode ? "MINIMAL" : "FULL");

  return 0;
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_platformCleanup(JNIEnv* env,
                                                                               jobject thiz) {
  (void)env;
  (void)thiz;

  // Cleanup in reverse order of initialization

  if (g_manifest_registry) {
    // Manifest registry cleanup (if needed)
    free(g_manifest_registry);
    g_manifest_registry = NULL;
    LOGI("Manifest registry cleaned up");
  }

  if (g_governor) {
    ethervox_governor_cleanup(g_governor);
    free(g_governor);
    g_governor = NULL;
    LOGI("Governor cleaned up");
  }

  if (g_registry) {
    ethervox_tool_registry_cleanup(g_registry);
    free(g_registry);
    g_registry = NULL;
    LOGI("Tool registry cleaned up");
  }

  if (g_memory_store) {
    ethervox_memory_cleanup(g_memory_store);
    free(g_memory_store);
    g_memory_store = NULL;
    LOGI("Memory store cleaned up");
  }

  if (g_platform) {
    ethervox_platform_cleanup(g_platform);
    free(g_platform);
    g_platform = NULL;
    LOGI("Platform cleaned up");
  }
}

// DEPRECATED: This function is no longer used.
// Initialization now happens in platformInit() using direct governor/registry globals.
// Kept for binary compatibility with old Java code, but returns success immediately.
JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_initializeWithModel(
    JNIEnv* env, jobject thiz, jstring model_path, jfloat temperature, jint max_tokens,
    jfloat top_p, jint context_length) {
  (void)env;
  (void)thiz;
  (void)model_path;
  (void)temperature;
  (void)max_tokens;
  (void)top_p;
  (void)context_length;

  LOGI("initializeWithModel() called but deprecated - use platformInit() instead");

  // Return success if already initialized via platformInit
  if (g_governor) {
    return JNI_TRUE;
  }

  LOGE("Governor not initialized - call platformInit() first");
  return JNI_FALSE;
}

// DEPRECATED: Old dialogue engine API - runtime param updates not supported by governor
// Governor uses config set at initialization time only
JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_updateLLMParams(
    JNIEnv* env, jobject thiz, jfloat temperature, jint max_tokens, jfloat top_p) {
  (void)env;
  (void)thiz;
  (void)temperature;
  (void)max_tokens;
  (void)top_p;

  LOGW("updateLLMParams() deprecated - governor does not support runtime param updates");
  LOGW("LLM parameters are set at initialization time only");
  return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_isLlmLoaded(JNIEnv* env,
                                                                               jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_governor) {
    return JNI_FALSE;
  }

  // Use governor API to check if model is loaded
  return ethervox_governor_is_loaded(g_governor) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_getPlatformName(JNIEnv* env,
                                                                                  jobject thiz) {
  (void)thiz;

  const char* name = ethervox_platform_get_name();
  return create_jstring(env, name);
}

// ===========================================================================
// Audio Initialization
// ===========================================================================

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_audioInit(JNIEnv* env, jobject thiz,
                                                                         jint sample_rate,
                                                                         jint channels,
                                                                         jint buffer_size) {
  (void)thiz;

  if (g_audio_runtime) {
    LOGI("Audio already initialized");
    return 0;
  }

  g_audio_runtime = (ethervox_audio_runtime_t*)calloc(1, sizeof(ethervox_audio_runtime_t));
  if (!g_audio_runtime) {
    LOGE("Failed to allocate audio runtime");
    return -1;
  }

  ethervox_audio_config_t config = ethervox_audio_get_default_config();
  config.sample_rate = (uint32_t)sample_rate;
  config.channels = (uint16_t)channels;
  config.buffer_size = (uint32_t)buffer_size;

  ethervox_result_t result = ethervox_audio_init(g_audio_runtime, &config);
  if (ethervox_is_error(result)) {
    LOGE("Failed to initialize audio");
    free(g_audio_runtime);
    g_audio_runtime = NULL;
    return -1;
  }

  LOGI("Audio initialized: %d Hz, %d channels, buffer=%d", sample_rate, channels, buffer_size);
  return 0;
}

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_audioStartCapture(JNIEnv* env,
                                                                                 jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_audio_runtime) {
    LOGE("Audio not initialized");
    return -1;
  }

  ethervox_result_t result = ethervox_audio_start_capture(g_audio_runtime);
  if (ethervox_is_error(result)) {
    LOGE("Failed to start audio capture");
    return -1;
  }

  LOGI("Audio capture started");
  return 0;
}

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_audioStopCapture(JNIEnv* env,
                                                                                jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_audio_runtime) {
    return 0;
  }

  ethervox_result_t result = ethervox_audio_stop_capture(g_audio_runtime);
  LOGI("Audio capture stopped");
  return ethervox_is_success(result) ? 0 : -1;
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_audioCleanup(JNIEnv* env,
                                                                            jobject thiz) {
  (void)env;
  (void)thiz;

  if (g_audio_runtime) {
    ethervox_audio_cleanup(g_audio_runtime);
    free(g_audio_runtime);
    g_audio_runtime = NULL;
    LOGI("Audio cleaned up");
  }
}

// ===========================================================================
// Wake Word Detection
// ===========================================================================

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_wakeWordInit(JNIEnv* env,
                                                                            jobject thiz,
                                                                            jstring wake_word,
                                                                            jfloat sensitivity) {
  (void)thiz;

  if (g_wake_runtime) {
    LOGI("Wake word already initialized");
    return 0;
  }

  g_wake_runtime = (ethervox_wake_runtime_t*)calloc(1, sizeof(ethervox_wake_runtime_t));
  if (!g_wake_runtime) {
    LOGE("Failed to allocate wake word runtime");
    return -1;
  }

  ethervox_wake_config_t config = ethervox_wake_get_default_config();

  const char* wake_word_str = (*env)->GetStringUTFChars(env, wake_word, NULL);
  config.wake_word = wake_word_str;
  config.sensitivity = (float)sensitivity;

  ethervox_result_t result = ethervox_wake_init(g_wake_runtime, &config);

  (*env)->ReleaseStringUTFChars(env, wake_word, wake_word_str);

  if (ethervox_is_error(result)) {
    LOGE("Failed to initialize wake word");
    free(g_wake_runtime);
    g_wake_runtime = NULL;
    return -1;
  }

  LOGI("Wake word initialized with sensitivity %.2f", sensitivity);
  return 0;
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_wakeWordCleanup(JNIEnv* env,
                                                                               jobject thiz) {
  (void)env;
  (void)thiz;

  if (g_wake_runtime) {
    // Note: would call ethervox_wake_cleanup if it exists
    free(g_wake_runtime);
    g_wake_runtime = NULL;
    LOGI("Wake word cleaned up");
  }
}

// ===========================================================================
// Speech-to-Text
// ===========================================================================

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_sttInit(JNIEnv* env, jobject thiz,
                                                                       jstring model_path,
                                                                       jstring language) {
  (void)thiz;

  if (g_stt_runtime) {
    LOGI("STT already initialized");
    return 0;
  }

  g_stt_runtime = (ethervox_stt_runtime_t*)calloc(1, sizeof(ethervox_stt_runtime_t));
  if (!g_stt_runtime) {
    LOGE("Failed to allocate STT runtime");
    return -1;
  }

  ethervox_stt_config_t config = ethervox_stt_get_default_config();

  const char* model_path_str = (*env)->GetStringUTFChars(env, model_path, NULL);
  const char* language_str = (*env)->GetStringUTFChars(env, language, NULL);

  config.model_path = model_path_str;
  config.language = language_str;

  ethervox_result_t result = ethervox_stt_init(g_stt_runtime, &config);

  (*env)->ReleaseStringUTFChars(env, model_path, model_path_str);
  (*env)->ReleaseStringUTFChars(env, language, language_str);

  if (ethervox_is_error(result)) {
    LOGE("Failed to initialize STT");
    free(g_stt_runtime);
    g_stt_runtime = NULL;
    return -1;
  }

  LOGI("STT initialized");
  return 0;
}

JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_sttStart(JNIEnv* env, jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_stt_runtime) {
    LOGE("STT not initialized");
    return -1;
  }

  ethervox_result_t result = ethervox_stt_start(g_stt_runtime);
  if (ethervox_is_error(result)) {
    LOGE("Failed to start STT");
    return -1;
  }

  LOGI("STT started");
  return 0;
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_sttCleanup(JNIEnv* env,
                                                                          jobject thiz) {
  (void)env;
  (void)thiz;

  if (g_stt_runtime) {
    // Note: would call ethervox_stt_cleanup if it exists
    free(g_stt_runtime);
    g_stt_runtime = NULL;
    LOGI("STT cleaned up");
  }
}

// ===========================================================================
// LLM / Dialogue Management
// ===========================================================================

// DEPRECATED: Legacy dialogue processing - use processGovernorQuery instead
// This function is kept for backwards compatibility but now delegates to governor
JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_processDialogue(
    JNIEnv* env, jobject thiz, jstring user_text, jstring language) {
  (void)thiz;
  (void)language;  // Language detection now handled by governor

  if (!g_governor) {
    LOGE("Governor not initialized");
    return create_jstring(env, "[ERROR] Governor not initialized");
  }

  const char* text = (*env)->GetStringUTFChars(env, user_text, NULL);

  LOGI("Processing dialogue (legacy): '%s'", text);

  // Process with governor execute API
  char* response = NULL;
  char* error = NULL;
  ethervox_governor_status_t status =
      ethervox_governor_execute(g_governor, text, &response, &error, NULL, NULL, NULL, NULL);

  (*env)->ReleaseStringUTFChars(env, user_text, text);

  if (status != ETHERVOX_GOVERNOR_SUCCESS || !response) {
    LOGE("Failed to process query: %s", error ? error : "Unknown error");
    jstring result = create_jstring(env, "[ERROR] Failed to generate response");
    if (response)
      free(response);
    if (error)
      free(error);
    return result;
  }

  // Format response with confidence placeholder for backwards compatibility
  char formatted_response[4096];
  snprintf(formatted_response, sizeof(formatted_response), "%s|100|false", response);

  jstring result = create_jstring(env, formatted_response);
  free(response);
  if (error)
    free(error);

  return result;
}

// Global callback context for streaming
typedef struct {
  JNIEnv* env;
  jobject callback_obj;
  jmethodID on_token_method;
  jmethodID on_complete_method;
  jmethodID on_error_method;
  jmethodID on_governor_progress_method;  // New: Governor progress callback
  bool conversation_ended;
  char utf8_buffer[4];  // Buffer for incomplete UTF-8 sequences (max 4 bytes)
  int utf8_buffer_len;  // Number of bytes currently in buffer
} jni_stream_context_t;

// Native callback function that will be called from C
static void native_token_callback(const char* token, void* user_data) {
  jni_stream_context_t* ctx = (jni_stream_context_t*)user_data;
  if (!ctx || !ctx->env || !ctx->callback_obj) {
    return;
  }

  // Guard against NULL token
  if (!token) {
    token = "(null token)";
  }

  // Log each token for debugging streaming issues
  __android_log_print(ANDROID_LOG_DEBUG, "EthervoxJNI", "Streaming token: '%s'", token);

  // Combine buffered incomplete UTF-8 bytes with new token
  size_t token_len = strlen(token);
  size_t combined_len = ctx->utf8_buffer_len + token_len;
  char* combined = malloc(combined_len + 1);
  if (!combined) {
    __android_log_print(ANDROID_LOG_ERROR, "EthervoxJNI", "Failed to allocate buffer");
    return;
  }

  // Copy buffered bytes + new token
  if (ctx->utf8_buffer_len > 0) {
    memcpy(combined, ctx->utf8_buffer, ctx->utf8_buffer_len);
    __android_log_print(ANDROID_LOG_DEBUG, "EthervoxJNI", "Prepending %d buffered bytes to token",
                        ctx->utf8_buffer_len);
  }
  memcpy(combined + ctx->utf8_buffer_len, token, token_len);
  combined[combined_len] = '\0';

  // Validate and find how much is valid UTF-8
  size_t valid_len = 0;
  bool is_valid = validate_and_fix_utf8(combined, &valid_len);

  if (!is_valid && valid_len < combined_len) {
    // There are incomplete bytes at the end - buffer them for next token
    ctx->utf8_buffer_len = combined_len - valid_len;
    memcpy(ctx->utf8_buffer, combined + valid_len, ctx->utf8_buffer_len);
    __android_log_print(ANDROID_LOG_DEBUG, "EthervoxJNI",
                        "Buffering %d incomplete UTF-8 bytes for next token", ctx->utf8_buffer_len);
  } else {
    // Everything is valid or we used all bytes
    ctx->utf8_buffer_len = 0;
  }

  // Send only the valid portion to Java
  if (valid_len > 0) {
    char* valid_str = malloc(valid_len + 1);
    if (valid_str) {
      memcpy(valid_str, combined, valid_len);
      valid_str[valid_len] = '\0';

      jstring j_token = (*ctx->env)->NewStringUTF(ctx->env, valid_str);
      (*ctx->env)->CallVoidMethod(ctx->env, ctx->callback_obj, ctx->on_token_method, j_token);
      (*ctx->env)->DeleteLocalRef(ctx->env, j_token);

      free(valid_str);
    }
  }

  free(combined);
}

// Governor progress callback function
static void native_governor_progress_callback(ethervox_governor_event_type_t event_type,
                                              const char* message, void* user_data) {
  jni_stream_context_t* ctx = (jni_stream_context_t*)user_data;
  if (!ctx || !ctx->env || !ctx->callback_obj || !ctx->on_governor_progress_method) {
    return;  // Callback not set or not available
  }

  // Guard against NULL message
  if (!message) {
    message = "(null message)";
  }

  // Map event type to string
  const char* event_str;
  switch (event_type) {
    case ETHERVOX_GOVERNOR_EVENT_ITERATION_START:
      event_str = "ITERATION_START";
      break;
    case ETHERVOX_GOVERNOR_EVENT_THINKING:
      event_str = "THINKING";
      break;
    case ETHERVOX_GOVERNOR_EVENT_TOOL_CALL:
      event_str = "TOOL_CALL";
      break;
    case ETHERVOX_GOVERNOR_EVENT_TOOL_RESULT:
      event_str = "TOOL_RESULT";
      break;
    case ETHERVOX_GOVERNOR_EVENT_TOOL_ERROR:
      event_str = "TOOL_ERROR";
      break;
    case ETHERVOX_GOVERNOR_EVENT_CONFIDENCE_UPDATE:
      event_str = "CONFIDENCE_UPDATE";
      break;
    case ETHERVOX_GOVERNOR_EVENT_CONTEXT_SUMMARIZING:
      event_str = "CONTEXT_SUMMARIZING";
      break;
    case ETHERVOX_GOVERNOR_EVENT_CONTEXT_CLEARED:
      event_str = "CONTEXT_CLEARED";
      break;
    case ETHERVOX_GOVERNOR_EVENT_MANIFEST_LOADING:
      event_str = "MANIFEST_LOADING";
      break;
    case ETHERVOX_GOVERNOR_EVENT_MANIFEST_READY:
      event_str = "MANIFEST_READY";
      break;
    case ETHERVOX_GOVERNOR_EVENT_MANIFEST_FALLBACK_LEVEL_1:
      event_str = "MANIFEST_FALLBACK_LEVEL_1";
      break;
    case ETHERVOX_GOVERNOR_EVENT_MANIFEST_FALLBACK_LEVEL_2:
      event_str = "MANIFEST_FALLBACK_LEVEL_2";
      break;
    case ETHERVOX_GOVERNOR_EVENT_COMPLETE:
      event_str = "COMPLETE";
      // Also call onComplete callback when governor finishes
      if (ctx->on_complete_method) {
        (*ctx->env)->CallVoidMethod(ctx->env, ctx->callback_obj, ctx->on_complete_method,
                                    (jboolean)ctx->conversation_ended);
      }
      break;
    default:
      event_str = "UNKNOWN";
  }

  __android_log_print(ANDROID_LOG_INFO, "EthervoxGovernor", "[%s] %s", event_str, message);

  jstring j_event = create_jstring(ctx->env, event_str);
  jstring j_message = create_jstring(ctx->env, message);
  (*ctx->env)->CallVoidMethod(ctx->env, ctx->callback_obj, ctx->on_governor_progress_method,
                              j_event, j_message);
  (*ctx->env)->DeleteLocalRef(ctx->env, j_event);
  (*ctx->env)->DeleteLocalRef(ctx->env, j_message);
}

// Governor streaming dialogue - uses token_callback for real-time token streaming
JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_processDialogueStreamingNative(
    JNIEnv* env, jobject thiz, jstring user_text, jstring language, jobject callback) {
  (void)thiz;
  (void)language;

  if (!g_governor) {
    LOGE("Governor not initialized");
    // Call error callback
    jclass callback_class = (*env)->GetObjectClass(env, callback);
    jmethodID on_error =
        (*env)->GetMethodID(env, callback_class, "onError", "(Ljava/lang/String;)V");
    if (on_error) {
      jstring error_msg = create_jstring(env, "Governor not initialized");
      (*env)->CallVoidMethod(env, callback, on_error, error_msg);
      (*env)->DeleteLocalRef(env, error_msg);
    }
    return;
  }

  const char* text = (*env)->GetStringUTFChars(env, user_text, NULL);

  LOGI("Processing dialogue (streaming): '%s'", text);

  // Get callback methods
  jclass callback_class = (*env)->GetObjectClass(env, callback);
  jmethodID on_token = (*env)->GetMethodID(env, callback_class, "onToken", "(Ljava/lang/String;)V");
  jmethodID on_complete = (*env)->GetMethodID(env, callback_class, "onComplete", "(Z)V");
  jmethodID on_error = (*env)->GetMethodID(env, callback_class, "onError", "(Ljava/lang/String;)V");
  jmethodID on_governor_progress = (*env)->GetMethodID(env, callback_class, "onGovernorProgress",
                                                       "(Ljava/lang/String;Ljava/lang/String;)V");

  // Setup streaming context
  jni_stream_context_t stream_ctx = {.env = env,
                                     .callback_obj = callback,
                                     .on_token_method = on_token,
                                     .on_complete_method = on_complete,
                                     .on_error_method = on_error,
                                     .on_governor_progress_method = on_governor_progress,
                                     .conversation_ended = false,
                                     .utf8_buffer_len = 0};

  // Execute with streaming token callback
  char* response = NULL;
  char* error = NULL;
  ethervox_governor_status_t status =
      ethervox_governor_execute(g_governor, text, &response, &error,
                                NULL,                               // metrics (optional)
                                native_governor_progress_callback,  // progress callback
                                native_token_callback,              // token callback for streaming
                                &stream_ctx                         // user data
      );

  (*env)->ReleaseStringUTFChars(env, user_text, text);

  if (status != ETHERVOX_GOVERNOR_SUCCESS || !response) {
    LOGE("Failed to process query: %s", error ? error : "Unknown error");
    if (on_error) {
      jstring error_msg = create_jstring(env, error ? error : "Failed to generate response");
      (*env)->CallVoidMethod(env, callback, on_error, error_msg);
      (*env)->DeleteLocalRef(env, error_msg);
    }
    if (response)
      free(response);
    if (error)
      free(error);
  } else {
    // NOTE: onComplete is already called via native_governor_progress_callback
    // from inside ethervox_governor_execute when it sends ETHERVOX_GOVERNOR_EVENT_COMPLETE.
    // DO NOT call it again here, as that causes a double-invocation and crashes.
    // Just clean up the response memory.
    free(response);
    if (error)
      free(error);
  }

  LOGI("Streaming dialogue complete");
}

// DEPRECATED: Old dialogue engine API - cancellation not yet implemented for governor
// TODO: Add governor cancellation support using llama_cancel flag
JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_cancelProcessing(JNIEnv* env,
                                                                                jobject thiz) {
  (void)env;
  (void)thiz;

  LOGW("cancelProcessing() deprecated - governor cancellation not yet implemented");
  LOGW("TODO: Add governor-level cancellation support");
}

// DEPRECATED: Language handling is now done by the governor's language detection
JNIEXPORT jint JNICALL Java_com_droid_ethervox_1core_NativeLib_setDialogueLanguage(
    JNIEnv* env, jobject thiz, jstring language) {
  (void)thiz;
  (void)language;

  const char* lang = (*env)->GetStringUTFChars(env, language, NULL);
  LOGW("setDialogueLanguage(%s) deprecated - language detection automatic", lang);
  (*env)->ReleaseStringUTFChars(env, language, lang);
  return 0;  // Success for compatibility
}

// DEPRECATED: Returns default language for compatibility
JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getDialogueLanguage(JNIEnv* env, jobject thiz) {
  (void)thiz;

  LOGW("getDialogueLanguage() deprecated - returning default 'en'");
  return create_jstring(env, "en");
}

// ===========================================================================
// Test/Utility Functions
// ===========================================================================

JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_stringFromJNI(JNIEnv* env,
                                                                                jobject thiz) {
  (void)thiz;

  const char* hello = "Hello from EthervoxAI native library!";
  LOGI("%s", hello);
  return create_jstring(env, hello);
}

JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_getVersion(JNIEnv* env,
                                                                             jobject thiz) {
  (void)thiz;
  return create_jstring(env, ETHERVOX_VERSION_STRING);
}

JNIEXPORT jstring JNICALL Java_com_droid_ethervox_1core_NativeLib_getBackendVersion(JNIEnv* env,
                                                                                    jobject thiz) {
  (void)thiz;
  // ETHERVOX_BACKEND_VERSION is guaranteed to be defined by CMake
  return create_jstring(env, ETHERVOX_BACKEND_VERSION);
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getActiveTimerStatus(JNIEnv* env, jobject thiz) {
  (void)thiz;

  char* status = ethervox_timer_get_active_status();
  if (!status) {
    return create_jstring(env, "{\"has_timer\": false}");
  }

  jstring result = create_jstring(env, status);
  free(status);
  return result;
}

JNIEXPORT jobject JNICALL
Java_com_droid_ethervox_1core_NativeLib_getDefaultLlmConfig(JNIEnv* env, jobject thiz) {
  (void)thiz;

  // Return default LLM config values directly
  ethervox_llm_config_t config = {
      .temperature = 0.7f, .max_tokens = 512, .top_p = 0.9f, .context_length = 2048};

  // Find the LlmConfig class
  jclass llmConfigClass = (*env)->FindClass(env, "com/droid/ethervox_core/LlmConfig");
  if (llmConfigClass == NULL) {
    return NULL;
  }

  // Get the constructor (Float, Int, Float, Int)
  jmethodID constructor = (*env)->GetMethodID(env, llmConfigClass, "<init>", "(FIFI)V");
  if (constructor == NULL) {
    (*env)->DeleteLocalRef(env, llmConfigClass);
    return NULL;
  }

  // Create the object
  jobject llmConfigObj =
      (*env)->NewObject(env, llmConfigClass, constructor, (jfloat)config.temperature,
                        (jint)config.max_tokens, (jfloat)config.top_p, (jint)config.context_length);

  (*env)->DeleteLocalRef(env, llmConfigClass);
  return llmConfigObj;
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getDefaultStartupPrompt(JNIEnv* env, jobject thiz) {
  (void)thiz;
  return (*env)->NewStringUTF(env, DEFAULT_STARTUP_PROMPT);
}

JNIEXPORT jobjectArray JNICALL
Java_com_droid_ethervox_1core_NativeLib_getSupportedLanguages(JNIEnv* env, jobject thiz) {
  (void)thiz;

  // Return supported languages directly
  static const char* languages[] = {"en", "es", "fr", "de", "it", "pt", "zh", "ja", "ko", NULL};

  // Count languages
  int count = 0;
  while (languages[count] != NULL) {
    count++;
  }

  // Create Java String array
  jclass stringClass = (*env)->FindClass(env, "java/lang/String");
  jobjectArray languageArray = (*env)->NewObjectArray(env, count, stringClass, NULL);

  // Populate array with language codes
  for (int i = 0; i < count; i++) {
    jstring langCode = create_jstring(env, languages[i]);
    (*env)->SetObjectArrayElement(env, languageArray, i, langCode);
    (*env)->DeleteLocalRef(env, langCode);
  }

  return languageArray;
}

// ===========================================================================
// Debug Mode Control
// ===========================================================================

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_setMemoryStorageDir(
    JNIEnv* env, jobject thiz, jstring storage_dir) {
  (void)thiz;

  if (!g_memory_store) {
    LOGE("Memory store not initialized");
    return;
  }

  const char* dir_path = (*env)->GetStringUTFChars(env, storage_dir, NULL);
  if (!dir_path) {
    LOGE("Failed to get storage directory path");
    return;
  }

  // Re-initialize memory store with storage directory
  // This will enable file persistence
  ethervox_result_t mem_init_result = ethervox_memory_init(g_memory_store, NULL, dir_path);
  if (ethervox_is_error(mem_init_result)) {
    LOGE("Failed to set memory storage directory: %s", dir_path);
    (*env)->ReleaseStringUTFChars(env, storage_dir, dir_path);
    return;
  }

  LOGI("Memory storage directory set to: %s", g_memory_store->storage_filepath);

  // Use platform-agnostic function to load previous session
  // This handles all the complexity of finding the most recent file,
  // preserving tags, IDs, and adding the "imported" tag
  uint32_t turns_loaded = 0;
  ethervox_result_t load_result =
      ethervox_memory_load_previous_session(g_memory_store, &turns_loaded);
  if (ethervox_is_success(load_result) && turns_loaded > 0) {
    LOGI("Memory: Loaded %u previous memories from last session", turns_loaded);
  }

  (*env)->ReleaseStringUTFChars(env, storage_dir, dir_path);
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_setDebugMode(JNIEnv* env,
                                                                            jobject thiz,
                                                                            jboolean enabled) {
  g_ethervox_debug_enabled = enabled ? 1 : 0;
  LOGI("Debug mode %s", g_ethervox_debug_enabled ? "enabled" : "disabled");
}

JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_getDebugMode(JNIEnv* env,
                                                                                jobject thiz) {
  return (jboolean)(g_ethervox_debug_enabled ? JNI_TRUE : JNI_FALSE);
}

// C callback that forwards logs to Java
/**
 * Sanitize UTF-8 string to be compatible with Java's Modified UTF-8
 * Replaces invalid or problematic bytes with '?'
 */
static void sanitize_utf8_for_java(char* buffer, size_t max_len) {
  if (!buffer)
    return;

  size_t i = 0;
  while (i < max_len && buffer[i] != '\0') {
    unsigned char c = buffer[i];

    // Check for invalid UTF-8 sequences
    if (c >= 0x80) {
      // Multi-byte UTF-8 sequence
      int bytes = 0;
      if ((c & 0xE0) == 0xC0)
        bytes = 2;
      else if ((c & 0xF0) == 0xE0)
        bytes = 3;
      else if ((c & 0xF8) == 0xF0)
        bytes = 4;

      // Verify continuation bytes
      bool valid = true;
      for (int j = 1; j < bytes && (i + j) < max_len; j++) {
        if ((buffer[i + j] & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }

      // Replace invalid sequences with '?'
      if (!valid || bytes == 0) {
        buffer[i] = '?';
        i++;
      } else {
        // Valid UTF-8 sequence, but Java Modified UTF-8 doesn't support 4-byte sequences
        // Replace 4-byte sequences with '?'
        if (bytes == 4) {
          buffer[i] = '?';
          for (int j = 1; j < bytes && (i + j) < max_len; j++) {
            memmove(&buffer[i + 1], &buffer[i + bytes], strlen(&buffer[i + bytes]) + 1);
          }
        } else {
          i += bytes;
        }
      }
    } else {
      i++;
    }
  }
}

static void java_log_callback_wrapper(int level, const char* tag, const char* message) {
  if (!g_jvm || !g_log_callback_obj || !g_log_callback_method) {
    return;
  }

  // Guard against NULL pointers
  if (!tag)
    tag = "EthervoxCore";
  if (!message)
    message = "";

  JNIEnv* env = NULL;
  jint result = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);

  bool detach = false;
  if (result == JNI_EDETACHED) {
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
      return;
    }
    detach = true;
  }

  // Sanitize strings for Java Modified UTF-8 compatibility
  char safe_tag[256];
  char safe_message[4096];
  strncpy(safe_tag, tag, sizeof(safe_tag) - 1);
  safe_tag[sizeof(safe_tag) - 1] = '\0';
  strncpy(safe_message, message, sizeof(safe_message) - 1);
  safe_message[sizeof(safe_message) - 1] = '\0';

  sanitize_utf8_for_java(safe_tag, sizeof(safe_tag));
  sanitize_utf8_for_java(safe_message, sizeof(safe_message));

  jstring jTag = (*env)->NewStringUTF(env, safe_tag);
  jstring jMessage = (*env)->NewStringUTF(env, safe_message);

  if (jTag && jMessage) {
    (*env)->CallVoidMethod(env, g_log_callback_obj, g_log_callback_method, level, jTag, jMessage);
  }

  if (jTag)
    (*env)->DeleteLocalRef(env, jTag);
  if (jMessage)
    (*env)->DeleteLocalRef(env, jMessage);

  if (detach) {
    (*g_jvm)->DetachCurrentThread(g_jvm);
  }
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_setLogCallback(JNIEnv* env,
                                                                              jobject thiz,
                                                                              jobject callback) {
  // Store JavaVM reference if not already stored
  if (!g_jvm) {
    (*env)->GetJavaVM(env, &g_jvm);
  }

  // Clear old callback
  if (g_log_callback_obj) {
    (*env)->DeleteGlobalRef(env, g_log_callback_obj);
    g_log_callback_obj = NULL;
    g_log_callback_method = NULL;
    g_ethervox_log_callback = NULL;
  }

  // Register new callback
  if (callback) {
    g_log_callback_obj = (*env)->NewGlobalRef(env, callback);

    jclass callbackClass = (*env)->GetObjectClass(env, callback);
    g_log_callback_method = (*env)->GetMethodID(env, callbackClass, "onLog",
                                                "(ILjava/lang/String;Ljava/lang/String;)V");

    if (g_log_callback_method) {
      g_ethervox_log_callback = java_log_callback_wrapper;
      LOGI("Log callback registered successfully");
    } else {
      LOGE("Failed to find onLog method in callback object");
      (*env)->DeleteGlobalRef(env, g_log_callback_obj);
      g_log_callback_obj = NULL;
    }
  } else {
    LOGI("Log callback cleared");
  }
}

JNIEXPORT jobject JNICALL
Java_com_droid_ethervox_1core_NativeLib_getLlamaPerformanceMetrics(JNIEnv* env, jobject thiz) {
  (void)thiz;

#if ETHERVOX_WITH_LLAMA && LLAMA_CPP_AVAILABLE
  // Get Governor
  if (!g_governor) {
    return NULL;
  }

  // Access the llama_context from Governor
  // Note: This requires accessing the internal structure
  // The governor structure has llm_ctx field
  struct ethervox_governor {
    ethervox_governor_config_t config;
    ethervox_tool_registry_t* tool_registry;
    struct llama_model* llm_model;
    struct llama_context* llm_ctx;
    // ... other fields we don't need
  };

  struct ethervox_governor* governor = (struct ethervox_governor*)g_governor;

  if (!governor->llm_ctx) {
    return NULL;
  }

  // Get performance data from llama.cpp
  struct llama_perf_context_data perf = llama_perf_context(governor->llm_ctx);

  // Only return metrics if there's actual data (model has been used)
  // If no tokens have been processed yet, return NULL
  if (perf.n_p_eval == 0 && perf.n_eval == 0) {
    return NULL;
  }

  // Create Java object to return metrics
  jclass metricsClass = (*env)->FindClass(env, "com/droid/ethervox_core/LlamaPerformanceMetrics");
  if (!metricsClass) {
    LOGE("Failed to find LlamaPerformanceMetrics class");
    return NULL;
  }

  jmethodID constructor = (*env)->GetMethodID(env, metricsClass, "<init>", "(DDDDDIII)V");
  if (!constructor) {
    LOGE("Failed to find LlamaPerformanceMetrics constructor");
    return NULL;
  }

  // Calculate tokens per second
  double prompt_tps =
      perf.n_p_eval > 0 && perf.t_p_eval_ms > 0 ? (perf.n_p_eval * 1000.0 / perf.t_p_eval_ms) : 0.0;
  double gen_tps =
      perf.n_eval > 0 && perf.t_eval_ms > 0 ? (perf.n_eval * 1000.0 / perf.t_eval_ms) : 0.0;

  // Debug logging (commented out - too verbose)
  // LOGI("llama_perf: n_eval=%d, t_eval_ms=%.2f, gen_tps=%.2f",
  //      (int)perf.n_eval, perf.t_eval_ms, gen_tps);

  jobject metrics = (*env)->NewObject(env, metricsClass, constructor, perf.t_load_ms,
                                      perf.t_p_eval_ms, perf.t_eval_ms, prompt_tps, gen_tps,
                                      (jint)perf.n_p_eval, (jint)perf.n_eval, (jint)perf.n_reused);

  return metrics;
#else
  return NULL;
#endif
}

// ===========================================================================
// Voice Tools / Transcription
// ===========================================================================

// Global voice session state for transcription
static ethervox_voice_session_t* g_voice_session = NULL;

/**
 * Get Android-specific files directory (set from Kotlin)
 * Returns NULL if not on Android or not set
 */
const char* ethervox_get_android_files_dir(void) {
#if defined(__ANDROID__)
  if (g_android_files_dir[0] != '\0') {
    return g_android_files_dir;
  }
#endif
  return NULL;
}

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_setAndroidFilesDir(
    JNIEnv* env, jobject thiz, jstring filesDir) {
  (void)thiz;

  if (!filesDir) {
    LOGE("Files directory is NULL");
    return;
  }

  const char* dir = (*env)->GetStringUTFChars(env, filesDir, NULL);
  if (dir) {
    strncpy(g_android_files_dir, dir, sizeof(g_android_files_dir) - 1);
    g_android_files_dir[sizeof(g_android_files_dir) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, filesDir, dir);

    LOGI("[Android] Files directory set to: %s", g_android_files_dir);
    LOGI("[Android] Whisper models: %s/models/whisper/", g_android_files_dir);
    LOGI("[Android] Transcripts: %s/transcripts/", g_android_files_dir);
  }
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_startVoiceTranscription(JNIEnv* env, jobject thiz) {
  (void)env;
  (void)thiz;

  LOGI("[Voice] ========================================");
  LOGI("[Voice] startVoiceTranscription called");
  LOGI("[Voice] ========================================");

  // Initialize voice session if not already done
  if (!g_voice_session) {
    LOGI("[Voice] Voice session is NULL - initializing for first time...");
    g_voice_session = (ethervox_voice_session_t*)malloc(sizeof(ethervox_voice_session_t));
    if (!g_voice_session) {
      LOGE("[Voice] ERROR: Failed to allocate voice session memory");
      return -1;
    }
    LOGI("[Voice] [OK] Voice session memory allocated");

    // Initialize voice tools (requires memory store)
    if (!g_memory_store) {
      LOGE("[Voice] ERROR: Memory store not initialized - required for voice tools");
      free(g_voice_session);
      g_voice_session = NULL;
      return -1;
    }
    LOGI("[Voice] [OK] Memory store exists");

    LOGI("[Voice] Calling ethervox_voice_tools_init...");
    int ret = ethervox_voice_tools_init(g_voice_session, g_memory_store);
    if (ret != 0) {
      LOGE("[Voice] [FAIL] ERROR: Failed to initialize voice tools: %d", ret);
      LOGE("[Voice] This usually means Whisper model not found!");
      LOGE("[Voice] Check Settings → Whisper Model section to download");
      free(g_voice_session);
      g_voice_session = NULL;
      return ret;
    }
    LOGI("[Voice] [OK] Voice tools initialized successfully!");
    LOGI("[Voice] [OK] Whisper model loaded and ready");
  } else {
    LOGI("[Voice] Voice session already initialized (is_initialized=%d)",
         g_voice_session->is_initialized);
  }

  // Verify session is properly initialized
  if (!g_voice_session->is_initialized) {
    LOGE("[Voice] ERROR: Voice session exists but is_initialized=false!");
    LOGE("[Voice] This shouldn't happen - initialization must have failed silently");
    return -1;
  }
  LOGI("[Voice] [OK] Session verification passed");

  // Start listening (recording + VAD)
  LOGI("[Voice] Starting voice recording...");
  int ret = ethervox_voice_tools_start_listen(g_voice_session);
  if (ret != 0) {
    LOGE("[Voice] [FAIL] ERROR: Failed to start voice recording: %d", ret);
    return ret;
  }

  LOGI("[Voice] [OK] Voice transcription started successfully!");
  LOGI("[Voice] ========================================");
  return 0;
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_stopVoiceTranscription(JNIEnv* env, jobject thiz) {
  (void)thiz;

  if (!g_voice_session) {
    LOGE("Voice session not initialized");
    return NULL;
  }

  // Stop listening and get transcript
  const char* transcript = NULL;
  int ret = ethervox_voice_tools_stop_listen(g_voice_session, &transcript);

  if (ret != 0) {
    LOGE("Failed to stop voice transcription: %d", ret);
    return NULL;
  }

  if (!transcript || transcript[0] == '\0') {
    LOGI("Transcription completed but no speech detected");
    return (*env)->NewStringUTF(env, "");
  }

  LOGI("Transcription completed: %s", transcript);

  // CRITICAL: Copy the transcript to Java string immediately
  // The transcript pointer points to g_voice_session->full_transcript buffer
  // which will be reused on the next recording session
  jstring result = create_jstring(env, transcript);

  // Clear the session transcript buffer to prevent it from being returned again
  // This must be done AFTER creating the Java string
  if (g_voice_session->full_transcript) {
    g_voice_session->full_transcript[0] = '\0';
    g_voice_session->transcript_len = 0;
    g_voice_session->segment_count = 0;
  }

  return result;
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getLastTranscriptFilePath(JNIEnv* env, jobject thiz) {
  (void)thiz;

  if (!g_voice_session) {
    LOGE("Voice session not initialized");
    return NULL;
  }

  if (g_voice_session->last_transcript_file[0] == '\0') {
    LOGI("No transcript file available");
    return NULL;
  }

  return create_jstring(env, g_voice_session->last_transcript_file);
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_isVoiceTranscribing(JNIEnv* env, jobject thiz) {
  (void)env;
  (void)thiz;

  if (!g_voice_session) {
    return JNI_FALSE;
  }

  return g_voice_session->is_recording ? JNI_TRUE : JNI_FALSE;
}

// ===========================================================================
// Mobile Optimization Features (Minimal Mode, Secret Mode)
// ===========================================================================

JNIEXPORT void JNICALL Java_com_droid_ethervox_1core_NativeLib_setPrivacyMode(JNIEnv* env,
                                                                              jobject thiz,
                                                                              jboolean enabled) {
  (void)env;
  (void)thiz;

  ethervox_memory_set_privacy_mode(enabled ? true : false);

  if (enabled) {
    LOGI("[JNI] SECRET MODE enabled - memory logging disabled");
  } else {
    LOGI("[JNI] Normal mode - memory logging enabled");
  }
}

JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_getPrivacyMode(JNIEnv* env,
                                                                                  jobject thiz) {
  (void)env;
  (void)thiz;

  return ethervox_memory_get_privacy_mode() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_droid_ethervox_1core_NativeLib_loadGovernorModelMinimal(
    JNIEnv* env, jobject thiz, jstring modelPath) {
  (void)thiz;

  if (!g_governor) {
    LOGE("Cannot load Governor model - governor not initialized");
    return JNI_FALSE;
  }

  const char* path = (*env)->GetStringUTFChars(env, modelPath, NULL);

  LOGI("[JNI] Loading Governor model in MINIMAL MODE (fast mobile loading)");
  LOGI("[JNI] Model path: %s", path);

  // NOTE: Minimal mode must be set during platformInit() via governor config
  // Cannot change config after governor is initialized
  // This function is now equivalent to loadGovernorModel
  // TODO: Add proper API to change governor mode at runtime

  ethervox_result_t result = ethervox_governor_load_model(g_governor, path, NULL, NULL);

  (*env)->ReleaseStringUTFChars(env, modelPath, path);

  if (ethervox_is_success(result)) {
    LOGI("[JNI] Governor model loaded successfully");
    LOGW("[JNI] MINIMAL MODE must be set via config in platformInit - cannot change at load time");
    return JNI_TRUE;
  } else {
    LOGE("Failed to load Governor model");
    return JNI_FALSE;
  }
}
