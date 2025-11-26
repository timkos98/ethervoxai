/**
 * @file governor.c
 * @brief Governor orchestration system implementation
 *
 * The Governor is the central reasoning engine that coordinates between
 * LLM inference and tool execution. It maintains a conversation loop
 * until a complete response is generated or limits are reached.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/governor.h"
#include "ethervox/config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#include <llama.h>
#define LLAMA_HEADER_AVAILABLE 1
#else
#define LLAMA_HEADER_AVAILABLE 0
#endif

#define GOV_LOG(...) ETHERVOX_LOGI(__VA_ARGS__)
#define GOV_ERROR(...) ETHERVOX_LOGE(__VA_ARGS__)

/**
 * Internal state for the Governor
 */
struct ethervox_governor {
    ethervox_governor_config_t config;
    ethervox_tool_registry_t* tool_registry;
    
#if defined(ETHERVOX_WITH_LLAMA) && LLAMA_HEADER_AVAILABLE
    struct llama_model* llm_model;
    struct llama_context* llm_ctx;
    llama_pos system_prompt_token_count;
    llama_pos current_kv_pos;  // Track current position in KV cache
    char* model_path;
    
    // Pre-tokenized static wrappers for speed optimization
    llama_token* tool_result_prefix_tokens;  // "<|im_start|>user\n<tool_result>"
    int tool_result_prefix_len;
    llama_token* tool_result_suffix_tokens;  // "</tool_result><|im_end|>\n<|im_start|>assistant\n"
    int tool_result_suffix_len;
#else
    void* llm_model;  // Placeholder
    void* llm_ctx;
    int32_t system_prompt_token_count;
    int32_t current_kv_pos;  // Track current position in KV cache
    char* model_path;
#endif
    
    // State tracking
    uint32_t last_iteration_count;
    bool initialized;
    bool llm_loaded;
};

/**
 * Extract tool calls from LLM response
 * Looks for <tool_call name="..." attr="..." /> tags
 * 
 * Returns: Number of tool calls found, fills tool_calls array
 */
static int extract_tool_calls(const char* response, char** tool_calls, int max_calls) {
    if (!response || !tool_calls) return 0;
    
    int count = 0;
    const char* pos = response;
    
    while (count < max_calls) {
        // Find next tool call
        const char* start = strstr(pos, "<tool_call");
        if (!start) break;
        
        const char* end = strstr(start, "/>");
        if (!end) break;
        
        end += 2; // Include the />
        
        // Extract full tag
        size_t tag_len = end - start;
        tool_calls[count] = malloc(tag_len + 1);
        if (!tool_calls[count]) break;
        
        strncpy(tool_calls[count], start, tag_len);
        tool_calls[count][tag_len] = '\0';
        
        count++;
        pos = end;
    }
    
    return count;
}

/**
 * Parse XML attribute from tool call tag
 * Example: name="calculator_compute" -> returns "calculator_compute"
 */
static char* parse_attribute(const char* tag, const char* attr_name) {
    if (!tag || !attr_name) return NULL;
    
    // Look for attr_name="value"
    char search[64];
    snprintf(search, sizeof(search), "%s=\"", attr_name);
    
    const char* start = strstr(tag, search);
    if (!start) return NULL;
    
    start += strlen(search);
    
    const char* end = strchr(start, '"');
    if (!end) return NULL;
    
    size_t len = end - start;
    char* value = malloc(len + 1);
    if (!value) return NULL;
    
    strncpy(value, start, len);
    value[len] = '\0';
    
    return value;
}

/**
 * Execute a single tool call
 * Parses the XML tag, extracts attributes, builds JSON, calls tool
 */
static int execute_tool_call(
    const char* tool_call_xml,
    ethervox_tool_registry_t* registry,
    char** result,
    char** error
) {
    // Extract tool name
    char* tool_name = parse_attribute(tool_call_xml, "name");
    if (!tool_name) {
        *error = strdup("Missing 'name' attribute in tool call");
        return -1;
    }
    
    // Find tool in registry
    const ethervox_tool_t* tool = ethervox_tool_registry_find(registry, tool_name);
    if (!tool) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Unknown tool: %s", tool_name);
        *error = strdup(err_msg);
        free(tool_name);
        return -1;
    }
    
    // Build JSON input from XML attributes
    // For now, support simple attribute -> JSON key mapping
    // TODO: More sophisticated JSON building for complex tools
    
    // Increased buffer to 64KB to accommodate large content parameters (e.g., file_write with large text)
    char json_input[65536] = "{";
    bool first = true;
    
    // Common attributes to extract
    const char* attrs[] = {
        // Calculator/compute
        "expression", "value", "percentage", "operation",
        "from", "to", "amount",
        "duration_seconds", "label", "hour", "minute",
        "decimal_places",
        // File tools
        "directory", "file_path", "pattern", "recursive",
        // Memory tools
        "text", "content", "tags", "query", "limit", "window_size", "format",
        "importance", "min_importance", "max_age_hours", "is_user",
        "memory_id", "filepath",
        NULL
    };
    
    for (int i = 0; attrs[i] != NULL; i++) {
        char* attr_value = parse_attribute(tool_call_xml, attrs[i]);
        if (attr_value) {
            if (!first) strcat(json_input, ", ");
            
            // Check if value is numeric (must have at least one digit)
            // Exception: memory_id, file_path, filepath, tags should always be strings
            bool force_string = (strcmp(attrs[i], "memory_id") == 0 ||
                               strcmp(attrs[i], "file_path") == 0 ||
                               strcmp(attrs[i], "filepath") == 0 ||
                               strcmp(attrs[i], "tags") == 0 ||
                               strcmp(attrs[i], "query") == 0 ||
                               strcmp(attrs[i], "text") == 0 ||
                               strcmp(attrs[i], "content") == 0 ||
                               strcmp(attrs[i], "directory") == 0 ||
                               strcmp(attrs[i], "pattern") == 0 ||
                               strcmp(attrs[i], "format") == 0 ||
                               strcmp(attrs[i], "label") == 0);
            
            bool is_numeric = false;
            if (!force_string) {
                bool has_digit = false;
                const char* p = attr_value;
                
                if (*p == '-' || *p == '+') p++;
                
                while (*p) {
                    if (*p >= '0' && *p <= '9') {
                        has_digit = true;
                    } else if (*p != '.') {
                        has_digit = false;
                        break;
                    }
                    p++;
                }
                
                is_numeric = has_digit && (p > attr_value);
            }
            
            // Append field directly to json_input to avoid 256-byte truncation
            size_t current_len = strlen(json_input);
            size_t remaining = sizeof(json_input) - current_len - 1;
            
            if (is_numeric) {
                snprintf(json_input + current_len, remaining, "\"%s\": %s", attrs[i], attr_value);
            } else {
                snprintf(json_input + current_len, remaining, "\"%s\": \"%s\"", attrs[i], attr_value);
            }
            
            first = false;
            free(attr_value);
        }
    }
    
    strcat(json_input, "}");
    
    GOV_LOG("Built JSON for tool '%s': %s", tool->name, json_input);
    
    free(tool_name);
    
    // Safety checks before execution
    if (!tool->execute) {
        *error = strdup("Tool has NULL execute pointer");
        return -1;
    }
    
    if (!result || !error) {
        GOV_ERROR("Invalid result or error pointers passed to execute_tool_call");
        return -1;
    }
    
    // Execute tool
    int ret = tool->execute(json_input, result, error);
    if (ret != 0) {
        GOV_ERROR("Tool '%s' execution failed with code %d, error: %s", 
                  tool->name, ret, error && *error ? *error : "unknown");
    } else {
        GOV_LOG("Tool '%s' executed successfully, result length: %zu", 
                tool->name, result && *result ? strlen(*result) : 0);
    }
    return ret;
}


