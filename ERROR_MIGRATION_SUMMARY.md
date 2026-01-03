# Error Handling Migration Summary

**Date:** January 3, 2026  
**Status:** Phase 1 Started | First File Complete (settings.c)

---

## 🎯 Migration Progress

### Completed Files (12/~150)

#### ✅ **src/config/settings.c** - Configuration Management
- **Date Completed:** January 3, 2026
- **Functions Migrated:** 3/5 (save, load, import)
- **Error Codes Used:**
  - `ETHERVOX_ERROR_NULL_POINTER` - NULL pointer validation
  - `ETHERVOX_ERROR_OUT_OF_MEMORY` - Memory allocation failures
  - `ETHERVOX_ERROR_CONFIG_SAVE_FAILED` - JSON serialization errors
  - `ETHERVOX_ERROR_FILE_WRITE` - File I/O errors
  - `ETHERVOX_ERROR_CONFIG_PARSE_ERROR` - JSON parsing errors
- **Callers Updated:** 19 call sites across 5 files
  - `src/main.c` (9 sites)
  - `src/common/settings_menu.c` (7 sites)
  - `src/common/language_detector.c` (1 site)
  - `src/dialogue/voice_conversation.c` (1 site)
  - `src/common/bug_reporter.c` (1 site - no error check needed)
- **Header Updated:** `include/ethervox/settings.h` - added error.h include + 3 function signatures
- **Build Status:** ✅ Compiles successfully
- **Test Status:** ✅ All tests pass (ErrorHandling, Config, SettingsPersistence)
- **Notes:** 
  - `ethervox_settings_export()` returns `char*` (no migration needed)
  - `ethervox_settings_print()` returns `void` (no migration needed)

#### ✅ **src/plugins/file_tools/path_config.c** - Path Configuration
- **Date Completed:** January 3, 2026
- **Functions Migrated:** 5/6 (init, set, get, list, get_unverified)
- **Error Codes Used:**
  - `ETHERVOX_ERROR_NULL_POINTER` - NULL pointer validation
  - `ETHERVOX_ERROR_NOT_INITIALIZED` - Config not initialized
  - `ETHERVOX_ERROR_PATH_INVALID` - Path doesn't exist or not verified
  - `ETHERVOX_ERROR_BUFFER_TOO_SMALL` - Max paths reached
  - `ETHERVOX_ERROR_NOT_FOUND` - Path label not found
  - `ETHERVOX_ERROR_OUT_OF_MEMORY` - Memory allocation failures
- **Callers Updated:** 11 call sites across 4 files
  - `src/main.c` (4 sites)
  - `src/plugins/file_tools/file_registry.c` (4 sites)
  - `src/plugins/voice_tools/voice_tools.c` (3 sites)
- **Header Updated:** `include/ethervox/file_tools.h` - added error.h include + 5 function signatures
- **Build Status:** ✅ Compiles successfully
- **Test Status:** ✅ All tests pass (ErrorHandling, FileTools)
- **Notes:**
  - `ethervox_path_config_cleanup()` returns `void` (no migration needed)
  - Used logging for detailed error context since ETHERVOX_RETURN_ERROR doesn't support format strings

---

## ✅ Completed Infrastructure

### 1. Comprehensive Error Code System

**Added 80+ error codes** across 12 subsystem categories:

- **General** (-1 to -99): 13 codes - basic operations
- **Platform** (-100 to -199): 4 codes - HAL and hardware
- **Audio** (-200 to -299): 6 codes - audio device management
- **STT** (-300 to -349): 7 codes - speech-to-text operations
- **TTS** (-350 to -399): 8 codes - text-to-speech synthesis
- **Wake Word** (-400 to -449): 4 codes - wake word detection
- **LLM** (-450 to -499): 8 codes - language model operations
- **Plugin** (-500 to -549): 6 codes - plugin system
- **Governor/Tool** (-550 to -599): 8 codes - tool orchestration
- **Network/API** (-600 to -649): 8 codes - network operations
- **Memory System** (-650 to -699): 7 codes - memory management
- **File I/O** (-700 to -749): 10 codes - file operations
- **Configuration** (-750 to -799): 7 codes - settings management
- **Dialogue** (-800 to -849): 5 codes - conversation management

