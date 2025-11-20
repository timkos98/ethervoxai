/**
 * @file ethervox_multiplatform_core.c
 * @brief JNI wrapper for EthervoxAI on Android
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 *
 * This file is part of EthervoxAI, licensed under CC BY-NC-SA 4.0.
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#include "ethervox/audio.h"
#include "ethervox/wake_word.h"
#include "ethervox/stt.h"
#include "ethervox/llm.h"
#include "ethervox/dialogue.h"
#include "ethervox/platform.h"

#define LOG_TAG "EthervoxJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global runtime instances (managed from Java layer)
static ethervox_audio_runtime_t* g_audio_runtime = NULL;
static ethervox_wake_runtime_t* g_wake_runtime = NULL;
static ethervox_stt_runtime_t* g_stt_runtime = NULL;
static ethervox_platform_t* g_platform = NULL;
static ethervox_dialogue_engine_t* g_dialogue_engine = NULL;

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

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_platformInit(
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
    
    ethervox_llm_config_t llm_config = ethervox_dialogue_get_default_llm_config();
    result = ethervox_dialogue_init(g_dialogue_engine, &llm_config);
    if (result != 0) {
        LOGE("Dialogue engine initialization failed");
        free(g_dialogue_engine);
        g_dialogue_engine = NULL;
        ethervox_platform_cleanup(g_platform);
        free(g_platform);
        g_platform = NULL;
        return -1;
    }
    
    LOGI("Platform and dialogue engine initialized successfully");
    return 0;
}

JNIEXPORT void JNICALL
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_platformCleanup(
        JNIEnv* env,
        jobject thiz) {
    (void)env;
    (void)thiz;
    
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

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_getPlatformName(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_audioInit(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_audioStartCapture(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_audioStopCapture(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_audioCleanup(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_wakeWordInit(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_wakeWordCleanup(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_sttInit(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_sttStart(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_sttCleanup(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_processDialogue(
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

JNIEXPORT jint JNICALL
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_setDialogueLanguage(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_getDialogueLanguage(
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
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_stringFromJNI(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    
    const char* hello = "Hello from EthervoxAI native library!";
    LOGI("%s", hello);
    return create_jstring(env, hello);
}

JNIEXPORT jstring JNICALL
Java_com_droid_ethervox_1multiplatform_1core_NativeLib_getVersion(
        JNIEnv* env,
        jobject thiz) {
    (void)thiz;
    return create_jstring(env, "EthervoxAI v0.1.0");
}

