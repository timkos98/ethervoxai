# Platform UI Architecture - Multi-Platform Frontend/Backend Decoupling

**Status:** Design Document  
**Created:** 2025-12-05  
**Purpose:** Decouple UI frontends from core backend to support macOS CLI, Android, iOS, web, and future platforms

---

## Problem Statement

### Current Issues

1. **Platform-Specific Code in main.c**
   - macOS-specific readline integration
   - Direct command parsing mixed with business logic
   - File I/O patterns assume POSIX filesystem
   - Signal handling specific to Unix systems
   - ~2000 lines of mixed UI and core logic

2. **No Clean API Boundary**
   - Commands directly call internal governor/memory functions
   - No standardized request/response format
   - Hard to expose functionality via JNI (Android) or Swift (iOS)
   - Difficult to test UI independently

3. **Tight Coupling**
   - Command handling logic embedded in main loop
   - Platform detection scattered throughout
   - No separation between "what" (functionality) and "how" (UI presentation)

---

## Architecture Overview

### Core Principles

1. **Backend as a Library**
   - Core functionality exposed via clean C API
   - Zero UI dependencies in backend
   - Platform-agnostic data structures
   - Thread-safe operations for async UI

2. **UI as Thin Clients**
   - Each platform implements UI layer independently
   - All platforms use same backend API
   - UI handles only: input gathering, output formatting, platform integration
   - Backend handles: business logic, state management, persistence

3. **Standardized Communication**
   - Request/response pattern for all operations
   - JSON or structured binary for complex data exchange
   - Event callbacks for async notifications (transcription progress, memory updates)

4. **File Structure Reflects Architecture**
   ```
   src/
   ├── core/                   # Platform-agnostic backend (new)
   │   ├── ethervox_api.c      # Public API facade
   │   ├── ethervox_api.h      # Public API header
   │   ├── session.c           # Session management
   │   ├── command_handler.c   # Command logic (no UI)
   │   └── event_bus.c         # Event notification system
   │
   ├── platform/               # Platform-specific implementations
   │   ├── common/             # Shared platform utilities
   │   │   ├── platform_base.h
   │   │   └── platform_utils.c
   │   │
   │   ├── macos/              # macOS-specific UI
   │   │   ├── main_macos.c    # macOS CLI entry point
   │   │   ├── readline_ui.c   # Readline integration
   │   │   └── macos_paths.c   # macOS path resolution
   │   │
   │   ├── android/            # Android JNI bridge
   │   │   ├── jni_bridge.c    # JNI → core API
   │   │   ├── android_ui.c    # Android-specific helpers
   │   │   └── android_paths.c # Android storage paths
   │   │
   │   ├── ios/                # iOS Swift bridge (future)
   │   │   └── swift_bridge.c  # Swift → core API
   │   │
   │   └── web/                # WebAssembly (future)
   │       └── wasm_bridge.c   # JavaScript → core API
   │
   ├── governor/               # Existing governor (no changes)
   ├── llm/                    # Existing LLM (no changes)
   ├── stt/                    # Existing STT (no changes)
   ├── audio/                  # Existing audio (no changes)
   └── plugins/                # Existing plugins (no changes)
   ```

---

## Core API Design

### Session Management

```c
// include/ethervox/api.h

// Opaque session handle (hides internal state)
typedef struct ethervox_session_t ethervox_session_t;

// Session initialization
typedef struct {
    const char* model_path;
    const char* memory_dir;
    const char* config_dir;
    
    // Optional callbacks for async events
    void (*on_log)(const char* message, int level, void* user_data);
    void (*on_progress)(const char* operation, float progress, void* user_data);
    void (*on_transcription)(const char* text, float confidence, void* user_data);
    
    void* user_data;
} ethervox_session_config_t;

// Create and initialize session
ethervox_session_t* ethervox_session_create(const ethervox_session_config_t* config);

// Cleanup and destroy session
void ethervox_session_destroy(ethervox_session_t* session);

// Check if session is ready for commands
bool ethervox_session_is_ready(ethervox_session_t* session);

// Get last error message (thread-local)
const char* ethervox_get_last_error(void);
```