int ethervox_governor_load_model(ethervox_governor_t* governor, const char* model_path) {
    if (!governor || !model_path) return -1;
    
#if !defined(ETHERVOX_WITH_LLAMA) || !LLAMA_HEADER_AVAILABLE
    GOV_ERROR("llama.cpp not available - cannot load model");
    return -1;
#else
    
    // If a model is already loaded, unload it first
    if (governor->llm_loaded) {
        GOV_LOG("[Governor] Unloading existing model before loading new one");
        
        // Free pre-tokenized wrappers
        if (governor->tool_result_prefix_tokens) {
            free(governor->tool_result_prefix_tokens);
            governor->tool_result_prefix_tokens = NULL;
            governor->tool_result_prefix_len = 0;
        }
        if (governor->tool_result_suffix_tokens) {
            free(governor->tool_result_suffix_tokens);
            governor->tool_result_suffix_tokens = NULL;
            governor->tool_result_suffix_len = 0;
        }
        
        // Free context and model
        if (governor->llm_ctx) {
            llama_free(governor->llm_ctx);
            governor->llm_ctx = NULL;
        }
        if (governor->llm_model) {
            llama_model_free(governor->llm_model);
            governor->llm_model = NULL;
        }
        if (governor->model_path) {
            free(governor->model_path);
            governor->model_path = NULL;
        }
        
        governor->llm_loaded = false;
        governor->system_prompt_token_count = 0;
        governor->current_kv_pos = 0;
    }
    
    GOV_LOG("[Governor] Loading model: %s", model_path);
    
    // Initialize llama.cpp backend
    llama_backend_init();
    
    // Model params - Use config.h defaults
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = ETHERVOX_GOVERNOR_GPU_LAYERS;
    model_params.use_mmap = ETHERVOX_GOVERNOR_USE_MMAP;
    model_params.use_mlock = false;  // Don't lock memory (let OS manage)
    
    // Load model
    governor->llm_model = llama_model_load_from_file(model_path, model_params);
    if (!governor->llm_model) {
        GOV_ERROR("Failed to load model from %s", model_path);
        return -1;
    }
    
    GOV_LOG("[Governor] Model loaded successfully");
    
    // Context params - Use config.h defaults
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = ETHERVOX_GOVERNOR_CONTEXT_SIZE;
    ctx_params.n_batch = ETHERVOX_GOVERNOR_BATCH_SIZE;
    ctx_params.n_threads = ETHERVOX_GOVERNOR_THREADS;
    ctx_params.n_threads_batch = ETHERVOX_GOVERNOR_THREADS;
    ctx_params.flash_attn_type = ETHERVOX_GOVERNOR_FLASH_ATTN_TYPE;  // Enable flash attention for speed and quantized KV cache
    ctx_params.type_k = ETHERVOX_GOVERNOR_KV_CACHE_TYPE;
    ctx_params.type_v = ETHERVOX_GOVERNOR_KV_CACHE_TYPE;
    
    // Create context
    governor->llm_ctx = llama_init_from_model(governor->llm_model, ctx_params);
    if (!governor->llm_ctx) {
        GOV_ERROR("Failed to create llama context");
        llama_model_free(governor->llm_model);
        governor->llm_model = NULL;
        return -1;
    }
    
    GOV_LOG("Context created successfully");
    
    // Build system prompt from tool registry
    // Increased to 16KB to accommodate all tools and examples
    char system_prompt[16384];
    if (ethervox_tool_registry_build_system_prompt(governor->tool_registry, 
                                                   system_prompt, sizeof(system_prompt)) != 0) {
        GOV_ERROR("Failed to build system prompt");
        llama_free(governor->llm_ctx);
        llama_model_free(governor->llm_model);
        governor->llm_ctx = NULL;
        governor->llm_model = NULL;
        return -1;
    }
    
    GOV_LOG("System prompt (%zu chars):\n%s", strlen(system_prompt), system_prompt);
    
    GOV_LOG("System prompt built (%zu chars)", strlen(system_prompt));
    
    // Tokenize system prompt
    const struct llama_vocab* vocab = llama_model_get_vocab(governor->llm_model);
    int n_tokens = -llama_tokenize(vocab, system_prompt, strlen(system_prompt), NULL, 0, true, false);
    
    if (n_tokens <= 0) {
        GOV_ERROR("Failed to tokenize system prompt");
        llama_free(governor->llm_ctx);
        llama_model_free(governor->llm_model);
        governor->llm_ctx = NULL;
        governor->llm_model = NULL;
        return -1;
    }
    
    llama_token* tokens = malloc(n_tokens * sizeof(llama_token));
    if (!tokens) {
        GOV_ERROR("Failed to allocate token buffer");
        llama_free(governor->llm_ctx);
        llama_model_free(governor->llm_model);
        governor->llm_ctx = NULL;
        governor->llm_model = NULL;
        return -1;
    }
    
    llama_tokenize(vocab, system_prompt, strlen(system_prompt), tokens, n_tokens, true, false);
    
    GOV_LOG("Processing %d system prompt tokens in chunks...", n_tokens);
    
    // Process in 1024-token chunks for speed (2x faster on high-end devices)
    int chunk_size = 1024;
    for (int i = 0; i < n_tokens; i += chunk_size) {
        int chunk_len = (i + chunk_size > n_tokens) ? (n_tokens - i) : chunk_size;
        bool is_final_chunk = (i + chunk_size >= n_tokens);
        
        // Log progress every 25%
        if (i == 0 || (i * 4 / n_tokens) > ((i - chunk_size) * 4 / n_tokens)) {
            int percent = (i * 100 / n_tokens);
            GOV_LOG("System prompt processing: %d%% (%d/%d tokens)", percent, i, n_tokens);
        }
        
        // Create batch with explicit positions and sequence ID
        llama_batch batch = llama_batch_init(chunk_len, 0, 1);
        batch.n_tokens = chunk_len;
        for (int j = 0; j < chunk_len; j++) {
            batch.token[j] = tokens[i + j];
            batch.pos[j] = i + j;  // Absolute position from start
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = 0;  // Use sequence 0 for everything
            batch.logits[j] = false;  // Skip logits for all tokens
        }
        // Only compute logits for the very last token of the entire system prompt
        if (is_final_chunk) {
            batch.logits[chunk_len - 1] = true;
        }
        
        if (llama_decode(governor->llm_ctx, batch) != 0) {
            GOV_ERROR("Failed to process system prompt chunk at token %d", i);
            llama_batch_free(batch);
            free(tokens);
            llama_free(governor->llm_ctx);
            llama_model_free(governor->llm_model);
            governor->llm_ctx = NULL;
            governor->llm_model = NULL;
            return -1;
        }
        
        llama_batch_free(batch);
    }
    
    free(tokens);
    
    governor->system_prompt_token_count = n_tokens;
    governor->current_kv_pos = n_tokens;  // Start after system prompt
    governor->model_path = strdup(model_path);
    
    // Pre-tokenize static tool result wrappers for speed
    const char* prefix = "<|im_start|>user\n<tool_result>";
    const char* suffix = "</tool_result><|im_end|>\n<|im_start|>assistant\n";
    
    governor->tool_result_prefix_len = -llama_tokenize(vocab, prefix, strlen(prefix), NULL, 0, false, false);
    governor->tool_result_prefix_tokens = malloc(governor->tool_result_prefix_len * sizeof(llama_token));
    if (governor->tool_result_prefix_tokens) {
        llama_tokenize(vocab, prefix, strlen(prefix), 
                      governor->tool_result_prefix_tokens, governor->tool_result_prefix_len, false, false);
    }
    
    governor->tool_result_suffix_len = -llama_tokenize(vocab, suffix, strlen(suffix), NULL, 0, false, false);
    governor->tool_result_suffix_tokens = malloc(governor->tool_result_suffix_len * sizeof(llama_token));
    if (governor->tool_result_suffix_tokens) {
        llama_tokenize(vocab, suffix, strlen(suffix), 
                      governor->tool_result_suffix_tokens, governor->tool_result_suffix_len, false, false);
    }
    
    governor->llm_loaded = true;
    
    GOV_LOG("System prompt processed into KV cache (%d tokens)", n_tokens);
    GOV_LOG("Pre-tokenized wrappers: prefix=%d tokens, suffix=%d tokens",
            governor->tool_result_prefix_len, governor->tool_result_suffix_len);
    GOV_LOG("[Governor] Model loaded and ready");;
    
    return 0;
#endif
}