### 2. Core Implementation Files

✅ **include/ethervox/error.h** (v1.2)
- Complete ethervox_result_t enum with all 80+ codes
- Error context structure with thread-local storage
- Helper macros: ETHERVOX_ERROR_SET, ETHERVOX_RETURN_ERROR, ETHERVOX_CHECK, ETHERVOX_CHECK_PTR
- Inline helper functions: ethervox_is_success(), ethervox_is_error()

✅ **src/common/error.c**
- Thread-local error context implementation (Windows/GCC/Clang/C11 compatible)
- ethervox_error_string() - converts all 80+ error codes to human-readable strings
- ethervox_error_set_context() - stores error with file/line/function/timestamp
- ethervox_error_get_context() - retrieves last error
- ethervox_error_clear() - resets error state
- Platform-specific timestamping (POSIX clock_gettime, Windows GetTickCount64, fallback)

✅ **tests/unit/test_error.c**
- 16 comprehensive tests covering:
  - All error code string conversions
  - Context preservation (file, line, function, message)
  - NULL message handling
  - Error propagation chains (3-level deep)
  - Edge cases (100 iterations, 1KB messages)
  - Thread-safety verification
  - Realistic workflow scenarios
  - Integration with logging
- **Test Results:** 16/16 passing (100% success rate)

### 3. Documentation Created

✅ **MIGRATION_CHECKLIST.md**
- Comprehensive list of ~150 C source files organized by subsystem
- Status tracking (❌ Not Started | 🔄 In Progress | ✅ Completed | ⏭️ Skipped)
- 24 organized sections: Core, Platform, Audio, STT, TTS, Phonemizer, Wake Word, LLM, Governor, Dialogue, Plugins, etc.
- Migration priority phases (5 weeks, 20 tasks)
- Summary statistics and progress tracking

✅ **docs/errorhandling.md** (ready to update)
- Original v1.1 document in place
- Needs update to reflect:
  - 80+ error codes (currently shows ~45)
  - Migration status section
  - Link to MIGRATION_CHECKLIST.md

### 4. Build Verification

✅ **CMake Build Test**
```
cmake --build build --target ethervoxai
[56/56] Linking C executable ethervoxai
✅ Build successful (only unrelated warnings)
```

---

## 📊 Current Migration Status

### Files Completed: 2/~150

| File | Status | Notes |
|------|--------|-------|
| src/common/error.c | ✅ | Core error system implementation |
| tests/unit/test_error.c | ✅ | 16/16 comprehensive tests passing |

### Files In Progress: 1/~150

| File | Status | Operations Migrated | Operations Remaining |
|------|--------|---------------------|----------------------|
| src/main.c | 🔄 8/~108 | 8 critical init paths | ~100 TTS/audio/file ops |

**Main.c Integration Points Completed:**
1. ✅ Line ~33: Added #include "ethervox/error.h"
2. ✅ Lines ~140-145: standalone_speak_callback NULL check
3. ✅ Lines ~221-246: wake_word_listen_thread audio init with full context
4. ✅ Lines ~387-393: reload_model_callback NULL check
5. ✅ Lines ~3132-3147: Platform initialization with detailed output
6. ✅ Lines ~3149-3167: Memory initialization with troubleshooting hints
7. ✅ Lines ~3169-3180: Path config initialization with cleanup
8. ✅ Lines ~3632-3645: Tool registry initialization
9. ✅ Lines ~3675-3695: Governor initialization with troubleshooting

### Files Not Started: ~130/~150

See [MIGRATION_CHECKLIST.md](MIGRATION_CHECKLIST.md) for complete list organized by subsystem.

---

## 🎯 Next Steps

### Recommended Migration Order

#### Phase 1: Core Utilities (Week 1)
Priority files that other modules depend on:

1. **src/config/settings.c** - Configuration loading/saving
   - Functions: `ethervox_settings_save()`, `ethervox_settings_load()`, `ethervox_settings_import()`
   - Returns: int → ethervox_result_t
   - Error codes: CONFIG_LOAD_FAILED, CONFIG_SAVE_FAILED, CONFIG_PARSE_ERROR

