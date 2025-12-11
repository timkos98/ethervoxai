/**
 * @file voice_conversation.c
 * @brief Real-time voice conversation implementation
 *
 * Manages background thread for wake word → Vosk STT → Governor → Piper TTS
 * conversation flow. Separate from transcription pipeline.
 */

#include "ethervox/conversation.h"
#include "ethervox/logging.h"
#include "ethervox/error.h"
#include "ethervox/governor.h"
#include "ethervox/stt.h"
#include "ethervox/audio.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Internal conversation session structure
 */
struct ethervox_conversation_session {
    ethervox_conversation_config_t config;
    ethervox_governor_t* governor;
    
    // Thread management
    pthread_t thread_id;
    pthread_mutex_t mutex;
    pthread_cond_t trigger_cond;
    bool thread_running;
    bool thread_should_exit;
    
    // State tracking
    ethervox_conversation_state_t state;
    uint64_t conversation_start_time_ms;
    uint64_t last_audio_time_ms;
    
    // Vosk STT runtime
    ethervox_stt_runtime_t stt_runtime;
    bool stt_initialized;
    
    // Audio runtime for microphone capture
    ethervox_audio_runtime_t audio_runtime;
    bool audio_initialized;
    
    // Piper TTS runtime (opaque pointer for now, will implement in piper_backend.c)
    void* piper_voice;
    
    // Audio capture
    ethervox_audio_buffer_t* audio_buffer;
    bool audio_capture_active;
};

/**
 * @brief Get current time in milliseconds
 */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Conversation processing thread
 */
