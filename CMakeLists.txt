cmake_minimum_required(VERSION 3.16)

project(EthervoxAI VERSION 0.0.5 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# =============================================================================
# Git Information Extraction (REQUIRED)
# =============================================================================

# Get version tag (cpp repo uses v* pattern, not backend-v*)
execute_process(
    COMMAND git describe --tags --abbrev=0 --match=v*
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)

# Get branch name (REQUIRED)
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_BRANCH_RESULT
)

# Get commit hash (REQUIRED)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_COMMIT_RESULT
)

# Verify git information is available
if(GIT_BRANCH_RESULT OR NOT GIT_BRANCH)
    message(FATAL_ERROR "Failed to get git branch. Ensure this is a git repository with commits.")
endif()

if(GIT_COMMIT_RESULT OR NOT GIT_COMMIT_HASH)
    message(FATAL_ERROR "Failed to get git commit hash. Ensure this is a git repository with commits.")
endif()

message(STATUS "Git Branch: ${GIT_BRANCH}")
message(STATUS "Git Commit: ${GIT_COMMIT_HASH}")

# Set backend version string
if(GIT_TAG_RESULT EQUAL 0 AND GIT_VERSION_TAG)
    # Tag exists - strip 'v' prefix (e.g., v0.0.5 -> 0.0.5)
    string(REPLACE "v" "" ETHERVOX_BACKEND_VERSION "${GIT_VERSION_TAG}")
    message(STATUS "Backend Version Tag: ${ETHERVOX_BACKEND_VERSION}")
else()
    # No tag - use branch-commit format
    set(ETHERVOX_BACKEND_VERSION "${GIT_BRANCH}-${GIT_COMMIT_HASH}")
    message(STATUS "Backend Version (no tag): ${ETHERVOX_BACKEND_VERSION}")
endif()

# Always define these at compile time
add_definitions(-DETHERVOX_BACKEND_VERSION="${ETHERVOX_BACKEND_VERSION}")
add_definitions(-DETHERVOX_GIT_BRANCH="${GIT_BRANCH}")
add_definitions(-DETHERVOX_GIT_COMMIT="${GIT_COMMIT_HASH}")

# =============================================================================
# GitHub Token Configuration (REQUIRED for bug reporter)
# =============================================================================
# Read GitHub token from environment variable at build time and validate it
# Priority: ETHERVOX_GITHUB_TOKEN_DESKTOP → ETHERVOX_GITHUB_TOKEN
# 
# The token MUST be set for builds to succeed. The validation script will:
# 1. Check if token exists
# 2. Verify token is valid via GitHub API
# 3. Check token has not expired
# 4. Verify access to timkos98/ethervoxai repository
#
# To create a token: https://github.com/settings/tokens
# Required permission: issues:write for timkos98/ethervoxai
#
# To disable validation (e.g., for development):
#   cmake -DSKIP_GITHUB_TOKEN_VALIDATION=ON
#   OR set environment variable: export SKIP_GITHUB_TOKEN_VALIDATION=1

# Check environment variable first, then CMake option
if(DEFINED ENV{SKIP_GITHUB_TOKEN_VALIDATION} AND NOT "$ENV{SKIP_GITHUB_TOKEN_VALIDATION}" STREQUAL "")
    set(SKIP_GITHUB_TOKEN_VALIDATION ON)
endif()

option(SKIP_GITHUB_TOKEN_VALIDATION "Skip GitHub token validation (disables bug reporter)" OFF)

if(DEFINED ENV{ETHERVOX_GITHUB_TOKEN_DESKTOP} AND NOT "$ENV{ETHERVOX_GITHUB_TOKEN_DESKTOP}" STREQUAL "")
    set(ETHERVOX_GITHUB_TOKEN "$ENV{ETHERVOX_GITHUB_TOKEN_DESKTOP}")
    message(STATUS "GitHub token: Using ETHERVOX_GITHUB_TOKEN_DESKTOP")
elseif(DEFINED ENV{ETHERVOX_GITHUB_TOKEN} AND NOT "$ENV{ETHERVOX_GITHUB_TOKEN}" STREQUAL "")
    set(ETHERVOX_GITHUB_TOKEN "$ENV{ETHERVOX_GITHUB_TOKEN}")
    message(STATUS "GitHub token: Using ETHERVOX_GITHUB_TOKEN")
else()
    set(ETHERVOX_GITHUB_TOKEN "")
endif()

# Validate the GitHub token using our validation script
# This will cause CMake configuration to FAIL if:
# - Token is not set
# - Token is invalid or expired
# - Token doesn't have required permissions
if(NOT SKIP_GITHUB_TOKEN_VALIDATION)
    set(GITHUB_TOKEN "${ETHERVOX_GITHUB_TOKEN}")
    include(cmake/validate_github_token.cmake)
else()
    message(WARNING "GitHub token validation SKIPPED - bug reporter will be disabled")
    set(ETHERVOX_GITHUB_TOKEN "")
endif()

add_definitions(-DETHERVOX_GITHUB_TOKEN="${ETHERVOX_GITHUB_TOKEN}")

