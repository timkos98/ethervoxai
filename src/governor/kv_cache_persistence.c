/**
 * @file kv_cache_persistence.c
 * @brief KV Cache save/load implementation
 *
 * Saves processed system prompt to disk for instant loading.
 * 
 * File format:
 * [Header 512 bytes]
 * [System Prompt Tokens: N * 4 bytes]
 * [KV Cache Data: variable size]
 * [Checksum: 32 bytes]
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/kv_cache_persistence.h"
#include "ethervox/governor.h"
#include "ethervox/logging.h"
#include "ethervox/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>  // For unlink(), fsync(), usleep()
#include <fcntl.h>   // For open(), O_RDONLY

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#include <llama.h>
#define LLAMA_AVAILABLE 1
#else
#define LLAMA_AVAILABLE 0
#endif

#define KV_CACHE_MAGIC 0x4B564341  // "KVCA"
#define KV_CACHE_VERSION 1

// Access governor internals (defined in governor.c)
#if LLAMA_AVAILABLE
extern llama_token* ethervox_governor_get_system_tokens(struct ethervox_governor* gov, int* len);
extern struct llama_context* ethervox_governor_get_context(struct ethervox_governor* gov);
extern void ethervox_governor_set_system_tokens(struct ethervox_governor* gov, 
                                               llama_token* tokens, int len);
#endif

/**
 * Extract model name from path
 */
static void extract_model_name(const char* model_path, char* name_out, size_t name_size) {
    const char* filename = strrchr(model_path, '/');
    if (!filename) filename = strrchr(model_path, '\\');
    if (!filename) filename = model_path;
    else filename++;
    
    // Copy and remove extension
    strncpy(name_out, filename, name_size - 1);
    name_out[name_size - 1] = '\0';
    
    char* ext = strrchr(name_out, '.');
    if (ext) *ext = '\0';
}

/**
 * Simple checksum (XOR-based for speed, SHA-256 would be better but adds dependency)
 */
static void calculate_checksum(const uint8_t* data, size_t data_size, uint8_t* checksum_out) {
    memset(checksum_out, 0, 32);
    for (size_t i = 0; i < data_size; i++) {
        checksum_out[i % 32] ^= data[i];
    }
}

bool ethervox_kv_cache_exists(const char* cache_path, const char* model_path) {
    if (!cache_path || !model_path) return false;
    
    // Check if file exists and is readable
    struct stat st;
    if (stat(cache_path, &st) != 0) {
        return false;
    }
    
    // Check if it's a regular file
    if (!S_ISREG(st.st_mode)) {
        ETHERVOX_LOG_WARN("KV cache path is not a regular file: %s", cache_path);
        return false;
    }
    
    // Sanity check: File should be at least 1KB (bare minimum for any valid cache)
    // System prompt caches can be small (15-20KB for 1B models)
    const size_t min_size = 1024;  // 1KB minimum (just to catch empty/corrupted files)
    
    if (st.st_size < min_size) {
        ETHERVOX_LOG_WARN("KV cache file suspiciously small: %s (%lld bytes)", 
                         cache_path, (long long)st.st_size);
        ETHERVOX_LOG_WARN("Deleting likely corrupted cache file");
        unlink(cache_path);  // Delete corrupted cache
        return false;
    }
    
    // File exists and looks valid - llama.cpp will do its own validation when loading
    ETHERVOX_LOG_INFO("Found KV cache file: %s (%lld bytes)", cache_path, (long long)st.st_size);
    return true;
}

