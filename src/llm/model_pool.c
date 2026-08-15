/**
 * @file model_pool.c
 * @brief Multi-model pool implementation
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/model_pool.h"
#include "ethervox/logging.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#include <llama.h>
#define LLAMA_AVAILABLE 1
#else
#define LLAMA_AVAILABLE 0
#endif

#if defined(MTMD_AVAILABLE) && MTMD_AVAILABLE
#include "mtmd.h"
#endif

// Platform-specific threading
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION mutex_t;
#define MUTEX_INIT(m) InitializeCriticalSection(&(m))
#define MUTEX_DESTROY(m) DeleteCriticalSection(&(m))
#define MUTEX_LOCK(m) EnterCriticalSection(&(m))
#define MUTEX_UNLOCK(m) LeaveCriticalSection(&(m))
#else
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define MUTEX_INIT(m) pthread_mutex_init(&(m), NULL)
#define MUTEX_DESTROY(m) pthread_mutex_destroy(&(m))
#define MUTEX_LOCK(m) pthread_mutex_lock(&(m))
#define MUTEX_UNLOCK(m) pthread_mutex_unlock(&(m))
#endif

// Memory estimation constants (bytes per token for KV cache)
#define KV_BYTES_PER_TOKEN_F16 2048  // FP16 KV cache, typical for 7-8B models
#define OVERHEAD_BYTES 128 * 1024 * 1024  // 128MB overhead per model

// Global backend state (refcounted)
static struct {
    mutex_t mutex;
    int refcount;
    bool initialized;
} g_backend = {0};

/**
 * Model handle structure
 */
struct ethervox_model_handle {
    struct llama_model* model;
    struct llama_context* ctx;
    void* mtmd_ctx;              // mtmd_context* for multimodal support (NULL if not supported)
    mutex_t inference_mutex;  // Serializes inference on this model
    uint64_t memory_bytes;    // Actual memory used
    char role[64];            // Model role (main, vision, embed)
    struct ethervox_model_handle* next;  // Linked list
};

/**
 * Model pool structure
 */
struct ethervox_model_pool {
    ethervox_paths_t paths;
    uint64_t budget_bytes;
    uint64_t used_bytes;
    mutex_t pool_mutex;  // Protects pool state
    ethervox_model_handle_t* models;  // Linked list of loaded models
};

/**
 * Initialize global llama backend (refcounted)
 */
static ethervox_result_t backend_init(void) {
    MUTEX_LOCK(g_backend.mutex);
    
    if (!g_backend.initialized) {
#if LLAMA_AVAILABLE
        llama_backend_init();
        g_backend.initialized = true;
        ETHERVOX_LOG_INFO("[ModelPool] Initialized llama backend");
#else
        MUTEX_UNLOCK(g_backend.mutex);
        return ETHERVOX_ERROR_NOT_IMPLEMENTED;
#endif
    }
    
    g_backend.refcount++;
    ETHERVOX_LOG_DEBUG("[ModelPool] Backend refcount: %d", g_backend.refcount);
    
    MUTEX_UNLOCK(g_backend.mutex);
    return ETHERVOX_SUCCESS;
}

/**
 * Cleanup global llama backend (refcounted)
 */
static void backend_cleanup(void) {
    MUTEX_LOCK(g_backend.mutex);
    
    if (g_backend.refcount > 0) {
        g_backend.refcount--;
        ETHERVOX_LOG_DEBUG("[ModelPool] Backend refcount: %d", g_backend.refcount);
        
        if (g_backend.refcount == 0 && g_backend.initialized) {
#if LLAMA_AVAILABLE
            llama_backend_free();
            g_backend.initialized = false;
            ETHERVOX_LOG_INFO("[ModelPool] Cleaned up llama backend");
#endif
        }
    }
    
    MUTEX_UNLOCK(g_backend.mutex);
}

/**
 * Estimate memory requirements for a model
 */