# Parse version components (MAJOR.MINOR.PATCH) only if we have a proper tag
if(GIT_TAG_RESULT EQUAL 0 AND GIT_VERSION_TAG AND ETHERVOX_BACKEND_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR ${CMAKE_MATCH_2})
    set(VERSION_PATCH ${CMAKE_MATCH_3})
    add_definitions(-DETHERVOX_VERSION_MAJOR=${VERSION_MAJOR})
    add_definitions(-DETHERVOX_VERSION_MINOR=${VERSION_MINOR})
    add_definitions(-DETHERVOX_VERSION_PATCH=${VERSION_PATCH})
    message(STATUS "Version components: ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
else()
    # No version tag - components remain 0.0.0 from config.h defaults
    message(STATUS "No version tag found - using 0.0.0 from config.h defaults")
endif()

# Platform detection and configuration
# Only auto-detect if not cross-compiling
if(NOT DEFINED TARGET_PLATFORM AND NOT CMAKE_TOOLCHAIN_FILE)
    if(ANDROID)
        set(TARGET_PLATFORM "ANDROID")
    elseif(WIN32)
        set(TARGET_PLATFORM "WINDOWS")
    elseif(UNIX AND NOT APPLE)
        set(TARGET_PLATFORM "LINUX")
    elseif(APPLE)
        set(TARGET_PLATFORM "MACOS")
    else()
        message(FATAL_ERROR "Unsupported platform")
    endif()
elseif(NOT DEFINED TARGET_PLATFORM)
    # Cross-compiling - let toolchain set the platform
    if(ANDROID)
        set(TARGET_PLATFORM "ANDROID")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(TARGET_PLATFORM "RPI")
    endif()
endif()

message(STATUS "Target platform: ${TARGET_PLATFORM}")
message(STATUS "===================================================")

# Platform-specific configurations
if(TARGET_PLATFORM STREQUAL "ESP32")
    # ESP32 specific settings
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/esp32-toolchain.cmake")
    add_definitions(-DTARGET_ESP32=1)
    add_definitions(-DPLATFORM_EMBEDDED=1)
    set(ETHERVOX_PLATFORM_ESP32 1)  # Set as CMake variable too
    
elseif(TARGET_PLATFORM STREQUAL "RPI")
    # Raspberry Pi specific settings
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rpi-toolchain.cmake")
    add_definitions(-DTARGET_RPI=1)
    add_definitions(-DETHERVOX_PLATFORM_RPI=1)  # Define as preprocessor macro
    add_definitions(-DPLATFORM_EMBEDDED=1)
    set(ETHERVOX_PLATFORM_RPI 1)  # Set as CMake variable too
    
elseif(TARGET_PLATFORM STREQUAL "WINDOWS")
    # Windows specific settings
    add_definitions(-DTARGET_WINDOWS=1)
    add_definitions(-DETHERVOX_PLATFORM_WINDOWS=1)
    add_definitions(-DETHERVOX_PLATFORM_DESKTOP=1)
    add_definitions(-DPLATFORM_DESKTOP=1)
    set(ETHERVOX_PLATFORM_WINDOWS 1)
    set(ETHERVOX_PLATFORM_DESKTOP 1)
    
elseif(TARGET_PLATFORM STREQUAL "LINUX")
    # Linux specific settings
    add_definitions(-DTARGET_LINUX=1)
    add_definitions(-DETHERVOX_PLATFORM_LINUX=1)
    add_definitions(-DETHERVOX_PLATFORM_DESKTOP=1)
    add_definitions(-DPLATFORM_DESKTOP=1)
    set(ETHERVOX_PLATFORM_LINUX 1)
    set(ETHERVOX_PLATFORM_DESKTOP 1)

elseif(TARGET_PLATFORM STREQUAL "MACOS")
    # macOS specific settings
    add_definitions(-DTARGET_MACOS=1)
    add_definitions(-DETHERVOX_PLATFORM_MACOS=1)
    add_definitions(-DETHERVOX_PLATFORM_DESKTOP=1)
    add_definitions(-DPLATFORM_DESKTOP=1)
    set(ETHERVOX_PLATFORM_MACOS 1)
    set(ETHERVOX_PLATFORM_DESKTOP 1)

elseif(TARGET_PLATFORM STREQUAL "ANDROID")
    # Android specific settings
    add_definitions(-DTARGET_ANDROID=1)
    add_definitions(-DETHERVOX_PLATFORM_ANDROID=1)
    add_definitions(-DPLATFORM_MOBILE=1)
    set(ETHERVOX_PLATFORM_ANDROID 1)
    set(ETHERVOX_PLATFORM_MOBILE 1)
    
    # Architecture-specific optimizations for Android - SAFE BASELINE
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        # ARM64 baseline optimizations - disabled SIMD/crypto to fix SIGILL crash
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fno-finite-math-only -march=armv8-a")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fno-finite-math-only -march=armv8-a")
        message(STATUS "Enabled SAFE ARM64 baseline optimizations for ${ANDROID_ABI}")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        # x86_64 AVX optimizations - maximum performance
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Ofast -fno-finite-math-only -march=x86-64-v3 -mtune=generic")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Ofast -fno-finite-math-only -march=x86-64-v3 -mtune=generic")
        message(STATUS "Enabled MAXIMUM x86_64 optimizations for ${ANDROID_ABI}")
    endif()
