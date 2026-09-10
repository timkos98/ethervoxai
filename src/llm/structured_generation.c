// SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary

/**
 * @file structured_generation.c
 * @brief Implementation of grammar-constrained generation with confidence scoring
 * 
 * Task: C3.3 - Log-probs and calibrated confidence for structured generation
 */

#include "ethervox/structured_generation.h"
#include "ethervox/chat_template.h"
#include "ethervox/error.h"
#include "ethervox/grammar.h"
#include "ethervox/logging.h"
#include "llama.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Maximum response length (256 KB)
#define MAX_RESPONSE_LENGTH (256 * 1024)

// Confidence clamp threshold
#define MIN_CONFIDENCE 0.01f

/**
 * Context for tracking generation state
 */
typedef struct {
    // Input
    struct llama_model* model;
    struct llama_context* ctx;
    ethervox_grammar_t* grammar;
    const ethervox_structured_gen_params_t* params;
    ethervox_event_cb event_callback;
    void* user_data;
    
    // Output accumulation
    char* output_buffer;
    size_t output_len;
    size_t output_capacity;
    
    // Confidence tracking
    double log_prob_sum;
    int content_token_count;    // Tokens that were NOT grammar-forced
    int total_token_count;
    
    // Sampling state
    struct llama_sampler* sampler;
    const struct llama_vocab* vocab;
} generation_context_t;

/**
 * Initialize generation context
 */
static ethervox_result_t init_generation_context(
    generation_context_t* gen_ctx,
    ethervox_model_handle_t* handle,
    ethervox_grammar_t* grammar,
    const ethervox_structured_gen_params_t* params,
    ethervox_event_cb event_callback,
    void* user_data
) {
    memset(gen_ctx, 0, sizeof(*gen_ctx));
    
    gen_ctx->model = ethervox_model_handle_get_model(handle);
    gen_ctx->ctx = ethervox_model_handle_get_context(handle);
    gen_ctx->grammar = grammar;
    gen_ctx->params = params;
    gen_ctx->event_callback = event_callback;
    gen_ctx->user_data = user_data;
    
    if (!gen_ctx->model || !gen_ctx->ctx) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Allocate output buffer
    gen_ctx->output_capacity = 16384;  // Start with 16 KB
    gen_ctx->output_buffer = malloc(gen_ctx->output_capacity);
    if (!gen_ctx->output_buffer) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    gen_ctx->output_buffer[0] = '\0';
    gen_ctx->output_len = 0;
    
    gen_ctx->vocab = llama_model_get_vocab(gen_ctx->model);
    
    return ETHERVOX_SUCCESS;
}

/**
 * Append token text to output buffer, growing if needed
 */
static ethervox_result_t append_token_text(
    generation_context_t* gen_ctx,
    const char* text,
    size_t len
) {
    // Check if we need to grow the buffer
    if (gen_ctx->output_len + len + 1 > gen_ctx->output_capacity) {
        size_t new_capacity = gen_ctx->output_capacity * 2;
        if (new_capacity > MAX_RESPONSE_LENGTH) {
            return ETHERVOX_ERROR_OUT_OF_MEMORY;
        }
        
        char* new_buffer = realloc(gen_ctx->output_buffer, new_capacity);
        if (!new_buffer) {
            return ETHERVOX_ERROR_OUT_OF_MEMORY;
        }
        
        gen_ctx->output_buffer = new_buffer;
        gen_ctx->output_capacity = new_capacity;
    }
    
    memcpy(gen_ctx->output_buffer + gen_ctx->output_len, text, len);
    gen_ctx->output_len += len;
    gen_ctx->output_buffer[gen_ctx->output_len] = '\0';
    
    return ETHERVOX_SUCCESS;
}

/**
 * Fire an event if callback is set
 */
static void fire_event(
    generation_context_t* gen_ctx,
    const ethervox_event_t* event
) {
    if (gen_ctx->event_callback) {
        gen_ctx->event_callback(event, gen_ctx->user_data);
    }
}

/**
 * Detect if a token is purely structural JSON (not content)
 * 
 * Returns true if the token is a structural character like braces, commas, colons, quotes
 * that are mandated by JSON format. Content tokens (property values, numbers) return false.
 */
