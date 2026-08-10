/**
 * @file model_pool.h
 * @brief Multi-model pool for concurrent model loading and execution
 *
 * Manages multiple loaded models with memory budget enforcement. Models can run
 * concurrently with per-model mutex serialization. Provides accurate memory estimation
 * to prevent OOM kills.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#ifndef ETHERVOX_MODEL_POOL_H
#define ETHERVOX_MODEL_POOL_H

#include "ethervox/error.h"
#include "ethervox/paths.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle to a model pool
 */
typedef struct ethervox_model_pool ethervox_model_pool_t;

/**
 * Opaque handle to a loaded model within a pool
 */
typedef struct ethervox_model_handle ethervox_model_handle_t;

/**
 * Progress callback for model loading
 * 
 * @param progress Value between 0.0 and 1.0
 * @param user_data User-provided context
 */
typedef void (*ethervox_progress_cb)(float progress, void* user_data);

/**
 * Model configuration for loading
 */
typedef struct {
    const char* model_path;        /**< Path to GGUF model file */
    uint32_t context_size;         /**< Context size in tokens */
    uint32_t n_threads;            /**< Number of threads for inference */
    bool use_gpu;                  /**< Whether to use GPU acceleration */
    const char* role;              /**< Model role (e.g., "main", "vision", "embed") */
} ethervox_model_config_t;

/**
 * Create a model pool with a memory budget
 * 
 * The pool manages loaded models and enforces the memory budget. Models that would
 * exceed the budget are rejected at load time.
 * 
 * @param paths Path configuration
 * @param budget_bytes Maximum memory budget in bytes (0 = no limit)
 * @param out Receives the created pool (caller must destroy)
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_model_pool_create(
    const ethervox_paths_t* paths,
    uint64_t budget_bytes,
    ethervox_model_pool_t** out
);

/**
 * Destroy a model pool
 * 
 * Unloads all models and frees resources. Any outstanding model handles become invalid.
 * 
 * @param pool Pool to destroy (NULL is safe)
 */
void ethervox_model_pool_destroy(ethervox_model_pool_t* pool);

/**
 * Load a model into the pool
 * 
 * Loads the specified model if it fits within the memory budget. The model is assigned
 * a unique handle for subsequent operations. Multiple models can be loaded and run
 * concurrently.
 * 
 * Thread-safe: Can be called concurrently from multiple threads.
 * 
 * @param pool Model pool
 * @param config Model configuration
 * @param progress_cb Optional progress callback
 * @param user_data User data for progress callback
 * @param out Receives model handle (caller must unload)
 * @return ETHERVOX_SUCCESS, ETHERVOX_ERROR_OUT_OF_MEMORY if budget exceeded, or error code
 */
ethervox_result_t ethervox_model_pool_load(
    ethervox_model_pool_t* pool,
    const ethervox_model_config_t* config,
    ethervox_progress_cb progress_cb,
    void* user_data,
    ethervox_model_handle_t** out
);

/**
 * Unload a model from the pool
 * 
 * Frees the model and its resources. The handle becomes invalid after this call.
 * 
 * Thread-safe: Can be called concurrently from multiple threads.
 * 
 * @param pool Model pool
 * @param handle Model handle (NULL is safe)
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_model_pool_unload(
    ethervox_model_pool_t* pool,
    ethervox_model_handle_t* handle
);

/**
 * Check if a model would fit in the pool
 * 
 * Estimates memory requirements without loading. Calculation includes:
 * - Model file size
 * - KV cache (context_size × bytes_per_token)
 * - Per-architecture overhead constant
 * 
 * Must be accurate within ±10% to prevent OOM kills.
 * 
 * @param pool Model pool
 * @param config Model configuration to check
 * @param out_fits Receives true if model would fit, false otherwise
 * @param out_required Receives estimated bytes required (can be NULL)
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_model_pool_would_fit(
    const ethervox_model_pool_t* pool,
    const ethervox_model_config_t* config,
    bool* out_fits,
    uint64_t* out_required
);

/**
 * Get current memory usage and budget
 * 
 * Reports current memory consumption across all loaded models.
 * 
 * @param pool Model pool
 * @param out_used Receives bytes currently used (can be NULL)
 * @param out_budget Receives total budget in bytes (can be NULL)
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_model_pool_memory_usage(
    const ethervox_model_pool_t* pool,
    uint64_t* out_used,
    uint64_t* out_budget
);

#ifdef __cplusplus
}
#endif

#endif /* ETHERVOX_MODEL_POOL_H */
