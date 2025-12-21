/**
 * @file global_tts.c
 * @brief Global TTS instance for shared use between CLI and conversation
 *
 * This provides a single TTS instance that can be initialized once at startup
 * and reused by multiple components (speak tool, /convon, etc).
 */

#include <pthread.h>
#include "ethervox/tts.h"

// Global TTS instance (for standalone speak tool, shared with conversation)
ethervox_tts_context_t* g_global_tts = NULL;
pthread_mutex_t g_tts_mutex = PTHREAD_MUTEX_INITIALIZER;
