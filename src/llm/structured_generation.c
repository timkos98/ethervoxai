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
#include <pthread.h>

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

    // Usage tracking (ETHERVOX_EVENT_USAGE) -- `n_prompt_tokens` set once by `prefill_prompt`;
    // `total_token_count` above already tracks completion tokens, reused as-is.
    int n_prompt_tokens;
    struct timespec start_time;

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
    clock_gettime(CLOCK_MONOTONIC, &gen_ctx->start_time);

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
 * Fire an event if callback is set. Returns the callback's bool result (true = keep going,
 * false = caller wants generation stopped now), or true when there is no callback.
 */
static bool fire_event(
    generation_context_t* gen_ctx,
    const ethervox_event_t* event
) {
    if (gen_ctx->event_callback) {
        return gen_ctx->event_callback(event, gen_ctx->user_data);
    }
    return true;
}

/**
 * Fires a real `ETHERVOX_EVENT_USAGE` -- always immediately before whichever `FINISHED` event
 * ends this call, so the stream still always ends with `FINISHED` (`ev-llm`'s own contract,
 * `generation.rs`'s doc comment: "the final element is always Finished"). Previously this event
 * type was declared in `event_stream.h` but constructed nowhere in the codebase.
 */
static void fire_usage_event(generation_context_t* gen_ctx) {
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (double)(end_time.tv_sec - gen_ctx->start_time.tv_sec) * 1000.0
        + (double)(end_time.tv_nsec - gen_ctx->start_time.tv_nsec) / 1e6;
    ethervox_event_t usage_event = {
        .type = ETHERVOX_EVENT_USAGE,
        .usage = {
            .prompt_tokens = (uint32_t)gen_ctx->n_prompt_tokens,
            .completion_tokens = (uint32_t)gen_ctx->total_token_count,
            .total_tokens = (uint32_t)(gen_ctx->n_prompt_tokens + gen_ctx->total_token_count),
            .elapsed_ms = elapsed_ms
        }
    };
    fire_event(gen_ctx, &usage_event);
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
    
    // Add grammar constraint BEFORE temperature. At temperature <= 0 (greedy), the temperature
    // sampler collapses every candidate's logit to -inf except the single argmax token
    // (llama_sampler_temp_impl's own "find the token with the highest logit and set the rest to
    // -inf"). If grammar ran after that, and the argmax token happened not to be grammar-valid,
    // the grammar sampler would be left filtering a candidate set that is already all -inf --
    // producing a degenerate distribution the rest of the chain (top_k/top_p/dist) cannot sample
    // from, which crashes deep in llama.cpp's sampling code (confirmed: reproduces with the most
    // trivial possible grammar at temperature 0.0, independent of lazy mode or grammar
    // complexity -- BACKLOG-59). Applying grammar first means temperature's argmax search only
    // ever considers logits llama.cpp's grammar sampler has already left non -inf.
    if (gen_ctx->grammar) {
        // Get GBNF source from grammar
        const char* gbnf_source = ethervox_grammar_get_source(gen_ctx->grammar);
        const char* root_rule = ethervox_grammar_get_root(gen_ctx->grammar);
        
        if (gbnf_source && root_rule) {
            struct llama_sampler* grammar_sampler = NULL;
            size_t trigger_word_count = 0;
            const char* const* trigger_words =
                ethervox_grammar_get_trigger_words(gen_ctx->grammar, &trigger_word_count);
            
            if (ethervox_grammar_is_lazy(gen_ctx->grammar) && trigger_words && trigger_word_count > 0) {
                // Lazy mode: free text until a trigger word appears, then the grammar engages
                // (mirrors llama_backend.c's existing, working pattern -- this function
                // previously ignored lazy mode entirely and always applied the grammar
                // immediately, regardless of what the caller set via
                // ethervox_grammar_set_lazy_mode).
                grammar_sampler = llama_sampler_init_grammar_lazy_patterns(
                    gen_ctx->vocab,
                    gbnf_source,
                    root_rule,
                    trigger_words,
                    trigger_word_count,
                    NULL, 0
                );
            } else {
                grammar_sampler = llama_sampler_init_grammar(
                    gen_ctx->vocab,
                    gbnf_source,
                    root_rule
                );
            }
            
            if (grammar_sampler) {
                llama_sampler_chain_add(sampler, grammar_sampler);
            } else {
                ETHERVOX_LOG_ERROR("Failed to initialize grammar sampler");
            }
        }
    }
    
    // Add temperature
    llama_sampler_chain_add(sampler, 
        llama_sampler_init_temp(gen_ctx->params->temperature));
    
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

ethervox_result_t ethervox_structured_render_prompt(
    const char* system_prompt,
    const char* prompt,
    const ethervox_chat_turn_t* turns,
    uint32_t turn_count,
    char** out
) {
    return ethervox_structured_render_prompt_with_history(
        system_prompt, NULL, 0, prompt, turns, turn_count, out);
}

ethervox_result_t ethervox_structured_render_prompt_with_history(
    const char* system_prompt,
    const ethervox_chat_turn_t* history,
    uint32_t history_count,
    const char* prompt,
    const ethervox_chat_turn_t* turns,
    uint32_t turn_count,
    char** out
) {
    if (!prompt || !out || (turn_count > 0 && !turns) || (history_count > 0 && !history)) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    // Every model this product ships is Granite (`MODELS-LICENSING.md`), so the template is fixed.
    const chat_template_t* tmpl = chat_template_get(CHAT_TEMPLATE_GRANITE, NULL);

    // Heap-allocated: prompt length is caller-controlled. Each turn adds a bounded marker overhead.
    size_t capacity = 512 + strlen(prompt) + (system_prompt ? strlen(system_prompt) : 0);
    for (uint32_t i = 0; i < turn_count; i++) {
        capacity += 512 + (turns[i].content ? strlen(turns[i].content) : 0);
    }
    for (uint32_t i = 0; i < history_count; i++) {
        capacity += 512 + (history[i].content ? strlen(history[i].content) : 0);
    }
    char* buf = malloc(capacity);
    if (!buf) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    buf[0] = '\0';
    size_t offset = 0;
    ethervox_result_t result = ETHERVOX_SUCCESS;

    if (system_prompt && system_prompt[0] != '\0') {
        result = chat_template_format_system(tmpl, system_prompt, buf + offset, capacity - offset);
        if (result != ETHERVOX_SUCCESS) goto fail;
        offset = strlen(buf);
    }

    for (uint32_t i = 0; i < history_count; i++) {
        const char* content = history[i].content ? history[i].content : "";
        if (history[i].role == ETHERVOX_CHAT_ROLE_USER) {
            result = chat_template_format_user(tmpl, content, buf + offset, capacity - offset);
            if (result != ETHERVOX_SUCCESS) goto fail;
        } else if (history[i].role == ETHERVOX_CHAT_ROLE_ASSISTANT) {
            int n = snprintf(buf + offset, capacity - offset, "%s%s%s",
                             tmpl->assistant_start, content, tmpl->assistant_end);
            if (n < 0 || (size_t)n >= capacity - offset) { result = ETHERVOX_ERROR_BUFFER_TOO_SMALL; goto fail; }
        } else {
            result = ETHERVOX_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        offset = strlen(buf);
    }

    result = chat_template_format_user(tmpl, prompt, buf + offset, capacity - offset);
    if (result != ETHERVOX_SUCCESS) goto fail;
    offset = strlen(buf);

    for (uint32_t i = 0; i < turn_count; i++) {
        const char* content = turns[i].content ? turns[i].content : "";
        if (turns[i].role == ETHERVOX_CHAT_ROLE_ASSISTANT) {
            int n = snprintf(buf + offset, capacity - offset, "%s%s%s",
                             tmpl->assistant_start, content, tmpl->assistant_end);
            if (n < 0 || (size_t)n >= capacity - offset) { result = ETHERVOX_ERROR_BUFFER_TOO_SMALL; goto fail; }
        } else if (turns[i].role == ETHERVOX_CHAT_ROLE_TOOL) {
            result = chat_template_format_tool_response(tmpl, content, buf + offset, capacity - offset);
            if (result != ETHERVOX_SUCCESS) goto fail;
        } else {
            result = ETHERVOX_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        offset = strlen(buf);
    }

    result = chat_template_format_assistant_start(tmpl, buf + offset, capacity - offset);
    if (result != ETHERVOX_SUCCESS) goto fail;

    *out = buf;
    return ETHERVOX_SUCCESS;

fail:
    free(buf);
    return result;
}

/**
 * The prompt tokens last evaluated into one context, so the next call can reuse the part of its
 * prompt that matches. A single entry: one assistant conversation drives the governor model at a
 * time, and a second context simply replaces it (a miss, never a wrong hit -- the KV state itself
 * is checked before anything is reused).
 */
static struct {
    pthread_mutex_t lock;
    const struct llama_context* ctx;
    llama_token* tokens;
    int count;
} g_prefix = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void prefix_cache_invalidate(void) {
    pthread_mutex_lock(&g_prefix.lock);
    free(g_prefix.tokens);
    g_prefix.tokens = NULL;
    g_prefix.count = 0;
    g_prefix.ctx = NULL;
    pthread_mutex_unlock(&g_prefix.lock);
}

static void prefix_cache_store(const struct llama_context* ctx, const llama_token* tokens, int count) {
    llama_token* copy = malloc((size_t)count * sizeof(llama_token));
    pthread_mutex_lock(&g_prefix.lock);
    free(g_prefix.tokens);
    if (copy) {
        memcpy(copy, tokens, (size_t)count * sizeof(llama_token));
    }
    g_prefix.tokens = copy;
    g_prefix.count = copy ? count : 0;
    g_prefix.ctx = copy ? ctx : NULL;
    pthread_mutex_unlock(&g_prefix.lock);
}

/**
 * How many leading tokens of `tokens` are already in `ctx`'s KV cache from the previous call.
 * At least one token is always left to evaluate, so the logits for sampling exist. 0 when reuse
 * is off (ETHERVOX_DISABLE_PREFIX_CACHE), the context differs, or its KV state no longer holds
 * what was cached (freed and reallocated context, another path decoded into it).
 */
static int prefix_cache_common_length(
    const struct llama_context* ctx, llama_memory_t mem, const llama_token* tokens, int n_tokens
) {
    if (getenv("ETHERVOX_DISABLE_PREFIX_CACHE")) {
        return 0;
    }
    int common = 0;
    pthread_mutex_lock(&g_prefix.lock);
    if (g_prefix.ctx == ctx && g_prefix.tokens && g_prefix.count > 0 &&
        llama_memory_seq_pos_max(mem, 0) >= (llama_pos)g_prefix.count - 1) {
        int limit = g_prefix.count < n_tokens - 1 ? g_prefix.count : n_tokens - 1;
        while (common < limit && g_prefix.tokens[common] == tokens[common]) {
            common++;
        }
    }
    pthread_mutex_unlock(&g_prefix.lock);
    return common;
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
 *
 * `system_prompt`, if non-NULL/non-empty, is rendered in the template's own system role ahead
 * of `prompt`'s user turn (`BACKLOG-80`): standing instructions folded into the user turn
 * instead make the model answer them conversationally rather than follow them -- the exact
 * "Understood, I will handle questions..." failure `BACKLOG-73` chased through prompt wording.
 */
static ethervox_result_t prefill_prompt(
    generation_context_t* gen_ctx,
    const ethervox_structured_gen_params_t* params,
    const char* prompt
) {
    // Every call here is a fresh, complete prompt (the caller always resends the whole
    // cumulative text, never an incremental continuation of a previous call's KV state), so the
    // context must not just keep appending at its ever-advancing position -- confirmed live: a
    // handful of separate calls against one model exhausted a 4096-token context and
    // `llama_decode` aborted with "failed to find a memory slot". Instead of always clearing,
    // the tokens shared with the previous call's prompt are kept (see `prefix_cache` below) and
    // only the differing tail is evaluated; anything else is discarded, same as a clear.
    llama_memory_t mem = llama_get_memory(gen_ctx->ctx);

    char* formatted = NULL;
    ethervox_result_t render_result = ethervox_structured_render_prompt_with_history(
        params->system_prompt, params->history, params->history_count, prompt, params->turns,
        params->turn_count, &formatted);
    if (render_result != ETHERVOX_SUCCESS) {
        return render_result;
    }
    // Opt-in: the exact bytes handed to the tokenizer, for diagnosing prompt-shape problems
    // (`BACKLOG-80` -- the diagnosis was made from reading code, this is how to check it live).
    if (getenv("ETHERVOX_LOG_PROMPT")) {
        fprintf(stderr, "[structured_generation] prompt bytes (%zu):\n%s\n<<<end prompt>>>\n",
                strlen(formatted), formatted);
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
    
    // Keep the longest prefix the context already holds for exactly these tokens.
    int reuse = prefix_cache_common_length(gen_ctx->ctx, mem, tokens, n_tokens);
    // Recurrent/hybrid models cannot drop the tail of a sequence: the trim reports failure and
    // the whole context is cleared instead (`data=false`: only the bookkeeping needs resetting).
    if (reuse > 0 && !llama_memory_seq_rm(mem, 0, reuse, -1)) {
        reuse = 0;
    }
    if (reuse == 0) {
        llama_memory_clear(mem, false);
    }
    ETHERVOX_LOG_INFO("Prompt %d tokens, %d reused from the previous call", n_tokens, reuse);

    // Evaluate the rest of the prompt in batches
    int batch_size = 512;
    for (int i = reuse; i < n_tokens; i += batch_size) {
        int batch_count = (i + batch_size > n_tokens) ? (n_tokens - i) : batch_size;
        
        struct llama_batch batch = llama_batch_get_one(tokens + i, batch_count);
        
        int result = llama_decode(gen_ctx->ctx, batch);
        if (result != 0) {
            prefix_cache_invalidate();
            free(tokens);
            free(formatted);
            return ETHERVOX_ERROR_FAILED;
        }
    }
    prefix_cache_store(gen_ctx->ctx, tokens, n_tokens);
    
    free(tokens);
    free(formatted);
    gen_ctx->n_prompt_tokens = n_tokens;
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
    bool finished_on_eog = false;
    
    for (uint32_t i = 0; i < max_tokens; i++) {
        // Sample next token
        llama_token new_token = llama_sampler_sample(gen_ctx->sampler, gen_ctx->ctx, -1);
        
        // Check for EOS
        if (llama_vocab_is_eog(gen_ctx->vocab, new_token)) {
            // Fire finished event
            fire_usage_event(gen_ctx);
            ethervox_event_t event = {
                .type = ETHERVOX_EVENT_FINISHED,
                .finished = { .finish_reason = "stop" }
            };
            fire_event(gen_ctx, &event);
            finished_on_eog = true;
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
            
            // Fire token event. The callback returning false (e.g. the caller cancelled) must
            // stop generation immediately -- previously this return value was discarded, so a
            // cancelled generation ran all the way to max_tokens regardless, pegging the CPU for
            // as long as that took and freezing the whole app/UI.
            ethervox_event_t token_event = {
                .type = ETHERVOX_EVENT_TOKEN,
                .token = {
                    .text = piece,
                    .token_id = new_token
                }
            };
            if (!fire_event(gen_ctx, &token_event)) {
                fire_usage_event(gen_ctx);
                ethervox_event_t cancelled_event = {
                    .type = ETHERVOX_EVENT_FINISHED,
                    .finished = { .finish_reason = "cancelled" }
                };
                fire_event(gen_ctx, &cancelled_event);
                llama_sampler_free(gen_ctx->sampler);
                return ETHERVOX_SUCCESS;
            }
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
    
    // Loop exhausted max_tokens without hitting EOS or cancellation -- no FINISHED event fires
    // here; the caller (`ev-llm`'s `run_generation`/`run_tool_aware_generation`) synthesizes one
    // when it sees the call succeeded without ever observing a Finished event. Usage still needs
    // firing here, before that synthesized Finished, for the same "always ends with Finished"
    // ordering. Not on the EOG path: it already fired Usage then Finished, and a second Usage
    // after Finished breaks the "stream ends with Finished" contract.
    if (!finished_on_eog) {
        fire_usage_event(gen_ctx);
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
        .include_logprobs = false,
        .system_prompt = NULL,
        .turns = NULL,
        .turn_count = 0,
        .history = NULL,
        .history_count = 0
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
    result = prefill_prompt(&gen_ctx, params, prompt);
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