/**
 * Initialize Governor with configuration
 */
int ethervox_governor_init(
    ethervox_governor_t** governor,
    const ethervox_governor_config_t* config,
    ethervox_tool_registry_t* tool_registry
) {
    if (!governor || !tool_registry) return -1;
    
    ethervox_governor_t* gov = calloc(1, sizeof(ethervox_governor_t));
    if (!gov) return -1;
    
    // Copy config or use defaults from config.h
    if (config) {
        gov->config = *config;
    } else {
        gov->config.confidence_threshold = ETHERVOX_GOVERNOR_CONFIDENCE_THRESHOLD;
        gov->config.max_iterations = ETHERVOX_GOVERNOR_MAX_ITERATIONS;
        gov->config.max_tool_calls_per_iteration = 10;
        gov->config.timeout_seconds = ETHERVOX_GOVERNOR_TIMEOUT_SECONDS;
        gov->config.max_tokens_per_response = 2048;  // Default limit for response generation
    }
    
    gov->tool_registry = tool_registry;
    gov->initialized = true;
    gov->llm_loaded = false;
    
    *governor = gov;
    return 0;
}

/**
 * Execute Governor reasoning loop
 * 
 * Flow:
 * 1. Submit query to LLM (appending to KV cache after system prompt)
 * 2. Parse response for tool calls
 * 3. If tool calls present, execute them and add results to conversation
 * 4. If no tool calls, return the response as final answer
 * 5. Repeat until answer generated, max iterations, or timeout
 */
