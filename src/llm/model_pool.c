/**
 * @file model_pool.c
 * @brief Multi-model pool implementation
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/model_pool.h"
#include "ethervox/logging.h"
#include "ethervox/platform_thread.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#include <llama.h>
#define LLAMA_AVAILABLE 1
#else
#define LLAMA_AVAILABLE 0
#endif

#if defined(MTMD_AVAILABLE) && MTMD_AVAILABLE
#include "mtmd.h"
#endif

// Convenience macros for mutex operations (ignore return values for now)
#define MUTEX_INIT(m) ethervox_mutex_init(&(m))
#define MUTEX_DESTROY(m) ethervox_mutex_destroy(&(m))
#define MUTEX_LOCK(m) ethervox_mutex_lock(&(m))
#define MUTEX_UNLOCK(m) ethervox_mutex_unlock(&(m))

// Memory estimation constants (bytes per token for KV cache)
#define KV_BYTES_PER_TOKEN_F16 2048  // FP16 KV cache, typical for 7-8B models
#define OVERHEAD_BYTES 128 * 1024 * 1024  // 128MB overhead per model

// Global backend state (refcounted)
static struct {
    ethervox_mutex_t mutex;
    int refcount;
    bool initialized;
} g_backend = {
    .mutex = ETHERVOX_MUTEX_INITIALIZER,
    .refcount = 0,
    .initialized = false
};

/**
 * Model handle structure
 */
struct ethervox_model_handle {
    struct llama_model* model;
    struct llama_context* ctx;
    void* mtmd_ctx;              // mtmd_context* for multimodal support (NULL if not supported)
    ethervox_mutex_t inference_mutex;     // Serializes inference on this model
    uint64_t memory_bytes;       // Actual memory used
    char role[64];               // Model role (main, vision, embed)
    
    // C3.4: Memory pressure and LRU eviction
    ethervox_residency_class_t residency;  // RESIDENT or ON_DEMAND
    time_t last_use;             // Last access timestamp (LRU tracking)
    time_t loaded_at;            // Load timestamp (for TTL calculation)
    int refcount;                // Reference count (active generations)
    uint32_t ttl_seconds;        // TTL for on-demand models (0 = no expiry)
    
    struct ethervox_model_handle* next;  // Linked list
};

/**
 * Model pool structure
 */
struct ethervox_model_pool {
    ethervox_paths_t paths;
    uint64_t budget_bytes;
    uint64_t used_bytes;
    ethervox_mutex_t pool_mutex;  // Protects pool state
    ethervox_model_handle_t* models;  // Linked list of loaded models
    
    // C3.4: Memory pressure and LRU eviction
    ethervox_memory_pressure_cb pressure_callback;  // OS pressure handler
    void* pressure_user_data;                       // User data for callback
    uint32_t max_on_demand;                         // Max concurrent on-demand (0 = no limit)
    uint32_t current_on_demand;                     // Current on-demand count
};

/**
 * Initialize global llama backend (refcounted)
 */
