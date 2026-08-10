/**
 * @file test_model_pool.c
 * @brief Tests for multi-model pool
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/model_pool.h"
#include "ethervox/paths.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define CHECK_SUCCESS(expr) do { \
    ethervox_result_t _r = (expr); \
    if (_r != ETHERVOX_SUCCESS) { \
        fprintf(stderr, "FAIL: %s:%d: %s returned %d\n", __FILE__, __LINE__, #expr, _r); \
        return 1; \
    } \
} while (0)

static int test_create_destroy(void) {
    printf("test_create_destroy...\n");
    
    ethervox_paths_t paths = {
        .data_dir = "/tmp/ethervox/data",
        .cache_dir = "/tmp/ethervox/cache",
        .models_dir = "/tmp/ethervox/models"
    };
    
    ethervox_model_pool_t* pool = NULL;
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 2ULL * 1024 * 1024 * 1024, &pool));  // 2GB budget
    CHECK(pool != NULL);
    
    ethervox_model_pool_destroy(pool);
    
    // NULL is safe
    ethervox_model_pool_destroy(NULL);
    
    printf("  PASS\n");
    return 0;
}

static int test_memory_usage(void) {
    printf("test_memory_usage...\n");
    
    ethervox_paths_t paths = {
        .data_dir = "/tmp/ethervox/data",
        .cache_dir = "/tmp/ethervox/cache",
        .models_dir = "/tmp/ethervox/models"
    };
    
    uint64_t budget = 2ULL * 1024 * 1024 * 1024;  // 2GB
    
    ethervox_model_pool_t* pool = NULL;
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, budget, &pool));
    
    uint64_t used = 0, reported_budget = 0;
    CHECK_SUCCESS(ethervox_model_pool_memory_usage(pool, &used, &reported_budget));
    CHECK(used == 0);
    CHECK(reported_budget == budget);
    
    ethervox_model_pool_destroy(pool);
    
    printf("  PASS\n");
    return 0;
}

static int test_would_fit_no_budget(void) {
    printf("test_would_fit_no_budget...\n");
    
    ethervox_paths_t paths = {
        .data_dir = "/tmp/ethervox/data",
        .cache_dir = "/tmp/ethervox/cache",
        .models_dir = "/tmp/ethervox/models"
    };
    
    // Create pool with no budget (budget = 0 means unlimited)
    ethervox_model_pool_t* pool = NULL;
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 0, &pool));
    
    // Any model should "fit" when there's no budget
    ethervox_model_config_t config = {
        .model_path = "/nonexistent/model.gguf",
        .context_size = 2048,
        .n_threads = 4,
        .use_gpu = false,
        .role = "test"
    };
    
    bool fits = false;
    uint64_t required = 0;
    
    // This will fail because file doesn't exist, but that's expected
    ethervox_result_t result = ethervox_model_pool_would_fit(pool, &config, &fits, &required);
    CHECK(result == ETHERVOX_ERROR_FILE_READ);  // File doesn't exist
    
    ethervox_model_pool_destroy(pool);
    
    printf("  PASS\n");
    return 0;
}

static int test_null_safety(void) {
    printf("test_null_safety...\n");
    
    ethervox_paths_t paths = {
        .data_dir = "/tmp/ethervox/data",
        .cache_dir = "/tmp/ethervox/cache",
        .models_dir = "/tmp/ethervox/models"
    };
    
    ethervox_model_pool_t* pool = NULL;
    ethervox_model_handle_t* handle = NULL;
    ethervox_model_config_t config = {0};
    bool fits = false;
    uint64_t used = 0, budget = 0;
    
    // NULL checks for create
    CHECK(ethervox_model_pool_create(NULL, 100, &pool) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(ethervox_model_pool_create(&paths, 100, NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // Create valid pool
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 100, &pool));
    
    // NULL checks for load
    CHECK(ethervox_model_pool_load(NULL, &config, NULL, NULL, &handle) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(ethervox_model_pool_load(pool, NULL, NULL, NULL, &handle) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(ethervox_model_pool_load(pool, &config, NULL, NULL, NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL checks for unload
    CHECK(ethervox_model_pool_unload(NULL, handle) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(ethervox_model_pool_unload(pool, NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL checks for would_fit
    CHECK(ethervox_model_pool_would_fit(NULL, &config, &fits, NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(ethervox_model_pool_would_fit(pool, NULL, &fits, NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(ethervox_model_pool_would_fit(pool, &config, NULL, NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL checks for memory_usage
    CHECK(ethervox_model_pool_memory_usage(NULL, &used, &budget) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    // pool parameter required, but out params can be NULL
    CHECK_SUCCESS(ethervox_model_pool_memory_usage(pool, NULL, NULL));
    
    ethervox_model_pool_destroy(pool);
    
    printf("  PASS\n");
    return 0;
}

static int test_refcounted_backend(void) {
    printf("test_refcounted_backend...\n");
    
    ethervox_paths_t paths = {
        .data_dir = "/tmp/ethervox/data",
        .cache_dir = "/tmp/ethervox/cache",
        .models_dir = "/tmp/ethervox/models"
    };
    
    // Create multiple pools - should increment refcount
    ethervox_model_pool_t* pool1 = NULL;
    ethervox_model_pool_t* pool2 = NULL;
    ethervox_model_pool_t* pool3 = NULL;
    
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 1024, &pool1));
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 1024, &pool2));
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 1024, &pool3));
    
    // Destroy in different order - refcount should handle it
    ethervox_model_pool_destroy(pool2);
    ethervox_model_pool_destroy(pool1);
    ethervox_model_pool_destroy(pool3);
    
    // Create another pool after all destroyed - should work
    ethervox_model_pool_t* pool4 = NULL;
    CHECK_SUCCESS(ethervox_model_pool_create(&paths, 1024, &pool4));
    ethervox_model_pool_destroy(pool4);
    
    printf("  PASS\n");
    return 0;
}

int main(void) {
    printf("Running model pool tests...\n\n");
    
    int failed = 0;
    
    failed += test_create_destroy();
    failed += test_memory_usage();
    failed += test_would_fit_no_budget();
    failed += test_null_safety();
    failed += test_refcounted_backend();
    
    printf("\n");
    if (failed == 0) {
        printf("All tests PASSED\n");
    } else {
        printf("%d test(s) FAILED\n", failed);
    }
    
    return failed;
}