### Command Execution

```c
// Command categories
typedef enum {
    ETHERVOX_CMD_CHAT,              // Regular conversation
    ETHERVOX_CMD_MEMORY,            // Memory operations (search, export, archive)
    ETHERVOX_CMD_MODEL,             // Model management (load, reload)
    ETHERVOX_CMD_VOICE,             // Voice/STT operations
    ETHERVOX_CMD_CONFIG,            // Configuration (paths, settings)
    ETHERVOX_CMD_SYSTEM,            // System operations (test, debug)
} ethervox_command_category_t;

// Generic command request
typedef struct {
    ethervox_command_category_t category;
    const char* command;            // e.g., "search", "load_model", "transcribe"
    const char* args;               // JSON string or simple text
    uint32_t flags;                 // Command-specific flags
} ethervox_command_request_t;

// Generic command response
typedef struct {
    int status;                     // 0 = success, non-zero = error code
    const char* result;             // JSON string or text result
    const char* error_message;      // Human-readable error (if status != 0)
    uint32_t flags;                 // Response-specific flags
    
    // Internal (managed by API)
    void* _internal;
} ethervox_command_response_t;

// Execute command (blocking)
int ethervox_execute_command(
    ethervox_session_t* session,
    const ethervox_command_request_t* request,
    ethervox_command_response_t* response
);

// Execute command (async - returns immediately, calls callback when done)
typedef void (*ethervox_command_callback_t)(
    const ethervox_command_response_t* response,
    void* user_data
);

int ethervox_execute_command_async(
    ethervox_session_t* session,
    const ethervox_command_request_t* request,
    ethervox_command_callback_t callback,
    void* user_data
);

// Free response resources
void ethervox_response_free(ethervox_command_response_t* response);
```

### High-Level Convenience API

```c
// Chat
int ethervox_chat(
    ethervox_session_t* session,
    const char* message,
    char** response,          // Caller must free with ethervox_free_string()
    char** error
);

// Memory search
typedef struct {
    char* text;
    float relevance;
    bool is_user_message;
    uint64_t timestamp;
    uint64_t memory_id;
} ethervox_search_result_t;

int ethervox_memory_search(
    ethervox_session_t* session,
    const char* query,
    ethervox_search_result_t** results,   // Array allocated by API
    uint32_t* result_count
);

void ethervox_search_results_free(ethervox_search_result_t* results, uint32_t count);

// Memory export
int ethervox_memory_export(
    ethervox_session_t* session,
    const char* output_path
);

// Model loading
int ethervox_load_model(
    ethervox_session_t* session,
    const char* model_path
);

// Voice transcription (async by nature)
typedef void (*ethervox_transcription_callback_t)(
    const char* text,
    float confidence,
    bool is_final,
    void* user_data
);

int ethervox_start_transcription(
    ethervox_session_t* session,
    const char* language_code,
    ethervox_transcription_callback_t callback,
    void* user_data
);

int ethervox_stop_transcription(
    ethervox_session_t* session,
    char** final_transcript,      // Saved to ~/.ethervox/transcripts/
    char** transcript_path
);

// Configuration
int ethervox_set_path(
    ethervox_session_t* session,
    const char* label,
    const char* path,
    const char* description
);

int ethervox_get_paths(
    ethervox_session_t* session,
    char*** labels,               // Array of strings
    char*** paths,
    uint32_t* count
);

// Free string returned by API
void ethervox_free_string(char* str);
```

---

## Platform Implementation Examples

### macOS CLI (`src/platform/macos/main_macos.c`)

