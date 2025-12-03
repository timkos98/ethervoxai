/**
 * @file voice_tools.c
 * @brief Voice tools implementation with Whisper STT
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/voice_tools.h"
#include "ethervox/governor.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#define LOG_ERROR(...) ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  ethervox_log(ETHERVOX_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)  ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_DEBUG(...) ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

// Global session pointer for tool wrappers
static ethervox_voice_session_t* g_voice_session = NULL;

/**
 * Background thread that captures audio and feeds to STT
 */
static void* audio_capture_thread(void* arg) {
    ethervox_voice_session_t* session = (ethervox_voice_session_t*)arg;
    
    ethervox_audio_buffer_t audio_buf;
    audio_buf.data = (float*)malloc(16000 * sizeof(float)); // 1 second buffer
    audio_buf.size = 16000;
    audio_buf.channels = 1;
    audio_buf.timestamp_us = 0;
    
    if (!audio_buf.data) {
        LOG_ERROR("Failed to allocate audio buffer");
        return NULL;
    }
    
    LOG_INFO("Audio capture thread started");
    
    while (session->is_recording && !session->stop_requested) {
        // Read audio from platform driver
        int samples_read = ethervox_audio_read(&session->audio_runtime, &audio_buf);
        
        if (samples_read > 0) {
            // Feed to STT
            ethervox_stt_result_t result;
            if (ethervox_stt_process(&session->stt_runtime, &audio_buf, &result) == 0) {
                if (result.text && strlen(result.text) > 0) {
                    // Append to transcript
                    size_t needed = session->transcript_len + strlen(result.text) + 2;
                    if (needed > session->transcript_capacity) {
                        session->transcript_capacity = needed * 2;
                        session->full_transcript = (char*)realloc(session->full_transcript, 
                                                                 session->transcript_capacity);
                    }
                    
                    if (session->transcript_len > 0) {
                        strcat(session->full_transcript, " ");
                        session->transcript_len++;
                    }
                    strcat(session->full_transcript, result.text);
                    session->transcript_len += strlen(result.text);
                    session->segment_count++;
                    
                    LOG_DEBUG("Segment %u: %s", session->segment_count, result.text);
                    ethervox_stt_result_free(&result);
                }
            }
        } else if (samples_read == 0) {
            // No audio available yet, sleep briefly
            usleep(50000); // 50ms
        } else {
            LOG_ERROR("Audio read error: %d", samples_read);
            break;
        }
    }
    
    free(audio_buf.data);
    LOG_INFO("Audio capture thread stopped");
    return NULL;
}

/**
 * Initialize voice tools with Whisper backend
 */
int ethervox_voice_tools_init(ethervox_voice_session_t* session, void* memory) {
    if (!session) {
        LOG_ERROR("Session is NULL");
        return -1;
    }
    
    memset(session, 0, sizeof(ethervox_voice_session_t));
    
    session->memory_store = memory;
    session->transcript_capacity = 1024 * 100; // 100KB initial
    session->full_transcript = (char*)calloc(session->transcript_capacity, 1);
    
    if (!session->full_transcript) {
        LOG_ERROR("Failed to allocate transcript buffer");
        return -1;
    }
    
    // Determine Whisper model path
    const char* possible_paths[] = {
        "models/whisper/base.en.bin",
        "./models/whisper/base.en.bin",
        "../models/whisper/base.en.bin",
        "../../models/whisper/base.en.bin",
        NULL
    };
    
    const char* model_path = NULL;
    for (int i = 0; possible_paths[i] != NULL; i++) {
        FILE* test = fopen(possible_paths[i], "rb");
        if (test) {
            fclose(test);
            model_path = possible_paths[i];
            LOG_INFO("Found Whisper model at: %s", model_path);
            break;
        }
    }
    
    if (!model_path) {
        LOG_ERROR("Whisper model not found. Tried: models/whisper/base.en.bin (and variations)");
        LOG_ERROR("Please ensure base.en.bin is in the models/whisper/ directory");
        free(session->full_transcript);
        return -1;
    }
    
    // Configure STT with Whisper backend
    ethervox_stt_config_t stt_config = {
        .backend = ETHERVOX_STT_BACKEND_WHISPER,  // Use Whisper!
        .model_path = model_path,
        .language = "auto",  // Auto-detect language by default
        .sample_rate = 16000,
        .enable_partial_results = true,
        .enable_punctuation = true,
        .vad_threshold = 0.5f
    };
    
    // Initialize STT with Whisper
    if (ethervox_stt_init(&session->stt_runtime, &stt_config) != 0) {
        LOG_ERROR("Failed to initialize Whisper STT");
        free(session->full_transcript);
        return -1;
    }
    
    // Initialize audio (will use platform-specific implementation)
    ethervox_audio_config_t audio_config = ethervox_audio_get_default_config();
    if (ethervox_audio_init(&session->audio_runtime, &audio_config) != 0) {
        LOG_WARN("Audio init failed - will use test data");
        // Continue anyway - we can test with simulated audio
    }
    
    session->is_initialized = true;
    LOG_INFO("Voice tools initialized with Whisper STT backend");
    
    return 0;
}