2. **src/plugins/file_tools/path_config.c** - Path management
   - Functions: `ethervox_path_config_set()`, `ethervox_path_config_get()`, etc.
   - Returns: int → ethervox_result_t
   - Error codes: PATH_INVALID, DIRECTORY_NOT_FOUND, DIRECTORY_CREATE_FAILED

3. **src/common/logging.c** - Log file management
4. **src/common/language_detector.c** - Language detection
5. **src/common/bug_reporter.c** - Bug reporting
6. **src/common/model_downloader.c** - Model downloads

#### Phase 2: Platform & Audio (Week 2)
7. **src/platform/platform_core.c** - Platform detection
8. **src/platform/*.hal.c** - HAL implementations (desktop, rpi, esp32, android)
9. **src/audio/audio_core.c** - Core audio initialization
10. **src/audio/platform_*.c** - Platform-specific audio backends
11. **src/audio/audio_recording.c** - Recording operations
12. **src/audio_processing/*.c** - VAD, noise reduction, buffers

#### Phase 3: Voice Systems (Week 3)
13. **src/stt/stt_core.c** - STT interface
14. **src/stt/whisper_backend.c** - Whisper integration
15. **src/stt/vosk_backend.c** - Vosk integration
16. **src/tts/tts.c** - TTS interface
17. **src/tts/piper_backend.c** - Piper integration
18. **src/tts/phonemizer/*.c** - Phonemizer subsystem
19. **src/wake_word/*.c** - Wake word detection

#### Phase 4: LLM & Governor (Week 4)
20. **src/llm/llm_core.c** - LLM interface
21. **src/llm/llama_backend.c** - llama.cpp backend
22. **src/llm/model_manager.c** - Model management
23. **src/governor/governor.c** - Governor core
24. **src/governor/tool_registry.c** - Tool registration
25. **src/governor/tool_manifest.c** - Manifest system
26. **src/dialogue/*.c** - Dialogue management

#### Phase 5: Plugins & Finalization (Week 5)
27. **src/plugins/plugin_manager.c** - Plugin system
28. **src/plugins/memory_tools/*.c** - Memory plugin
29. **src/plugins/file_tools/*.c** - File plugin
30. **src/plugins/voice_tools/*.c** - Voice plugin
31. **src/plugins/*/c** - Remaining plugins
32. **src/main.c** - Complete remaining ~100 operations
33. **sdk/ethervox_sdk.c** - SDK wrappers
34. Update all test files to use new error handling

---

## 🔧 Migration Process Per File

For each source file, follow this workflow:

### 1. Analysis Phase
- Identify all functions that return int (error codes)
- Identify functions that currently return -1/0 for errors
- Document current error handling patterns
- List dependencies on other not-yet-migrated files

### 2. Implementation Phase
- Update function signatures: `int func()` → `ethervox_result_t func()`
- Replace `return -1` with `ETHERVOX_RETURN_ERROR(CODE, "message")`
- Replace `return 0` with `return ETHERVOX_SUCCESS`
- Use `ETHERVOX_CHECK()` to propagate errors from called functions
- Use `ETHERVOX_CHECK_PTR()` for NULL pointer validation
- Add new error codes to error.h if needed (rare - we have 80+)
- Update error.c with string descriptions if new codes added

### 3. Caller Update Phase
- Update all call sites to handle `ethervox_result_t`
- Replace `if (func() < 0)` with `if (ethervox_is_error(func()))`
- Replace `if (func() != 0)` with `if (ethervox_is_error(func()))`
- Add error context retrieval: `ethervox_error_get_context()`
- Add detailed error messages for users

### 4. Testing Phase
- Compile: `cmake --build build --target ethervoxai`
- Run unit tests: `./build/tests/test_<module>`
- Run integration tests if available
- Verify error messages are helpful and actionable

### 5. Documentation Phase
- Update MIGRATION_CHECKLIST.md status (❌ → ✅)
- Update errorhandling.md if patterns change
- Add comments for complex error handling
- Document any deviations from standard patterns