```c
#include "ethervox/api.h"
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

static void on_log(const char* message, int level, void* user_data) {
    (void)user_data;
    if (level >= ETHERVOX_LOG_WARN) {
        fprintf(stderr, "[LOG] %s\n", message);
    }
}

static void on_progress(const char* operation, float progress, void* user_data) {
    (void)user_data;
    printf("\r%s: %.0f%%", operation, progress * 100.0f);
    fflush(stdout);
    if (progress >= 1.0f) printf("\n");
}

int main(int argc, char** argv) {
    // Parse macOS-specific args (--model, --memory-dir, etc.)
    const char* model_path = parse_args(argc, argv);
    
    // Initialize session
    ethervox_session_config_t config = {
        .model_path = model_path,
        .memory_dir = NULL,  // Use default ~/.ethervox/memory
        .config_dir = NULL,  // Use default ~/.ethervox
        .on_log = on_log,
        .on_progress = on_progress,
        .on_transcription = NULL,
        .user_data = NULL
    };
    
    ethervox_session_t* session = ethervox_session_create(&config);
    if (!session) {
        fprintf(stderr, "Failed to create session: %s\n", ethervox_get_last_error());
        return 1;
    }
    
    printf("EthervoxAI ready. Type /help for commands.\n");
    
    // Main readline loop
    while (true) {
        char* line = readline("> ");
        if (!line) break;
        
        if (line[0] != '\0') {
            add_history(line);
        }
        
        if (strcmp(line, "/quit") == 0) {
            free(line);
            break;
        }
        
        // Handle slash commands
        if (line[0] == '/') {
            handle_slash_command(session, line);
        } else {
            // Regular chat
            char* response = NULL;
            char* error = NULL;
            
            if (ethervox_chat(session, line, &response, &error) == 0) {
                printf("\n%s\n\n", response);
                ethervox_free_string(response);
            } else {
                fprintf(stderr, "Error: %s\n", error);
                ethervox_free_string(error);
            }
        }
        
        free(line);
    }
    
    ethervox_session_destroy(session);
    return 0;
}

static void handle_slash_command(ethervox_session_t* session, const char* line) {
    if (strncmp(line, "/search ", 8) == 0) {
        ethervox_search_result_t* results = NULL;
        uint32_t count = 0;
        
        if (ethervox_memory_search(session, line + 8, &results, &count) == 0) {
            printf("\nFound %u results:\n", count);
            for (uint32_t i = 0; i < count; i++) {
                printf("[%.2f] %s\n", results[i].relevance, results[i].text);
            }
            ethervox_search_results_free(results, count);
        }
    } else if (strncmp(line, "/load ", 6) == 0) {
        if (ethervox_load_model(session, line + 6) == 0) {
            printf("Model loaded successfully\n");
        } else {
            fprintf(stderr, "Failed to load model: %s\n", ethervox_get_last_error());
        }
    }
    // ... more commands
}
```

### Android JNI Bridge (`src/platform/android/jni_bridge.c`)