static ethervox_result_t backend_init(void) {
    MUTEX_LOCK(g_backend.mutex);
    
    if (!g_backend.initialized) {
#if LLAMA_AVAILABLE
        llama_backend_init();
        ggml_backend_load_all();
        if (ggml_backend_reg_count() == 0) {
            ETHERVOX_LOG_ERROR("[ModelPool] No ggml backends loaded, cannot load models");
            MUTEX_UNLOCK(g_backend.mutex);
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        g_backend.initialized = true;
        ETHERVOX_LOG_INFO("[ModelPool] Initialized llama backend (%d ggml backends available)", 
                         (int)ggml_backend_reg_count());
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
    
    // Global backend mutex is statically initialized, safe to call backend_init()
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
    
    // C3.4: Retry-once logic - if doesn't fit, try evicting and retry
    if (!fits && config->residency == ETHERVOX_RESIDENCY_ON_DEMAND) {
        ETHERVOX_LOG_INFO("[ModelPool] Model doesn't fit, attempting LRU eviction (need %llu MB)",
                         (unsigned long long)(required_bytes / (1024 * 1024)));
        
        // Try to free enough space for this model
        uint64_t bytes_needed = required_bytes - (pool->budget_bytes - pool->used_bytes);
        uint64_t freed = 0;
        
        // Protect the role we're trying to load (don't evict same role)
        const char* protected[2] = { config->role, NULL };
        result = ethervox_model_pool_evict_lru(pool, bytes_needed, protected, &freed);
        
        if (result == ETHERVOX_SUCCESS && freed > 0) {
            ETHERVOX_LOG_INFO("[ModelPool] Evicted %llu MB, retrying load",
                             (unsigned long long)(freed / (1024 * 1024)));
            
            // Retry would_fit check
            result = ethervox_model_pool_would_fit(pool, config, &fits, &required_bytes);
            if (result != ETHERVOX_SUCCESS) {
                return result;
            }
        }
    }
    
    if (!fits) {
        ETHERVOX_LOG_ERROR("[ModelPool] Model requires %llu MB but only %llu MB available (after eviction attempt)",
                          (unsigned long long)(required_bytes / (1024 * 1024)),
                          (unsigned long long)((pool->budget_bytes - pool->used_bytes) / (1024 * 1024)));
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    // C3.4: Check max_on_demand limit
    if (config->residency == ETHERVOX_RESIDENCY_ON_DEMAND && pool->max_on_demand > 0) {
        MUTEX_LOCK(pool->pool_mutex);
        uint32_t current = pool->current_on_demand;
        MUTEX_UNLOCK(pool->pool_mutex);
        
        if (current >= pool->max_on_demand) {
            ETHERVOX_LOG_INFO("[ModelPool] Max on-demand limit reached (%u), evicting LRU", pool->max_on_demand);
            
            // Evict one on-demand model (protect the role we're loading)
            const char* protected[2] = { config->role, NULL };
            uint64_t freed = 0;
            result = ethervox_model_pool_evict_lru(pool, 0, protected, &freed);
            
            if (result != ETHERVOX_SUCCESS) {
                ETHERVOX_LOG_ERROR("[ModelPool] Failed to evict for max_on_demand enforcement");
                return ETHERVOX_ERROR_OUT_OF_MEMORY;
            }
        }
    }
    
    ETHERVOX_LOG_INFO("[ModelPool] Loading model: %s (role=%s, ctx=%u)",
                     config->model_path, config->role ? config->role : "none", config->context_size);
    
    // Load model
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config->use_gpu ? 99 : 0;  // Use GPU if requested
    
    struct llama_model* model = llama_model_load_from_file(config->model_path, model_params);
    if (!model) {
        ETHERVOX_LOG_ERROR("[ModelPool] Failed to load model: %s", config->model_path);
        return ETHERVOX_ERROR_FILE_READ;
    }
    
    // Create context
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config->context_size;
    ctx_params.n_threads = config->n_threads;
    ctx_params.n_threads_batch = config->n_threads;
    ctx_params.n_seq_max = config->n_seq_max > 0 ? config->n_seq_max : 1;
    ctx_params.kv_unified = config->kv_unified;
    
    struct llama_context* ctx = llama_init_from_model(model, ctx_params);
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
    
    // C3.4: Initialize LRU and residency fields
    time_t now = time(NULL);
    handle->residency = config->residency;
    handle->last_use = now;
    handle->loaded_at = now;
    handle->refcount = 0;
    handle->ttl_seconds = (config->ttl_seconds > 0) ? config->ttl_seconds : 90;  // Default 90s
    
    // Initialize mutex BEFORE any error paths that might destroy it
    MUTEX_INIT(handle->inference_mutex);
    
    // Load mmproj if provided (for multimodal support)
#if defined(MTMD_AVAILABLE) && MTMD_AVAILABLE
    if (config->mmproj_path) {
        ETHERVOX_LOG_INFO("[ModelPool] About to load mmproj: %s", config->mmproj_path);
        ETHERVOX_LOG_INFO("[ModelPool] Model pointer: %p, Context pointer: %p", (void*)model, (void*)ctx);
        
        struct mtmd_context_params mtmd_params = mtmd_context_params_default();
        mtmd_params.use_gpu = config->use_gpu;
        mtmd_params.print_timings = false;
        mtmd_params.n_threads = (int)config->n_threads;
        // mtmd_tokenize() splits prompts on this marker; NULL keeps mtmd's own
        // default ("<__media__>"). Callers whose prompts embed a different
        // literal marker (e.g. Granite Speech's "<|audio|>") must set it.
        if (config->media_marker) {
            mtmd_params.media_marker = config->media_marker;
        }
        
        ETHERVOX_LOG_INFO("[ModelPool] Calling mtmd_init_from_file...");
        void* mctx = mtmd_init_from_file(config->mmproj_path, model, mtmd_params);
        ETHERVOX_LOG_INFO("[ModelPool] mtmd_init_from_file returned: %p", mctx);
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
    
    // Add to pool
    MUTEX_LOCK(pool->pool_mutex);
    handle->next = pool->models;
    pool->models = handle;
    pool->used_bytes += required_bytes;
    
    // C3.4: Update on-demand counter
    if (handle->residency == ETHERVOX_RESIDENCY_ON_DEMAND) {
        pool->current_on_demand++;
    }
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
        
        // C3.4: Update on-demand counter
        if (handle->residency == ETHERVOX_RESIDENCY_ON_DEMAND) {
            pool->current_on_demand--;
        }
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

struct llama_model* ethervox_model_handle_get_model(
    ethervox_model_handle_t* handle
) {
    return handle ? handle->model : NULL;
}

struct llama_context* ethervox_model_handle_get_context(
    ethervox_model_handle_t* handle
) {
    return handle ? handle->ctx : NULL;
}

// N6.3: mtmd getter for multimodal models
struct mtmd_context* ethervox_model_handle_get_mtmd(
    ethervox_model_handle_t* handle
) {
    return handle ? handle->mtmd_ctx : NULL;
}

// ============================================================================
// C3.4: Memory Pressure and LRU Eviction
// ============================================================================

ethervox_result_t ethervox_model_pool_set_pressure_callback(
    ethervox_model_pool_t* pool,
    ethervox_memory_pressure_cb callback,
    void* user_data
) {
    if (!pool) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    MUTEX_LOCK(pool->pool_mutex);
    pool->pressure_callback = callback;
    pool->pressure_user_data = user_data;
    MUTEX_UNLOCK(pool->pool_mutex);
    
    ETHERVOX_LOG_INFO("[ModelPool] Memory pressure callback %s",
                     callback ? "registered" : "unregistered");
    return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_model_pool_set_max_on_demand(
    ethervox_model_pool_t* pool,
    uint32_t max_concurrent
) {
    if (!pool) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    MUTEX_LOCK(pool->pool_mutex);
    pool->max_on_demand = max_concurrent;
    MUTEX_UNLOCK(pool->pool_mutex);
    
    ETHERVOX_LOG_INFO("[ModelPool] Max concurrent on-demand models: %u %s",
                     max_concurrent, max_concurrent == 0 ? "(no limit)" : "");
    return ETHERVOX_SUCCESS;
}

/**
 * Helper: Check if role is protected
 */
static bool is_role_protected(const char* role, const char** protected_roles) {
    if (!protected_roles || !role) {
        return false;
    }
    
    for (int i = 0; protected_roles[i] != NULL; i++) {
        if (strcmp(role, protected_roles[i]) == 0) {
            return true;
        }
    }
    return false;
}

ethervox_result_t ethervox_model_pool_evict_lru(
    ethervox_model_pool_t* pool,
    uint64_t bytes_to_free,
    const char** protected_roles,
    uint64_t* out_freed
) {
    if (!pool) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    MUTEX_LOCK(pool->pool_mutex);
    
    uint64_t freed = 0;
    bool continue_evicting = true;
    
    while (continue_evicting) {
        // Find LRU on-demand model that can be evicted
        ethervox_model_handle_t* lru_candidate = NULL;
        time_t oldest_use = 0;
        
        for (ethervox_model_handle_t* h = pool->models; h != NULL; h = h->next) {
            // Skip resident models
            if (h->residency != ETHERVOX_RESIDENCY_ON_DEMAND) {
                continue;
            }
            
            // Skip models with active references (in-flight generation)
            if (h->refcount > 0) {
                continue;
            }
            
            // Skip protected roles
            if (is_role_protected(h->role, protected_roles)) {
                continue;
            }
            
            // Track oldest (LRU)
            if (lru_candidate == NULL || h->last_use < oldest_use) {
                lru_candidate = h;
                oldest_use = h->last_use;
            }
        }
        
        // No more evictable models
        if (lru_candidate == NULL) {
            ETHERVOX_LOG_INFO("[ModelPool] LRU eviction: no more evictable models (freed %llu MB)",
                             (unsigned long long)(freed / (1024 * 1024)));
            break;
        }
        
        // Evict this model
        ETHERVOX_LOG_INFO("[ModelPool] Evicting LRU model: role=%s, last_use=%ld seconds ago",
                         lru_candidate->role,
                         (long)(time(NULL) - lru_candidate->last_use));
        
        uint64_t model_bytes = lru_candidate->memory_bytes;
        freed += model_bytes;
        
        // Unlock before calling unload (unload acquires the lock)
        MUTEX_UNLOCK(pool->pool_mutex);
        ethervox_result_t result = ethervox_model_pool_unload(pool, lru_candidate);
        MUTEX_LOCK(pool->pool_mutex);
        
        if (result != ETHERVOX_SUCCESS) {
            ETHERVOX_LOG_ERROR("[ModelPool] Failed to unload LRU model");
            break;
        }
        
        // Check if we've freed enough (if target was specified)
        if (bytes_to_free > 0 && freed >= bytes_to_free) {
            ETHERVOX_LOG_INFO("[ModelPool] LRU eviction target met: freed %llu MB",
                             (unsigned long long)(freed / (1024 * 1024)));
            continue_evicting = false;
        }
    }
    
    MUTEX_UNLOCK(pool->pool_mutex);
    
    if (out_freed) {
        *out_freed = freed;
    }
    
    return ETHERVOX_SUCCESS;
}