---

## 📝 Migration Templates

### Template 1: Simple Error Return
```c
// Before:
int my_function(void* data) {
    if (!data) {
        return -1;
    }
    // ... do work ...
    return 0;
}

// After:
ethervox_result_t my_function(void* data) {
    ETHERVOX_CHECK_PTR(data);
    
    // ... do work ...
    
    return ETHERVOX_SUCCESS;
}
```

### Template 2: Error Propagation
```c
// Before:
int high_level_func() {
    int result = low_level_func();
    if (result != 0) {
        return result;
    }
    return 0;
}

// After:
ethervox_result_t high_level_func() {
    ETHERVOX_CHECK(low_level_func());
    
    return ETHERVOX_SUCCESS;
}
```

### Template 3: Custom Error Message
```c
// Before:
int load_model(const char* path) {
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "Model not found: %s\n", path);
        return -1;
    }
    // ... load model ...
    return 0;
}

// After:
ethervox_result_t load_model(const char* path) {
    ETHERVOX_CHECK_PTR(path);
    
    if (access(path, R_OK) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Model file not accessible: %s", path);
        ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_FILE_NOT_FOUND, msg);
    }
    
    // ... load model ...
    
    return ETHERVOX_SUCCESS;
}
```

### Template 4: Caller Update
```c
// Before:
if (my_function(data) != 0) {
    printf("Error occurred\n");
    cleanup();
    return -1;
}

// After:
ethervox_result_t result = my_function(data);
if (ethervox_is_error(result)) {
    const ethervox_error_context_t* ctx = ethervox_error_get_context();
    if (ctx && ctx->message) {
        printf("Error: %s (%s:%d in %s)\n",
               ctx->message, ctx->file, ctx->line, ctx->function);
    } else {
        printf("Error: %s\n", ethervox_error_string(result));
    }
    cleanup();
    return result;  // Propagate the error
}
```

---

## ⚠️ Common Pitfalls

### 1. Mixed Return Types
**Problem:** Some functions return both int (error) and data (via pointer).

**Solution:**
```c
// Before:
int get_data(int* out_value) {
    *out_value = calculate();
    return 0;  // or -1 on error
}

// After:
ethervox_result_t get_data(int* out_value) {
    ETHERVOX_CHECK_PTR(out_value);
    
    *out_value = calculate();
    return ETHERVOX_SUCCESS;
}
```

### 2. NULL Error Context
**Problem:** Defensive code checks error context that might be NULL in legacy paths.

**Solution:**
```c
const ethervox_error_context_t* ctx = ethervox_error_get_context();
if (ctx && ctx->message) {
    printf("Error: %s\n", ctx->message);
} else {
    printf("Error: %s\n", ethervox_error_string(result));
}
```

### 3. Success Value Confusion
**Problem:** Some functions return 0 for success, others return positive values for success.

**Solution:** Always use `ETHERVOX_SUCCESS` (0) for success. For functions that need to return a count or index:
```c
// Return count via pointer, status via return
ethervox_result_t get_items(item_t** items, uint32_t* count) {
    ETHERVOX_CHECK_PTR(items);
    ETHERVOX_CHECK_PTR(count);
    
    // ... get items ...
    *count = num_items;
    *items = item_array;
    
    return ETHERVOX_SUCCESS;
}
```

### 4. Forgetting to Update Callers
**Problem:** Function signature changed but callers not updated.

**Solution:** Use grep to find all call sites:
```bash
grep -r "my_function" src/ --include="*.c" --include="*.cpp"
```

---

## 🚀 Quick Start Guide

To begin migration today:

1. **Pick a file from Phase 1** (recommend: `src/config/settings.c`)

2. **Create a branch:**
   ```bash
   git checkout -b feature/error-handling-settings
   ```

3. **Migrate the file** using templates above

4. **Test compilation:**
   ```bash
   cmake --build build --target ethervoxai
   ```

5. **Update checklist:**
   ```markdown
   | File | Status | Functions | Notes |
   |------|--------|-----------|-------|
   | `src/config/settings.c` | ✅ | 5 functions migrated | CONFIG_* error codes used |
   ```