endif()

# Show which platform macros are defined
message(STATUS "Show which platform macros are defined")
if(DEFINED ETHERVOX_PLATFORM_RPI)
    message(STATUS "ETHERVOX_PLATFORM_RPI is DEFINED")
endif()
if(DEFINED ETHERVOX_PLATFORM_ESP32)
    message(STATUS "ETHERVOX_PLATFORM_ESP32 is DEFINED")
endif()
if(DEFINED ETHERVOX_PLATFORM_WINDOWS)
    message(STATUS "ETHERVOX_PLATFORM_WINDOWS is DEFINED")
endif()
if(DEFINED ETHERVOX_PLATFORM_LINUX)
    message(STATUS "ETHERVOX_PLATFORM_LINUX is DEFINED")
endif()
if(DEFINED ETHERVOX_PLATFORM_DESKTOP)
    message(STATUS "ETHERVOX_PLATFORM_DESKTOP is DEFINED")
endif()
if(DEFINED ETHERVOX_PLATFORM_ANDROID)
    message(STATUS "ETHERVOX_PLATFORM_ANDROID is DEFINED")
endif()
if(DEFINED ETHERVOX_PLATFORM_MOBILE)
    message(STATUS "ETHERVOX_PLATFORM_MOBILE is DEFINED")
endif()

# Define runtime directory paths based on platform
if(TARGET_PLATFORM STREQUAL "WINDOWS")
    # Windows: %USERPROFILE%\.ethervox
    add_definitions(-DETHERVOX_RUNTIME_DIR_BASE="%USERPROFILE%\\\\.ethervox")
elseif(TARGET_PLATFORM STREQUAL "ANDROID")
    # Android: app-specific internal storage (handled at runtime via Java)
    add_definitions(-DETHERVOX_RUNTIME_DIR_BASE=NULL)
elseif(TARGET_PLATFORM STREQUAL "ESP32")
    # ESP32: SPIFFS or SD card
    add_definitions(-DETHERVOX_RUNTIME_DIR_BASE="/spiffs/ethervox")
else()
    # Unix-like (macOS, Linux, RPI): ~/.ethervox
    add_definitions(-DETHERVOX_RUNTIME_DIR_BASE="~/.ethervox")
endif()

message(STATUS "Runtime directory base configured for ${TARGET_PLATFORM}")

# Git info already extracted and defined above
# GIT_COMMIT_HASH and GIT_BRANCH are already available

# Build configuration
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_EXAMPLES "Build example applications" ON)
option(ENABLE_DEBUG "Enable debug logging" OFF)
option(WITH_LLAMA "Enable Llama.cpp backend support" ON)
option(WITH_TINYLLAMA "Enable TinyLlama backend support" ON)

# Fetch cJSON for settings persistence
include(FetchContent)
FetchContent_Declare(
    cJSON
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18
    PATCH_COMMAND sed -i.bak "s/cmake_minimum_required(VERSION 3.0)/cmake_minimum_required(VERSION 3.16)/" CMakeLists.txt || true
)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cJSON)

# Find libcurl for bug reporter (not needed on Android - uses Kotlin BugReporter instead)
if(NOT TARGET_PLATFORM STREQUAL "ANDROID")
    find_package(CURL REQUIRED)
    if(CURL_FOUND)
        message(STATUS "Found CURL: ${CURL_LIBRARIES}")
    else()
        message(FATAL_ERROR "CURL not found - required for bug reporter")
    endif()
else()
    message(STATUS "Skipping CURL (Android uses Kotlin BugReporter)")
endif()

if(ENABLE_DEBUG)
    add_definitions(-DDEBUG_ENABLED=1)
endif()

# LLM backend configuration
if(WITH_LLAMA)
    add_definitions(-DETHERVOX_WITH_LLAMA=1)
    message(STATUS "Llama.cpp backend enabled")
endif()

if(WITH_TINYLLAMA)
    add_definitions(-DETHERVOX_WITH_TINYLLAMA=1)
    message(STATUS "TinyLlama backend enabled")
endif()

# llama.cpp integration
message(STATUS "Checking llama.cpp: WITH_LLAMA=${WITH_LLAMA}, CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Checking path: ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp")
    message(STATUS "llama.cpp directory EXISTS")
else()
    message(WARNING "llama.cpp directory DOES NOT EXIST at ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp")
endif()