static void* conversation_thread(void* arg) {
    ethervox_conversation_session_t* session = (ethervox_conversation_session_t*)arg;
    
    pthread_mutex_lock(&session->mutex);
    
    while (!session->thread_should_exit) {
        // Wait for wake word trigger
        session->state = ETHERVOX_CONV_STATE_IDLE;
        
        while (!session->thread_should_exit && session->state == ETHERVOX_CONV_STATE_IDLE) {
            pthread_cond_wait(&session->trigger_cond, &session->mutex);
        }
        
        if (session->thread_should_exit) {
            break;
        }
        
        // Conversation triggered
        session->conversation_start_time_ms = get_time_ms();
        session->state = ETHERVOX_CONV_STATE_LISTENING;
        
        pthread_mutex_unlock(&session->mutex);
        
        printf("\n[Conversation] Thread activated, starting speech processing...\n");
        
        // TODO: Play wake confirmation beep if enabled
        if (session->config.enable_beep_on_wake) {
            // ethervox_audio_play_beep(440, 100); // Short beep
        }
        
        // Listen for user speech with Vosk
        char recognized_text[1024] = {0};
        bool speech_detected = false;
        
        printf("[Conversation] Initializing STT...\n");
        
        // Initialize STT if not already done
        if (!session->stt_initialized) {
            ethervox_stt_config_t stt_config = ethervox_stt_get_default_config();
            
            // Use Whisper streaming on desktop (already built-in)
            // On Android, this will be overridden to use lightweight STT
            stt_config.backend = ETHERVOX_STT_BACKEND_WHISPER;
            
            // Auto-detect Whisper model
            const char* home = getenv("HOME");
            static char whisper_model_path[512];
            if (home) {
                snprintf(whisper_model_path, sizeof(whisper_model_path), 
                         "%s/.ethervox/models/whisper/base.bin", home);
                stt_config.model_path = whisper_model_path;
            } else {
                stt_config.model_path = NULL;
            }
            
            stt_config.sample_rate = 16000;
            stt_config.enable_partial_results = true; // Whisper streaming supports this
            
            printf("[Conversation] Calling ethervox_stt_init with model: %s\n", stt_config.model_path ? stt_config.model_path : "NULL");
            
            if (ethervox_stt_init(&session->stt_runtime, &stt_config) == 0) {
                session->stt_initialized = true;
                printf("[Conversation] ✓ Whisper streaming STT initialized\n");
                ETHERVOX_LOG_INFO("Whisper streaming STT initialized for conversation");
            } else {
                printf("[Conversation] ❌ Failed to initialize Whisper STT\n");
                ETHERVOX_LOG_ERROR("Failed to initialize Whisper STT");
                pthread_mutex_lock(&session->mutex);
                session->state = ETHERVOX_CONV_STATE_ERROR;
                pthread_mutex_unlock(&session->mutex);
                continue;
            }
        }
        
        // Start STT session
        if (ethervox_stt_start(&session->stt_runtime) != 0) {
            ETHERVOX_LOG_ERROR("Failed to start STT session");
            pthread_mutex_lock(&session->mutex);
            session->state = ETHERVOX_CONV_STATE_IDLE;
            pthread_mutex_unlock(&session->mutex);
            continue;
        }
        
        // Start audio capture
        // Initialize audio runtime if not already done
        if (!session->audio_initialized) {
            ethervox_audio_config_t audio_config = {0};
            audio_config.sample_rate = session->config.vosk.sample_rate;
            audio_config.channels = 1;
            audio_config.bits_per_sample = 16;
            audio_config.buffer_size = 4096;
            
            if (ethervox_audio_register_platform_driver(&session->audio_runtime) == 0 &&
                session->audio_runtime.driver.init(&session->audio_runtime, &audio_config) == 0) {
                session->audio_initialized = true;
                ETHERVOX_LOG_INFO("Audio runtime initialized for conversation");
            } else {
                ETHERVOX_LOG_ERROR("Failed to initialize audio runtime");
                pthread_mutex_lock(&session->mutex);
                session->state = ETHERVOX_CONV_STATE_ERROR;
                pthread_mutex_unlock(&session->mutex);
                continue;
            }
        }
        
        // Start microphone capture
        if (session->audio_runtime.driver.start_capture(&session->audio_runtime) != 0) {
            ETHERVOX_LOG_ERROR("Failed to start audio capture");
            pthread_mutex_lock(&session->mutex);
            session->state = ETHERVOX_CONV_STATE_IDLE;
            pthread_mutex_unlock(&session->mutex);
            continue;
        }
        
        // Allocate audio buffer for processing
        ethervox_audio_buffer_t audio_buf;
        audio_buf.size = 16000 * 5; // 5 seconds
        audio_buf.channels = 1;
        audio_buf.data = (float*)calloc(audio_buf.size, sizeof(float));
        audio_buf.timestamp_us = 0;
        
        if (!audio_buf.data) {
            ETHERVOX_LOG_ERROR("Failed to allocate audio buffer");
            session->audio_runtime.driver.stop_capture(&session->audio_runtime);
            pthread_mutex_lock(&session->mutex);
            session->state = ETHERVOX_CONV_STATE_IDLE;
            pthread_mutex_unlock(&session->mutex);
            continue;
        }
        
        session->audio_buffer = &audio_buf;
        session->audio_capture_active = true;
        
        // Listening loop with Vosk
        uint64_t listen_start = get_time_ms();
        uint64_t last_audio_time = listen_start;
        bool timeout_reached = false;
        size_t total_samples_captured = 0;
        
        ETHERVOX_LOG_INFO("Listening for speech (timeout: %d ms)...", session->config.listen_timeout_ms);
        
        while (session->audio_capture_active && !timeout_reached) {
            // Read audio from microphone
            ethervox_audio_buffer_t read_buffer;
            read_buffer.size = 3200; // 200ms at 16kHz
            read_buffer.channels = 1;
            read_buffer.data = (float*)calloc(read_buffer.size, sizeof(float));
            
            if (!read_buffer.data) {
                ETHERVOX_LOG_ERROR("Failed to allocate read buffer");
                break;
            }
            
            int samples_read = session->audio_runtime.driver.read_audio(&session->audio_runtime, &read_buffer);
            
            if (samples_read > 0) {
                // Process audio with Vosk
                ethervox_stt_result_t result;
                memset(&result, 0, sizeof(result));
                
                int ret = ethervox_stt_process(&session->stt_runtime, &read_buffer, &result);
                
                if (ret == 0) {
                    if (result.is_final && result.text && strlen(result.text) > 0) {
                        // Got final result - conversation complete
                        strncpy(recognized_text, result.text, sizeof(recognized_text) - 1);
                        speech_detected = true;
                        ETHERVOX_LOG_INFO("Final recognition: %s", result.text);
                        free(result.text);
                        free(read_buffer.data);
                        break;
                    } else if (result.is_partial && result.text && strlen(result.text) > 0) {
                        // Partial result - keep listening
                        ETHERVOX_LOG_DEBUG("Partial: %s", result.text);
                        last_audio_time = get_time_ms();
                        free(result.text);
                    }
                }
                
                total_samples_captured += samples_read;
            }
            
            free(read_buffer.data);
            
            uint64_t now = get_time_ms();
            
            // Check for silence timeout (no speech activity)
            if (total_samples_captured > 0 && (now - last_audio_time > session->config.listen_timeout_ms)) {
                ETHERVOX_LOG_DEBUG("Silence timeout reached");
                timeout_reached = true;
                break;
            }
            
            // Check for max listen time
            if (now - listen_start > session->config.listen_timeout_ms && total_samples_captured == 0) {
                ETHERVOX_LOG_DEBUG("Listen timeout reached without speech");
                timeout_reached = true;
                break;
            }
            
            // Check for conversation timeout
            if (now - session->conversation_start_time_ms > session->config.conversation_timeout_ms) {
                ETHERVOX_LOG_DEBUG("Conversation timeout reached");
                timeout_reached = true;
                break;
            }
            
            // Small sleep to avoid busy-wait
            usleep(10000); // 10ms
        }
        
        // Stop audio capture
        session->audio_capture_active = false;
        
        // Finalize STT to get any remaining text
        ethervox_stt_result_t final_result;
        if (ethervox_stt_finalize(&session->stt_runtime, &final_result) == 0) {
            if (final_result.text && strlen(final_result.text) > 0) {
                strncpy(recognized_text, final_result.text, sizeof(recognized_text) - 1);
                speech_detected = true;
                ETHERVOX_LOG_INFO("Recognized: %s", final_result.text);
                free(final_result.text);
            }
        }
        
        ethervox_stt_stop(&session->stt_runtime);
        
        // Free audio buffer
        if (session->audio_buffer && session->audio_buffer->data) {
            free(session->audio_buffer->data);
        }
        session->audio_buffer = NULL;
        
        pthread_mutex_lock(&session->mutex);
        
        if (!speech_detected || strlen(recognized_text) == 0) {
            // No speech detected, return to idle
            session->state = ETHERVOX_CONV_STATE_IDLE;
            pthread_mutex_unlock(&session->mutex);
            continue;
        }
        
        // TODO: Play listening end beep if enabled
        if (session->config.enable_beep_on_listen_end) {
            // ethervox_audio_play_beep(880, 100);
        }
        
        // Process with Governor
        session->state = ETHERVOX_CONV_STATE_PROCESSING;
        pthread_mutex_unlock(&session->mutex);
        
        ETHERVOX_LOG_INFO("Processing user input with Governor: %s", recognized_text);
        
        // Send to Governor and get response
        if (session->governor) {
            char* llm_response = NULL;
            char* error_msg = NULL;
            
            ethervox_governor_status_t status = ethervox_governor_execute(
                session->governor,
                recognized_text,
                &llm_response,
                &error_msg,
                NULL,  // metrics (optional)
                NULL,  // progress callback (optional)
                NULL,  // token callback (optional)
                NULL   // user_data (optional)
            );
            
            if (status == ETHERVOX_GOVERNOR_SUCCESS && llm_response && strlen(llm_response) > 0) {
                ETHERVOX_LOG_INFO("Governor response: %s", llm_response);
                
                pthread_mutex_lock(&session->mutex);
                session->state = ETHERVOX_CONV_STATE_SPEAKING;
                pthread_mutex_unlock(&session->mutex);
                
                // TODO: Synthesize with Piper TTS
                // For now, just print the response
                printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
                printf("🤖 Assistant: %s\n", llm_response);
                printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
                
                // TODO: When Piper TTS is implemented:
                // ethervox_audio_buffer_t tts_output;
                // if (ethervox_piper_synthesize(session->piper_voice, llm_response, &tts_output) == 0) {
                //     ethervox_audio_play(&session->audio_runtime, &tts_output);
                //     free(tts_output.data);
                // }
                
                free(llm_response);
            } else {
                ETHERVOX_LOG_WARN("Governor failed to generate response: %s", 
                                error_msg ? error_msg : "unknown error");
                if (error_msg) {
                    free(error_msg);
                }
            }
        } else {
            ETHERVOX_LOG_WARN("No Governor instance available");
        }
        
        pthread_mutex_lock(&session->mutex);
    }
    
    session->state = ETHERVOX_CONV_STATE_UNINITIALIZED;
    session->thread_running = false;
    pthread_mutex_unlock(&session->mutex);
    
    return NULL;
}