```c
#include "ethervox/api.h"
#include <jni.h>
#include <android/log.h>

#define LOG_TAG "EthervoxJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global session (one per process)
static ethervox_session_t* g_session = NULL;
static JavaVM* g_jvm = NULL;

// Callbacks bridge to Java
static void on_log_jni(const char* message, int level, void* user_data) {
    (void)user_data;
    
    JNIEnv* env;
    (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    
    // Call Java: EthervoxCallback.onLog(message, level)
    jclass callback_class = (*env)->FindClass(env, "ai/ethervox/EthervoxCallback");
    jmethodID on_log = (*env)->GetStaticMethodID(env, callback_class, "onLog", 
                                                  "(Ljava/lang/String;I)V");
    jstring jmsg = (*env)->NewStringUTF(env, message);
    (*env)->CallStaticVoidMethod(env, callback_class, on_log, jmsg, level);
    (*env)->DeleteLocalRef(env, jmsg);
}

// JNI: Initialize session
JNIEXPORT jlong JNICALL
Java_ai_ethervox_EthervoxSession_nativeCreate(
    JNIEnv* env,
    jobject thiz,
    jstring jModelPath,
    jstring jMemoryDir
) {
    (*env)->GetJavaVM(env, &g_jvm);
    
    const char* model_path = (*env)->GetStringUTFChars(env, jModelPath, NULL);
    const char* memory_dir = (*env)->GetStringUTFChars(env, jMemoryDir, NULL);
    
    ethervox_session_config_t config = {
        .model_path = model_path,
        .memory_dir = memory_dir,
        .config_dir = NULL,
        .on_log = on_log_jni,
        .on_progress = NULL,  // TODO: Bridge to Java
        .on_transcription = NULL,
        .user_data = NULL
    };
    
    ethervox_session_t* session = ethervox_session_create(&config);
    
    (*env)->ReleaseStringUTFChars(env, jModelPath, model_path);
    (*env)->ReleaseStringUTFChars(env, jMemoryDir, memory_dir);
    
    if (!session) {
        LOGE("Failed to create session: %s", ethervox_get_last_error());
        return 0;
    }
    
    g_session = session;
    return (jlong)session;
}

// JNI: Chat
JNIEXPORT jstring JNICALL
Java_ai_ethervox_EthervoxSession_chat(
    JNIEnv* env,
    jobject thiz,
    jlong sessionPtr,
    jstring jMessage
) {
    ethervox_session_t* session = (ethervox_session_t*)sessionPtr;
    const char* message = (*env)->GetStringUTFChars(env, jMessage, NULL);
    
    char* response = NULL;
    char* error = NULL;
    
    int result = ethervox_chat(session, message, &response, &error);
    
    (*env)->ReleaseStringUTFChars(env, jMessage, message);
    
    if (result == 0 && response) {
        jstring jresponse = (*env)->NewStringUTF(env, response);
        ethervox_free_string(response);
        return jresponse;
    } else {
        LOGE("Chat failed: %s", error ? error : "unknown");
        if (error) ethervox_free_string(error);
        return NULL;
    }
}

// JNI: Memory search
JNIEXPORT jobjectArray JNICALL
Java_ai_ethervox_EthervoxSession_searchMemory(
    JNIEnv* env,
    jobject thiz,
    jlong sessionPtr,
    jstring jQuery
) {
    ethervox_session_t* session = (ethervox_session_t*)sessionPtr;
    const char* query = (*env)->GetStringUTFChars(env, jQuery, NULL);
    
    ethervox_search_result_t* results = NULL;
    uint32_t count = 0;
    
    int result = ethervox_memory_search(session, query, &results, &count);
    
    (*env)->ReleaseStringUTFChars(env, jQuery, query);
    
    if (result != 0 || count == 0) {
        return NULL;
    }
    
    // Create Java array of SearchResult objects
    jclass result_class = (*env)->FindClass(env, "ai/ethervox/SearchResult");
    jobjectArray jresults = (*env)->NewObjectArray(env, count, result_class, NULL);
    
    for (uint32_t i = 0; i < count; i++) {
        jstring jtext = (*env)->NewStringUTF(env, results[i].text);
        
        // Call SearchResult constructor
        jmethodID constructor = (*env)->GetMethodID(env, result_class, "<init>", 
                                                    "(Ljava/lang/String;FZJ)V");
        jobject jresult = (*env)->NewObject(env, result_class, constructor,
                                            jtext,
                                            results[i].relevance,
                                            results[i].is_user_message,
                                            results[i].timestamp);
        
        (*env)->SetObjectArrayElement(env, jresults, i, jresult);
        (*env)->DeleteLocalRef(env, jtext);
        (*env)->DeleteLocalRef(env, jresult);
    }
    
    ethervox_search_results_free(results, count);
    return jresults;
}

// JNI: Cleanup
JNIEXPORT void JNICALL
Java_ai_ethervox_EthervoxSession_nativeDestroy(
    JNIEnv* env,
    jobject thiz,
    jlong sessionPtr
) {
    ethervox_session_t* session = (ethervox_session_t*)sessionPtr;
    ethervox_session_destroy(session);
    g_session = NULL;
}
```

### Android Kotlin API (`android/src/main/kotlin/EthervoxSession.kt`)