if(WITH_LLAMA AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp")
    message(STATUS "Adding llama.cpp from external/llama.cpp")
    
    # llama.cpp build options
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    
    # Enable CPU backend optimizations
    set(GGML_CPU ON CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)  # Disable native optimizations (can cause slowdowns)
    
    # Android-specific settings
    if(ANDROID)
        set(LLAMA_METAL OFF CACHE BOOL "" FORCE)
        set(LLAMA_CUBLAS OFF CACHE BOOL "" FORCE)
        set(LLAMA_CLBLAST OFF CACHE BOOL "" FORCE)
        set(LLAMA_OPENBLAS OFF CACHE BOOL "" FORCE)
        set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
        
        # Vulkan GPU acceleration is complex on Android and requires device-specific setup
        # Currently disabled - relying on ARM NEON optimizations instead
        # Future: Enable with proper Android Vulkan SDK integration
        set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
        
        # Disable NEON for ARM processors to fix SIGILL crash
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR ANDROID_ABI STREQUAL "arm64-v8a")
            set(GGML_CPU_ARM_NEON OFF CACHE BOOL "" FORCE)
            message(STATUS "Disabled ARM NEON optimizations for llama.cpp (fixing SIGILL crash)")
        endif()
        
        # Android 15+ 16KB page size compatibility - apply to llama.cpp libraries
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
        message(STATUS "Applied 16KB page alignment to llama.cpp libraries for Android 15+ compatibility")
    endif()
    
    # Add llama.cpp as subdirectory
    add_subdirectory(external/llama.cpp)
    
    add_definitions(-DLLAMA_CPP_AVAILABLE=1)
    message(STATUS "llama.cpp successfully configured")
elseif(WITH_LLAMA)
    message(WARNING "llama.cpp not found in external/llama.cpp - LLM features will be limited")
    add_definitions(-DLLAMA_CPP_AVAILABLE=0)
else()
    add_definitions(-DLLAMA_CPP_AVAILABLE=0)
endif()

# whisper.cpp integration for speech-to-text
option(WITH_WHISPER "Enable whisper.cpp for speech-to-text" ON)

