cmake_minimum_required(VERSION 3.16)

project(EthervoxAI VERSION 0.0.3 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
    
    # Architecture-specific optimizations for Android - MAXIMUM PERFORMANCE
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        # ARM64 NEON optimizations with aggressive flags
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Ofast -fno-finite-math-only -march=armv8-a+fp+simd+crypto -mtune=cortex-a76")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Ofast -fno-finite-math-only -march=armv8-a+fp+simd+crypto -mtune=cortex-a76")
        message(STATUS "Enabled MAXIMUM ARM64 NEON optimizations for ${ANDROID_ABI}")
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

# Build configuration
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_EXAMPLES "Build example applications" ON)
option(ENABLE_DEBUG "Enable debug logging" OFF)
option(WITH_LLAMA "Enable Llama.cpp backend support" ON)
option(WITH_TINYLLAMA "Enable TinyLlama backend support" ON)

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
        # Enable NEON for ARM processors
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR ANDROID_ABI STREQUAL "arm64-v8a")
            set(GGML_CPU_ARM_NEON ON CACHE BOOL "" FORCE)
            message(STATUS "Enabled ARM NEON optimizations for llama.cpp")
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

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Common infrastructure sources
set(COMMON_SOURCES
    src/common/error.c
    src/common/logging.c
)

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

# Shared audio core implementation (excluding platform-specific files)
list(APPEND ETHERVOXAI_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/audio_core.c")

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
    list(APPEND ETHERVOXAI_SOURCES ethervox_multiplatform_core.c)
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
    set(ETHERVOXAI_PLATFORM_LIBS asound pthread curl ssl crypto m)
elseif(APPLE)
    set(ETHERVOXAI_PLATFORM_LIBS "-framework CoreAudio" "-framework AudioUnit" "-framework CoreFoundation" pthread curl)
endif()

if(ETHERVOXAI_PLATFORM_LIBS)
    target_link_libraries(ethervoxai PUBLIC ${ETHERVOXAI_PLATFORM_LIBS})
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
    endif()
    
    set_target_properties(ethervoxai_app PROPERTIES OUTPUT_NAME ethervoxai)
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