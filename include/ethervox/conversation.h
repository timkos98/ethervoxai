/**
 * @file conversation.h
 * @brief Real-time voice conversation system with Granite Speech ASR and
 *        platform-native TTS (desktop: Piper; Android/iOS: OS TTS)
 *
 * Provides lightweight voice interaction for LLM conversations, triggered by
 * wake word detection. Separate from transcription pipeline (also Granite
 * Speech-based, PLUS variant) which is for meeting notes and long-form
 * dictation with speaker attribution.
 *
 * Architecture:
 * - Wake word detection → Signal conversation thread
 * - Granite Speech BASE ASR (one-shot per utterance) → Process speech
 * - Send to Governor → Get response
 * - TTS (desktop: Piper, mobile: platform-native) → Speak response
 * - Return to wake word listening
 *
 * Thread model: Background thread waits on condition variable, processes
 * conversation when signaled, returns to idle after timeout.
 */

#ifndef ETHERVOX_CONVERSATION_H
#define ETHERVOX_CONVERSATION_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "ethervox/error.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations to avoid circular dependencies
typedef struct ethervox_governor ethervox_governor_t;

/**
 * @brief Conversation session state
 */
typedef enum {
    ETHERVOX_CONV_STATE_UNINITIALIZED = 0,
    ETHERVOX_CONV_STATE_IDLE,          // Waiting for wake word trigger
    ETHERVOX_CONV_STATE_LISTENING,     // Capturing user speech
    ETHERVOX_CONV_STATE_PROCESSING,    // Sending to Governor
    ETHERVOX_CONV_STATE_SPEAKING,      // Playing TTS response
    ETHERVOX_CONV_STATE_ERROR
} ethervox_conversation_state_t;

/**
 * @brief Granite Speech ASR configuration (subset relevant to Mode 1
 * conversation; see ethervox/stt.h's ethervox_stt_config_t for the full,
 * authoritative config used to actually initialize the backend - these
 * fields exist for API-level documentation/defaults only)
 */
typedef struct {
    const char* model_path;            // Path to Granite Speech GGUF (BASE variant)
    const char* mmproj_path;           // Path to companion mmproj GGUF (audio projector)
    uint32_t sample_rate;              // Audio sample rate (16000 Hz, fixed by the encoder)
} ethervox_granite_speech_config_t;

/**
 * @brief Piper TTS configuration (desktop only - Android/iOS use platform-
 * native TTS, which has no equivalent model-path config; see
 * src/dialogue/voice_conversation.c's conversation_on_speak)
 */
typedef struct {
    const char* model_path;            // Path to Piper .onnx model
    const char* config_path;           // Path to model config JSON
    float speed;                       // Speech speed multiplier (1.0 = normal)
    int sample_rate;                   // Output sample rate (22050 Hz typical)
} ethervox_piper_config_t;

/**
 * @brief Conversation session configuration
 */
typedef struct {
    ethervox_granite_speech_config_t stt;  // ASR configuration (BASE variant)
    ethervox_piper_config_t piper;         // TTS configuration (desktop only)
    
    int listen_timeout_ms;             // Silence timeout to stop listening
    int conversation_timeout_ms;       // Max conversation duration
    int audio_buffer_size;             // Size of audio ring buffer
    
    bool enable_beep_on_wake;          // Play feedback when wake word detected
    bool enable_beep_on_listen_end;    // Play feedback when listening ends
    bool always_listening;             // Continuously transcribe without wake word (desktop mode)
} ethervox_conversation_config_t;

/**
 * @brief Opaque conversation session runtime
 */
typedef struct ethervox_conversation_session ethervox_conversation_session_t;

/**
 * @brief Get default conversation configuration
 * 
 * @return Default configuration with recommended settings
 */
ethervox_conversation_config_t ethervox_conversation_get_default_config(void);

/**
 * @brief Initialize conversation session
 * 
 * Creates background thread that waits for wake word triggers. Thread
 * lifecycle: idle → triggered → listening → processing → speaking → idle.
 * 
 * @param config Session configuration
 * @param governor_runtime Governor instance for LLM interaction
 * @return Session handle on success, NULL on failure
 */
ethervox_conversation_session_t* ethervox_conversation_init(
    const ethervox_conversation_config_t* config,
    ethervox_governor_t* governor_runtime
);

/**
 * @brief Start conversation session (enable background thread)
 * 
 * Thread begins waiting for wake word signals. Does not block.
 * 
 * @param session Session handle
 * @return 0 on success, negative on error
 */
ethervox_result_t ethervox_conversation_start(ethervox_conversation_session_t* session);

/**
 * @brief Stop conversation session (disable background thread)
 * 
 * Completes current conversation if active, then stops accepting new triggers.
 * 
 * @param session Session handle
 * @return 0 on success, negative on error
 */
ethervox_result_t ethervox_conversation_stop(ethervox_conversation_session_t* session);

/**
 * @brief Trigger conversation from wake word detection
 * 
 * Signals background thread to begin listening. Safe to call from wake word
 * callback. If already processing a conversation, this is a no-op.
 * 
 * @param session Session handle
 * @return 0 on success, negative on error
 */
ethervox_result_t ethervox_conversation_trigger(ethervox_conversation_session_t* session);

/**
 * @brief Get current conversation state
 * 
 * Thread-safe query of session state.
 * 
 * @param session Session handle
 * @return Current state
 */
ethervox_conversation_state_t ethervox_conversation_get_state(
    const ethervox_conversation_session_t* session
);

/**
 * @brief Check if conversation is active
 * 
 * @param session Session handle
 * @return true if listening, processing, or speaking; false if idle or error
 */
bool ethervox_conversation_is_active(
    const ethervox_conversation_session_t* session
);

/**
 * @brief Cleanup and free conversation session
 * 
 * Stops background thread, releases Vosk/Piper resources, frees memory.
 * 
 * @param session Session handle (may be NULL)
 */
void ethervox_conversation_cleanup(ethervox_conversation_session_t* session);

/**
 * @brief Get phonemizer context from conversation session
 * 
 * For voice training and pronunciation improvement.
 * 
 * @param session Conversation session
 * @return Phonemizer context (internal type, cast to void*) or NULL if not available
 */
void* ethervox_conversation_get_phonemizer(ethervox_conversation_session_t* session);

/**
 * @brief Get TTS context from conversation session
 * 
 * For voice training audio synthesis.
 * 
 * @param session Conversation session
 * @return TTS context (internal type, cast to void*) or NULL if not available
 */
void* ethervox_conversation_get_tts(ethervox_conversation_session_t* session);

/**
 * @brief Get STT context from conversation session
 * 
 * For voice training transcription.
 * 
 * @param session Conversation session
 * @return STT context (internal type, cast to void*) or NULL if not available
 */
void* ethervox_conversation_get_stt(ethervox_conversation_session_t* session);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_CONVERSATION_H