ethervox_conversation_config_t ethervox_conversation_get_default_config(void) {
    ethervox_conversation_config_t config = {0};
    
    // Vosk configuration
    config.vosk.model_path = NULL; // Will auto-detect in ~/.ethervox/models/vosk/
    config.vosk.sample_rate = 16000;
    config.vosk.max_alternatives = 1;
    config.vosk.partial_results = true;
    
    // Piper configuration
    config.piper.model_path = NULL; // Will auto-detect in ~/.ethervox/models/piper/
    config.piper.config_path = NULL;
    config.piper.speed = 1.0f;
    config.piper.sample_rate = 22050;
    
    // Conversation timeouts
    config.listen_timeout_ms = 5000;        // 5 seconds of silence to stop listening
    config.conversation_timeout_ms = 30000;  // 30 seconds max conversation
    config.audio_buffer_size = 16000 * 10;   // 10 seconds at 16kHz
    
    // Audio feedback
    config.enable_beep_on_wake = true;
    config.enable_beep_on_listen_end = true;
    
    return config;
}

ethervox_conversation_session_t* ethervox_conversation_init(
    const ethervox_conversation_config_t* config,
    ethervox_governor_t* governor_runtime
) {
    if (!config) {
        ETHERVOX_LOG_ERROR("conversation_init: NULL config");
        return NULL;
    }
    
    ethervox_conversation_session_t* session = 
        (ethervox_conversation_session_t*)calloc(1, sizeof(ethervox_conversation_session_t));
    if (!session) {
        ETHERVOX_LOG_ERROR("conversation_init: allocation failed");
        return NULL;
    }
    
    // Copy configuration
    memcpy(&session->config, config, sizeof(ethervox_conversation_config_t));
    session->governor = governor_runtime;
    
    // Initialize threading primitives
    pthread_mutex_init(&session->mutex, NULL);
    pthread_cond_init(&session->trigger_cond, NULL);
    session->thread_running = false;
    session->thread_should_exit = false;
    session->state = ETHERVOX_CONV_STATE_UNINITIALIZED;
    
    // Initialize STT and audio
    session->stt_initialized = false;
    memset(&session->stt_runtime, 0, sizeof(ethervox_stt_runtime_t));
    session->audio_initialized = false;
    memset(&session->audio_runtime, 0, sizeof(ethervox_audio_runtime_t));
    session->audio_buffer = NULL;
    session->audio_capture_active = false;
    
    // TODO: Initialize Piper voice
    // session->piper_voice = piper_voice_load(model_path, config_path);
    
    ETHERVOX_LOG_INFO("Conversation session initialized (STT lazy-loaded, Piper TODO)");
    
    return session;
}