```kotlin
package ai.ethervox

class EthervoxSession(
    modelPath: String,
    memoryDir: String? = null
) : AutoCloseable {
    private val sessionPtr: Long
    
    init {
        System.loadLibrary("ethervox")
        sessionPtr = nativeCreate(modelPath, memoryDir ?: getDefaultMemoryDir())
        if (sessionPtr == 0L) {
            throw RuntimeException("Failed to create Ethervox session")
        }
    }
    
    fun chat(message: String): String? {
        return chat(sessionPtr, message)
    }
    
    fun searchMemory(query: String): List<SearchResult>? {
        return searchMemory(sessionPtr, query)?.toList()
    }
    
    fun loadModel(path: String): Boolean {
        return nativeLoadModel(sessionPtr, path)
    }
    
    override fun close() {
        if (sessionPtr != 0L) {
            nativeDestroy(sessionPtr)
        }
    }
    
    private external fun nativeCreate(modelPath: String, memoryDir: String): Long
    private external fun chat(sessionPtr: Long, message: String): String?
    private external fun searchMemory(sessionPtr: Long, query: String): Array<SearchResult>?
    private external fun nativeLoadModel(sessionPtr: Long, path: String): Boolean
    private external fun nativeDestroy(sessionPtr: Long)
    
    private fun getDefaultMemoryDir(): String {
        return "${android.os.Environment.getExternalStorageDirectory()}/ethervox/memory"
    }
}

data class SearchResult(
    val text: String,
    val relevance: Float,
    val isUserMessage: Boolean,
    val timestamp: Long
)

object EthervoxCallback {
    @JvmStatic
    fun onLog(message: String, level: Int) {
        when (level) {
            LOG_ERROR -> android.util.Log.e("Ethervox", message)
            LOG_WARN -> android.util.Log.w("Ethervox", message)
            else -> android.util.Log.i("Ethervox", message)
        }
    }
    
    const val LOG_DEBUG = 0
    const val LOG_INFO = 1
    const val LOG_WARN = 2
    const val LOG_ERROR = 3
}
```

---

## Migration Plan

### Phase 1: Extract Core API (Week 1)

**Goal:** Create `ethervox_api.c/h` without breaking existing main.c

1. **Create API Layer**
   - [ ] Create `src/core/ethervox_api.h` with public API
   - [ ] Implement `ethervox_session_create/destroy`
   - [ ] Implement `ethervox_chat()` wrapper
   - [ ] Implement `ethervox_memory_search()` wrapper

2. **Test in Parallel**
   - [ ] Keep existing main.c as-is
   - [ ] Create `tests/test_api.c` to test new API
   - [ ] Ensure API layer works identically to direct calls

3. **No Breaking Changes**
   - Governor, memory, LLM remain unchanged
   - main.c still compiles and works
   - New API is additive only

### Phase 2: Refactor macOS CLI (Week 2)

**Goal:** Migrate macOS main.c to use new API

1. **Create Platform Directory**
   - [ ] Move main.c → `src/platform/macos/main_macos.c`
   - [ ] Extract readline logic → `src/platform/macos/readline_ui.c`
   - [ ] Extract command parsing → `src/core/command_handler.c` (platform-agnostic)

2. **Rewrite main_macos.c**
   - [ ] Use `ethervox_session_create()` instead of direct governor init
   - [ ] Use `ethervox_chat()` instead of `ethervox_governor_execute()`
   - [ ] Use `ethervox_memory_search()` wrapper

3. **Validate Behavior**
   - [ ] All existing commands work identically
   - [ ] Memory persistence unchanged
   - [ ] Model loading works
   - [ ] Performance equivalent

### Phase 3: Android JNI Bridge (Week 3)

**Goal:** Create Android bindings using new API

1. **JNI Implementation**
   - [ ] Create `src/platform/android/jni_bridge.c`
   - [ ] Implement JNI wrappers for all API functions
   - [ ] Handle string marshalling (UTF-8 ↔ Java String)

2. **Kotlin API**
   - [ ] Create `EthervoxSession.kt` class
   - [ ] Implement callbacks (log, progress, transcription)
   - [ ] Add Android-specific path resolution

3. **Test App**
   - [ ] Create minimal Android test app
   - [ ] Test chat, memory search, model loading
   - [ ] Verify no crashes or memory leaks

### Phase 4: Documentation & Examples (Week 4)

1. **API Documentation**
   - [ ] Document all public API functions
   - [ ] Add usage examples for each platform
   - [ ] Create migration guide for existing code

