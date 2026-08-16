/**
 * @file structured_generation.h
 * @brief Grammar-constrained generation with calibrated confidence scoring
 *
 * Provides structured generation (JSON output) with confidence metrics based on
 * log-probabilities. Confidence excludes grammar-forced structural tokens to
 * measure only the model's content choices.
 *
 * Copyright (c) 2024-2026 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#ifndef ETHERVOX_STRUCTURED_GENERATION_H
#define ETHERVOX_STRUCTURED_GENERATION_H

#include "ethervox/error.h"
#include "ethervox/event_stream.h"
#include "ethervox/grammar.h"
#include "ethervox/model_pool.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generation parameters for structured output
 * 
 * Determinism boundary (C4.2):
 * Given the same seed, same prompt, same model, and same thread count, generation
 * produces IDENTICAL byte-for-byte output. This holds for:
 * - Fixed build configuration (same llama.cpp version, same compile flags)
 * - Same backend (CPU-only, Metal, CUDA, etc.) - switching backends breaks determinism
 * - Same thread count (n_threads) - different thread counts may reorder floating-point ops
 * 
 * Determinism does NOT hold across:
 * - Different llama.cpp versions (internal sampling may change)
 * - Different backends (Metal vs CPU may use different math)
 * - Different thread counts (parallel reductions may sum in different orders)
 * - Different hardware with different SIMD instructions
 * 
 * For reproducible evals and golden tests: pin build, backend, threads, and seed.
 * Use seed=0 for true randomness (seeded from system entropy).
 */
typedef struct {
    uint32_t max_tokens;        /**< Maximum tokens to generate (default: 512) */
    float temperature;          /**< Sampling temperature (default: 0.7) */
    float top_p;                /**< Nucleus sampling parameter (default: 0.9) */
    uint32_t seed;              /**< Random seed (0 = random, non-zero = deterministic) */
    bool include_logprobs;      /**< Fire ETHERVOX_EVENT_LOGPROB events (default: false) */
} ethervox_structured_gen_params_t;

/**
 * Generate structured output with calibrated confidence
 *
 * Performs grammar-constrained generation and calculates a confidence score
 * based on the geometric mean of content token probabilities, excluding
 * grammar-forced structural tokens (e.g., JSON braces, commas where required).
 *
 * Confidence calculation:
 * - For each generated token, get its log-probability
 * - Determine if token was grammar-forced (only 1 valid candidate)
 * - Exclude forced tokens from confidence calculation
 * - confidence = exp(mean(log_probs of non-forced tokens))
 * - Range: [0.0, 1.0], where 0.0 = no confidence, 1.0 = certain
 *
 * Edge cases:
 * - All tokens grammar-forced → confidence = 0.0
 * - Empty output → confidence = 0.0
 * - Very low confidence (<0.01) → clamped to 0.0
 *
 * @param handle Model handle from model pool
 * @param prompt Input prompt text
 * @param grammar Grammar constraint (from ethervox_grammar_compile or _from_json_schema)
 * @param params Generation parameters (NULL = defaults)
 * @param event_callback Optional event callback for streaming (NULL = no events)
 * @param user_data User data passed to event callback
 * @param out_json Output JSON string (caller must free with free())
 * @param out_confidence Output confidence score [0.0, 1.0] (nullable if not needed)
 * @return ETHERVOX_SUCCESS or error code
 *
 * @note Thread-safety: Serialized via model handle's inference mutex
 * @note Grammar must remain valid for the duration of the call
 * @note Fires ETHERVOX_EVENT_LOGPROB if params->include_logprobs == true
 * @note Fires ETHERVOX_EVENT_TOKEN, ETHERVOX_EVENT_FINISHED, ETHERVOX_EVENT_ERROR as appropriate
 *
 * @example
 * ethervox_grammar_t* grammar = NULL;
 * ethervox_grammar_from_json_schema("{\"type\": \"object\", ...}", &grammar);
 * 
 * char* json_output = NULL;
 * float confidence = 0.0f;
 * 
 * ethervox_structured_gen_params_t params = {
 *     .max_tokens = 256,
 *     .temperature = 0.7f,
 *     .include_logprobs = true
 * };
 * 
 * ethervox_result_t result = ethervox_generate_structured(
 *     model_handle,
 *     "Extract person name from: John Smith, age 30",
 *     grammar,
 *     &params,
 *     my_event_callback,
 *     user_data,
 *     &json_output,
 *     &confidence
 * );
 * 
 * if (result == ETHERVOX_SUCCESS) {
 *     printf("JSON: %s (confidence: %.2f)\n", json_output, confidence);
 *     free(json_output);
 * }
 * 
 * ethervox_grammar_free(grammar);
 */
ethervox_result_t ethervox_generate_structured(
    ethervox_model_handle_t* handle,
    const char* prompt,
    ethervox_grammar_t* grammar,
    const ethervox_structured_gen_params_t* params,
    ethervox_event_cb event_callback,
    void* user_data,
    char** out_json,
    float* out_confidence
);

/**
 * Get default generation parameters
 *
 * @return Default parameters struct
 */
ethervox_structured_gen_params_t ethervox_structured_gen_params_default(void);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_STRUCTURED_GENERATION_H
