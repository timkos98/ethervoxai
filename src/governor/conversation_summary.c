/**
 * @file conversation_summary.c
 * @brief Simple LLM-based conversation summarization (MVP)
 * 
 * MVP: Manual trigger, blocking operation, always regenerate from scratch
 * 
 * Copyright (c) 2024-2026 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/conversation_summary.h"
#include "ethervox/config.h"
#include "ethervox/error.h"
#include "ethervox/memory_tools.h"
#include "ethervox/kv_cache_persistence.h"
#include <llama.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SUMMARY_TOKEN_BUDGET 800  // Max tokens for summary

// Forward declaration of internal helpers
static int build_conversation_context_from_memory(
    ethervox_memory_store_t* memory,
    char* context_out,
    size_t context_size
);

static ethervox_result_t generate_summary_with_llm(
    struct ethervox_governor* governor,
    const char* prompt,
    char* summary_out,
    size_t summary_size
);

static const char* get_model_basename(const char* model_path);

/**
 * Generate LLM-based conversation summary (MVP: Simple & Blocking)
 */
ethervox_result_t ethervox_generate_conversation_summary(
    struct ethervox_governor* governor,
    ethervox_memory_store_t* memory_store,
    char* summary_out,
    size_t summary_size
) {
    if (!governor || !memory_store || !summary_out || summary_size == 0) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    ETHERVOX_LOGI("Starting conversation summarization (MVP)");
    
    if (!memory_store || !memory_store->is_initialized) {
        ETHERVOX_LOGI("No memory store available - nothing to summarize");
        summary_out[0] = '\0';
        return ETHERVOX_SUCCESS;
    }
    
    // Build conversation context from memory entries
    char conversation_context[8192] = {0};
    int ctx_len = build_conversation_context_from_memory(
        memory_store, conversation_context, sizeof(conversation_context)
    );
    
    if (ctx_len == 0) {
        ETHERVOX_LOGI("No conversation to summarize");
        summary_out[0] = '\0';
        return ETHERVOX_SUCCESS;
    }
    
    // Create summarization prompt (EXCLUDES action items explicitly)
    char summary_prompt[10240];
    snprintf(summary_prompt, sizeof(summary_prompt),
        "Summarize this conversation in 3-5 concise sentences.\\n\\n"
        "Focus ONLY on:\\n"
        "- Topics discussed (technical, personal, professional)\\n"
        "- User information shared (name, preferences, context)\\n"
        "- Decisions made or conclusions reached\\n"
        "- Key insights or learning\\n\\n"
        "DO NOT include:\\n"
        "- Reminders or tasks (stored separately)\\n"
        "- Todos or action items\\n"
        "- Future commitments\\n\\n"
        "Conversation:\\n%s\\n\\n"
        "Summary (3-5 sentences, max %d tokens):",
        conversation_context,
        SUMMARY_TOKEN_BUDGET
    );
    
    // Use governor's LLM to generate summary (blocking)
    ethervox_result_t result = generate_summary_with_llm(
        governor, summary_prompt, summary_out, summary_size
    );
    
    if (ethervox_is_success(result)) {
        ETHERVOX_LOGI("Summary generated: %zu chars", strlen(summary_out));
    } else {
        ETHERVOX_LOGE("Failed to generate summary");
    }
    
    return result;
}

/**
 * Save conversation summary to KV cache file
 */
ethervox_result_t ethervox_save_conversation_summary(
    struct ethervox_governor* governor,
    const char* summary_text,
    const char* cache_dir
) {
    if (!governor || !summary_text || !cache_dir) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Build cache file path
    char cache_path[512];
    const char* model_basename = get_model_basename(
        ethervox_governor_get_model_path(governor)
    );
    
    snprintf(cache_path, sizeof(cache_path),
             "%s/conversation_summary_%s.kvcache",
             cache_dir, model_basename);
    
    // Save KV cache state with summary loaded
    ethervox_result_t result = ethervox_kv_cache_save(governor, cache_path);
    
    if (ethervox_is_success(result)) {
        ETHERVOX_LOGI("Conversation summary saved to: %s", cache_path);
    } else {
        ETHERVOX_LOGE("Failed to save conversation summary");
    }
    
    return result;
}

/**
 * Load conversation summary from KV cache file
 */
ethervox_result_t ethervox_load_conversation_summary(
    struct ethervox_governor* governor,
    const char* cache_dir
) {
    if (!governor || !cache_dir) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Build cache file path
    char cache_path[512];
    const char* model_basename = get_model_basename(
        ethervox_governor_get_model_path(governor)
    );
    
    snprintf(cache_path, sizeof(cache_path),
             "%s/conversation_summary_%s.kvcache",
             cache_dir, model_basename);
    
    const char* model_path = ethervox_governor_get_model_path(governor);
    
    // Check if file exists
    if (!ethervox_kv_cache_exists(cache_path, model_path)) {
        ETHERVOX_LOGI("No conversation summary found (first session)");
        return ETHERVOX_SUCCESS;  // Not an error
    }
    
    // Load KV cache
    ethervox_result_t result = ethervox_kv_cache_load(governor, cache_path);
    
    if (ethervox_is_success(result)) {
        ETHERVOX_LOGI("Conversation summary loaded from: %s", cache_path);
    } else {
        ETHERVOX_LOGW("Failed to load conversation summary (will generate new)");
    }
    
    return result;
}

// ============================================================================
// Internal Helper Functions
// ============================================================================

/**
 * Build conversation context string from memory entries
 * MVP: Simple extraction of recent entries (last 20 or all if less)
 */