ethervox_result_t ethervox_kv_cache_save(
    struct ethervox_governor* governor,
    const char* cache_path
) {
    if (!governor || !cache_path) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !LLAMA_AVAILABLE
    ETHERVOX_LOG_ERROR("KV cache save not available: llama.cpp not linked");
    return ETHERVOX_ERROR_NOT_IMPLEMENTED;
#else
    
    ETHERVOX_LOG_INFO("Saving KV cache to: %s", cache_path);
    
    // Get system prompt tokens
    int token_count = 0;
    llama_token* tokens = ethervox_governor_get_system_tokens(governor, &token_count);
    
    if (!tokens || token_count == 0) {
        ETHERVOX_LOG_ERROR("No system prompt tokens to save");
        return ETHERVOX_ERROR_NOT_INITIALIZED;
    }
    
    ETHERVOX_LOG_INFO("System prompt tokens to save: %d", token_count);
    
    // Get llama context
    struct llama_context* ctx = ethervox_governor_get_context(governor);
    if (!ctx) {
        ETHERVOX_LOG_ERROR("No llama context available");
        return ETHERVOX_ERROR_NOT_INITIALIZED;
    }
    
    // Ensure cache directory exists
    // Extract directory from cache_path
    char dir_path[512];
    strncpy(dir_path, cache_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    
    char* last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';  // Terminate at last slash to get directory
        
        ETHERVOX_LOG_INFO("Checking cache directory: %s", dir_path);
        
        // Create directory if it doesn't exist
        struct stat st;
        if (stat(dir_path, &st) != 0) {
            // Directory doesn't exist, create it
            ETHERVOX_LOG_WARN("Cache directory doesn't exist, creating: %s", dir_path);
            
            #if defined(__APPLE__)
            // iOS/macOS: Directory should be created by Swift, but create if needed
            if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                ETHERVOX_LOG_ERROR("Failed to create cache directory: %s (errno=%d: %s)", 
                                  dir_path, errno, strerror(errno));
                return ETHERVOX_ERROR_PLATFORM_OPERATION_FAILED;
            }
            ETHERVOX_LOG_INFO("Created cache directory: %s", dir_path);
            #elif defined(__ANDROID__)
            // Android: create with permissions 0755
            if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                ETHERVOX_LOG_ERROR("Failed to create cache directory: %s (errno=%d: %s)", 
                                  dir_path, errno, strerror(errno));
                return ETHERVOX_ERROR_PLATFORM_OPERATION_FAILED;
            }
            ETHERVOX_LOG_INFO("Created cache directory: %s", dir_path);
            #else
            // Other Unix systems
            if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                ETHERVOX_LOG_ERROR("Failed to create cache directory: %s (errno=%d: %s)", 
                                  dir_path, errno, strerror(errno));
                return ETHERVOX_ERROR_PLATFORM_OPERATION_FAILED;
            }
            ETHERVOX_LOG_INFO("Created cache directory: %s", dir_path);
            #endif
        } else {
            ETHERVOX_LOG_INFO("Cache directory already exists: %s", dir_path);
        }
    } else {
        ETHERVOX_LOG_ERROR("Invalid cache path (no directory separator): %s", cache_path);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    ETHERVOX_LOG_INFO("Calling llama_state_seq_save_file for sequence 1 (permanent)...");
    
    // Use llama.cpp's sequence-based state save function
    // Save sequence 1 which contains the system prompt (generated in seq 1)
    // NOTE: We generate system prompt into seq 1 (permanent), so we save from seq 1
    size_t bytes_written = llama_state_seq_save_file(
        ctx, 
        cache_path,
        1,  // Save sequence 1 (system prompt lives here after generation)
        tokens, 
        (size_t)token_count
    );
    
    if (bytes_written == 0) {
        ETHERVOX_LOG_ERROR("llama_state_seq_save_file() failed for sequence 0 (returned 0 bytes)");
        ETHERVOX_LOG_ERROR("Check file permissions on: %s", cache_path);
        return ETHERVOX_ERROR_FILE_WRITE;
    }
    
    ETHERVOX_LOG_INFO("✓ llama_state_seq_save_file returned %zu bytes", bytes_written);
    
    // CRITICAL: Force flush to disk before verification (iOS file protection issue)
    // llama_state_seq_save_file uses FILE* which is buffered - must sync to disk
    ETHERVOX_LOG_INFO("Forcing file sync to disk...");
    
    #if defined(__APPLE__)
    // iOS/macOS: Open file in write mode and call fsync() to force kernel to write all buffers to disk
    // MUST use O_WRONLY or O_RDWR - O_RDONLY will not sync writes!
    int fd = open(cache_path, O_WRONLY);
    if (fd >= 0) {
        if (fsync(fd) == 0) {
            ETHERVOX_LOG_INFO("✓ File synced to disk successfully");
        } else {
            ETHERVOX_LOG_WARN("fsync() failed (errno=%d: %s) - file may not be fully written", 
                             errno, strerror(errno));
        }
        close(fd);
    } else {
        ETHERVOX_LOG_WARN("Cannot open file for fsync (errno=%d: %s)", errno, strerror(errno));
    }
    #else
    // Android/other: Use sync() as fallback
    sync();
    ETHERVOX_LOG_INFO("✓ Called sync() to flush buffers");
    #endif
    
    // Small delay to let iOS/macOS file system stabilize (helps with file protection)
    #if defined(__APPLE__)
    usleep(200000);  // 200ms delay (increased from 100ms for better reliability)
    #endif
    
    // Verify file was actually written to disk
    struct stat verify_st;
    if (stat(cache_path, &verify_st) == 0) {
        ETHERVOX_LOG_INFO("✓ Verified file on disk: %lld bytes", (long long)verify_st.st_size);
        
        // Check if file size matches what we wrote
        if ((size_t)verify_st.st_size != bytes_written) {
            ETHERVOX_LOG_ERROR("Cache file size mismatch: disk=%lld bytes, expected=%zu bytes",
                              (long long)verify_st.st_size, bytes_written);
            
            // Only fail if file is significantly smaller (>10% difference)
            // Small differences might be due to file system alignment
            size_t size_diff = bytes_written > (size_t)verify_st.st_size 
                             ? bytes_written - (size_t)verify_st.st_size
                             : (size_t)verify_st.st_size - bytes_written;
            
            if (size_diff > bytes_written / 10) {  // More than 10% difference
                ETHERVOX_LOG_ERROR("File was not fully written - iOS buffering issue");
                ETHERVOX_LOG_ERROR("Deleting incomplete cache file");
                unlink(cache_path);  // Delete corrupted partial file
                return ETHERVOX_ERROR_FILE_WRITE;
            } else {
                ETHERVOX_LOG_WARN("Minor file size difference (%zu bytes) - likely file system alignment",
                                 size_diff);
            }
        }
    } else {
        ETHERVOX_LOG_ERROR("Failed to verify saved file: %s (errno=%d: %s)",
                          cache_path, errno, strerror(errno));
        return ETHERVOX_ERROR_FILE_WRITE;
    }
    
    ETHERVOX_LOG_INFO("✓ KV cache saved: %d tokens + KV state (seq 1 permanent) to %s (%zu bytes)", 
                     token_count, cache_path, bytes_written);
    
    return ETHERVOX_SUCCESS;
#endif
}