if(WITH_WHISPER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp")
    message(STATUS "Adding whisper.cpp from external/whisper.cpp")
    
    # whisper.cpp build options
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    
    # Match llama.cpp settings for consistency
    set(GGML_CPU ON CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    
    # Android-specific settings
    if(ANDROID)
        set(WHISPER_METAL OFF CACHE BOOL "" FORCE)
        set(WHISPER_CUBLAS OFF CACHE BOOL "" FORCE)
        set(WHISPER_CLBLAST OFF CACHE BOOL "" FORCE)
        set(WHISPER_OPENBLAS OFF CACHE BOOL "" FORCE)
        
        # Disable NEON for ARM processors to fix SIGILL crash
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR ANDROID_ABI STREQUAL "arm64-v8a")
            set(GGML_CPU_ARM_NEON OFF CACHE BOOL "" FORCE)
            message(STATUS "Disabled ARM NEON optimizations for whisper.cpp (fixing SIGILL crash)")
        endif()
        
        # Android 15+ 16KB page size compatibility
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
        message(STATUS "Applied 16KB page alignment to whisper.cpp libraries for Android 15+ compatibility")
    endif()
    
    # Add whisper.cpp as subdirectory
    add_subdirectory(external/whisper.cpp)
    
    add_definitions(-DWHISPER_CPP_AVAILABLE=1)
    message(STATUS "whisper.cpp successfully configured")
elseif(WITH_WHISPER)
    message(WARNING "whisper.cpp not found in external/whisper.cpp - Voice features will be disabled")
    add_definitions(-DWHISPER_CPP_AVAILABLE=0)
else()
    add_definitions(-DWHISPER_CPP_AVAILABLE=0)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Common infrastructure sources
set(COMMON_SOURCES
    src/common/error.c
    src/common/logging.c
    src/common/settings_menu.c
    src/common/model_downloader.c
    src/config/settings.c
    src/audio/reference_buffer.c
    src/audio/aec_speex.c
    src/tts/tts.c
    src/tts/text_normalizer.c
    src/tts/piper_backend.c
    src/tts/phonemizer/phonemizer.c
    src/tts/phonemizer/dictionary.c
    src/tts/phonemizer/espeak_dict.c
    src/tts/phonemizer/rules_en.c
    src/tts/phonemizer/rules_de.c
    src/tts/phonemizer/dict_chinese.c
    src/tts/phonemizer/chinese_segmenter.c
    src/tts/phonemizer/pronunciation_overrides.c
    src/tts/phonemizer/pronunciation_trainer.c
)

# Espeak-trained dictionary embeddings (optional, improves accuracy)
# To enable: run tools/train_phonemizer.sh before building
option(ENABLE_ESPEAK_DICT "Enable embedded espeak-trained dictionaries" ON)

set(ESPEAK_DICT_ENABLED FALSE)
if(ENABLE_ESPEAK_DICT)
    # Check for generated dictionary headers
    set(ESPEAK_DICT_DIR "${CMAKE_SOURCE_DIR}/src/tts/phonemizer/data")
    
    if(EXISTS "${ESPEAK_DICT_DIR}/espeak_dict_en_us.h")
        message(STATUS "✅ Espeak dictionary (en-us) found - enabling")
        add_definitions(-DESPEAK_DICT_EN_US_ENABLED)
        set(ESPEAK_DICT_ENABLED TRUE)
    else()
        message(STATUS "⚠️  Espeak dictionary (en-us) not found. Run: tools/train_phonemizer.sh")
    endif()
    
    if(EXISTS "${ESPEAK_DICT_DIR}/espeak_dict_en_gb.h")
        message(STATUS "✅ Espeak dictionary (en-gb) found - enabling")
        add_definitions(-DESPEAK_DICT_EN_GB_ENABLED)
        set(ESPEAK_DICT_ENABLED TRUE)
    endif()
    
    if(EXISTS "${ESPEAK_DICT_DIR}/espeak_dict_de.h")
        message(STATUS "✅ Espeak dictionary (de) found - enabling")
        add_definitions(-DESPEAK_DICT_DE_ENABLED)
        set(ESPEAK_DICT_ENABLED TRUE)
    endif()
    
    # Add the data file if any dictionaries are enabled
    if(ESPEAK_DICT_ENABLED)
        list(APPEND COMMON_SOURCES src/tts/phonemizer/espeak_dict_data.c)
        message(STATUS "✅ Espeak dictionary data file included in build")
    endif()
endif()

# Add bug_reporter.c only for non-Android platforms (Android uses Kotlin BugReporter)
if(NOT TARGET_PLATFORM STREQUAL "ANDROID")
    list(APPEND COMMON_SOURCES src/common/bug_reporter.c)
endif()

# Collect core source files (non-platform specific)
file(GLOB ETHERVOXAI_CORE_SOURCES 
    "src/dialogue/*.c"
    "src/plugins/*.c"
    "src/wake_word/*.c"
    "src/stt/*.c"
    "src/governor/*.c"
    "src/plugins/compute_tools/*.c"
    "src/plugins/timer_tools/*.c"
)

# Note: src/dialogue/voice_conversation.c is included via GLOB above

# Exclude test files from the main library
list(FILTER ETHERVOXAI_CORE_SOURCES EXCLUDE REGEX ".*test_.*\\.c$")

# Add LLM sources only if LLM backends are enabled
if(WITH_LLAMA OR WITH_TINYLLAMA)
    file(GLOB LLM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/llm/*.c")
    message(STATUS "Found LLM sources: ${LLM_SOURCES}")
    # Exclude tinyllama_backend.c as it uses outdated APIs
    list(FILTER LLM_SOURCES EXCLUDE REGEX ".*tinyllama_backend\\.c$")
    message(STATUS "After filtering: ${LLM_SOURCES}")
    list(APPEND ETHERVOXAI_CORE_SOURCES ${LLM_SOURCES})
    message(STATUS "LLM backend sources included")
else()
    message(STATUS "LLM backend sources excluded (WITH_LLAMA=OFF, WITH_TINYLLAMA=OFF)")
endif()

# Add memory tools plugin sources
file(GLOB MEMORY_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/memory_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${MEMORY_TOOLS_SOURCES})
message(STATUS "Memory tools plugin sources included")

# Add file tools plugin sources
file(GLOB FILE_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/file_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${FILE_TOOLS_SOURCES})
message(STATUS "File tools plugin sources included")

# Add unit conversion plugin sources
file(GLOB UNIT_CONVERSION_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/unit_conversion/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${UNIT_CONVERSION_SOURCES})
message(STATUS "Unit conversion plugin sources included")

# Add startup prompt tools plugin sources
file(GLOB STARTUP_PROMPT_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/startup_prompt_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${STARTUP_PROMPT_TOOLS_SOURCES})

# System info tools
file(GLOB SYSTEM_INFO_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/system_info_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${SYSTEM_INFO_TOOLS_SOURCES})
message(STATUS "Startup prompt tools plugin sources included")

# Add context tools plugin sources
file(GLOB CONTEXT_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/context_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${CONTEXT_TOOLS_SOURCES})
message(STATUS "Context tools plugin sources included")

# Add conversation tools plugin sources (speak, listen for LLM-controlled interaction)
file(GLOB CONVERSATION_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/conversation_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${CONVERSATION_TOOLS_SOURCES})
message(STATUS "Conversation tools plugin sources included")

# Add voice tools plugin sources
file(GLOB VOICE_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/voice_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${VOICE_TOOLS_SOURCES})
message(STATUS "Voice tools plugin sources included")

# Add meta tools plugin sources (get_tool_info for dynamic schema loading)
file(GLOB META_TOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/meta_tools/*.c")
list(APPEND ETHERVOXAI_CORE_SOURCES ${META_TOOLS_SOURCES})
message(STATUS "Meta tools plugin sources included")

# Add dialogue sources (including integration tests)
file(GLOB DIALOGUE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/dialogue/*.c")
list(FILTER DIALOGUE_SOURCES EXCLUDE REGEX ".*test_.*\\.c$")  # Exclude test_ files
list(APPEND ETHERVOXAI_CORE_SOURCES ${DIALOGUE_SOURCES})
message(STATUS "Dialogue sources included: ${DIALOGUE_SOURCES}")

# Shared audio core implementation (excluding platform-specific files)
list(APPEND ETHERVOXAI_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/audio_core.c")
list(APPEND ETHERVOXAI_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/audio_recording.c")

# Platform-specific source files
# Check multiple conditions for RPI detection
if(ETHERVOX_PLATFORM_RPI OR 
   TARGET_PLATFORM STREQUAL "RPI" OR 
   CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    message(STATUS "Building for Raspberry Pi - using RPI platform sources")
    set(PLATFORM_SOURCES
        src/audio/platform_rpi.c
        src/platform/rpi_hal.c
    )
elseif(TARGET_PLATFORM STREQUAL "ESP32")
    message(STATUS "Building for ESP32 - using ESP32 platform sources")
    set(PLATFORM_SOURCES
        src/audio/platform_esp32.c
        src/platform/esp32_hal.c
    )
elseif(WIN32 OR TARGET_PLATFORM STREQUAL "WINDOWS")
    message(STATUS "Building for Windows - using Windows platform sources")
    set(PLATFORM_SOURCES
        src/audio/platform_windows.c
        src/platform/desktop_hal.c
    )
elseif(APPLE OR TARGET_PLATFORM STREQUAL "MACOS")
    message(STATUS "Building for macOS - using macOS platform sources")
    set(PLATFORM_SOURCES
        src/audio/platform_macos.c
        src/platform/desktop_hal.c
    )
elseif(ANDROID OR TARGET_PLATFORM STREQUAL "ANDROID")
    message(STATUS "Building for Android - using Android platform sources")
    set(PLATFORM_SOURCES
        src/audio/platform_android.c
        src/platform/android_hal.c
    )
else()
    message(STATUS "Building for Linux - using Linux platform sources")
    # Linux/Desktop default
    set(PLATFORM_SOURCES
        src/audio/platform_linux.c
        src/platform/desktop_hal.c
    )
endif()

# Combine core and platform sources for the reusable library
# set(ETHERVOXAI_SOURCES ${ETHERVOXAI_CORE_SOURCES} ${PLATFORM_SOURCES} src/platform/platform_core.c)

# Combine core and platform sources for the reusable library
set(ETHERVOXAI_SOURCES ${COMMON_SOURCES} ${ETHERVOXAI_CORE_SOURCES} ${PLATFORM_SOURCES} src/platform/platform_core.c)

# Add JNI wrapper for Android
if(ANDROID)
    list(APPEND ETHERVOXAI_SOURCES 
        src/platform/ethervox_android_core.c
        src/platform/model_management_jni.c
    )
endif()

# Debug: Print sources being compiled
# message(STATUS "Core sources: ${ETHERVOXAI_CORE_SOURCES}")
# message(STATUS "Platform sources: ${PLATFORM_SOURCES}")

# Debug: Print sources being compiled
message(STATUS "Common sources: ${COMMON_SOURCES}")
message(STATUS "Core sources: ${ETHERVOXAI_CORE_SOURCES}")
message(STATUS "Platform sources: ${PLATFORM_SOURCES}")
message(STATUS "FINAL ETHERVOXAI_SOURCES: ${ETHERVOXAI_SOURCES}")

# Create library - shared for Android, static for others
if(ANDROID)
    add_library(ethervoxai SHARED ${ETHERVOXAI_SOURCES})
    
    # Add 16KB page alignment for Android 15+ compatibility
    target_link_options(ethervoxai PRIVATE
        -Wl,-z,max-page-size=16384
    )
    message(STATUS "Enabled 16KB page alignment for Android")
else()
    add_library(ethervoxai STATIC ${ETHERVOXAI_SOURCES})
endif()

target_include_directories(ethervoxai
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link llama.cpp if available
if(WITH_LLAMA)
    if(TARGET llama)
        target_include_directories(ethervoxai PRIVATE 
            ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/include
            ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/src
            ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/vendor
        )
        target_link_libraries(ethervoxai PUBLIC llama)
        message(STATUS "Linked llama.cpp library to ethervoxai")
    else()
        message(WARNING "llama target not found - checking for ggml and llama-base")
        if(TARGET ggml AND TARGET llama-base)
            target_include_directories(ethervoxai PRIVATE 
                ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/include
                ${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/src
            )
            target_link_libraries(ethervoxai PUBLIC ggml llama-base)
            message(STATUS "Linked ggml and llama-base libraries to ethervoxai")
        else()
            message(WARNING "No llama.cpp targets found - LLM will not be available")
        endif()
    endif()
endif()

# Link whisper.cpp if available
if(WITH_WHISPER)
    if(TARGET whisper)
        target_include_directories(ethervoxai PRIVATE 
            ${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp/include
            ${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp/src
        )
        target_link_libraries(ethervoxai PUBLIC whisper)
        message(STATUS "Linked whisper.cpp library to ethervoxai")
    else()
        message(WARNING "whisper target not found - Voice features will be limited")
    endif()
endif()

# Link cJSON for settings persistence
if(TARGET cjson)
    target_link_libraries(ethervoxai PUBLIC cjson)
    target_include_directories(ethervoxai PRIVATE ${cjson_SOURCE_DIR})
    message(STATUS "Linked cJSON library to ethervoxai")
else()
    message(WARNING "cJSON target not found - settings persistence will fail")
endif()

# Link Speex DSP for AEC (echo cancellation) and resampling
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SPEEX speexdsp)
    if(SPEEX_FOUND)
        target_link_libraries(ethervoxai PUBLIC ${SPEEX_LINK_LIBRARIES})
        target_include_directories(ethervoxai PRIVATE ${SPEEX_INCLUDE_DIRS})
        target_link_directories(ethervoxai PRIVATE ${SPEEX_LIBRARY_DIRS})
        message(STATUS "Linked Speex DSP library: ${SPEEX_VERSION}")
        message(STATUS "Speex lib dirs: ${SPEEX_LIBRARY_DIRS}")
        message(STATUS "Speex link libs: ${SPEEX_LINK_LIBRARIES}")
    else()
        message(WARNING "Speex DSP not found - AEC will use passthrough mode. Install with: brew install speexdsp")
    endif()
    
    # Link ONNX Runtime for Piper TTS
    pkg_check_modules(ONNXRUNTIME onnxruntime)
    if(ONNXRUNTIME_FOUND)
        target_link_libraries(ethervoxai PUBLIC ${ONNXRUNTIME_LINK_LIBRARIES})
        target_include_directories(ethervoxai PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
        target_link_directories(ethervoxai PRIVATE ${ONNXRUNTIME_LIBRARY_DIRS})
        target_compile_definitions(ethervoxai PUBLIC HAVE_PIPER_TTS=1)
        message(STATUS "Linked ONNX Runtime library: ${ONNXRUNTIME_VERSION}")
        message(STATUS "✅ Piper TTS backend ENABLED")
    else()
        # Try manual detection (Homebrew on macOS)
        find_path(ONNXRUNTIME_INCLUDE_DIR 
            NAMES onnxruntime_c_api.h
            PATHS /opt/homebrew/Cellar/onnxruntime/*/include/onnxruntime
                  /usr/local/Cellar/onnxruntime/*/include/onnxruntime
                  /opt/homebrew/include/onnxruntime
                  /usr/local/include/onnxruntime
        )
        find_library(ONNXRUNTIME_LIBRARY
            NAMES onnxruntime
            PATHS /opt/homebrew/Cellar/onnxruntime/*/lib
                  /usr/local/Cellar/onnxruntime/*/lib
                  /opt/homebrew/lib
                  /usr/local/lib
        )
        
        if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
            # Include parent directory so we can use #include <onnxruntime/...>
            get_filename_component(ONNXRUNTIME_INCLUDE_PARENT "${ONNXRUNTIME_INCLUDE_DIR}" DIRECTORY)
            target_include_directories(ethervoxai PRIVATE ${ONNXRUNTIME_INCLUDE_PARENT})
            target_link_libraries(ethervoxai PUBLIC ${ONNXRUNTIME_LIBRARY})
            target_compile_definitions(ethervoxai PUBLIC HAVE_PIPER_TTS=1)
            message(STATUS "Found ONNX Runtime manually:")
            message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_PARENT}")
            message(STATUS "  Library: ${ONNXRUNTIME_LIBRARY}")
            message(STATUS "✅ Piper TTS backend ENABLED")
        else()
            message(WARNING "ONNX Runtime not found - Piper TTS will be unavailable. Install with: brew install onnxruntime")
        endif()
    endif()
    
    # espeak-ng REMOVED - replaced with custom phonemizer (MIT-licensed)
    # See src/tts/phonemizer.c for dictionary-based G2P implementation
    message(STATUS "Using custom phonemizer (espeak-ng dependency removed for licensing)")
else()
    message(WARNING "PkgConfig not found - skipping Speex and ONNX Runtime detection")
endif()

# Platform-specific linking for library consumers
set(ETHERVOXAI_PLATFORM_LIBS "")

if(WIN32)
    set(ETHERVOXAI_PLATFORM_LIBS winmm ws2_32 crypt32 wininet)
elseif(TARGET_PLATFORM STREQUAL "RPI")
    # For cross-compilation, link libraries directly (they're installed via apt for the target arch)
    set(ETHERVOXAI_PLATFORM_LIBS asound pthread curl ssl crypto m)
    
    # Only search for bcm2835 in sysroot if CMAKE_SYSROOT is defined
    if(CMAKE_SYSROOT)
        set(_ethervox_rpi_lib_paths
            ${CMAKE_SYSROOT}/lib
            ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf
            ${CMAKE_SYSROOT}/../lib
            ${CMAKE_SYSROOT}/../lib/arm-linux-gnueabihf
        )

        find_library(BCM2835_LIB bcm2835 PATHS ${_ethervox_rpi_lib_paths} NO_DEFAULT_PATH)
        if(BCM2835_LIB)
            list(APPEND ETHERVOXAI_PLATFORM_LIBS ${BCM2835_LIB})
            target_compile_definitions(ethervoxai PUBLIC ETHERVOX_HAVE_BCM2835=1)
        else()
            target_compile_definitions(ethervoxai PUBLIC ETHERVOX_HAVE_BCM2835=0)
            message(WARNING "bcm2835 library not found in Raspberry Pi sysroot; GPIO features disabled")
        endif()
    else()
        # No sysroot - skip bcm2835 (typical for GitHub Actions cross-compilation)
        target_compile_definitions(ethervoxai PUBLIC ETHERVOX_HAVE_BCM2835=0)
        message(STATUS "No CMAKE_SYSROOT defined; skipping bcm2835 library search")
    endif()

elseif(ANDROID)
    # Android libraries
    find_library(LOG_LIB log)
    find_library(ANDROID_LIB android)
    set(ETHERVOXAI_PLATFORM_LIBS ${LOG_LIB} ${ANDROID_LIB} m)
    
    # Add AAudio for API 26+, OpenSL ES for API 24+
    if(ANDROID_PLATFORM_LEVEL GREATER_EQUAL 26)
        find_library(AAUDIO_LIB aaudio)
        if(AAUDIO_LIB)
            list(APPEND ETHERVOXAI_PLATFORM_LIBS ${AAUDIO_LIB})
            message(STATUS "AAudio library found: ${AAUDIO_LIB}")
        endif()
    endif()
    
    find_library(OPENSLES_LIB OpenSLES)
    if(OPENSLES_LIB)
        list(APPEND ETHERVOXAI_PLATFORM_LIBS ${OPENSLES_LIB})
        message(STATUS "OpenSL ES library found: ${OPENSLES_LIB}")
    endif()
    
elseif(UNIX AND NOT APPLE)
    set(ETHERVOXAI_PLATFORM_LIBS asound pthread ${CURL_LIBRARIES} ssl crypto m)
elseif(APPLE)
    set(ETHERVOXAI_PLATFORM_LIBS "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreFoundation" pthread ${CURL_LIBRARIES})
endif()

if(ETHERVOXAI_PLATFORM_LIBS)
    target_link_libraries(ethervoxai PUBLIC ${ETHERVOXAI_PLATFORM_LIBS})
    target_include_directories(ethervoxai PRIVATE ${CURL_INCLUDE_DIRS})
endif()

set_property(GLOBAL PROPERTY ETHERVOXAI_PLATFORM_LIBS "${ETHERVOXAI_PLATFORM_LIBS}")

# CLI executable built on top of the reusable library (not for Android)
if(NOT ANDROID)
    add_executable(ethervoxai_app src/main.c)
    target_link_libraries(ethervoxai_app PRIVATE ethervoxai)
    
    # Add readline for command history on macOS/Linux
    if(APPLE OR UNIX)
        find_library(READLINE_LIBRARY readline)
        if(READLINE_LIBRARY)
            target_link_libraries(ethervoxai_app PRIVATE ${READLINE_LIBRARY})
            message(STATUS "Readline library found: ${READLINE_LIBRARY}")
        else()
            message(WARNING "Readline library not found - command history will be disabled")
        endif()
        
        # Add ncurses for settings menu
        find_library(NCURSES_LIBRARY ncurses)
        if(NCURSES_LIBRARY)
            target_link_libraries(ethervoxai_app PRIVATE ${NCURSES_LIBRARY})
            message(STATUS "Ncurses library found: ${NCURSES_LIBRARY}")
        else()
            message(WARNING "Ncurses library not found - settings menu will be disabled")
        endif()
    endif()
    
    set_target_properties(ethervoxai_app PROPERTIES OUTPUT_NAME ethervoxai)
endif()

# Voice quality tuner tool
if(NOT ANDROID)
    add_executable(voice_tuner tools/voice_tuner.c)
    target_link_libraries(voice_tuner PRIVATE ethervoxai cjson)
    target_include_directories(voice_tuner PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${cjson_SOURCE_DIR})
    set_target_properties(voice_tuner PROPERTIES OUTPUT_NAME ethervox-tune-voice)
    install(TARGETS voice_tuner RUNTIME DESTINATION bin)
    
    # Emotion tester tool for multi-speaker models
    add_executable(emotion_tester tools/emotion_tester.c)
    target_link_libraries(emotion_tester PRIVATE ethervoxai cjson)
    target_include_directories(emotion_tester PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${cjson_SOURCE_DIR})
    set_target_properties(emotion_tester PROPERTIES OUTPUT_NAME ethervox-test-emotions)
    install(TARGETS emotion_tester RUNTIME DESTINATION bin)
endif()

# Optional: Examples and tests (if directories exist and not Android)
if(BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples" AND NOT ANDROID)
    add_subdirectory(examples)
endif()

if(BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests" AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS ethervoxai
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib)
if(NOT ANDROID)
    install(TARGETS ethervoxai_app RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include)
install(FILES README.md LICENSE DESTINATION share/doc/ethervoxai)

# Basic CPack configuration so `cmake --build --target package` works everywhere
set(CPACK_PACKAGE_NAME "EthervoxAI")
set(CPACK_PACKAGE_VENDOR "EtherVoxAI")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "EtherVoxAI Voice Runtime")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_GENERATOR "TGZ")
include(CPack)