/**
 * Start listening session
 */
int ethervox_voice_tools_start_listen(ethervox_voice_session_t* session) {
    if (!session || !session->is_initialized) {
        LOG_ERROR("Session not initialized");
        return -1;
    }
    
    if (session->is_recording) {
        LOG_WARN("Already recording");
        return 0;
    }
    
    // Reset transcript
    session->full_transcript[0] = '\0';
    session->transcript_len = 0;
    session->segment_count = 0;
    session->session_start_time = time(NULL);
    session->stop_requested = false;
    
    // Start STT
    if (ethervox_stt_start(&session->stt_runtime) != 0) {
        LOG_ERROR("Failed to start STT");
        return -1;
    }
    
    // Start audio capture
    if (!session->audio_runtime.is_initialized) {
        LOG_ERROR("Audio runtime not initialized");
        ethervox_stt_stop(&session->stt_runtime);
        return -1;
    }
    
    if (ethervox_audio_start_capture(&session->audio_runtime) != 0) {
        LOG_ERROR("Failed to start audio capture");
        ethervox_stt_stop(&session->stt_runtime);
        return -1;
    }
    
    session->is_recording = true;
    
    // Start background capture thread
    pthread_t* thread = (pthread_t*)malloc(sizeof(pthread_t));
    if (thread && pthread_create(thread, NULL, audio_capture_thread, session) == 0) {
        session->capture_thread = thread;
        LOG_INFO("Voice recording session started with background thread");
    } else {
        LOG_ERROR("Failed to create capture thread");
        free(thread);
        ethervox_audio_stop_capture(&session->audio_runtime);
        ethervox_stt_stop(&session->stt_runtime);
        session->is_recording = false;
        return -1;
    }
    
    return 0;
}

/**
 * Stop listening and get transcript
 */