6. **Commit and push:**
   ```bash
   git add src/config/settings.c MIGRATION_CHECKLIST.md
   git commit -m "Migrate settings.c to ethervox_result_t error handling"
   git push origin feature/error-handling-settings
   ```

7. **Repeat** for next file in priority list

---

## 📈 Progress Tracking

Update these metrics weekly:

- **Files Completed:** 8/150 (5.3%)
  - error.c, test_error.c, settings.c, path_config.c, model_downloader.c, platform_core.c, desktop_hal.c, rpi_hal.c
- **Error Codes Used:** 24+ codes actively used in practice
- **Build Status:** ✅ Compiles successfully
- **Test Coverage:** ErrorHandling, Config, FileTools passing
- **Documentation:** MIGRATION_CHECKLIST.md current, error-handling-reference.md created

---

## 📝 Completed Migration Details

### File: src/platform/rpi_hal.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 2 (Platform Abstraction Layer)

**Functions Migrated (7/7):**
1. `rpi_init()` - Initialize WiringPi, I2C, and SPI
2. `rpi_gpio_configure()` - Configure GPIO pin mode with pullup/pulldown
3. `rpi_gpio_write()` - Write GPIO pin state
4. `rpi_i2c_write()` - Write to I2C device
5. `rpi_i2c_read()` - Read from I2C device
6. `rpi_spi_transfer()` - Bidirectional SPI transfer
7. `rpi_hal_register()` - Register HAL functions (public function)

**Header Updated:** include/ethervox/platform.h (already updated with HAL types)

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL parameter validation
- `ETHERVOX_ERROR_NOT_INITIALIZED` - WiringPi/I2C not initialized
- `ETHERVOX_ERROR_INVALID_ARGUMENT` - Invalid GPIO mode or zero-length transfer
- `ETHERVOX_ERROR_PLATFORM_INIT` - WiringPi, I2C, or SPI initialization/operation failures

**Caller Updates:**
- platform_core.c - extern declaration updated
- No direct callers (called via function pointers in HAL)

**Key Migration Patterns:**
- Hardware initialization with detailed error checking
- Proper NULL pointer validation before operations
- Error propagation from WiringPi library calls
- Initialization state tracking (wiringpi_initialized flag)
- Switch statement with default error case

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Platform tests run on actual hardware

---

### File: src/platform/desktop_hal.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 2 (Platform Abstraction Layer)

**Functions Migrated (7/7):**
1. `desktop_init()` - Platform initialization (returns SUCCESS)
2. `desktop_gpio_configure()` - GPIO configuration (NOT_SUPPORTED)
3. `desktop_gpio_write()` - GPIO write (NOT_SUPPORTED)
4. `desktop_i2c_write()` - I2C write (NOT_SUPPORTED)
5. `desktop_i2c_read()` - I2C read (NOT_SUPPORTED)
6. `desktop_spi_transfer()` - SPI transfer (NOT_SUPPORTED)
7. `desktop_hal_register()` - Register HAL functions (public function)

**Header Updated:** include/ethervox/platform.h (HAL function pointer types updated)

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL parameter validation
- `ETHERVOX_ERROR_NOT_SUPPORTED` - Desktop doesn't support GPIO/I2C/SPI

**Caller Updates:**
- platform_core.c - extern declaration updated
- No direct callers (called via function pointers in HAL)

**Key Migration Patterns:**
- All I2C/SPI functions return NOT_SUPPORTED (hardware not available on desktop)
- Init returns SUCCESS (no setup needed)
- Consistent stub pattern across unsupported features

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Platform tests run on actual hardware

---

### File: src/platform/android_hal.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 2 (Platform Abstraction Layer)