static bool is_token_structural(
    generation_context_t* gen_ctx,
    llama_token token,
    const char* token_text,
    int token_len
) {
    // If token text is empty or very short, check common structural tokens
    if (token_len == 0) {
        return true;  // Empty token is structural
    }
    
    // Single-character structural tokens
    if (token_len == 1) {
        char c = token_text[0];
        if (c == '{' || c == '}' || c == '[' || c == ']' || 
            c == ',' || c == ':' || c == '"' || c == ' ' || 
            c == '\n' || c == '\t') {
            return true;
        }
    }
    
    // Multi-character whitespace/formatting
    bool all_whitespace = true;
    for (int i = 0; i < token_len; i++) {
        char c = token_text[i];
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') {
            all_whitespace = false;
            break;
        }
    }
    
    return all_whitespace;
}

/**
 * Create sampler chain with grammar constraint
 */
static struct llama_sampler* create_sampler(
    generation_context_t* gen_ctx
) {
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    sparams.no_perf = false;
    
    struct llama_sampler* sampler = llama_sampler_chain_init(sparams);
    if (!sampler) {
        return NULL;
    }
    
    // Add penalties (frequency, presence, repeat)
    llama_sampler_chain_add(sampler, 
        llama_sampler_init_penalties(128, 1.1f, 0.0f, 0.0f));
    
    // Add temperature
    llama_sampler_chain_add(sampler, 
        llama_sampler_init_temp(gen_ctx->params->temperature));
    
    // Add grammar constraint
    if (gen_ctx->grammar) {
        // Get GBNF source from grammar
        const char* gbnf_source = ethervox_grammar_get_source(gen_ctx->grammar);
        const char* root_rule = ethervox_grammar_get_root(gen_ctx->grammar);
        
        if (gbnf_source && root_rule) {
            struct llama_sampler* grammar_sampler = llama_sampler_init_grammar(
                gen_ctx->vocab,
                gbnf_source,
                root_rule
            );
            
            if (grammar_sampler) {
                llama_sampler_chain_add(sampler, grammar_sampler);
            } else {
                ETHERVOX_LOG_ERROR("Failed to initialize grammar sampler");
            }
        }
    }
    
    // Add top-k and top-p
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(gen_ctx->params->top_p, 1));
    
    // Add final distribution sampler with seed
    // If seed=0, use time-based random seed; otherwise use specified seed
    uint32_t actual_seed = gen_ctx->params->seed;
    if (actual_seed == 0) {
        actual_seed = (uint32_t)time(NULL);
    }
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(actual_seed));
    
    return sampler;
}

/**
 * Tokenize and evaluate prompt
 *
 * `prompt` arrives here as the caller's raw text (e.g. a user's chat message) -- with no chat
 * template applied, an instruct-tuned model like Granite sees ungrammatical input with no role
 * markers or turn-end signal and immediately predicts EOG (`0 content tokens`, confirmed against
 * a real model). `governor.c` (the older, Android-facing code path) already solved this via
 * `chat_template.c`; this path never carried that fix over when it was added. Every model this
 * product ships is Granite (`MODELS-LICENSING.md`'s allow-list), so the template is fixed rather
 * than filename-sniffed via `chat_template_detect()` (Android's own use of that detector is a
 * best-effort fallback for user-supplied models, not applicable here).
 */