int ethervox_conversation_start(ethervox_conversation_session_t* session) {
    if (!session) {
        ETHERVOX_LOG_ERROR("conversation_start: NULL session");
        return -EINVAL;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    if (session->thread_running) {
        pthread_mutex_unlock(&session->mutex);
        ETHERVOX_LOG_WARN("conversation_start: already running");
        return 0; // Not an error
    }
    
    session->thread_should_exit = false;
    
    int rc = pthread_create(&session->thread_id, NULL, conversation_thread, session);
    if (rc != 0) {
        pthread_mutex_unlock(&session->mutex);
        ETHERVOX_LOG_ERROR("conversation_start: pthread_create failed: %d", rc);
        return -rc;
    }
    
    session->thread_running = true;
    
    pthread_mutex_unlock(&session->mutex);
    
    ETHERVOX_LOG_INFO("Conversation thread started");
    
    return 0;
}

int ethervox_conversation_stop(ethervox_conversation_session_t* session) {
    if (!session) {
        ETHERVOX_LOG_ERROR("conversation_stop: NULL session");
        return -EINVAL;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    if (!session->thread_running) {
        pthread_mutex_unlock(&session->mutex);
        return 0; // Already stopped
    }
    
    session->thread_should_exit = true;
    pthread_cond_signal(&session->trigger_cond); // Wake up thread
    
    pthread_mutex_unlock(&session->mutex);
    
    // Wait for thread to exit
    pthread_join(session->thread_id, NULL);
    
    ETHERVOX_LOG_INFO("Conversation thread stopped");
    
    return 0;
}

int ethervox_conversation_trigger(ethervox_conversation_session_t* session) {
    if (!session) {
        ETHERVOX_LOG_ERROR("conversation_trigger: NULL session");
        return -EINVAL;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    if (!session->thread_running) {
        pthread_mutex_unlock(&session->mutex);
        ETHERVOX_LOG_WARN("conversation_trigger: thread not running");
        return -EINVAL;
    }
    
    // Only trigger if idle
    if (session->state == ETHERVOX_CONV_STATE_IDLE) {
        session->state = ETHERVOX_CONV_STATE_LISTENING;
        pthread_cond_signal(&session->trigger_cond);
        ETHERVOX_LOG_DEBUG("Conversation triggered by wake word");
    } else {
        ETHERVOX_LOG_DEBUG("Conversation trigger ignored (already active)");
    }
    
    pthread_mutex_unlock(&session->mutex);
    
    return 0;
}

ethervox_conversation_state_t ethervox_conversation_get_state(
    const ethervox_conversation_session_t* session
) {
    if (!session) {
        return ETHERVOX_CONV_STATE_UNINITIALIZED;
    }
    
    // Read state atomically
    ethervox_conversation_state_t state;
    pthread_mutex_lock((pthread_mutex_t*)&session->mutex);
    state = session->state;
    pthread_mutex_unlock((pthread_mutex_t*)&session->mutex);
    
    return state;
}

bool ethervox_conversation_is_active(
    const ethervox_conversation_session_t* session
) {
    ethervox_conversation_state_t state = ethervox_conversation_get_state(session);
    return (state == ETHERVOX_CONV_STATE_LISTENING ||
            state == ETHERVOX_CONV_STATE_PROCESSING ||
            state == ETHERVOX_CONV_STATE_SPEAKING);
}

void ethervox_conversation_cleanup(ethervox_conversation_session_t* session) {
    if (!session) {
        return;
    }
    
    // Stop thread if running
    if (session->thread_running) {
        ethervox_conversation_stop(session);
    }
    
    // Cleanup STT runtime
    if (session->stt_initialized) {
        ethervox_stt_cleanup(&session->stt_runtime);
        session->stt_initialized = false;
    }
    
    // TODO: Cleanup Piper
    // if (session->piper_voice) piper_voice_free(session->piper_voice);
    
    // Free audio buffer if still allocated
    if (session->audio_buffer && session->audio_buffer->data) {
        free(session->audio_buffer->data);
        session->audio_buffer = NULL;
    }
    
    // Destroy threading primitives
    pthread_mutex_destroy(&session->mutex);
    pthread_cond_destroy(&session->trigger_cond);
    
    free(session);
    
    ETHERVOX_LOG_INFO("Conversation session cleaned up");
}