ethervox_governor_status_t ethervox_governor_execute(
    ethervox_governor_t* governor,
    const char* user_query,
    char** response,
    char** error,
    ethervox_confidence_metrics_t* metrics,
    ethervox_governor_progress_callback progress_callback,
    void (*token_callback)(const char* token, void* user_data),
    void* user_data
) {
    if (!governor || !governor->initialized || !user_query || !response) {
        if (error) *error = strdup("Invalid parameters");
        return ETHERVOX_GOVERNOR_ERROR;
    }
    
    // Check if LLM is loaded
    if (!governor->llm_loaded || !governor->llm_ctx) {
        if (error) *error = strdup("Governor model not loaded - call ethervox_governor_load_model first");
        return ETHERVOX_GOVERNOR_ERROR;
    }
    
#if !defined(ETHERVOX_WITH_LLAMA) || !LLAMA_HEADER_AVAILABLE
    if (error) *error = strdup("llama.cpp not available");
    return ETHERVOX_GOVERNOR_ERROR;
#else
    
    // Reset iteration counter
    governor->last_iteration_count = 0;
    
    // DO NOT clear KV cache - maintain conversation history
    // The KV cache contains: system prompt + previous conversation turns + tool results
    // Only clear if we exceed context limits
    
    // Note: For now we keep the full conversation in KV cache
    // TODO: Add conversation window management when approaching context limits
    
    const struct llama_vocab* vocab = llama_model_get_vocab(governor->llm_model);
    
    // Build conversation history in Qwen2.5 format
    // Include recent context from memory if available
    char conversation[8192];
    size_t conv_pos = 0;
    
    // Check if we have a memory store to get recent conversation turns
    // We want the actual RECENT conversation, not a search result
    // Access the memory store directly if possible through the tool registry
    
    // For now, just use the current user query without trying to inject old context
    // The memory is better used explicitly via memory_search tool when needed
    
    // Add current user query
    conv_pos += snprintf(conversation + conv_pos, sizeof(conversation) - conv_pos,
        "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", user_query);
    
    // Track how much of conversation has been processed to avoid re-processing
    size_t processed_length = 0;
    
    for (uint32_t iteration = 0; iteration < governor->config.max_iterations; iteration++) {
        governor->last_iteration_count = iteration + 1;
        
        // Notify iteration start
        if (progress_callback) {
            char iter_msg[128];
            snprintf(iter_msg, sizeof(iter_msg), "Iteration %d/%d", 
                    iteration + 1, governor->config.max_iterations);
            progress_callback(ETHERVOX_GOVERNOR_EVENT_ITERATION_START, iter_msg, user_data);
        }
        
        GOV_LOG("Governor iteration %d: %s", iteration + 1, conversation);
        
        // Notify thinking
        if (progress_callback) {
            progress_callback(ETHERVOX_GOVERNOR_EVENT_THINKING, 
                            "Analyzing query...", user_data);
        }
        
        // Generate LLM response
        // Increased to 64KB to accommodate large tool calls (e.g., file_write with substantial content)
        char llm_response_buffer[65536] = {0};
        
        // Only tokenize and decode the NEW part of the conversation (what hasn't been processed yet)
        const char* new_content = conversation + processed_length;
        size_t new_content_len = strlen(new_content);
        
        GOV_LOG("KV cache status: processed_length=%zu, conversation_length=%zu, new_content_length=%zu",
                processed_length, strlen(conversation), new_content_len);
        GOV_LOG("New content to process: '%s'", new_content_len > 0 ? new_content : "(none)");
        
        if (new_content_len > 0) {
            // Tokenize only the new content
            int n_tokens = -llama_tokenize(vocab, new_content, new_content_len, NULL, 0, false, false);
            GOV_LOG("Tokenizing %zu chars of new content into %d tokens", new_content_len, n_tokens);
            if (n_tokens <= 0) {
                if (error) *error = strdup("Failed to tokenize conversation");
                return ETHERVOX_GOVERNOR_ERROR;
            }
            
            llama_token* tokens = malloc(n_tokens * sizeof(llama_token));
            if (!tokens) {
                if (error) *error = strdup("Memory allocation failed");
                return ETHERVOX_GOVERNOR_ERROR;
            }
            
            llama_tokenize(vocab, new_content, new_content_len, tokens, n_tokens, false, false);
            
            GOV_LOG("About to decode: current_kv_pos=%d, n_tokens=%d, n_ctx=%d",
                    governor->current_kv_pos, n_tokens, llama_n_ctx(governor->llm_ctx));
            
            // Check if we're about to exceed context window
            int n_ctx = llama_n_ctx(governor->llm_ctx);
            if (governor->current_kv_pos + n_tokens > n_ctx) {
                GOV_ERROR("Context window exceeded: current_pos=%d + new_tokens=%d > n_ctx=%d",
                         governor->current_kv_pos, n_tokens, n_ctx);
                free(tokens);
                if (error) *error = strdup("Context window exceeded - conversation too long");
                return ETHERVOX_GOVERNOR_ERROR;
            }
            
            // Process tokens in chunks to respect batch size limit (n_batch = 1024)
            const int n_batch = 1024;  // llama.cpp default batch size
            int tokens_processed = 0;
            
            while (tokens_processed < n_tokens) {
                int batch_size = (n_tokens - tokens_processed > n_batch) ? n_batch : (n_tokens - tokens_processed);
                
                llama_batch batch = llama_batch_init(batch_size, 0, 1);
                batch.n_tokens = batch_size;
                for (int i = 0; i < batch_size; i++) {
                    batch.token[i] = tokens[tokens_processed + i];
                    batch.pos[i] = governor->current_kv_pos + tokens_processed + i;
                    batch.n_seq_id[i] = 1;
                    batch.seq_id[i][0] = 0;
                    batch.logits[i] = false;
                }
                // Only need logits from last token of final batch
                if (tokens_processed + batch_size >= n_tokens) {
                    batch.logits[batch_size - 1] = true;
                }
                
                GOV_LOG("Decoding batch chunk: n_tokens=%d, pos_start=%d, pos_end=%d (chunk %d/%d)", 
                        batch_size, governor->current_kv_pos + tokens_processed, 
                        governor->current_kv_pos + tokens_processed + batch_size - 1,
                        tokens_processed / n_batch + 1, (n_tokens + n_batch - 1) / n_batch);
                
                // Decode tokens (append to KV cache)
                if (llama_decode(governor->llm_ctx, batch) != 0) {
                    GOV_ERROR("Decode failed: n_tokens=%d, pos_start=%d, pos_end=%d", 
                             batch_size, governor->current_kv_pos + tokens_processed, 
                             governor->current_kv_pos + tokens_processed + batch_size - 1);
                    llama_batch_free(batch);
                    free(tokens);
                    if (error) *error = strdup("Failed to decode conversation");
                    return ETHERVOX_GOVERNOR_ERROR;
                }
                
                llama_batch_free(batch);
                tokens_processed += batch_size;
            }            free(tokens);
            
            // Update KV cache position
            governor->current_kv_pos += n_tokens;
            
            // Update processed length to include what we just processed
            processed_length = strlen(conversation);
        }
        
        // Generate response tokens - Use config.h defaults
        struct llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        
        // Add repetition penalty first to prevent loops
        llama_sampler_chain_add(sampler, llama_sampler_init_penalties(
            ETHERVOX_GOVERNOR_PENALTY_LAST_N,
            ETHERVOX_GOVERNOR_REPETITION_PENALTY,
            ETHERVOX_GOVERNOR_FREQUENCY_PENALTY,
            ETHERVOX_GOVERNOR_PRESENCE_PENALTY
        ));
        
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(ETHERVOX_GOVERNOR_TEMPERATURE));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));
        
        int generated_count = 0;
        const int max_tokens = governor->config.max_tokens_per_response;  // Use configured limit
        bool inside_tool_call = false;  // Track if we're inside a tool call
        
        while (generated_count < max_tokens) {
            llama_token next_token = llama_sampler_sample(sampler, governor->llm_ctx, -1);
            
            // Check for EOG token (proper way)
            if (llama_vocab_is_eog(vocab, next_token)) {
                break;
            }
            
            // Decode token to text
            char token_text[128];
            int n_chars = llama_token_to_piece(vocab, next_token, token_text, sizeof(token_text), 0, false);
            if (n_chars > 0 && n_chars < (int)sizeof(token_text)) {
                token_text[n_chars] = '\0';
                
                // Add to buffer first
                strncat(llm_response_buffer, token_text, sizeof(llm_response_buffer) - strlen(llm_response_buffer) - 1);
                
                // Check for stop sequences in text form (in case they're generated as text tokens)
                // Check as soon as we see the start of a stop token to prevent partial generation
                // Look for " <|" or just "<|" at the end of the buffer
                size_t buf_len = strlen(llm_response_buffer);
                if (buf_len >= 3 && strcmp(llm_response_buffer + buf_len - 3, " <|") == 0) {
                    // Truncate to remove " <|"
                    llm_response_buffer[buf_len - 3] = '\0';
                    GOV_LOG("Stop token pattern ' <|' detected, ending generation");
                    break;
                } else if (buf_len >= 2 && strcmp(llm_response_buffer + buf_len - 2, "<|") == 0) {
                    // Truncate to remove "<|"
                    llm_response_buffer[buf_len - 2] = '\0';
                    GOV_LOG("Stop token pattern '<|' detected, ending generation");
                    break;
                }
                // Also check for <|im anywhere in the buffer (middle of special token)
                if (strstr(llm_response_buffer, "<|im")) {
                    char* stop_pos = strstr(llm_response_buffer, " <|im");
                    if (!stop_pos) stop_pos = strstr(llm_response_buffer, "<|im");
                    if (stop_pos) {
                        *stop_pos = '\0';
                        GOV_LOG("Stop token pattern detected in text, removed and ending generation");
                    }
                    break;
                }
                
                // Check if we're entering or exiting a tool call
                if (!inside_tool_call && strstr(llm_response_buffer, "<tool_call")) {
                    inside_tool_call = true;
                }
                
                // Determine if we should stream this token
                bool should_stream = false;
                if (inside_tool_call) {
                    // Inside tool call - check if we're exiting
                    if (strstr(llm_response_buffer, "/>")) {
                        inside_tool_call = false;
                    }
                    // Don't stream anything while inside tool call
                    should_stream = false;
                } else {
                    // Not inside tool call - check if we might be starting one
                    size_t buf_len = strlen(llm_response_buffer);
                    const char* buf_end = llm_response_buffer + buf_len;
                    
                    // Check if buffer ends with potential tool call start patterns
                    bool might_be_tool_start = false;
                    if (buf_len >= 1 && strcmp(buf_end - 1, "<") == 0) {
                        might_be_tool_start = true;  // Just added '<'
                    } else if (buf_len >= 5 && strcmp(buf_end - 5, "<tool") == 0) {
                        might_be_tool_start = true;  // Building '<tool'
                    } else if (buf_len >= 6 && strcmp(buf_end - 6, "<tool_") == 0) {
                        might_be_tool_start = true;  // Building '<tool_'
                    } else if (buf_len >= 11 && strcmp(buf_end - 11, "<tool_call") == 0) {
                        might_be_tool_start = true;  // Just completed '<tool_call'
                    }
                    
                    // Check if token itself contains stop sequence fragments
                    bool is_stop_fragment = (
                        strstr(token_text, "im_end") != NULL ||
                        strstr(token_text, "im_start") != NULL ||
                        strstr(token_text, "|>") != NULL ||
                        strstr(token_text, "<|") != NULL ||
                        strcmp(token_text, "<") == 0 ||
                        strcmp(token_text, ">") == 0 ||
                        strcmp(token_text, "|") == 0
                    );
                    
                    // Only stream if we're sure it's not a tool call, no STOP sequences, and not a stop fragment
                    should_stream = !might_be_tool_start && 
                                   !is_stop_fragment &&
                                   !strstr(llm_response_buffer, "STOP") && 
                                   !strstr(llm_response_buffer, "<|im_end|>");
                }
                
                if (should_stream && token_callback) {
                    token_callback(token_text, user_data);
                }
            }
            
            // Check for stop sequences in the accumulated response
            if (strstr(llm_response_buffer, "<|im_end|>") || 
                strstr(llm_response_buffer, "STOP")) {
                // Remove the stop sequence from output
                char* stop_pos = strstr(llm_response_buffer, "<|im_end|>");
                if (stop_pos) *stop_pos = '\0';
                stop_pos = strstr(llm_response_buffer, "STOP");
                if (stop_pos) *stop_pos = '\0';
                GOV_LOG("Stop sequence detected, ending generation");
                break;
            }
            
            // Feed token back to context for next prediction with explicit position
            // Check if we're about to exceed context window
            int n_ctx = llama_n_ctx(governor->llm_ctx);
            if (governor->current_kv_pos >= n_ctx) {
                GOV_ERROR("Context window exceeded during generation: current_pos=%d >= n_ctx=%d",
                         governor->current_kv_pos, n_ctx);
                break;  // Stop generation gracefully
            }
            
            llama_batch next_batch = llama_batch_init(1, 0, 1);
            next_batch.n_tokens = 1;
            next_batch.token[0] = next_token;
            next_batch.pos[0] = governor->current_kv_pos;
            next_batch.n_seq_id[0] = 1;
            next_batch.seq_id[0][0] = 0;
            next_batch.logits[0] = true;
            
            if (llama_decode(governor->llm_ctx, next_batch) != 0) {
                llama_batch_free(next_batch);
                break;
            }
            llama_batch_free(next_batch);
            
            governor->current_kv_pos++;
            generated_count++;
            
            // Early stopping conditions to prevent hallucination
            // 1. Stop immediately after tool_call closing tag
            if (strstr(llm_response_buffer, "<tool_call") && strstr(llm_response_buffer, "/>")) {
                GOV_LOG("Early stop: Tool call completed (%d tokens)", generated_count);
                break;  // Tool call complete, stop immediately
            }
            
            // 2. Stop if we see <|im_start|> or <tool_result> (model is hallucinating examples from system prompt)
            if (strstr(llm_response_buffer, "<|im_start") || strstr(llm_response_buffer, "<tool_result")) {
                // Truncate the buffer to remove the hallucinated content
                char* hallucination_start = strstr(llm_response_buffer, "<|im_start");
                if (!hallucination_start) hallucination_start = strstr(llm_response_buffer, "<tool_result");
                if (hallucination_start) *hallucination_start = '\0';
                
                // Also remove any <|im variant before it
                char* end_marker = strstr(llm_response_buffer, " <|im");
                if (!end_marker) end_marker = strstr(llm_response_buffer, "<|im");
                if (end_marker) *end_marker = '\0';
                
                GOV_LOG("Early stop: Hallucination detected and truncated (%d tokens)", generated_count);
                break;
            }
        }
        
        llama_sampler_free(sampler);
        
        GOV_LOG("Generation complete: %d tokens generated", generated_count);
        
        // Clean up any stop tokens that made it into the output
        // Check for all possible partial versions starting with <|im
        char* stop_marker = strstr(llm_response_buffer, " <|im");
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, "<|im");
        if (stop_marker) {
            *stop_marker = '\0';
        }
        
        const char* llm_response = llm_response_buffer;
        GOV_LOG("Generated response: %s", llm_response);
        
        if (metrics) {
            metrics->iteration_count = iteration + 1;
            metrics->tool_calls_made = 0;
        }
        
        // Extract tool calls
        char* tool_calls[10];
        int num_tools = extract_tool_calls(llm_response, tool_calls, 10);
        
        if (metrics) {
            metrics->tool_calls_made = num_tools;
        }
        
        // Execute tools if present
        if (num_tools > 0) {
            for (int i = 0; i < num_tools; i++) {
                char* tool_result = NULL;
                char* tool_error = NULL;
                
                // Extract tool name for progress notification
                char* tool_name = parse_attribute(tool_calls[i], "name");
                if (tool_name && progress_callback) {
                    char tool_msg[256];
                    snprintf(tool_msg, sizeof(tool_msg), "Calling tool: %s", tool_name);
                    progress_callback(ETHERVOX_GOVERNOR_EVENT_TOOL_CALL, tool_msg, user_data);
                }
                
                int status = execute_tool_call(
                    tool_calls[i],
                    governor->tool_registry,
                    &tool_result,
                    &tool_error
                );
                
                if (status == 0 && tool_result) {
                    GOV_LOG("Tool '%s' called and e successfully, result length: %zu", 
                            tool_name ? tool_name : "unknown", strlen(tool_result));
                    GOV_LOG("Tool result content: %s", tool_result);
                    
                    // Notify tool result (simplified - avoid formatting in hot path)
                    if (progress_callback) {
                        progress_callback(ETHERVOX_GOVERNOR_EVENT_TOOL_RESULT, tool_result, user_data);
                    }
                    
                    // Use pre-tokenized wrappers for speed - bypass string operations entirely
                    // Instead of: concatenate string → tokenize → decode
                    // Do: decode prefix tokens → decode result tokens → decode suffix tokens
                    
                    // Decode prefix: "<|im_start|>user\n<tool_result>"
                    if (governor->tool_result_prefix_tokens && governor->tool_result_prefix_len > 0) {
                        llama_batch prefix_batch = llama_batch_init(governor->tool_result_prefix_len, 0, 1);
                        prefix_batch.n_tokens = governor->tool_result_prefix_len;
                        for (int j = 0; j < governor->tool_result_prefix_len; j++) {
                            prefix_batch.token[j] = governor->tool_result_prefix_tokens[j];
                            prefix_batch.pos[j] = governor->current_kv_pos + j;
                            prefix_batch.n_seq_id[j] = 1;
                            prefix_batch.seq_id[j][0] = 0;
                            prefix_batch.logits[j] = false;
                        }
                        llama_decode(governor->llm_ctx, prefix_batch);
                        llama_batch_free(prefix_batch);
                        governor->current_kv_pos += governor->tool_result_prefix_len;
                    }
                    
                    // Tokenize and decode actual tool result (in chunks if large)
                    size_t result_len = strlen(tool_result);
                    int result_n_tokens = -llama_tokenize(vocab, tool_result, result_len, NULL, 0, false, false);
                    if (result_n_tokens > 0) {
                        // Check if tool result will fit in context
                        int n_ctx = llama_n_ctx(governor->llm_ctx);
                        if (governor->current_kv_pos + result_n_tokens > n_ctx) {
                            GOV_LOG("Tool result too large: %d tokens would exceed context (pos=%d, ctx=%d). Truncating.",
                                    result_n_tokens, governor->current_kv_pos, n_ctx);
                            // Truncate to fit
                            result_n_tokens = n_ctx - governor->current_kv_pos;
                            if (result_n_tokens <= 0) {
                                GOV_ERROR("No context space remaining for tool result");
                                continue;  // Skip this tool result
                            }
                        }
                        
                        llama_token* result_tokens = malloc(result_n_tokens * sizeof(llama_token));
                        if (result_tokens) {
                            llama_tokenize(vocab, tool_result, result_len, result_tokens, result_n_tokens, false, false);
                            
                            // Process in chunks to respect batch size limit
                            const int n_batch = 1024;
                            int tokens_processed = 0;
                            
                            while (tokens_processed < result_n_tokens) {
                                int batch_size = (result_n_tokens - tokens_processed > n_batch) ? n_batch : (result_n_tokens - tokens_processed);
                                
                                llama_batch result_batch = llama_batch_init(batch_size, 0, 1);
                                result_batch.n_tokens = batch_size;
                                for (int j = 0; j < batch_size; j++) {
                                    result_batch.token[j] = result_tokens[tokens_processed + j];
                                    result_batch.pos[j] = governor->current_kv_pos + j;
                                    result_batch.n_seq_id[j] = 1;
                                    result_batch.seq_id[j][0] = 0;
                                    result_batch.logits[j] = false;
                                }
                                
                                if (llama_decode(governor->llm_ctx, result_batch) != 0) {
                                    GOV_ERROR("Failed to decode tool result chunk at pos %d", governor->current_kv_pos);
                                    llama_batch_free(result_batch);
                                    break;
                                }
                                llama_batch_free(result_batch);
                                
                                governor->current_kv_pos += batch_size;
                                tokens_processed += batch_size;
                            }
                            
                            free(result_tokens);
                        }
                    }
                    
                    // Decode suffix: "</tool_result><|im_end|>\n<|im_start|>assistant\n"
                    if (governor->tool_result_suffix_tokens && governor->tool_result_suffix_len > 0) {
                        llama_batch suffix_batch = llama_batch_init(governor->tool_result_suffix_len, 0, 1);
                        suffix_batch.n_tokens = governor->tool_result_suffix_len;
                        for (int j = 0; j < governor->tool_result_suffix_len; j++) {
                            suffix_batch.token[j] = governor->tool_result_suffix_tokens[j];
                            suffix_batch.pos[j] = governor->current_kv_pos + j;
                            suffix_batch.n_seq_id[j] = 1;
                            suffix_batch.seq_id[j][0] = 0;
                            suffix_batch.logits[j] = (j == governor->tool_result_suffix_len - 1);  // Only last needs logits
                        }
                        llama_decode(governor->llm_ctx, suffix_batch);
                        llama_batch_free(suffix_batch);
                        governor->current_kv_pos += governor->tool_result_suffix_len;
                    }
                    
                    // Update processed_length - no string concat needed
                    processed_length = strlen(conversation);
                    
                    free(tool_result);
                } else if (tool_error) {
                    // Format and add tool error to LLM context
                    char error_msg[512];
                    snprintf(error_msg, sizeof(error_msg),
                        "<tool_error>%s</tool_error>", tool_error);
                    
                    GOV_LOG("Tool error: %s", tool_error);
                    
                    // Notify error
                    if (progress_callback) {
                        progress_callback(ETHERVOX_GOVERNOR_EVENT_TOOL_ERROR, error_msg, user_data);
                    }
                    
                    // Add error tokens to LLM context using same approach as tool_result
                    // Decode prefix: "<|im_start|>user\n" (error is treated like a tool_result for format purposes)
                    const char* error_prefix = "<|im_start|>user\n";
                    int prefix_n_tokens = -llama_tokenize(vocab, error_prefix, strlen(error_prefix), NULL, 0, false, false);
                    if (prefix_n_tokens > 0) {
                        llama_token* prefix_tokens = malloc(prefix_n_tokens * sizeof(llama_token));
                        if (prefix_tokens) {
                            llama_tokenize(vocab, error_prefix, strlen(error_prefix), prefix_tokens, prefix_n_tokens, false, false);
                            llama_batch prefix_batch = llama_batch_init(prefix_n_tokens, 0, 1);
                            prefix_batch.n_tokens = prefix_n_tokens;
                            for (int j = 0; j < prefix_n_tokens; j++) {
                                prefix_batch.token[j] = prefix_tokens[j];
                                prefix_batch.pos[j] = governor->current_kv_pos + j;
                                prefix_batch.n_seq_id[j] = 1;
                                prefix_batch.seq_id[j][0] = 0;
                                prefix_batch.logits[j] = false;
                            }
                            llama_decode(governor->llm_ctx, prefix_batch);
                            llama_batch_free(prefix_batch);
                            governor->current_kv_pos += prefix_n_tokens;
                            free(prefix_tokens);
                        }
                    }
                    
                    // Add error message tokens
                    int error_n_tokens = -llama_tokenize(vocab, error_msg, strlen(error_msg), NULL, 0, false, false);
                    if (error_n_tokens > 0) {
                        int n_ctx = llama_n_ctx(governor->llm_ctx);
                        if (governor->current_kv_pos + error_n_tokens > n_ctx) {
                            error_n_tokens = n_ctx - governor->current_kv_pos;
                            if (error_n_tokens <= 0) {
                                GOV_ERROR("No context space for tool error");
                                free(tool_error);
                                if (tool_name) free(tool_name);
                                free(tool_calls[i]);
                                continue;
                            }
                        }
                        
                        llama_token* error_tokens = malloc(error_n_tokens * sizeof(llama_token));
                        if (error_tokens) {
                            llama_tokenize(vocab, error_msg, strlen(error_msg), error_tokens, error_n_tokens, false, false);
                            
                            const int n_batch = 1024;
                            int tokens_processed = 0;
                            while (tokens_processed < error_n_tokens) {
                                int batch_size = (error_n_tokens - tokens_processed > n_batch) ? n_batch : (error_n_tokens - tokens_processed);
                                llama_batch error_batch = llama_batch_init(batch_size, 0, 1);
                                error_batch.n_tokens = batch_size;
                                for (int j = 0; j < batch_size; j++) {
                                    error_batch.token[j] = error_tokens[tokens_processed + j];
                                    error_batch.pos[j] = governor->current_kv_pos + j;
                                    error_batch.n_seq_id[j] = 1;
                                    error_batch.seq_id[j][0] = 0;
                                    error_batch.logits[j] = false;
                                }
                                llama_decode(governor->llm_ctx, error_batch);
                                llama_batch_free(error_batch);
                                governor->current_kv_pos += batch_size;
                                tokens_processed += batch_size;
                            }
                            free(error_tokens);
                        }
                    }
                    
                    // Add suffix: "<|im_end|>\n<|im_start|>assistant\n"
                    const char* error_suffix = "<|im_end|>\n<|im_start|>assistant\n";
                    int suffix_n_tokens = -llama_tokenize(vocab, error_suffix, strlen(error_suffix), NULL, 0, false, false);
                    if (suffix_n_tokens > 0) {
                        llama_token* suffix_tokens = malloc(suffix_n_tokens * sizeof(llama_token));
                        if (suffix_tokens) {
                            llama_tokenize(vocab, error_suffix, strlen(error_suffix), suffix_tokens, suffix_n_tokens, false, false);
                            llama_batch suffix_batch = llama_batch_init(suffix_n_tokens, 0, 1);
                            suffix_batch.n_tokens = suffix_n_tokens;
                            for (int j = 0; j < suffix_n_tokens; j++) {
                                suffix_batch.token[j] = suffix_tokens[j];
                                suffix_batch.pos[j] = governor->current_kv_pos + j;
                                suffix_batch.n_seq_id[j] = 1;
                                suffix_batch.seq_id[j][0] = 0;
                                suffix_batch.logits[j] = (j == suffix_n_tokens - 1);
                            }
                            llama_decode(governor->llm_ctx, suffix_batch);
                            llama_batch_free(suffix_batch);
                            governor->current_kv_pos += suffix_n_tokens;
                            free(suffix_tokens);
                        }
                    }
                    
                    free(tool_error);
                }
                
                if (tool_name) free(tool_name);
                free(tool_calls[i]);
            }
            
            // If we executed tools, continue iteration to let LLM incorporate results
            // (unless we're at max iterations)
            if (iteration + 1 < governor->config.max_iterations) {
                continue;
            }
        } else {
            // No tools executed - this is a final response
            // Return the response and mark as complete
            if (response) {
                *response = strdup(llm_response);
                if (!*response) {
                    if (error) *error = strdup("Memory allocation failed");
                    return ETHERVOX_GOVERNOR_ERROR;
                }
            }
            
            if (progress_callback) {
                progress_callback(ETHERVOX_GOVERNOR_EVENT_COMPLETE, 
                                "Answer ready", user_data);
            }
            
            return ETHERVOX_GOVERNOR_SUCCESS;
        }
    }
    
    // Reached max iterations
    if (error) {
        *error = strdup("Maximum iterations reached without sufficient confidence");
    }
    return ETHERVOX_GOVERNOR_TIMEOUT;
    