static ethervox_result_t prefill_prompt(
    generation_context_t* gen_ctx,
    const char* prompt
) {
    const chat_template_t* tmpl = chat_template_get(CHAT_TEMPLATE_GRANITE, NULL);
    // Heap-allocated, not a stack array: prompt length is caller-controlled and the role-marker
    // overhead is small, but a large prompt on a constrained thread stack would be a real risk.
    size_t formatted_capacity = strlen(prompt) + 512;
    char* formatted = malloc(formatted_capacity);
    if (!formatted) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    size_t offset = 0;

    ethervox_result_t result = chat_template_format_user(tmpl, prompt, formatted, formatted_capacity);
    if (result != ETHERVOX_SUCCESS) {
        free(formatted);
        return result;
    }
    offset = strlen(formatted);

    result = chat_template_format_assistant_start(
        tmpl, formatted + offset, formatted_capacity - offset);
    if (result != ETHERVOX_SUCCESS) {
        free(formatted);
        return result;
    }

    // Tokenize prompt
    int n_tokens_max = (int)strlen(formatted) + 256;  // Over-estimate
    llama_token* tokens = malloc(n_tokens_max * sizeof(llama_token));
    if (!tokens) {
        free(formatted);
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    const struct llama_vocab* vocab = llama_model_get_vocab(gen_ctx->model);
    int n_tokens = llama_tokenize(
        vocab,
        formatted,
        (int)strlen(formatted),
        tokens,
        n_tokens_max,
        true,   // add_special (add BOS)
        true    // parse_special -- the role markers above are literal text and must be
                // recognised as the model's actual special tokens, not tokenized as sub-word text
    );
    
    if (n_tokens < 0) {
        free(tokens);
        free(formatted);
        return ETHERVOX_ERROR_FAILED;
    }
    
    // Evaluate prompt in batches
    int batch_size = 512;
    for (int i = 0; i < n_tokens; i += batch_size) {
        int batch_count = (i + batch_size > n_tokens) ? (n_tokens - i) : batch_size;
        
        struct llama_batch batch = llama_batch_get_one(tokens + i, batch_count);
        
        int result = llama_decode(gen_ctx->ctx, batch);
        if (result != 0) {
            free(tokens);
            free(formatted);
            return ETHERVOX_ERROR_FAILED;
        }
    }
    
    free(tokens);
    free(formatted);
    return ETHERVOX_SUCCESS;
}

/**
 * Main generation loop
 */
static ethervox_result_t generate_tokens(generation_context_t* gen_ctx) {
    gen_ctx->sampler = create_sampler(gen_ctx);
    if (!gen_ctx->sampler) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    uint32_t max_tokens = gen_ctx->params->max_tokens;
    
    for (uint32_t i = 0; i < max_tokens; i++) {
        // Sample next token
        llama_token new_token = llama_sampler_sample(gen_ctx->sampler, gen_ctx->ctx, -1);
        
        // Check for EOS
        if (llama_vocab_is_eog(gen_ctx->vocab, new_token)) {
            // Fire finished event
            ethervox_event_t event = {
                .type = ETHERVOX_EVENT_FINISHED,
                .finished = { .finish_reason = "stop" }
            };
            fire_event(gen_ctx, &event);
            break;
        }
        
        // Get token text
        char piece[256];
        int n_piece = llama_token_to_piece(gen_ctx->vocab, new_token, piece, sizeof(piece) - 1, 0, false);
        
        if (n_piece > 0) {
            // llama_token_to_piece() does not NUL-terminate; callers across the FFI boundary
            // (ev-llm's token_text()) read this as a C string via CStr::from_ptr, so leaving it
            // unterminated reads past the real text into stale stack bytes from prior iterations.
            piece[n_piece] = '\0';

            // Append to output
            ethervox_result_t append_result = append_token_text(gen_ctx, piece, n_piece);
            if (append_result != ETHERVOX_SUCCESS) {
                llama_sampler_free(gen_ctx->sampler);
                return append_result;
            }
            
            // Fire token event
            ethervox_event_t token_event = {
                .type = ETHERVOX_EVENT_TOKEN,
                .token = {
                    .text = piece,
                    .token_id = new_token
                }
            };
            fire_event(gen_ctx, &token_event);
        }
        
        // Extract log-probability (AFTER sampling, BEFORE decode)
        // Get logits for current position
        float* logits = llama_get_logits_ith(gen_ctx->ctx, -1);
        if (logits) {
            // Get sampled token's logit
            float sampled_logit = logits[new_token];
            
            // Calculate log probability (softmax)
            int n_vocab = llama_vocab_n_tokens(gen_ctx->vocab);
            float max_logit = sampled_logit;
            for (int j = 0; j < n_vocab; j++) {
                if (logits[j] > max_logit) {
                    max_logit = logits[j];
                }
            }
            
            // Log-sum-exp for normalization
            double sum_exp = 0.0;
            for (int j = 0; j < n_vocab; j++) {
                sum_exp += exp(logits[j] - max_logit);
            }
            float log_prob = (sampled_logit - max_logit) - logf(sum_exp);
            float prob = expf(log_prob);
            
            // Detect if token is structural (not content)
            bool is_structural = is_token_structural(gen_ctx, new_token, piece, n_piece);
            
            // Track for confidence calculation (exclude structural tokens)
            gen_ctx->total_token_count++;
            if (!is_structural) {
                gen_ctx->log_prob_sum += log_prob;
                gen_ctx->content_token_count++;
            }
            
            // Fire logprob event if requested
            if (gen_ctx->params->include_logprobs) {
                ethervox_event_t logprob_event = {
                    .type = ETHERVOX_EVENT_LOGPROB,
                    .logprob = {
                        .token_id = new_token,
                        .token_text = n_piece > 0 ? piece : "",
                        .logprob = log_prob,
                        .prob = prob
                    }
                };
                fire_event(gen_ctx, &logprob_event);
            }
        }
        
        // Decode token for next iteration
        struct llama_batch batch = llama_batch_get_one(&new_token, 1);
        int decode_result = llama_decode(gen_ctx->ctx, batch);
        if (decode_result != 0) {
            llama_sampler_free(gen_ctx->sampler);
            return ETHERVOX_ERROR_FAILED;
        }
    }
    
    llama_sampler_free(gen_ctx->sampler);
    return ETHERVOX_SUCCESS;
}

/**
 * Calculate final confidence score
 */
static float calculate_confidence(const generation_context_t* gen_ctx) {
    if (gen_ctx->content_token_count == 0) {
        // All tokens were grammar-forced, or no tokens generated
        return 0.0f;
    }
    
    // Geometric mean: exp(mean of log-probs)
    double mean_log_prob = gen_ctx->log_prob_sum / gen_ctx->content_token_count;
    float confidence = expf(mean_log_prob);
    
    // Clamp very low confidence to 0.0
    if (confidence < MIN_CONFIDENCE) {
        confidence = 0.0f;
    }
    
    // Clamp to [0.0, 1.0]
    if (confidence > 1.0f) confidence = 1.0f;
    if (confidence < 0.0f) confidence = 0.0f;
    
    return confidence;
}

// ============================================================================
// Public API
// ============================================================================

ethervox_structured_gen_params_t ethervox_structured_gen_params_default(void) {
    ethervox_structured_gen_params_t params = {
        .max_tokens = 512,
        .temperature = 0.7f,
        .top_p = 0.9f,
        .seed = 0,
        .include_logprobs = false
    };
    return params;
}

ethervox_result_t ethervox_generate_structured(
    ethervox_model_handle_t* handle,
    const char* prompt,
    ethervox_grammar_t* grammar,
    const ethervox_structured_gen_params_t* params,
    ethervox_event_cb event_callback,
    void* user_data,
    char** out_json,
    float* out_confidence
) {
    if (!handle || !prompt || !out_json) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Use default params if not provided
    ethervox_structured_gen_params_t default_params = ethervox_structured_gen_params_default();
    if (!params) {
        params = &default_params;
    }
    
    // Initialize generation context
    generation_context_t gen_ctx;
    ethervox_result_t result = init_generation_context(
        &gen_ctx, handle, grammar, params, event_callback, user_data
    );
    
    if (result != ETHERVOX_SUCCESS) {
        return result;
    }
    
    // Prefill prompt
    result = prefill_prompt(&gen_ctx, prompt);
    if (result != ETHERVOX_SUCCESS) {
        free(gen_ctx.output_buffer);
        return result;
    }
    
    // Generate tokens
    result = generate_tokens(&gen_ctx);
    if (result != ETHERVOX_SUCCESS) {
        free(gen_ctx.output_buffer);
        return result;
    }
    
    // Calculate confidence
    float confidence = calculate_confidence(&gen_ctx);
    
    // Return results
    *out_json = gen_ctx.output_buffer;  // Caller takes ownership
    
    if (out_confidence) {
        *out_confidence = confidence;
    }
    
    ETHERVOX_LOG_INFO("Structured generation complete: %zu chars, %d content tokens, confidence %.3f",
                      gen_ctx.output_len, gen_ctx.content_token_count, confidence);
    
    return ETHERVOX_SUCCESS;
}