ethervox_result_t ethervox_kv_cache_load(
    struct ethervox_governor* governor,
    const char* cache_path
) {
    if (!governor || !cache_path) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !LLAMA_AVAILABLE
    ETHERVOX_LOG_ERROR("KV cache load not available: llama.cpp not linked");
    return ETHERVOX_ERROR_NOT_IMPLEMENTED;
#else
    
    ETHERVOX_LOG_INFO("Loading KV cache from: %s", cache_path);
    
    // Check for old (non-sequence) format and delete if found
    // Magic number for old format: "ggsn" (0x6767736e)
    FILE* check_file = fopen(cache_path, "rb");
    if (check_file) {
        uint32_t magic = 0;
        if (fread(&magic, sizeof(magic), 1, check_file) == 1) {
            if (magic == 0x6767736e) {  // Old general state format
                fclose(check_file);
                ETHERVOX_LOG_WARN("Detected old (non-sequence) cache format - deleting for regeneration");
                unlink(cache_path);
                return ETHERVOX_ERROR_FILE_READ;  // Trigger regeneration
            }
        }
        fclose(check_file);
    }
    
    // Get llama context
    struct llama_context* ctx = ethervox_governor_get_context(governor);
    if (!ctx) {
        ETHERVOX_LOG_ERROR("No llama context available");
        return ETHERVOX_ERROR_NOT_INITIALIZED;
    }
    
    // Allocate buffer for tokens (max 8192 from context size)
    const size_t max_tokens = 8192;
    llama_token* tokens = (llama_token*)malloc(max_tokens * sizeof(llama_token));
    if (!tokens) {
        ETHERVOX_LOG_ERROR("Failed to allocate token buffer");
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    // Use llama.cpp's sequence-based state load function
    // Load into sequence 1 where system prompt belongs (permanent master)
    size_t n_tokens_loaded = 0;
    size_t bytes_read = llama_state_seq_load_file(
        ctx, 
        cache_path,
        1,  // Load into sequence 1 (system prompt permanent location)
        tokens, 
        max_tokens, 
        &n_tokens_loaded
    );
    
    if (bytes_read == 0) {
        ETHERVOX_LOG_ERROR("llama_state_seq_load_file() failed for sequence 1");
        ETHERVOX_LOG_WARN("Deleting incompatible cache file");
        unlink(cache_path);  // Delete incompatible cache
        free(tokens);
        return ETHERVOX_ERROR_FILE_READ;
    }
    
    // Validate loaded token count
    if (n_tokens_loaded == 0 || n_tokens_loaded < 100) {
        ETHERVOX_LOG_ERROR("Invalid token count loaded: %zu (expected >= 100)", n_tokens_loaded);
        
        // CRITICAL: Clear sequence 1 to avoid leaving it in a corrupt state
        llama_memory_t mem = llama_get_memory(ctx);
        llama_memory_seq_rm(mem, 1, -1, -1);  // Clear all of sequence 1
        ETHERVOX_LOG_WARN("Cleared corrupt sequence 1 from partial cache load");
        
        // Delete the invalid cache file
        unlink(cache_path);
        ETHERVOX_LOG_WARN("Deleted invalid cache file (will regenerate)");
        
        free(tokens);
        return ETHERVOX_ERROR_FILE_READ;
    }
    
    // Store loaded tokens in governor for recovery/reset
    ethervox_governor_set_system_tokens(governor, tokens, (int)n_tokens_loaded);
    
    ETHERVOX_LOG_INFO("✓ KV cache loaded instantly: %zu tokens + KV state (seq 1 permanent) ready (%zu bytes)", 
                     n_tokens_loaded, bytes_read);
    
    return ETHERVOX_SUCCESS;
#endif
}

ethervox_result_t ethervox_kv_cache_get_path(
    const char* model_path,
    const char* files_dir,
    char* output,
    size_t output_size
) {
    if (!model_path || !files_dir || !output || output_size == 0) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Extract model name
    char model_name[128];
    extract_model_name(model_path, model_name, sizeof(model_name));
    
    // Build path: <files_dir>/cache/system_prompt_{model}.kvcache
    int written = snprintf(output, output_size, 
                          "%s/cache/system_prompt_%s.kvcache",
                          files_dir, model_name);
    
    if (written < 0 || (size_t)written >= output_size) {
        return ETHERVOX_ERROR_BUFFER_TOO_SMALL;
    }
    
    return ETHERVOX_SUCCESS;
}

// Accessor functions (to be added to governor.c)
#if LLAMA_AVAILABLE
// These need to be implemented in governor.c to access private fields
__attribute__((weak))
llama_token* ethervox_governor_get_system_tokens(struct ethervox_governor* gov, int* len) {
    // Implemented in governor.c
    (void)gov; (void)len;
    return NULL;
}

__attribute__((weak))
struct llama_context* ethervox_governor_get_context(struct ethervox_governor* gov) {
    // Implemented in governor.c
    (void)gov;
    return NULL;
}

__attribute__((weak))
void ethervox_governor_set_system_tokens(struct ethervox_governor* gov, 
                                        llama_token* tokens, int len) {
    // Implemented in governor.c
    (void)gov; (void)tokens; (void)len;
}

__attribute__((weak))
const char* ethervox_governor_get_model_path(struct ethervox_governor* gov) {
    // Implemented in governor.c
    (void)gov;
    return "";
}
#endif
