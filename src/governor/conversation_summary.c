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

// PERFORMANCE: Reduced from 800 to 400 tokens for faster mobile generation
#define SUMMARY_TOKEN_BUDGET 400  // Max tokens for summary

// Store last generated summary for UI display
static char g_last_summary[4096] = {0};
static bool g_has_summary = false;

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
    
    // Build conversation context from memory entries (OPTIMIZED for mobile)
    // Reduced from 8KB to 1.5KB for faster processing on 1B models
    char conversation_context[1536] = {0};
    int ctx_len = build_conversation_context_from_memory(
        memory_store, conversation_context, sizeof(conversation_context)
    );
    
    if (ctx_len == 0) {
        ETHERVOX_LOGI("No conversation to summarize");
        summary_out[0] = '\0';
        return ETHERVOX_SUCCESS;
    }
    
    // Create summarization prompt (SIMPLIFIED for speed)
    // Reduced from 10KB to ~2KB for faster tokenization
    char summary_prompt[2048];
    snprintf(summary_prompt, sizeof(summary_prompt),
        "Summarize in 2-3 sentences. Focus: topics, user info, decisions. Exclude: todos, reminders.\\n\\n%s\\n\\nSummary:",
        conversation_context
    );
    
    ETHERVOX_LOGI("Generated prompt: %zu chars from %d bytes of context", 
                  strlen(summary_prompt), ctx_len);
    
    // Use governor's LLM to generate summary (blocking)
    ethervox_result_t result = generate_summary_with_llm(
        governor, summary_prompt, summary_out, summary_size
    );
    
    if (ethervox_is_success(result)) {
        ETHERVOX_LOGI("Summary generated: %zu chars", strlen(summary_out));
        
        // Store for UI display
        strncpy(g_last_summary, summary_out, sizeof(g_last_summary) - 1);
        g_last_summary[sizeof(g_last_summary) - 1] = '\0';
        g_has_summary = true;
        
        // Store summary in memory.json with "context_summary" tag
        // This marks the checkpoint for what's been summarized
        if (memory_store && memory_store->is_initialized) {
            uint64_t memory_id;
            const char* tags[] = {"context_summary"};
            
            ethervox_result_t store_result = ethervox_memory_store_add(
                memory_store,
                summary_out,
                tags,
                1,  // tag count
                1.0f,  // high importance
                false,  // not a user message
                &memory_id
            );
            
            if (ethervox_is_success(store_result)) {
                ETHERVOX_LOGI("Stored summary in memory.json as checkpoint");
            } else {
                ETHERVOX_LOGW("Failed to store summary in memory: %d", store_result);
            }
        }
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
 * OPTIMIZED: Reduced from 20 to 8 entries, truncate long messages
 * This dramatically improves mobile performance (1B models)
 */
static int build_conversation_context_from_memory(
    ethervox_memory_store_t* memory,
    char* context_out,
    size_t context_size
) {
    if (!memory || !context_out || context_size == 0) {
        return 0;
    }
    
    // PERFORMANCE: Reduced from 20 to 8 entries for mobile
    int max_entries = 8;
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
        
        // PERFORMANCE: Truncate long entries to 150 chars max
        const char* role = entry->is_user_message ? "User" : "Assistant";
        size_t text_len = strlen(entry->text);
        
        if (text_len > 150) {
            // Truncate and add ellipsis
            char truncated[154];
            strncpy(truncated, entry->text, 150);
            truncated[150] = '.';
            truncated[151] = '.';
            truncated[152] = '.';
            truncated[153] = '\0';
            
            int n = snprintf(context_out + written, context_size - written,
                            "%s: %s\\n", role, truncated);
            if (n < 0 || written + n >= context_size) break;
            written += n;
        } else {
            int n = snprintf(context_out + written, context_size - written,
                            "%s: %s\\n", role, entry->text);
            if (n < 0 || written + n >= context_size) break;
            written += n;
        }
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
    
    // Get current KV position - not used for temp generation
    int32_t saved_kv_pos = ethervox_governor_get_kv_pos(governor);
    
    // Use sequence 1 for temporary summary generation (will be cleared after)
    // With 2-sequence architecture: seq 0 = system prompt + conversation, seq 1 = temp workspace
    int temp_seq_id = 1;
    
    // Process prompt into KV cache in temporary sequence 2
    // PERFORMANCE: Use batch size of 128 (was 512) for faster mobile processing
    int n_seq_max = llama_n_seq_max(ctx);
    llama_batch batch = llama_batch_init(128, 0, n_seq_max);
    
    for (int i = 0; i < n_prompt; i += 128) {
        int chunk = (i + 128 > n_prompt) ? (n_prompt - i) : 128;
        
        batch.n_tokens = chunk;
        for (int j = 0; j < chunk; j++) {
            batch.token[j] = prompt_tokens[i + j];
            batch.pos[j] = i + j;  // Start from 0 in temp sequence
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = temp_seq_id;  // Use sequence 2 (temporary)
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
    int current_pos = n_prompt;  // Continue from end of prompt in sequence 2
    
    // Create generation batch ONCE (reuse for all tokens) - PERFORMANCE FIX
    llama_batch gen_batch = llama_batch_init(1, 0, n_seq_max);
    
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
        
        // Feed token back for next iteration in sequence 2 (reuse batch)
        gen_batch.n_tokens = 1;
        gen_batch.token[0] = next_token;
        gen_batch.pos[0] = current_pos + i;
        gen_batch.n_seq_id[0] = 1;
        gen_batch.seq_id[0][0] = temp_seq_id;  // Continue in sequence 2
        gen_batch.logits[0] = true;
        
        if (llama_decode(ctx, gen_batch) != 0) {
            break;
        }
    }
    
    // Free batch ONCE after generation loop
    llama_batch_free(gen_batch);
    llama_sampler_free(sampler);
    
    // Clear temporary sequence 2 (entire sequence - this always succeeds)
    llama_memory_t mem = llama_get_memory(ctx);
    bool cleared = llama_memory_seq_rm(mem, temp_seq_id, -1, -1);
    
    if (!cleared) {
        ETHERVOX_LOGW("Failed to clear temp sequence %d (non-fatal)", temp_seq_id);
    } else {
        ETHERVOX_LOGI("Cleared temporary summary generation from sequence %d", temp_seq_id);
    }
    
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

/**
 * Get the last generated conversation summary text
 * Returns the summary text for UI display
 */
const char* ethervox_get_last_summary(void) {
    if (!g_has_summary) {
        return "No summary generated yet. Use the 'Summarize Conversation' button to create one.";
    }
    return g_last_summary;
}

/**
 * Restore conversation context from memory after loading summary
 * 
 * Loads:
 * 1. Conversations AFTER last summary checkpoint
 * 2. Active tasks (reminder, todo, action_item tags)
 * 3. Important context (important, personal_info, user_preference tags)
 * 
 * @param governor Governor instance
 * @param memory_store Memory store
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_restore_context_from_memory(
    struct ethervox_governor* governor,
    ethervox_memory_store_t* memory_store
) {
    if (!governor || !memory_store || !memory_store->is_initialized) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    ETHERVOX_LOGI("Restoring conversation context from memory...");
    
    // Find the last summary checkpoint timestamp
    uint64_t summary_timestamp = 0;
    for (int i = memory_store->entry_count - 1; i >= 0; i--) {
        ethervox_memory_entry_t* entry = &memory_store->entries[i];
        
        // Check for context_summary tag
        for (uint32_t j = 0; j < entry->tag_count; j++) {
            if (strcmp(entry->tags[j], "context_summary") == 0) {
                summary_timestamp = entry->timestamp;
                ETHERVOX_LOGI("Found summary checkpoint at timestamp: %llu", summary_timestamp);
                
                // Store the summary text for UI display
                strncpy(g_last_summary, entry->text, sizeof(g_last_summary) - 1);
                g_last_summary[sizeof(g_last_summary) - 1] = '\0';
                g_has_summary = true;
                goto found_summary;
            }
        }
    }
    
    // Collect entries to restore
    int conversations_loaded = 0;
    int tasks_loaded = 0;
    int context_loaded = 0;
    
found_summary:
    
    for (uint32_t i = 0; i < memory_store->entry_count; i++) {
        ethervox_memory_entry_t* entry = &memory_store->entries[i];
        bool should_load = false;
        
        // Skip the summary itself
        bool is_summary = false;
        for (uint32_t j = 0; j < entry->tag_count; j++) {
            if (strcmp(entry->tags[j], "context_summary") == 0) {
                is_summary = true;
                break;
            }
        }
        if (is_summary) continue;
        
        // Load conversations after summary
        if (entry->timestamp > summary_timestamp && entry->tag_count == 0) {
            // Regular conversation message (no special tags)
            should_load = true;
            conversations_loaded++;
        }
        
        // Load active tasks
        for (uint32_t j = 0; j < entry->tag_count; j++) {
            if (strcmp(entry->tags[j], "reminder") == 0 ||
                strcmp(entry->tags[j], "todo") == 0 ||
                strcmp(entry->tags[j], "action_item") == 0) {
                should_load = true;
                tasks_loaded++;
                break;
            }
        }
        
        // Load important context
        for (uint32_t j = 0; j < entry->tag_count; j++) {
            if (strcmp(entry->tags[j], "important") == 0 ||
                strcmp(entry->tags[j], "personal_info") == 0 ||
                strcmp(entry->tags[j], "user_preference") == 0) {
                should_load = true;
                context_loaded++;
                break;
            }
        }
        
        // TODO: Actually load these into KV cache
        // For now, just counting what would be loaded
        // This would require tokenizing and adding to KV cache
        if (should_load) {
            // Future: tokenize entry->text and add to KV cache
        }
    }
    
    ETHERVOX_LOGI("Context restoration complete: %d conversations, %d tasks, %d context items",
                  conversations_loaded, tasks_loaded, context_loaded);
    
    return ETHERVOX_SUCCESS;
}