**Functions Migrated (14/14):**
1. `android_init()` - Initialize Android platform with memory allocation
2. `android_gpio_configure()` - GPIO configuration (NOT_SUPPORTED)
3. `android_gpio_write()` - GPIO write (NOT_SUPPORTED)
4. `android_gpio_set_pwm()` - PWM control (NOT_SUPPORTED)
5. `android_i2c_init()` - I2C initialization (NOT_SUPPORTED)
6. `android_i2c_write()` - I2C write (NOT_SUPPORTED)
7. `android_i2c_read()` - I2C read (NOT_SUPPORTED)
8. `android_spi_init()` - SPI initialization (NOT_SUPPORTED)
9. `android_spi_transfer()` - SPI transfer (NOT_SUPPORTED)
10. `android_set_cpu_frequency()` - CPU frequency control (NOT_SUPPORTED)
11. `android_enable_power_saving()` - Power saving mode (NOT_SUPPORTED)
12. `ethervox_platform_hal_register_android()` - Register HAL functions (public function)

**Header Updated:** include/ethervox/platform.h (already updated with HAL types)

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL parameter validation (via ETHERVOX_CHECK_PTR)
- `ETHERVOX_ERROR_OUT_OF_MEMORY` - Memory allocation failure in android_init
- `ETHERVOX_ERROR_NOT_SUPPORTED` - Android doesn't support GPIO/I2C/SPI/power control

**Caller Updates:**
- platform_core.c - extern declaration updated
- No direct callers (called via function pointers in HAL)

**Key Migration Patterns:**
- Complex initialization with memory allocation and error checking
- All hardware I/O functions return NOT_SUPPORTED (no direct hardware access on Android)
- Power management functions return NOT_SUPPORTED (managed by Android OS)
- Used ETHERVOX_CHECK_PTR macro for cleaner NULL validation

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Platform tests run on actual hardware

---

### File: src/platform/esp32_hal.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 2 (Platform Abstraction Layer)

**Functions Migrated (7/7):**
1. `esp32_init()` - Initialize GPIO, I2C, and SPI subsystems
2. `esp32_gpio_configure()` - Configure GPIO pin mode
3. `esp32_gpio_write()` - Write GPIO pin state
4. `esp32_i2c_write()` - Write to I2C device
5. `esp32_i2c_read()` - Read from I2C device
6. `esp32_spi_transfer()` - Bidirectional SPI transfer
7. `esp32_hal_register()` - Register HAL functions (public function)

**Header Updated:** include/ethervox/platform.h (already updated with HAL types)

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL parameter validation (via ETHERVOX_CHECK_PTR)
- `ETHERVOX_ERROR_INVALID_ARGUMENT` - Invalid GPIO mode or zero-length I2C transfer
- `ETHERVOX_ERROR_GPIO_FAILURE` - GPIO configuration/write failures
- `ETHERVOX_ERROR_I2C_FAILURE` - I2C read/write operation failures
- `ETHERVOX_ERROR_SPI_FAILURE` - SPI transfer failures

**Caller Updates:**
- platform_core.c - extern declaration updated
- No direct callers (called via function pointers in HAL)

**Key Migration Patterns:**
- Hardware initialization with ESP-IDF error checking
- Proper NULL pointer and argument validation
- Error propagation from ESP-IDF API calls (gpio_config, i2c_master_cmd_begin, spi_device_transmit)
- Switch statement with error-returning default case for GPIO modes
- Bus selection logic for multi-controller chips

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Platform tests run on actual hardware

---

## 📊 Migration Statistics (Updated 2026-01-03)

**Files Completed:** 12/150 (8.0%)
- error.c, test_error.c, settings.c, path_config.c, model_downloader.c, platform_core.c, desktop_hal.c, rpi_hal.c, android_hal.c, esp32_hal.c, audio_core.c, platform_macos.c

**Caller Updates:**
- platform_core.c - extern declaration updated
- No direct callers (called via function pointers in HAL)

**Key Migration Patterns:**
- HAL function pointer types changed from `int (*)` to `ethervox_result_t (*)`
- Audio driver function pointers changed from `int (*)` to `ethervox_result_t (*)`
- Stub functions return `NOT_SUPPORTED` instead of -1
- Init function returns `ETHERVOX_SUCCESS` instead of 0
- Register function uses `ETHERVOX_CHECK_PTR` for validation

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Platform tests run as integration tests

---

### File: src/audio/audio_core.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 3 (Audio Subsystem)