#endif  // ETHERVOX_WITH_LLAMA && LLAMA_HEADER_AVAILABLE
}

/**
 * Get the number of iterations used in last execute call
 */
uint32_t ethervox_governor_get_last_iteration_count(ethervox_governor_t* governor) {
    return governor ? governor->last_iteration_count : 0;
}

/**
 * Cleanup Governor resources
 */
void ethervox_governor_cleanup(ethervox_governor_t* governor) {
    if (!governor) return;
    
#if defined(ETHERVOX_WITH_LLAMA) && LLAMA_HEADER_AVAILABLE
    if (governor->llm_ctx) {
        llama_free(governor->llm_ctx);
        governor->llm_ctx = NULL;
    }
    if (governor->llm_model) {
        llama_model_free(governor->llm_model);
        governor->llm_model = NULL;
    }
    if (governor->model_path) {
        free(governor->model_path);
        governor->model_path = NULL;
    }
    
    // Free pre-tokenized wrappers
    if (governor->tool_result_prefix_tokens) {
        free(governor->tool_result_prefix_tokens);
        governor->tool_result_prefix_tokens = NULL;
    }
    if (governor->tool_result_suffix_tokens) {
        free(governor->tool_result_suffix_tokens);
        governor->tool_result_suffix_tokens = NULL;
    }
    
    llama_backend_free();
#endif
    
    free(governor);
}

ethervox_tool_registry_t* ethervox_governor_get_registry(ethervox_governor_t* governor) {
    return governor ? governor->tool_registry : NULL;
}