int ethervox_voice_tools_stop_listen(ethervox_voice_session_t* session, const char** transcript_out) {
    if (!session || !session->is_initialized) {
        LOG_ERROR("Session not initialized");
        return -1;
    }
    
    if (!session->is_recording) {
        LOG_WARN("Not currently recording");
        return -1;
    }
    
    // Signal thread to stop
    session->stop_requested = true;
    session->is_recording = false;
    
    // Wait for capture thread to finish
    if (session->capture_thread) {
        pthread_t* thread = (pthread_t*)session->capture_thread;
        pthread_join(*thread, NULL);
        free(thread);
        session->capture_thread = NULL;
        LOG_INFO("Capture thread joined");
    }
    
    // Stop audio capture
    if (session->audio_runtime.is_initialized) {
        ethervox_audio_stop_capture(&session->audio_runtime);
    }
    
    // Finalize STT to get any remaining transcript
    ethervox_stt_result_t final_result;
    if (ethervox_stt_finalize(&session->stt_runtime, &final_result) == 0) {
        if (final_result.text && strlen(final_result.text) > 0) {
            // Append final text to transcript
            size_t needed = session->transcript_len + strlen(final_result.text) + 2;
            if (needed > session->transcript_capacity) {
                session->transcript_capacity = needed * 2;
                session->full_transcript = (char*)realloc(session->full_transcript, session->transcript_capacity);
            }
            if (session->transcript_len > 0) {
                strcat(session->full_transcript, " ");
            }
            strcat(session->full_transcript, final_result.text);
            session->transcript_len = strlen(session->full_transcript);
        }
        ethervox_stt_result_free(&final_result);
    }
    
    ethervox_stt_stop(&session->stt_runtime);
    session->is_recording = false;
    
    // Save transcript to file with timestamp
    session->last_transcript_file[0] = '\0';
    if (session->transcript_len > 0) {
        const char* home = getenv("HOME");
        char transcript_dir[512];
        
        if (home) {
            snprintf(transcript_dir, sizeof(transcript_dir), "%s/.ethervox/transcripts", home);
            
            // Create directory if it doesn't exist
            mkdir(transcript_dir, 0755);
            
            // Generate filename with timestamp
            time_t now = time(NULL);
            struct tm* tm_info = localtime(&now);
            char timestamp[64];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
            
            snprintf(session->last_transcript_file, sizeof(session->last_transcript_file), 
                    "%s/transcript_%s.txt", transcript_dir, timestamp);
            
            // Write transcript to file
            FILE* f = fopen(session->last_transcript_file, "w");
            if (f) {
                fprintf(f, "Voice Transcript - Recorded: %s\n", timestamp);
                fprintf(f, "Duration: %llu seconds\n", 
                       (unsigned long long)(time(NULL) - session->session_start_time));
                fprintf(f, "Segments: %u\n", session->segment_count);
                fprintf(f, "========================================\n\n");
                fprintf(f, "%s\n", session->full_transcript);
                fclose(f);
                
                LOG_INFO("Saved transcript to: %s", session->last_transcript_file);
                
                // Also store in memory system
                if (session->memory_store) {
                    ethervox_memory_store_t* mem = (ethervox_memory_store_t*)session->memory_store;
                    const char* tags[] = {"voice", "transcript", "whisper"};
                    uint64_t memory_id = 0;
                    
                    // Store with file path reference
                    char memory_content[2048];
                    snprintf(memory_content, sizeof(memory_content),
                            "Voice transcript saved to: %s\n\nTranscript:\n%s",
                            session->last_transcript_file, session->full_transcript);
                    
                    ethervox_memory_store_add(mem, memory_content, tags, 3, 0.9f, false, &memory_id);
                    LOG_INFO("Stored in memory (ID: %llu) with file reference", (unsigned long long)memory_id);
                }
            } else {
                LOG_ERROR("Failed to save transcript to: %s", session->last_transcript_file);
                session->last_transcript_file[0] = '\0';
            }
        }
    }
    
    *transcript_out = session->full_transcript;
    
    LOG_INFO("Voice recording session stopped - %zu chars transcribed", session->transcript_len);
    
    return 0;
}

/**
 * Check if recording
 */
bool ethervox_voice_tools_is_recording(const ethervox_voice_session_t* session) {
    return session && session->is_recording;
}

/**
 * Cleanup
 */
void ethervox_voice_tools_cleanup(ethervox_voice_session_t* session) {
    if (!session) return;
    
    if (session->is_recording) {
        const char* transcript;
        ethervox_voice_tools_stop_listen(session, &transcript);
    }
    
    ethervox_stt_cleanup(&session->stt_runtime);
    
    if (session->audio_runtime.is_initialized) {
        ethervox_audio_cleanup(&session->audio_runtime);
    }
    
    if (session->full_transcript) {
        free(session->full_transcript);
        session->full_transcript = NULL;
    }
    
    session->is_initialized = false;
    
    LOG_INFO("Voice tools cleaned up");
}

/**
 * Tool wrapper: listen_and_summarize
 * 
 * This tool allows the LLM to start/stop voice recording with Whisper transcription.
 * The LLM must call with action="start" then action="stop" to get the transcript.
 */