2. **Example Apps**
   - [ ] Minimal macOS CLI (300 lines)
   - [ ] Minimal Android app (Kotlin + JNI)
   - [ ] Example showing async operations

3. **CI/CD Updates**
   - [ ] Build both macOS and Android in CI
   - [ ] Run API tests on both platforms
   - [ ] Package Android .aar artifact

---

## File Structure Changes

### Before (Current)

```
src/
├── main.c                 # 2100+ lines, macOS-specific
├── governor/
├── llm/
├── stt/
└── plugins/
```

### After (Proposed)

```
src/
├── core/                  # NEW: Platform-agnostic backend
│   ├── ethervox_api.c     # Public API implementation
│   ├── ethervox_api.h     # Public API header
│   ├── session.c          # Session state management
│   ├── command_handler.c  # Command logic (no UI)
│   └── event_bus.c        # Event notification
│
├── platform/              # NEW: Platform implementations
│   ├── common/
│   │   ├── platform_base.h
│   │   └── platform_utils.c
│   │
│   ├── macos/
│   │   ├── main_macos.c    # macOS CLI (uses ethervox_api.h)
│   │   ├── readline_ui.c
│   │   └── CMakeLists.txt
│   │
│   ├── android/
│   │   ├── jni_bridge.c
│   │   ├── CMakeLists.txt
│   │   └── kotlin/
│   │       └── EthervoxSession.kt
│   │
│   └── ios/               # Future
│       └── swift_bridge.c
│
├── governor/              # Unchanged
├── llm/                   # Unchanged
├── stt/                   # Unchanged
├── audio/                 # Unchanged
└── plugins/               # Unchanged

include/
└── ethervox/
    └── api.h              # NEW: Public API header (installed)
```

---

## Benefits

1. **Code Reuse:** Write business logic once, use everywhere
2. **Platform Freedom:** Each platform uses native UI patterns (SwiftUI, Jetpack Compose, etc.)
3. **Testability:** Backend testable without UI, UI testable with mock backend
4. **Maintainability:** Clear separation of concerns, easier to find bugs
5. **Extensibility:** New platforms (web, iOS) can be added without touching core

---

## Trade-offs & Considerations

### Performance

- **Pro:** No performance impact - API is thin wrapper
- **Con:** One extra function call (negligible)

### Complexity

- **Pro:** Clearer architecture, easier onboarding
- **Con:** More files to navigate initially

### Migration Risk

- **Mitigation:** Incremental approach, keep old main.c working during transition
- **Rollback:** Easy - just keep using old main.c

---

## Success Criteria

- [ ] macOS CLI works identically using new API
- [ ] Android app can chat, search memory, load models
- [ ] Zero regressions in existing functionality
- [ ] API documented with examples
- [ ] All platforms build in CI

---

## Future Enhancements

### iOS Support

```swift
// Swift API (wraps C API via bridge)
class EthervoxSession {
    private let sessionPtr: OpaquePointer
    
    init(modelPath: String, memoryDir: String? = nil) throws {
        guard let ptr = ethervox_session_create(...) else {
            throw EthervoxError.initFailed
        }
        self.sessionPtr = ptr
    }
    
    func chat(_ message: String) async throws -> String {
        return try await withCheckedThrowingContinuation { continuation in
            ethervox_chat_async(sessionPtr, message) { response, userData in
                continuation.resume(returning: String(cString: response))
            }
        }
    }
}
```

### WebAssembly

```javascript
// JavaScript API (wraps WASM module)
const ethervox = await EthervoxModule();

const session = ethervox.createSession({
    modelPath: '/models/granite-4.0.gguf',
    onLog: (msg, level) => console.log(msg),
    onProgress: (op, pct) => updateProgressBar(pct)
});

const response = await session.chat("Hello!");
console.log(response);
```

---

## Conclusion

This architecture provides:
1. Clean separation between UI and core logic
2. Reusable backend across all platforms
3. Platform-native UI experiences
4. Easy testing and maintenance
5. Future-proof extensibility

**Estimated Timeline:** 4 weeks for full migration  
**Risk Level:** Low (incremental, non-breaking)  
**Impact:** High (enables multi-platform deployment)