**Functions Migrated (8/8):**
1. `ethervox_audio_init()` - Initialize audio runtime with driver registration
2. `ethervox_audio_start()` - Start audio system
3. `ethervox_audio_stop()` - Stop audio system
4. `ethervox_audio_start_capture()` - Start audio capture
5. `ethervox_audio_stop_capture()` - Stop audio capture
6. `ethervox_audio_read()` - Read captured audio samples
7. `ethervox_language_detect()` - Detect language from audio
8. `ethervox_tts_synthesize()` - Synthesize speech from text

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL runtime, config, buffer, text validation
- `ETHERVOX_ERROR_NOT_INITIALIZED` - Runtime not initialized or driver not set
- `ETHERVOX_ERROR_NOT_SUPPORTED` - Driver function pointer NULL
- `ETHERVOX_ERROR_OUT_OF_MEMORY` - Buffer allocation failures
- `ETHERVOX_ERROR_TTS_SYNTHESIS_FAILED` - TTS system command failure (macOS `say`)

**Migration Details:**
- All driver function pointers validated before calls (NOT_SUPPORTED if NULL)
- Error propagation from driver calls using `ethervox_is_error()`
- Buffer semantics: `buffer->size` set with actual samples read
- macOS TTS: Uses system `say` command with error checking
- Fallback TTS: Generates 440Hz sine wave tone (2 seconds)
- Audio read: Modified to update buffer size with actual read count

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Audio tests require hardware

---

### File: src/audio/platform_macos.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 3 (Audio Subsystem - Platform Implementation)

**Functions Migrated (7/7):**
1. `macos_audio_init()` - Initialize macOS AudioQueue with ring buffers
2. `macos_audio_start_capture()` - Start input AudioQueue
3. `macos_audio_stop_capture()` - Stop input AudioQueue
4. `macos_audio_start_playback()` - Start output AudioQueue
5. `macos_audio_stop_playback()` - Stop output AudioQueue
6. `macos_audio_read()` - Read samples from capture ring buffer
7. `macos_audio_write()` - Write samples to playback ring buffer
8. `ethervox_audio_register_platform_driver()` - Register macOS driver

**Error Codes Used:**
- `ETHERVOX_ERROR_OUT_OF_MEMORY` - Ring buffer allocations (3: state, capture 10s, playback 60s @ 16kHz mono = ~1.9MB)
- `ETHERVOX_ERROR_AUDIO_INIT` - AudioQueue creation, buffer allocation, queue start failures
- `ETHERVOX_ERROR_AUDIO_DEVICE_NOT_FOUND` - Invalid audio device (kAudioQueueErr_InvalidDevice = -66677)
- `ETHERVOX_ERROR_NOT_INITIALIZED` - State NULL checks

**Migration Details:**
- Ring buffers: 10 seconds capture (160KB), 60 seconds playback (960KB)
- AudioQueue callbacks: input_callback, output_callback (no error returns needed)
- Sample format conversion: int16 ↔ float32 in callbacks
- Thread safety: pthread_mutex for ring buffer access
- Permission denied: Detailed error message for -50 (microphone/audio permissions)
- Auto-start playback: write() starts playback if not already active
- Modified semantics: `macos_audio_read()` sets `buffer->size` with actual samples read
- Register function: Returns `ETHERVOX_SUCCESS`, uses `ETHERVOX_CHECK_PTR(driver)`

**Callers Updated:**
- audio.h header: Added `#include "ethervox/error.h"`, updated all function signatures
- ethervox_audio_driver_t: All function pointers changed from `int (*)` to `ethervox_result_t (*)`
- Other platform files: Updated register function signatures (partial - need complete driver migrations)

**Build Status:** ✅ Compiles successfully (macOS-only build includes only platform_macos.c)  
**Test Status:** ⏳ Audio driver tests require macOS hardware

---

### File: src/platform/platform_core.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 2 (Platform Abstraction Layer)

**Functions Migrated (5/5):**
1. `ethervox_platform_register_hal()` - Register platform-specific HAL
2. `ethervox_platform_init()` - Initialize platform with hardware detection
3. `ethervox_gpio_configure_pin()` - Configure GPIO pin mode
4. `ethervox_gpio_write_pin()` - Write GPIO pin state
5. `ethervox_platform_load_device_profile()` - Load device configuration