static int tool_listen_and_summarize_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_voice_session_t* session = g_voice_session;
    
    if (!session || !session->is_initialized) {
        *error = strdup("Voice tools not initialized");
        return -1;
    }
    
    // Parse action parameter
    char action[32] = {0};
    const char* action_start = strstr(args_json, "\"action\":");
    if (action_start) {
        action_start = strchr(action_start, ':') + 1;
        while (*action_start == ' ' || *action_start == '"') action_start++;
        const char* action_end = strchr(action_start, '"');
        if (action_end) {
            size_t len = action_end - action_start;
            if (len < sizeof(action)) {
                strncpy(action, action_start, len);
                action[len] = '\0';
            }
        }
    }
    
    if (strcmp(action, "start") == 0) {
        // Start recording
        if (ethervox_voice_tools_start_listen(session) != 0) {
            *error = strdup("Failed to start listening");
            return -1;
        }
        
        *result = strdup("{\"status\":\"recording\",\"message\":\"Voice recording started. Use /end command or call with action='stop' to finish.\"}");
        return 0;
        
    } else if (strcmp(action, "stop") == 0) {
        // Stop recording and get transcript
        const char* transcript;
        if (ethervox_voice_tools_stop_listen(session, &transcript) != 0) {
            *error = strdup("Failed to stop listening or not recording");
            return -1;
        }
        
        // Build JSON response with transcript
        size_t res_len = strlen(transcript) + 512;
        char* res = malloc(res_len);
        snprintf(res, res_len, 
                "{\"status\":\"completed\",\"transcript\":\"%s\",\"length\":%zu,\"segments\":%u}",
                transcript,
                session->transcript_len,
                session->segment_count);
        
        *result = res;
        
        // Store transcript in memory
        if (session->memory_store && strlen(transcript) > 0) {
            const char* tags[] = {"voice", "transcript", "whisper"};
            uint64_t memory_id = 0;
            
            // Cast to memory store and add
            ethervox_memory_store_t* mem = (ethervox_memory_store_t*)session->memory_store;
            ethervox_memory_store_add(mem, transcript, tags, 3, 0.8f, false, &memory_id);
            
            LOG_INFO("Stored voice transcript in memory (ID: %llu)", (unsigned long long)memory_id);
        }
        
        return 0;
        
    } else if (strcmp(action, "status") == 0) {
        // Check recording status
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg),
                "{\"is_recording\":%s,\"transcript_length\":%zu,\"segments\":%u}",
                session->is_recording ? "true" : "false",
                session->transcript_len,
                session->segment_count);
        *result = strdup(status_msg);
        return 0;
        
    } else {
        *error = strdup("Invalid action. Use 'start', 'stop', or 'status'");
        return -1;
    }
}

/**
 * Tool definition for listen_and_summarize
 */
static ethervox_tool_t listen_tool = {
    .name = "listen_and_summarize",
    .description = "Start or stop voice recording with Whisper STT transcription and speaker detection. "
                   "Call with {\"action\":\"start\"} to begin recording (user will use /stoptranscribe command to end), "
                   "or {\"action\":\"stop\"} to get the final transcript with speaker labels. "
                   "Transcript will be automatically stored in memory.",
    .parameters_json_schema = "{\"type\":\"object\","
        "\"properties\":{"
        "\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\",\"status\"],"
        "\"description\":\"Action to perform: start recording, stop and get transcript, or check status\"}"
        "},"
        "\"required\":[\"action\"]}",
    .execute = tool_listen_and_summarize_wrapper,
    .is_deterministic = false,
    .requires_confirmation = false,
    .is_stateful = true,
    .estimated_latency_ms = 100.0f
};

/**
 * Register voice tools with Governor
 */
int ethervox_voice_tools_register(void* registry, ethervox_voice_session_t* session) {
    if (!registry || !session) {
        LOG_ERROR("Invalid parameters for voice tools registration");
        return -1;
    }
    
    g_voice_session = session;
    
    ethervox_tool_registry_t* reg = (ethervox_tool_registry_t*)registry;
    
    if (ethervox_tool_registry_add(reg, &listen_tool) != 0) {
        LOG_ERROR("Failed to register listen_and_summarize tool");
        return -1;
    }
    
    LOG_INFO("Registered listen_and_summarize tool with Governor (Whisper STT)");
    
    return 0;
}