static int build_conversation_context_from_memory(
    ethervox_memory_store_t* memory,
    char* context_out,
    size_t context_size
) {
    if (!memory || !context_out || context_size == 0) {
        return 0;
    }
    
    // Get last 20 entries (or all if less)
    int max_entries = 20;
    int start_idx = (memory->entry_count > max_entries) ?
                    (memory->entry_count - max_entries) : 0;
    
    size_t written = 0;
    context_out[0] = '\0';
    
    for (uint32_t i = start_idx; i < memory->entry_count; i++) {
        ethervox_memory_entry_t* entry = &memory->entries[i];
        
        // Skip entries tagged as "context_summary" (those are summaries themselves)
        bool is_summary = false;
        for (uint32_t j = 0; j < entry->tag_count; j++) {
            if (strcmp(entry->tags[j], "context_summary") == 0) {
                is_summary = true;
                break;
            }
        }
        
        if (is_summary) {
            continue;
        }
        
        // Format: "User: <text>\\nAssistant: <text>\\n"
        const char* role = entry->is_user_message ? "User" : "Assistant";
        int n = snprintf(context_out + written, context_size - written,
                        "%s: %s\\n",
                        role, entry->text);
        
        if (n < 0 || written + n >= context_size) {
            break;  // Buffer full
        }
        
        written += n;
    }
    
    return (int)written;
}

/**
 * Generate summary using governor's LLM (synchronous/blocking)
 * MVP: Simple generation, no progress callbacks
 */
static ethervox_result_t generate_summary_with_llm(
    struct ethervox_governor* governor,
    const char* prompt,
    char* summary_out,
    size_t summary_size
) {
    // Get LLM context and model from governor
    struct llama_model* model = ethervox_governor_get_llm_model(governor);
    struct llama_context* ctx = ethervox_governor_get_llm_context(governor);
    
    if (!model || !ctx) {
        ETHERVOX_LOGE("Governor LLM not available");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Tokenize prompt
    const struct llama_vocab* vocab = llama_model_get_vocab(model);
    llama_token* prompt_tokens = (llama_token*)malloc(2048 * sizeof(llama_token));
    if (!prompt_tokens) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt),
                                   prompt_tokens, 2048, true, false);
    
    if (n_prompt <= 0) {
        free(prompt_tokens);
        ETHERVOX_LOGE("Failed to tokenize prompt");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Get current KV position to restore later
    int32_t saved_kv_pos = ethervox_governor_get_kv_pos(governor);
    int temp_kv_start = saved_kv_pos;
    
    // Process prompt into KV cache at temporary position
    llama_batch batch = llama_batch_init(512, 0, 1);
    
    for (int i = 0; i < n_prompt; i += 512) {
        int chunk = (i + 512 > n_prompt) ? (n_prompt - i) : 512;
        
        batch.n_tokens = chunk;
        for (int j = 0; j < chunk; j++) {
            batch.token[j] = prompt_tokens[i + j];
            batch.pos[j] = temp_kv_start + i + j;
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j] = (i + j == n_prompt - 1);  // Last token gets logits
        }
        
        if (llama_decode(ctx, batch) != 0) {
            ETHERVOX_LOGE("Failed to decode summary prompt");
            llama_batch_free(batch);
            free(prompt_tokens);
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
    }
    
    llama_batch_free(batch);
    free(prompt_tokens);
    
    // Generate summary tokens (blocking)
    struct llama_sampler* sampler = llama_sampler_chain_init(
        llama_sampler_chain_default_params()
    );
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.3f));  // Low temp for focused output
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));
    
    char* summary_text = (char*)calloc(4096, 1);
    if (!summary_text) {
        llama_sampler_free(sampler);
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    int summary_len = 0;
    int current_pos = temp_kv_start + n_prompt;
    
    // Generate up to token budget
    for (int i = 0; i < SUMMARY_TOKEN_BUDGET; i++) {
        llama_token next_token = llama_sampler_sample(sampler, ctx, -1);
        llama_sampler_accept(sampler, next_token);
        
        // Check for EOS
        if (next_token == llama_vocab_eos(vocab)) {
            break;
        }
        
        // Decode token to text
        char piece[32];
        int n_chars = llama_token_to_piece(vocab, next_token, piece, sizeof(piece), 0, false);
        
        if (n_chars > 0 && summary_len + n_chars < 4095) {
            memcpy(summary_text + summary_len, piece, n_chars);
            summary_len += n_chars;
            summary_text[summary_len] = '\0';
        }
        
        // Feed token back for next iteration
        llama_batch gen_batch = llama_batch_init(1, 0, 1);
        gen_batch.n_tokens = 1;
        gen_batch.token[0] = next_token;
        gen_batch.pos[0] = current_pos + i;
        gen_batch.n_seq_id[0] = 1;
        gen_batch.seq_id[0][0] = 0;
        gen_batch.logits[0] = true;
        
        if (llama_decode(ctx, gen_batch) != 0) {
            llama_batch_free(gen_batch);
            break;
        }
        
        llama_batch_free(gen_batch);
    }
    
    llama_sampler_free(sampler);
    
    // Clear temporary KV cache (don't keep summary generation in cache)
    llama_memory_t mem = llama_get_memory(ctx);
    llama_memory_seq_rm(mem, 0, temp_kv_start, -1);
    
    // Copy to output
    strncpy(summary_out, summary_text, summary_size - 1);
    summary_out[summary_size - 1] = '\0';
    free(summary_text);
    
    ETHERVOX_LOGI("Generated summary: %d chars", summary_len);
    
    return ETHERVOX_SUCCESS;
}

/**
 * Extract model basename from full path
 */
static const char* get_model_basename(const char* model_path) {
    if (!model_path) {
        return "unknown";
    }
    
    const char* basename = strrchr(model_path, '/');
    if (basename) {
        basename++;  // Skip the '/'
    } else {
        basename = model_path;
    }
    
    return basename;
}