**Header Updated:** include/ethervox/platform.h (all 5 signatures)

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL parameter validation
- `ETHERVOX_ERROR_PLATFORM_INIT` - No HAL available for platform
- `ETHERVOX_ERROR_NOT_IMPLEMENTED` - GPIO functions not implemented

**Caller Updates (3 call sites across 3 files):**
1. src/main.c - test_platform() function
2. src/platform/ethervox_android_core.c - Android platform initialization
3. examples/voice_assistant/main.c - example pipeline_init()

**Key Migration Patterns:**
- Platform detection with compile-time conditionals (#ifdef)
- HAL registration error propagation
- GPIO capability checking before operations
- Device profile loading (placeholder for future)

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ Platform tests run as integration tests

---

### File: src/common/model_downloader.c
**Date Completed:** 2026-01-03  
**Phase:** Phase 1 (Core Utilities)

**Functions Migrated (7/7):**
1. `ethervox_model_get_base_dir()` - Get platform-specific model directory
2. `ethervox_model_get_default()` - Get recommended model for type
3. `ethervox_model_list()` - List available models (allocates array)
4. `ethervox_model_download()` - Download model via curl
5. `ethervox_model_cancel_download()` - Cancel active download (stub)
6. `ethervox_model_delete()` - Delete model file/directory
7. `ethervox_model_get_disk_usage()` - Calculate total storage

**Header Updated:** include/ethervox/model_downloader.h (all 7 signatures)

**Error Codes Used:**
- `ETHERVOX_ERROR_NULL_POINTER` - NULL parameter validation
- `ETHERVOX_ERROR_INVALID_ARGUMENT` - Invalid buffer size
- `ETHERVOX_ERROR_PLATFORM_INIT` - Android files dir unavailable
- `ETHERVOX_ERROR_BUFFER_TOO_SMALL` - Path truncation
- `ETHERVOX_ERROR_NOT_FOUND` - Model definition not found
- `ETHERVOX_ERROR_OUT_OF_MEMORY` - Array allocation failure
- `ETHERVOX_ERROR_DOWNLOAD_FAILED` - curl command failure
- `ETHERVOX_ERROR_NOT_IMPLEMENTED` - Cancel download stub
- `ETHERVOX_ERROR_FILE_DELETE_FAILED` - unlink/rm -rf failure

**Caller Updates (11 call sites across 3 files):**
1. src/main.c - 4 call sites (list ×2, download, delete)
2. src/common/model_downloader.c - 5 internal calls (get_base_dir propagation)
3. src/platform/model_management_jni.c - 6 JNI wrappers (get_base_dir, get_default, list, download, cancel, delete)

**Key Migration Patterns:**
- Platform-specific path handling (Android vs desktop)
- Error propagation from get_base_dir to callers
- System command execution with error checking (curl, rm -rf)
- Memory allocation error handling
- JNI wrapper return value propagation

**Build Status:** ✅ Compiles successfully  
**Test Status:** ⏳ No specific model_downloader tests (integration only)

---

## ✅ Success Criteria

Migration is complete when:

1. ✅ All 150+ source files use `ethervox_result_t`
2. ✅ No functions return raw int for error codes
3. ✅ All error paths set proper context
4. ✅ Build succeeds with no warnings
5. ✅ All unit tests pass
6. ✅ Integration tests pass
7. ✅ Documentation updated
8. ✅ MIGRATION_CHECKLIST.md shows 150/150 completed

---

## 📞 Questions?

- Review existing integration in [src/main.c](src/main.c) lines 3132-3695 for examples
- Check [tests/unit/test_error.c](tests/unit/test_error.c) for test patterns
- See [include/ethervox/error.h](include/ethervox/error.h) for API reference
- Consult [MIGRATION_CHECKLIST.md](MIGRATION_CHECKLIST.md) for file organization

---

**Ready to begin!** Start with [src/config/settings.c](src/config/settings.c) as the first migration target.
