/**
 * @file ethervox_android_core.c
 * @brief Android JNI wrapper for EthervoxAI
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 *
 * This file is part of EthervoxAI, licensed under CC BY-NC-SA 4.0.
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>

#include "ethervox/audio.h"
#include "ethervox/wake_word.h"
#include "ethervox/stt.h"
#include "ethervox/llm.h"
#include "ethervox/dialogue.h"
#include "ethervox/governor.h"
#include "ethervox/tool_manifest.h"
#include "ethervox/timer_tools.h"
#include "ethervox/memory_tools.h"
#include "ethervox/voice_tools.h"
#include "ethervox/platform.h"
#include "ethervox/config.h"
#include "ethervox/tool_prompt_optimizer.h"
#include "ethervox/integration_tests.h"
#include "ethervox/llm_tool_tests.h"

#if ETHERVOX_WITH_LLAMA && LLAMA_CPP_AVAILABLE
#include "llama.h"
#endif

#define LOGI(...) ETHERVOX_LOGI(__VA_ARGS__)
#define LOGE(...) ETHERVOX_LOGE(__VA_ARGS__)

// Global debug mode flag (defined in logging.c, referenced here)
extern int g_ethervox_debug_enabled;

// Global log callback for sending C logs to Java/Kotlin debug window
ethervox_log_callback_t g_ethervox_log_callback = NULL;

// Logging helper that sends to both logcat and callback
void ethervox_log_with_callback(int level, const char* tag, const char* fmt, ...) {
    if (!g_ethervox_debug_enabled) return;
    
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
static ethervox_dialogue_engine_t* g_dialogue_engine = NULL;
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

static jstring create_jstring(JNIEnv* env, const char* str) {
    if (!str) {
        return (*env)->NewStringUTF(env, "");
    }
    return (*env)->NewStringUTF(env, str);
}

// ===========================================================================
// Platform Initialization
// ===========================================================================

// Track last load error code for corruption detection
static int g_last_governor_load_error = 0;

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_loadGovernorModel(
    JNIEnv* env, jobject thiz, jstring modelPath) {
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
        LOGE("Cannot load Governor model - dialogue engine or governor not initialized");
        g_last_governor_load_error = -1;
        return JNI_FALSE;
    }
    
    const char* path = (*env)->GetStringUTFChars(env, modelPath, NULL);
    
    LOGI("[JNI] Loading Governor model from: %s", path);
    
    ethervox_governor_t* governor = (ethervox_governor_t*)g_dialogue_engine->governor;
    int result = ethervox_governor_load_model(governor, path);
    
    (*env)->ReleaseStringUTFChars(env, modelPath, path);
    
    // Store error code for corruption detection
    g_last_governor_load_error = result;
    
    if (result == 0) {
        LOGI("[JNI] Governor model loaded successfully");
        
        // Initialize Tool Manifest System (after Governor model is loaded)
        // This is optional - graceful fallback if it fails
        if (!g_dialogue_engine->manifest_registry) {
            LOGI("[JNI] Initializing manifest registry after model load");
            
            // Get the model path from JNI parameter (we need to get it again since we released it)
            const char* model_path_for_manifest = (*env)->GetStringUTFChars(env, modelPath, NULL);
            
            tool_manifest_registry_t* manifest = malloc(sizeof(tool_manifest_registry_t));
            if (manifest) {
                memset(manifest, 0, sizeof(tool_manifest_registry_t));
                
                // Try to initialize with manifest
                int manifest_result = ethervox_governor_init_with_manifest(governor, model_path_for_manifest, manifest);
                LOGI("[JNI] Manifest init result: %d", manifest_result);
                
                if (manifest_result == 0) {
                    g_dialogue_engine->manifest_registry = manifest;
                    
                    // Report manifest status
                    if (manifest->tools_available) {
                        if (manifest->optimized_cache) {
                            LOGI("Manifest ready: Level 0 (optimized prompts loaded)");
                        } else {
                            LOGI("Manifest ready: Level 1 (binary one-liners)");
                        }
                    } else {
                        LOGE("Manifest fallback: Level 2 (LLM-only, consider optimization)");
                    }
                } else {
                    free(manifest);
                    LOGE("Manifest unavailable - using runtime registry only");
                }
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
Java_com_droid_ethervox_1core_NativeLib_wasLastLoadCorrupted(
    JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    
    // -2 indicates probable corruption
    return (g_last_governor_load_error == -2) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_unloadGovernorModel(
    JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
        LOGE("Cannot unload Governor model - dialogue engine or governor not initialized");
        return JNI_FALSE;
    }
    
    LOGI("[JNI] Unloading Governor model to free memory");
    
    ethervox_governor_t* governor = (ethervox_governor_t*)g_dialogue_engine->governor;
    int result = ethervox_governor_unload_model(governor);
    
    if (result == 0) {
        LOGI("[JNI] Governor model unloaded successfully");
        return JNI_TRUE;
    } else {
        LOGE("Failed to unload Governor model");
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_reloadGovernorModel(
    JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
        LOGE("Cannot reload Governor model - dialogue engine or governor not initialized");
        return JNI_FALSE;
    }
    
    LOGI("[JNI] Reloading Governor model");
    
    ethervox_governor_t* governor = (ethervox_governor_t*)g_dialogue_engine->governor;
    int result = ethervox_governor_reload_model(governor);
    
    if (result == 0) {
        LOGI("[JNI] Governor model reloaded successfully");
        return JNI_TRUE;
    } else {
        LOGE("Failed to reload Governor model");
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_isGovernorLoaded(
    JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
        return JNI_FALSE;
    }
    
    ethervox_governor_t* governor = (ethervox_governor_t*)g_dialogue_engine->governor;
    return ethervox_governor_is_loaded(governor) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_droid_ethervox_1core_NativeLib_getRegisteredPlugins(
    JNIEnv* env, jobject thiz) {
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor_tool_registry) {
        // Return empty array if not initialized
        jclass stringClass = (*env)->FindClass(env, "java/lang/String");
        return (*env)->NewObjectArray(env, 0, stringClass, NULL);
    }
    
    ethervox_tool_registry_t* registry = (ethervox_tool_registry_t*)g_dialogue_engine->governor_tool_registry;
    int tool_count = registry->tool_count;
    
    // Create Java string array
    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    jobjectArray result = (*env)->NewObjectArray(env, tool_count, stringClass, NULL);
    
    // Populate array with tool names
    for (int i = 0; i < tool_count; i++) {
        jstring toolName = (*env)->NewStringUTF(env, registry->tools[i].name);
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
    int result;
    bool completed;
    pthread_mutex_t mutex;
} optimization_thread_data_t;

// Optimization thread function
static void* optimization_thread_func(void* arg) {
    optimization_thread_data_t* data = (optimization_thread_data_t*)arg;
    
    LOGI("Optimization thread started for model: %s", data->model_path);
    
    // Run V2 optimizer (this can take 30-60 seconds)
    int result = ethervox_optimize_tool_prompts_v2(
        data->governor,
        data->model_path,
        data->manifest_registry
    );
    
    // Update result atomically
    pthread_mutex_lock(&data->mutex);
    data->result = result;
    data->completed = true;
    pthread_mutex_unlock(&data->mutex);
    
    if (result == 0) {
        LOGI("Optimization thread completed successfully");
    } else {
        LOGE("Optimization thread failed with code: %d", result);
    }
    
    return NULL;
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_optimizeToolPrompts(
    JNIEnv* env, jobject thiz, jstring modelPath) {
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
        LOGE("Governor not loaded - cannot optimize tool prompts");
        return -1;
    }
    
    if (!g_dialogue_engine->manifest_registry) {
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
    thread_data->governor = (ethervox_governor_t*)g_dialogue_engine->governor;
    thread_data->manifest_registry = (tool_manifest_registry_t*)g_dialogue_engine->manifest_registry;
    snprintf(thread_data->model_path, sizeof(thread_data->model_path), "%s", model_path);
    thread_data->result = -999;  // Sentinel for "not completed"
    thread_data->completed = false;
    pthread_mutex_init(&thread_data->mutex, NULL);
    
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
    
    if (result == 0) {
        LOGI("Tool prompt optimization completed successfully");
    } else {
        LOGE("Tool prompt optimization failed with code: %d", result);
    }
    
    return result;
}

JNIEXPORT jobject JNICALL
Java_com_droid_ethervox_1core_NativeLib_loadOptimizedPrompts(
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
    
    int result = ethervox_load_optimized_prompts(
        model_path,
        instruction,
        sizeof(instruction),
        examples,
        sizeof(examples)
    );
    
    (*env)->ReleaseStringUTFChars(env, modelPath, model_path);
    
    if (result != 0) {
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
    
    jobject optimizedPrompts = (*env)->NewObject(env, optimizedPromptsClass, constructor,
                                                 instructionStr, examplesStr);
    
    (*env)->DeleteLocalRef(env, instructionStr);
    (*env)->DeleteLocalRef(env, examplesStr);
    (*env)->DeleteLocalRef(env, optimizedPromptsClass);
    
    return optimizedPrompts;
}

/**
 * Get manifest registry information
 * Returns JSON string with current manifest state
 */
JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getManifestInfo(
    JNIEnv* env,
    jobject thiz) {
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->manifest_registry) {
        // No manifest available - return minimal JSON
        return (*env)->NewStringUTF(env, 
            "{\"available\":false,\"tool_count\":0,\"fallback_level\":3,\"optimized_loaded\":false,\"tools_detected\":false,\"tools_loaded_count\":0}");
    }
    
    tool_manifest_registry_t* manifest = 
        (tool_manifest_registry_t*)g_dialogue_engine->manifest_registry;
    
    ETHERVOX_LOGI("DEBUG getManifestInfo: reading tools_loaded_count=%u", manifest->tools_loaded_count);
    
    // Build JSON response with guard status
    char json[512];
    snprintf(json, sizeof(json),
        "{\"available\":%s,\"tool_count\":%u,\"fallback_level\":%u,\"optimized_loaded\":%s,\"tools_detected\":%s,\"tools_loaded_count\":%u}",
        manifest->tools_available ? "true" : "false",
        manifest->header.tool_count,
        manifest->fallback_level,
        manifest->optimization_loaded ? "true" : "false",
        manifest->tools_detected ? "true" : "false",
        manifest->tools_loaded_count
    );
    
    return (*env)->NewStringUTF(env, json);
}

/**
 * Get the optimized tool prompts directory path
 * Returns the absolute path where optimized JSON files are stored/expected
 */
JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getOptimizedDir(
    JNIEnv* env,
    jobject thiz) {
    (void)thiz;
    
    char optimized_dir[512];
    
    if (g_android_files_dir[0] != '\0') {
        // Android: Use app files directory
        snprintf(optimized_dir, sizeof(optimized_dir),
                 "%s/tools/optimized", g_android_files_dir);
    } else {
        // Fallback (should not happen on Android)
        const char* home = getenv("HOME");
        snprintf(optimized_dir, sizeof(optimized_dir),
                 "%s/.ethervox/tools/optimized", 
                 home ? home : ".");
    }
    
    return (*env)->NewStringUTF(env, optimized_dir);
}

/**
 * Check if optimization file exists for a given model
 * Returns true if the optimized JSON file exists on disk
 */
JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_optimizationFileExists(
    JNIEnv* env,
    jobject thiz,
    jstring modelPath) {
    (void)thiz;
    
    if (!modelPath) {
        return JNI_FALSE;
    }
    
    const char* model_path_str = (*env)->GetStringUTFChars(env, modelPath, NULL);
    if (!model_path_str) {
        return JNI_FALSE;
    }
    
    // Extract model name from path (e.g., /path/to/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf -> Qwen2.5-0.5B-Instruct-Q4_K_M)
    const char* filename = strrchr(model_path_str, '/');
    if (!filename) {
        filename = model_path_str;
    } else {
        filename++; // Skip the '/'
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
    if (i > 0 && model_name[i-1] == '-') {
        model_name[i-1] = '\0';
    }
    
    (*env)->ReleaseStringUTFChars(env, modelPath, model_path_str);
    
    // Build path to optimization file
    char optimized_path[512];
    if (g_android_files_dir[0] != '\0') {
        snprintf(optimized_path, sizeof(optimized_path),
                 "%s/tools/optimized/%s.json", 
                 g_android_files_dir, model_name);
    } else {
        const char* home = getenv("HOME");
        snprintf(optimized_path, sizeof(optimized_path),
                 "%s/.ethervox/tools/optimized/%s.json", 
                 home ? home : ".", model_name);
    }
    
    // Check if file exists
    struct stat st;
    jboolean exists = (stat(optimized_path, &st) == 0) ? JNI_TRUE : JNI_FALSE;
    
    ETHERVOX_LOGI("Checking optimization file: %s -> %s", 
                  optimized_path, exists ? "EXISTS" : "NOT FOUND");
    
    return exists;
}

/**
 * Run comprehensive integration tests
 */
JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_runIntegrationTests(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    LOGI("Running integration tests...");
    run_integration_tests();
    
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
JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_runLlmToolTests(
        JNIEnv* env,
        jobject thiz,
        jstring modelPath,
        jboolean verbose) {
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
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
    
    ethervox_governor_t* governor = (ethervox_governor_t*)g_dialogue_engine->governor;
    
    LOGI("Running LLM tool tests (verbose=%d)...", verbose);
    run_llm_tool_tests(governor, g_memory_store, model_path_str, (bool)verbose);
    
    // Log completion with test reports path
    if (g_android_files_dir[0] != '\0') {
        LOGI("LLM tool tests complete. Reports saved to: %s/tests/", g_android_files_dir);
    } else {
        LOGI("LLM tool tests complete");
    }
    
    (*env)->ReleaseStringUTFChars(env, modelPath, model_path_str);
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_platformInit(
        JNIEnv* env,
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
    
    int result = ethervox_platform_init(g_platform);
    if (result != 0) {
        LOGE("Failed to initialize platform");
        free(g_platform);
        g_platform = NULL;
        return -1;
    }
    
    // Initialize dialogue engine with default config
    g_dialogue_engine = (ethervox_dialogue_engine_t*)calloc(1, sizeof(ethervox_dialogue_engine_t));
    if (!g_dialogue_engine) {
        LOGE("Failed to allocate dialogue engine");
        ethervox_platform_cleanup(g_platform);
        free(g_platform);
        g_platform = NULL;
        return -1;
    }
    
    // Initialize memory store BEFORE dialogue engine so tools can register
    g_memory_store = (ethervox_memory_store_t*)calloc(1, sizeof(ethervox_memory_store_t));
    if (g_memory_store) {
        // Initialize with NULL session_id (will be auto-generated)
        // and NULL storage_dir for now (will be set via setMemoryStorageDir)
        if (ethervox_memory_init(g_memory_store, NULL, NULL) != 0) {
            LOGE("Failed to initialize memory store");
            free(g_memory_store);
            g_memory_store = NULL;
        } else {
            LOGI("Memory store initialized (in-memory mode until storage dir set)");
            // Set memory store in dialogue engine BEFORE dialogue_init
            ethervox_dialogue_set_memory_store(g_memory_store);
        }
    } else {
        LOGE("Failed to allocate memory store");
    }
    
    ethervox_llm_config_t llm_config = ethervox_dialogue_get_default_llm_config();
    result = ethervox_dialogue_init(g_dialogue_engine, &llm_config);
    if (result != 0) {
        LOGE("Dialogue engine initialization failed");
        free(g_dialogue_engine);
        g_dialogue_engine = NULL;
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
    
    LOGI("Platform, memory store, and dialogue engine initialized successfully");
    
    return 0;
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_platformCleanup(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (g_memory_store) {
        ethervox_memory_cleanup(g_memory_store);
        free(g_memory_store);
        g_memory_store = NULL;
        LOGI("Memory store cleaned up");
    }
    
    if (g_dialogue_engine) {
        ethervox_dialogue_cleanup(g_dialogue_engine);
        free(g_dialogue_engine);
        g_dialogue_engine = NULL;
        LOGI("Dialogue engine cleaned up");
    }
    
    if (g_platform) {
        ethervox_platform_cleanup(g_platform);
        free(g_platform);
        g_platform = NULL;
        LOGI("Platform cleaned up");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_initializeWithModel(
        JNIEnv* env,
        jobject thiz,
        jstring model_path,
        jfloat temperature,
        jint max_tokens,
        jfloat top_p,
        jint context_length) {
    (void)thiz;
    
    if (!model_path) {
        LOGE("Model path is null");
        return JNI_FALSE;
    }
    
    const char* path = (*env)->GetStringUTFChars(env, model_path, NULL);
    if (!path) {
        LOGE("Failed to get model path string");
        return JNI_FALSE;
    }
    
    LOGI("Initializing dialogue engine with model: %s (temp=%.2f, max_tokens=%d, top_p=%.2f, ctx=%d)",
         path, temperature, max_tokens, top_p, context_length);
    
    // Create directories for manifest storage
    if (g_android_files_dir) {
        char tools_dir[512];
        snprintf(tools_dir, sizeof(tools_dir), "%s/tools", g_android_files_dir);
        mkdir(tools_dir, 0755);
        
        char optimized_dir[512];
        snprintf(optimized_dir, sizeof(optimized_dir), "%s/tools/optimized", g_android_files_dir);
        mkdir(optimized_dir, 0755);
        
        LOGI("Created manifest directories in: %s", g_android_files_dir);
    }
    
    // Clean up existing dialogue engine if any
    if (g_dialogue_engine) {
        ethervox_dialogue_cleanup(g_dialogue_engine);
        free(g_dialogue_engine);
        g_dialogue_engine = NULL;
    }
    
    // Allocate new dialogue engine
    g_dialogue_engine = (ethervox_dialogue_engine_t*)calloc(1, sizeof(ethervox_dialogue_engine_t));
    if (!g_dialogue_engine) {
        LOGE("Failed to allocate dialogue engine");
        (*env)->ReleaseStringUTFChars(env, model_path, path);
        return JNI_FALSE;
    }
    
    // Create LLM config with user settings - ENABLE GPU ACCELERATION
    ethervox_llm_config_t llm_config = ethervox_dialogue_get_default_llm_config();
    llm_config.model_path = strdup(path);
    llm_config.model_name = strdup("TinyLlama-1.1B");
    llm_config.temperature = temperature;
    llm_config.max_tokens = (uint32_t)max_tokens;
    llm_config.top_p = top_p;
    llm_config.context_length = (uint32_t)context_length;
    llm_config.use_gpu = true;  // Force GPU acceleration ON
    llm_config.gpu_layers = 99;  // Offload entire model to GPU
    
    LOGI("[DEBUG] JNI: Before dialogue_init, llm_config.model_path=%s", llm_config.model_path ? llm_config.model_path : "NULL");
    
    // Initialize dialogue engine with LLM
    int result = ethervox_dialogue_init(g_dialogue_engine, &llm_config);
    
    // Free temporary config strings
    free(llm_config.model_path);
    free(llm_config.model_name);
    (*env)->ReleaseStringUTFChars(env, model_path, path);
    
    if (result != 0) {
        LOGE("Failed to initialize dialogue engine with model");
        free(g_dialogue_engine);
        g_dialogue_engine = NULL;
        return JNI_FALSE;
    }
    
    // Check if LLM was actually loaded
    if (g_dialogue_engine->llm_backend && g_dialogue_engine->use_llm_for_unknown) {
        LOGI("Dialogue engine initialized with LLM support enabled");
    } else {
        LOGI("Dialogue engine initialized (LLM not available, using pattern-based responses)");
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_updateLLMParams(
        JNIEnv* env,
        jobject thiz,
        jfloat temperature,
        jint max_tokens,
        jfloat top_p) {
    (void)env;
    (void)thiz;
    
    if (!g_dialogue_engine || !g_dialogue_engine->llm_backend) {
        LOGE("LLM backend not initialized");
        return JNI_FALSE;
    }
    
    LOGI("Updating LLM params: temp=%.2f, max_tokens=%d, top_p=%.2f", 
         temperature, max_tokens, top_p);
    
    // Create config with new parameters
    ethervox_llm_config_t config = {0};
    config.temperature = temperature;
    config.max_tokens = (uint32_t)max_tokens;
    config.top_p = top_p;
    
    // Use the backend's update_config function if available
    ethervox_llm_backend_t* backend = g_dialogue_engine->llm_backend;
    if (!backend) {
        LOGE("Backend is null");
        return JNI_FALSE;
    }
    
    if (backend->update_config) {
        int result = backend->update_config(backend, &config);
        if (result == 0) {
            LOGI("LLM params updated successfully via backend function");
            return JNI_TRUE;
        } else {
            LOGE("Failed to update LLM params via backend function");
            return JNI_FALSE;
        }
    } else {
        LOGE("Backend does not support runtime config updates");
        return JNI_FALSE;
    }
    
    LOGI("LLM params updated successfully");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_isLlmLoaded(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_dialogue_engine) {
        return JNI_FALSE;
    }
    
    // Check if LLM backend is loaded and available
    ethervox_llm_backend_t* backend = (ethervox_llm_backend_t*)g_dialogue_engine->llm_backend;
    if (backend && 
        g_dialogue_engine->use_llm_for_unknown &&
        backend->is_loaded) {
        return JNI_TRUE;
    }
    
    return JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getPlatformName(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    
    const char* name = ethervox_platform_get_name();
    return create_jstring(env, name);
}

// ===========================================================================
// Audio Initialization
// ===========================================================================

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_audioInit(
        JNIEnv* env,
        jobject thiz,
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
    
    int result = ethervox_audio_init(g_audio_runtime, &config);
    if (result != 0) {
        LOGE("Failed to initialize audio");
        free(g_audio_runtime);
        g_audio_runtime = NULL;
        return -1;
    }
    
    LOGI("Audio initialized: %d Hz, %d channels, buffer=%d", 
         sample_rate, channels, buffer_size);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_audioStartCapture(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_audio_runtime) {
        LOGE("Audio not initialized");
        return -1;
    }
    
    int result = ethervox_audio_start_capture(g_audio_runtime);
    if (result != 0) {
        LOGE("Failed to start audio capture");
        return -1;
    }
    
    LOGI("Audio capture started");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_audioStopCapture(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_audio_runtime) {
        return 0;
    }
    
    int result = ethervox_audio_stop_capture(g_audio_runtime);
    LOGI("Audio capture stopped");
    return result;
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_audioCleanup(
        JNIEnv* env,
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

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_wakeWordInit(
        JNIEnv* env,
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
    
    int result = ethervox_wake_init(g_wake_runtime, &config);
    
    (*env)->ReleaseStringUTFChars(env, wake_word, wake_word_str);
    
    if (result != 0) {
        LOGE("Failed to initialize wake word");
        free(g_wake_runtime);
        g_wake_runtime = NULL;
        return -1;
    }
    
    LOGI("Wake word initialized with sensitivity %.2f", sensitivity);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_wakeWordCleanup(
        JNIEnv* env,
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

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_sttInit(
        JNIEnv* env,
        jobject thiz,
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
    
    int result = ethervox_stt_init(g_stt_runtime, &config);
    
    (*env)->ReleaseStringUTFChars(env, model_path, model_path_str);
    (*env)->ReleaseStringUTFChars(env, language, language_str);
    
    if (result != 0) {
        LOGE("Failed to initialize STT");
        free(g_stt_runtime);
        g_stt_runtime = NULL;
        return -1;
    }
    
    LOGI("STT initialized");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_sttStart(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_stt_runtime) {
        LOGE("STT not initialized");
        return -1;
    }
    
    int result = ethervox_stt_start(g_stt_runtime);
    if (result != 0) {
        LOGE("Failed to start STT");
        return -1;
    }
    
    LOGI("STT started");
    return 0;
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_sttCleanup(
        JNIEnv* env,
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

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_processDialogue(
        JNIEnv* env,
        jobject thiz,
        jstring user_text,
        jstring language) {
    (void)thiz;
    
    if (!g_dialogue_engine) {
        LOGE("Dialogue engine not initialized");
        return create_jstring(env, "[ERROR] Dialogue engine not initialized");
    }
    
    const char* text = (*env)->GetStringUTFChars(env, user_text, NULL);
    const char* lang = (*env)->GetStringUTFChars(env, language, NULL);
    
    LOGI("Processing dialogue: '%s' (language: %s)", text, lang);
    
    // Parse intent from user input
    ethervox_intent_t intent;
    memset(&intent, 0, sizeof(ethervox_intent_t));
    
    ethervox_dialogue_intent_request_t intent_req = {
        .text = text,
        .language_code = lang
    };
    
    int result = ethervox_dialogue_parse_intent(g_dialogue_engine, &intent_req, &intent);
    if (result != 0) {
        LOGE("Failed to parse intent");
        (*env)->ReleaseStringUTFChars(env, user_text, text);
        (*env)->ReleaseStringUTFChars(env, language, lang);
        return create_jstring(env, "[ERROR] Failed to parse intent");
    }
    
    LOGI("Intent detected: %s (confidence: %.2f)", 
         ethervox_intent_type_to_string(intent.type), intent.confidence);
    
    // Process with LLM/dialogue engine
    ethervox_llm_response_t response;
    memset(&response, 0, sizeof(ethervox_llm_response_t));
    
    result = ethervox_dialogue_process_llm(g_dialogue_engine, &intent, NULL, &response);
    if (result != 0) {
        LOGE("Failed to process LLM");
        ethervox_intent_free(&intent);
        (*env)->ReleaseStringUTFChars(env, user_text, text);
        (*env)->ReleaseStringUTFChars(env, language, lang);
        return create_jstring(env, "[ERROR] Failed to generate response");
    }
    
    LOGI("Response generated: %s (confidence: %.0f%%, conversation_ended: %s)", 
         response.text ? response.text : "(null)", response.confidence * 100.0f,
         response.conversation_ended ? "true" : "false");
    
    // Format response with confidence and conversation_ended flag appended
    char formatted_response[1024];
    snprintf(formatted_response, sizeof(formatted_response), "%s|%.0f|%s", 
             response.text ? response.text : "No response", 
             response.confidence * 100.0f,
             response.conversation_ended ? "true" : "false");
    
    // Create Java string from formatted response
    jstring result_str = create_jstring(env, formatted_response);
    
    // Cleanup
    ethervox_intent_free(&intent);
    ethervox_llm_response_free(&response);
    (*env)->ReleaseStringUTFChars(env, user_text, text);
    (*env)->ReleaseStringUTFChars(env, language, lang);
    
    return result_str;
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
    
    jstring j_token = create_jstring(ctx->env, token);
    (*ctx->env)->CallVoidMethod(ctx->env, ctx->callback_obj, ctx->on_token_method, j_token);
    (*ctx->env)->DeleteLocalRef(ctx->env, j_token);
}

// Governor progress callback function
static void native_governor_progress_callback(
    ethervox_governor_event_type_t event_type,
    const char* message,
    void* user_data
) {
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
            break;
        default:
            event_str = "UNKNOWN";
    }
    
    __android_log_print(ANDROID_LOG_INFO, "EthervoxGovernor", "[%s] %s", event_str, message);
    
    jstring j_event = create_jstring(ctx->env, event_str);
    jstring j_message = create_jstring(ctx->env, message);
    (*ctx->env)->CallVoidMethod(ctx->env, ctx->callback_obj, ctx->on_governor_progress_method, j_event, j_message);
    (*ctx->env)->DeleteLocalRef(ctx->env, j_event);
    (*ctx->env)->DeleteLocalRef(ctx->env, j_message);
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_processDialogueStreamingNative(
        JNIEnv* env,
        jobject thiz,
        jstring user_text,
        jstring language,
        jobject callback) {
    (void)thiz;
    
    if (!g_dialogue_engine) {
        LOGE("Dialogue engine not initialized");
        // Call error callback
        jclass callback_class = (*env)->GetObjectClass(env, callback);
        jmethodID on_error = (*env)->GetMethodID(env, callback_class, "onError", "(Ljava/lang/String;)V");
        if (on_error) {
            jstring error_msg = create_jstring(env, "Dialogue engine not initialized");
            (*env)->CallVoidMethod(env, callback, on_error, error_msg);
            (*env)->DeleteLocalRef(env, error_msg);
        }
        return;
    }
    
    const char* text = (*env)->GetStringUTFChars(env, user_text, NULL);
    const char* lang = (*env)->GetStringUTFChars(env, language, NULL);
    
    LOGI("Processing dialogue (streaming): '%s' (language: %s)", text, lang);
    
    // Get callback methods
    jclass callback_class = (*env)->GetObjectClass(env, callback);
    jmethodID on_token = (*env)->GetMethodID(env, callback_class, "onToken", "(Ljava/lang/String;)V");
    jmethodID on_complete = (*env)->GetMethodID(env, callback_class, "onComplete", "(Z)V");
    jmethodID on_error = (*env)->GetMethodID(env, callback_class, "onError", "(Ljava/lang/String;)V");
    jmethodID on_governor_progress = (*env)->GetMethodID(env, callback_class, "onGovernorProgress", "(Ljava/lang/String;Ljava/lang/String;)V");
    
    if (!on_token || !on_complete || !on_error) {
        LOGE("Failed to get callback methods");
        (*env)->ReleaseStringUTFChars(env, user_text, text);
        (*env)->ReleaseStringUTFChars(env, language, lang);
        return;
    }
    
    // Setup stream context
    jni_stream_context_t stream_ctx = {
        .env = env,
        .callback_obj = callback,
        .on_token_method = on_token,
        .on_complete_method = on_complete,
        .on_error_method = on_error,
        .on_governor_progress_method = on_governor_progress,  // May be NULL if not implemented
        .conversation_ended = false
    };
    
    // Parse intent
    ethervox_intent_t intent;
    memset(&intent, 0, sizeof(ethervox_intent_t));
    
    ethervox_dialogue_intent_request_t intent_req = {
        .text = text,
        .language_code = lang
    };
    
    int result = ethervox_dialogue_parse_intent(g_dialogue_engine, &intent_req, &intent);
    if (result != 0) {
        LOGE("Failed to parse intent");
        jstring error_msg = create_jstring(env, "Failed to parse intent");
        (*env)->CallVoidMethod(env, callback, on_error, error_msg);
        (*env)->DeleteLocalRef(env, error_msg);
        (*env)->ReleaseStringUTFChars(env, user_text, text);
        (*env)->ReleaseStringUTFChars(env, language, lang);
        return;
    }
    
    LOGI("Intent detected: %s (confidence: %.2f)", 
         ethervox_intent_type_to_string(intent.type), intent.confidence);
    
    // Call onPunctuatedPrompt if the text was modified by smart punctuation
    if (on_governor_progress && intent.raw_text && strcmp(text, intent.raw_text) != 0) {
        jstring punctuated_text = create_jstring(env, intent.raw_text);
        if (punctuated_text) {
            // Get the onPunctuatedPrompt method
            jmethodID on_punctuated = (*env)->GetMethodID(env, callback_class, "onPunctuatedPrompt", "(Ljava/lang/String;)V");
            if (on_punctuated) {
                (*env)->CallVoidMethod(env, callback, on_punctuated, punctuated_text);
            }
            (*env)->DeleteLocalRef(env, punctuated_text);
        }
    }
    
    // Process with LLM using streaming
    result = ethervox_dialogue_process_llm_stream(g_dialogue_engine, &intent, NULL, 
                                                   native_token_callback, &stream_ctx,
                                                   &stream_ctx.conversation_ended,
                                                   native_governor_progress_callback);
    
    if (result != 0) {
        LOGE("Failed to process LLM stream");
        jstring error_msg = create_jstring(env, "Failed to generate response");
        (*env)->CallVoidMethod(env, callback, on_error, error_msg);
        (*env)->DeleteLocalRef(env, error_msg);
    } else {
        // Call complete callback with conversation_ended flag
        (*env)->CallVoidMethod(env, callback, on_complete, (jboolean)stream_ctx.conversation_ended);
    }
    
    // Cleanup
    ethervox_intent_free(&intent);
    (*env)->ReleaseStringUTFChars(env, user_text, text);
    (*env)->ReleaseStringUTFChars(env, language, lang);
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_cancelProcessing(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_dialogue_engine) {
        LOGE("Dialogue engine not initialized");
        return;
    }
    
    // Get the LLM backend handle
    ethervox_llm_backend_t* backend = (ethervox_llm_backend_t*)g_dialogue_engine->llm_backend;
    if (backend && backend->handle) {
        // Access llama context and set cancel flag
        typedef struct {
            void* model;
            void* ctx;
            uint32_t n_ctx;
            uint32_t n_predict;
            float temperature;
            float top_p;
            uint32_t n_gpu_layers;
            uint32_t n_threads;
            uint32_t seed;
            char* loaded_model_path;
            bool use_mlock;
            bool use_mmap;
            volatile bool cancel_requested;
        } llama_ctx_t;
        
        llama_ctx_t* llama_ctx = (llama_ctx_t*)backend->handle;
        llama_ctx->cancel_requested = true;
        LOGI("LLM processing cancellation requested");
    }
}

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1core_NativeLib_setDialogueLanguage(
        JNIEnv* env,
        jobject thiz,
        jstring language) {
    (void)thiz;
    
    if (!g_dialogue_engine) {
        LOGE("Dialogue engine not initialized");
        return -1;
    }
    
    const char* lang = (*env)->GetStringUTFChars(env, language, NULL);
    
    int result = ethervox_dialogue_set_language(g_dialogue_engine, lang);
    if (result == 0) {
        LOGI("Dialogue language set to: %s", lang);
    } else {
        LOGE("Failed to set dialogue language to: %s", lang);
    }
    
    (*env)->ReleaseStringUTFChars(env, language, lang);
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getDialogueLanguage(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    
    if (!g_dialogue_engine) {
        LOGE("Dialogue engine not initialized");
        return create_jstring(env, "en");
    }
    
    const char* lang = ethervox_dialogue_get_language(g_dialogue_engine);
    return create_jstring(env, lang ? lang : "en");
}

// ===========================================================================
// Test/Utility Functions
// ===========================================================================

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_stringFromJNI(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    
    const char* hello = "Hello from EthervoxAI native library!";
    LOGI("%s", hello);
    return create_jstring(env, hello);
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getVersion(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    return create_jstring(env, ETHERVOX_VERSION_STRING);
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getBackendVersion(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    // ETHERVOX_BACKEND_VERSION is guaranteed to be defined by CMake
    return create_jstring(env, ETHERVOX_BACKEND_VERSION);
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_getActiveTimerStatus(
        JNIEnv* env,
        jobject thiz) {
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
Java_com_droid_ethervox_1core_NativeLib_getDefaultLlmConfig(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    
    // Get default config from C
    ethervox_llm_config_t config = ethervox_dialogue_get_default_llm_config();

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
    jobject llmConfigObj = (*env)->NewObject(env, llmConfigClass, constructor,
                                             (jfloat)config.temperature,
                                             (jint)config.max_tokens,
                                             (jfloat)config.top_p,
                                             (jint)config.context_length);

    (*env)->DeleteLocalRef(env, llmConfigClass);
    return llmConfigObj;
}

JNIEXPORT jobjectArray JNICALL
Java_com_droid_ethervox_1core_NativeLib_getSupportedLanguages(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    
    // Get supported languages from dialogue core
    const char** languages = ethervox_dialogue_get_supported_languages();
    
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

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_setMemoryStorageDir(
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
    if (ethervox_memory_init(g_memory_store, NULL, dir_path) != 0) {
        LOGE("Failed to set memory storage directory: %s", dir_path);
        (*env)->ReleaseStringUTFChars(env, storage_dir, dir_path);
        return;
    }
    
    LOGI("Memory storage directory set to: %s", g_memory_store->storage_filepath);
    
    // Use platform-agnostic function to load previous session
    // This handles all the complexity of finding the most recent file,
    // preserving tags, IDs, and adding the "imported" tag
    uint32_t turns_loaded = 0;
    if (ethervox_memory_load_previous_session(g_memory_store, &turns_loaded) == 0 && turns_loaded > 0) {
        LOGI("Memory: Loaded %u previous memories from last session", turns_loaded);
    }
    
    (*env)->ReleaseStringUTFChars(env, storage_dir, dir_path);
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_setDebugMode(JNIEnv* env, jobject thiz, jboolean enabled) {
    g_ethervox_debug_enabled = enabled ? 1 : 0;
    LOGI("Debug mode %s", g_ethervox_debug_enabled ? "enabled" : "disabled");
}

JNIEXPORT jboolean JNICALL
Java_com_droid_ethervox_1core_NativeLib_getDebugMode(JNIEnv* env, jobject thiz) {
    return (jboolean)(g_ethervox_debug_enabled ? JNI_TRUE : JNI_FALSE);
}

// C callback that forwards logs to Java
static void java_log_callback_wrapper(int level, const char* tag, const char* message) {
    if (!g_jvm || !g_log_callback_obj || !g_log_callback_method) {
        return;
    }
    
    // Guard against NULL pointers
    if (!tag) tag = "EthervoxCore";
    if (!message) message = "";
    
    JNIEnv* env = NULL;
    jint result = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    
    bool detach = false;
    if (result == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            return;
        }
        detach = true;
    }
    
    jstring jTag = (*env)->NewStringUTF(env, tag);
    jstring jMessage = (*env)->NewStringUTF(env, message);
    
    if (jTag && jMessage) {
        (*env)->CallVoidMethod(env, g_log_callback_obj, g_log_callback_method, level, jTag, jMessage);
    }
    
    if (jTag) (*env)->DeleteLocalRef(env, jTag);
    if (jMessage) (*env)->DeleteLocalRef(env, jMessage);
    
    if (detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_setLogCallback(
    JNIEnv* env, jobject thiz, jobject callback) {
    
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
        g_log_callback_method = (*env)->GetMethodID(env, callbackClass, 
            "onLog", "(ILjava/lang/String;Ljava/lang/String;)V");
        
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
Java_com_droid_ethervox_1core_NativeLib_getLlamaPerformanceMetrics(
    JNIEnv* env, jobject thiz) {
    (void)thiz;
    
#if ETHERVOX_WITH_LLAMA && LLAMA_CPP_AVAILABLE
    // Get Governor from dialogue engine
    if (!g_dialogue_engine || !g_dialogue_engine->governor) {
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
    
    struct ethervox_governor* governor = (struct ethervox_governor*)g_dialogue_engine->governor;
    
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
    double prompt_tps = perf.n_p_eval > 0 && perf.t_p_eval_ms > 0 ? 
        (perf.n_p_eval * 1000.0 / perf.t_p_eval_ms) : 0.0;
    double gen_tps = perf.n_eval > 0 && perf.t_eval_ms > 0 ? 
        (perf.n_eval * 1000.0 / perf.t_eval_ms) : 0.0;
    
    // Debug logging (commented out - too verbose)
    // LOGI("llama_perf: n_eval=%d, t_eval_ms=%.2f, gen_tps=%.2f", 
    //      (int)perf.n_eval, perf.t_eval_ms, gen_tps);
    
    jobject metrics = (*env)->NewObject(env, metricsClass, constructor,
        perf.t_load_ms,
        perf.t_p_eval_ms,
        perf.t_eval_ms,
        prompt_tps,
        gen_tps,
        (jint)perf.n_p_eval,
        (jint)perf.n_eval,
        (jint)perf.n_reused
    );
    
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

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1core_NativeLib_setAndroidFilesDir(
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
Java_com_droid_ethervox_1core_NativeLib_startVoiceTranscription(
    JNIEnv* env, jobject thiz) {
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
        LOGI("[Voice] ✓ Voice session memory allocated");
        
        // Initialize voice tools (requires memory store)
        if (!g_memory_store) {
            LOGE("[Voice] ERROR: Memory store not initialized - required for voice tools");
            free(g_voice_session);
            g_voice_session = NULL;
            return -1;
        }
        LOGI("[Voice] ✓ Memory store exists");
        
        LOGI("[Voice] Calling ethervox_voice_tools_init...");
        int ret = ethervox_voice_tools_init(g_voice_session, g_memory_store);
        if (ret != 0) {
            LOGE("[Voice] ✗ ERROR: Failed to initialize voice tools: %d", ret);
            LOGE("[Voice] This usually means Whisper model not found!");
            LOGE("[Voice] Check Settings → Whisper Model section to download");
            free(g_voice_session);
            g_voice_session = NULL;
            return ret;
        }
        LOGI("[Voice] ✓ Voice tools initialized successfully!");
        LOGI("[Voice] ✓ Whisper model loaded and ready");
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
    LOGI("[Voice] ✓ Session verification passed");
    
    // Start listening (recording + VAD)
    LOGI("[Voice] Starting voice recording...");
    int ret = ethervox_voice_tools_start_listen(g_voice_session);
    if (ret != 0) {
        LOGE("[Voice] ✗ ERROR: Failed to start voice recording: %d", ret);
        return ret;
    }
    
    LOGI("[Voice] ✓ Voice transcription started successfully!");
    LOGI("[Voice] ========================================");
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1core_NativeLib_stopVoiceTranscription(
    JNIEnv* env, jobject thiz) {
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
Java_com_droid_ethervox_1core_NativeLib_getLastTranscriptFilePath(
    JNIEnv* env, jobject thiz) {
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
Java_com_droid_ethervox_1core_NativeLib_isVoiceTranscribing(
    JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    
    if (!g_voice_session) {
        return JNI_FALSE;
    }
    
    return g_voice_session->is_recording ? JNI_TRUE : JNI_FALSE;
}