static ethervox_result_t estimate_memory(
    const ethervox_model_config_t* config,
    uint64_t* out_bytes
) {
    if (!config || !out_bytes) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Get model file size
    struct stat st;
    if (stat(config->model_path, &st) != 0) {
        ETHERVOX_LOG_ERROR("[ModelPool] Cannot stat model file: %s", config->model_path);
        return ETHERVOX_ERROR_FILE_READ;
    }
    
    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t kv_cache_size = config->context_size * KV_BYTES_PER_TOKEN_F16;
    
    *out_bytes = file_size + kv_cache_size + OVERHEAD_BYTES;
    
    ETHERVOX_LOG_DEBUG("[ModelPool] Memory estimate: file=%llu KB, kv=%llu KB, overhead=%llu KB, total=%llu KB",
                      (unsigned long long)(file_size / 1024),
                      (unsigned long long)(kv_cache_size / 1024),
                      (unsigned long long)(OVERHEAD_BYTES / 1024),
                      (unsigned long long)(*out_bytes / 1024));
    
    return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_model_pool_create(
    const ethervox_paths_t* paths,
    uint64_t budget_bytes,
    ethervox_model_pool_t** out
) {
    if (!paths || !out) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Initialize global backend mutex if needed (BEFORE calling backend_init which locks it!)
    if (!g_backend.initialized && g_backend.refcount == 0) {
        MUTEX_INIT(g_backend.mutex);
    }
    
    ethervox_result_t result = backend_init();
    if (result != ETHERVOX_SUCCESS) {
        return result;
    }
    
    ethervox_model_pool_t* pool = (ethervox_model_pool_t*)calloc(1, sizeof(ethervox_model_pool_t));
    if (!pool) {
        backend_cleanup();
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    // Copy paths structure
    memcpy(&pool->paths, paths, sizeof(ethervox_paths_t));
    pool->budget_bytes = budget_bytes;
    pool->used_bytes = 0;
    pool->models = NULL;
    
    MUTEX_INIT(pool->pool_mutex);
    
    ETHERVOX_LOG_INFO("[ModelPool] Created pool with budget %llu MB",
                     (unsigned long long)(budget_bytes / (1024 * 1024)));
    
    *out = pool;
    return ETHERVOX_SUCCESS;
}

void ethervox_model_pool_destroy(ethervox_model_pool_t* pool) {
    if (!pool) {
        return;
    }
    
    ETHERVOX_LOG_INFO("[ModelPool] Destroying pool");
    
    // Unload all models
    ethervox_model_handle_t* handle = pool->models;
    while (handle) {
        ethervox_model_handle_t* next = handle->next;
        ethervox_model_pool_unload(pool, handle);
        handle = next;
    }
    
    MUTEX_DESTROY(pool->pool_mutex);
    free(pool);
    
    backend_cleanup();
}

ethervox_result_t ethervox_model_pool_load(
    ethervox_model_pool_t* pool,
    const ethervox_model_config_t* config,
    ethervox_progress_cb progress_cb,
    void* user_data,
    ethervox_model_handle_t** out
) {
    if (!pool || !config || !out) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !LLAMA_AVAILABLE
    (void)progress_cb;
    (void)user_data;
    ETHERVOX_LOG_ERROR("[ModelPool] llama.cpp not available");
    return ETHERVOX_ERROR_NOT_IMPLEMENTED;
#else
    
    // Check if model would fit
    uint64_t required_bytes = 0;
    bool fits = false;
    ethervox_result_t result = ethervox_model_pool_would_fit(pool, config, &fits, &required_bytes);
    if (result != ETHERVOX_SUCCESS) {
        return result;
    }
    
    if (!fits) {
        ETHERVOX_LOG_ERROR("[ModelPool] Model requires %llu MB but only %llu MB available",
                          (unsigned long long)(required_bytes / (1024 * 1024)),
                          (unsigned long long)((pool->budget_bytes - pool->used_bytes) / (1024 * 1024)));
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    ETHERVOX_LOG_INFO("[ModelPool] Loading model: %s (role=%s, ctx=%u)",
                     config->model_path, config->role ? config->role : "none", config->context_size);
    
    // Load model
    struct llama_model_params model_params = llama_model_default_params();
    model_params.load_mode = LLAMA_LOAD_MODE_MMAP;
    
    struct llama_model* model = llama_load_model_from_file(config->model_path, model_params);
    if (!model) {
        ETHERVOX_LOG_ERROR("[ModelPool] Failed to load model: %s", config->model_path);
        return ETHERVOX_ERROR_FILE_READ;
    }
    
    // Create context
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config->context_size;
    ctx_params.n_threads = config->n_threads;
    ctx_params.n_threads_batch = config->n_threads;
    
    struct llama_context* ctx = llama_new_context_with_model(model, ctx_params);
    if (!ctx) {
        llama_free_model(model);
        ETHERVOX_LOG_ERROR("[ModelPool] Failed to create context");
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    // Create handle
    ethervox_model_handle_t* handle = (ethervox_model_handle_t*)calloc(1, sizeof(ethervox_model_handle_t));
    if (!handle) {
        llama_free(ctx);
        llama_free_model(model);
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    handle->model = model;
    handle->ctx = ctx;
    handle->mtmd_ctx = NULL;  // Initialize to NULL
    handle->memory_bytes = required_bytes;
    if (config->role) {
        strncpy(handle->role, config->role, sizeof(handle->role) - 1);
    }
    
    // Load mmproj if provided (for multimodal support)
#if defined(MTMD_AVAILABLE) && MTMD_AVAILABLE
    if (config->mmproj_path) {
        struct mtmd_context_params mtmd_params = mtmd_context_params_default();
        mtmd_params.use_gpu = config->use_gpu;
        mtmd_params.print_timings = false;
        mtmd_params.n_threads = (int)config->n_threads;
        mtmd_params.media_marker = NULL;  // Use default from mmproj metadata
        
        void* mctx = mtmd_init_from_file(config->mmproj_path, model, mtmd_params);
        if (!mctx) {
            ETHERVOX_LOG_ERROR("[ModelPool] Failed to load mmproj: %s", config->mmproj_path);
            MUTEX_DESTROY(handle->inference_mutex);
            free(handle);
            llama_free(ctx);
            llama_free_model(model);
            return ETHERVOX_ERROR_FILE_READ;
        }
        
        handle->mtmd_ctx = mctx;
        ETHERVOX_LOG_INFO("[ModelPool] Loaded mmproj: %s (vision=%d, audio=%d)",
                         config->mmproj_path,
                         mtmd_support_vision(mctx),
                         mtmd_support_audio(mctx));
    }
#else
    if (config->mmproj_path) {
        ETHERVOX_LOG_WARN("[ModelPool] mmproj requested but MTMD not available in this build");
    }
#endif
    
    MUTEX_INIT(handle->inference_mutex);
    
    // Add to pool
    MUTEX_LOCK(pool->pool_mutex);
    handle->next = pool->models;
    pool->models = handle;
    pool->used_bytes += required_bytes;
    MUTEX_UNLOCK(pool->pool_mutex);
    
    ETHERVOX_LOG_INFO("[ModelPool] Loaded model successfully (using %llu MB / %llu MB)",
                     (unsigned long long)(pool->used_bytes / (1024 * 1024)),
                     (unsigned long long)(pool->budget_bytes / (1024 * 1024)));
    
    *out = handle;
    return ETHERVOX_SUCCESS;
#endif
}

ethervox_result_t ethervox_model_pool_unload(
    ethervox_model_pool_t* pool,
    ethervox_model_handle_t* handle
) {
    if (!pool || !handle) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    ETHERVOX_LOG_INFO("[ModelPool] Unloading model (role=%s)", handle->role[0] ? handle->role : "none");
    
    // Remove from pool
    MUTEX_LOCK(pool->pool_mutex);
    ethervox_model_handle_t** prev = &pool->models;
    while (*prev && *prev != handle) {
        prev = &(*prev)->next;
    }
    if (*prev) {
        *prev = handle->next;
        pool->used_bytes -= handle->memory_bytes;
    }
    MUTEX_UNLOCK(pool->pool_mutex);
    
    // Free resources
#if LLAMA_AVAILABLE
    // Free mtmd context if loaded
#if defined(MTMD_AVAILABLE) && MTMD_AVAILABLE
    if (handle->mtmd_ctx) {
        mtmd_free((mtmd_context*)handle->mtmd_ctx);
        handle->mtmd_ctx = NULL;
    }
#endif
    
    if (handle->ctx) {
        llama_free(handle->ctx);
    }
    if (handle->model) {
        llama_free_model(handle->model);
    }
#endif
    
    MUTEX_DESTROY(handle->inference_mutex);
    free(handle);
    
    ETHERVOX_LOG_INFO("[ModelPool] Model unloaded (using %llu MB / %llu MB)",
                     (unsigned long long)(pool->used_bytes / (1024 * 1024)),
                     (unsigned long long)(pool->budget_bytes / (1024 * 1024)));
    
    return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_model_pool_would_fit(
    const ethervox_model_pool_t* pool,
    const ethervox_model_config_t* config,
    bool* out_fits,
    uint64_t* out_required
) {
    if (!pool || !config || !out_fits) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    uint64_t required = 0;
    ethervox_result_t result = estimate_memory(config, &required);
    if (result != ETHERVOX_SUCCESS) {
        return result;
    }
    
    if (out_required) {
        *out_required = required;
    }
    
    // Check against budget (if budget is 0, no limit)
    if (pool->budget_bytes == 0) {
        *out_fits = true;
    } else {
        uint64_t available = pool->budget_bytes - pool->used_bytes;
        *out_fits = (required <= available);
    }
    
    ETHERVOX_LOG_DEBUG("[ModelPool] would_fit: required=%llu MB, available=%llu MB, fits=%d",
                      (unsigned long long)(required / (1024 * 1024)),
                      (unsigned long long)((pool->budget_bytes - pool->used_bytes) / (1024 * 1024)),
                      *out_fits);
    
    return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_model_pool_memory_usage(
    const ethervox_model_pool_t* pool,
    uint64_t* out_used,
    uint64_t* out_budget
) {
    if (!pool) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    if (out_used) {
        *out_used = pool->used_bytes;
    }
    if (out_budget) {
        *out_budget = pool->budget_bytes;
    }
    
    return ETHERVOX_SUCCESS;
}
