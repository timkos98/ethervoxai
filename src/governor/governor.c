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
#include "ethervox/chat_template.h"
#include "ethervox/config.h"
#include "ethervox/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#include <llama.h>
#include <ggml.h>
#define LLAMA_HEADER_AVAILABLE 1
#else
#define LLAMA_HEADER_AVAILABLE 0
#endif

#define GOV_LOG(...) ETHERVOX_LOGI(__VA_ARGS__)
#define GOV_ERROR(...) ETHERVOX_LOGE(__VA_ARGS__)

// Global flag to track llama backend initialization (should only happen once per process)
static bool g_llama_backend_initialized = false;

// Global flag to detect model corruption from llama.cpp error messages
static bool g_model_corruption_detected = false;

// Forward declarations for helper functions (defined after struct definition)
#if defined(ETHERVOX_WITH_LLAMA) && LLAMA_HEADER_AVAILABLE
static bool parse_memory_search_text(const char* json_result, char* text_out, size_t text_out_size);
static int load_tokens_to_kv_cache(struct ethervox_governor* governor, const llama_token* tokens, int n_tokens, const char* log_prefix);
static int load_conversation_summary(struct ethervox_governor* governor, const char* tag_filter_json, const char* context_prefix, const char* log_prefix);
#endif

// GGML log callback to capture llama.cpp errors
static void governor_ggml_log_callback(enum ggml_log_level level, const char * text, void * user_data) {
    (void)user_data;
    // Remove trailing newline if present
    size_t len = strlen(text);
    char* clean_text = (char*)alloca(len + 1);
    strcpy(clean_text, text);
    if (len > 0 && clean_text[len - 1] == '\n') {
        clean_text[len - 1] = '\0';
    }
    
    // Detect corruption in error messages
    if (level == GGML_LOG_LEVEL_ERROR) {
        if (strstr(clean_text, "corrupted") != NULL || 
            strstr(clean_text, "incomplete") != NULL ||
            strstr(clean_text, "not within the file bounds") != NULL) {
            g_model_corruption_detected = true;
            GOV_ERROR("[llama.cpp] CORRUPTION DETECTED: %s", clean_text);
        } else {
            GOV_ERROR("[llama.cpp] %s", clean_text);
        }
    } else if (level == GGML_LOG_LEVEL_WARN) {
        GOV_LOG("[llama.cpp WARN] %s", clean_text);
    } else {
        GOV_LOG("[llama.cpp] %s", clean_text);
    }
}

// Progress callback for model loading
static bool governor_load_progress_callback(float progress, void * user_data) {
    (void)user_data;
    GOV_LOG("[Governor] Model load progress: %.1f%%", progress * 100.0f);
    return true;  // Continue loading
}

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
    
    // Saved system prompt for recovery after nuclear clear
    llama_token* system_prompt_tokens;
    int system_prompt_tokens_len;
    
    // Pre-tokenized static wrappers for speed optimization (from chat_template)
    llama_token* tool_result_prefix_tokens;  // chat_template->tool_result_start
    int tool_result_prefix_len;
    llama_token* tool_result_suffix_tokens;  // chat_template->tool_result_end
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
    bool tool_execution_enabled;  // Allow disabling tool execution (for optimization)
    bool system_prompt_lost;      // Track if nuclear clear wiped the system prompt
    bool tools_available;          // Track if tools are enabled (false in minimal mode)
    volatile bool interrupt_requested;  // Set by interrupt callback to abort generation
    
    // Context management
    context_manager_state_t context_manager;
    conversation_history_t conversation_history;
    uint32_t turn_counter;
    
    // Chat template for formatting
    const chat_template_t* chat_template;
};

// ============================================================================
// Helper Function Implementations (Summary Loading)
// ============================================================================

#if defined(ETHERVOX_WITH_LLAMA) && LLAMA_HEADER_AVAILABLE

/**
 * Parse JSON memory search result to extract the "text" field
 * Format: {"results":[{"text":"...", "relevance":0.95}], "count":1}
 */
static bool parse_memory_search_text(const char* json_result, char* text_out, size_t text_out_size) {
    if (!json_result || !text_out || text_out_size == 0) {
        return false;
    }
    
    text_out[0] = '\0';
    
    char* text_start = strstr(json_result, "\"text\":\"");
    if (!text_start) {
        return false;
    }
    
    text_start += 8; // Skip past "text":"
    char* text_end = strstr(text_start, "\",\"relevance\"");
    if (!text_end) {
        text_end = strstr(text_start, "\"}");
    }
    
    if (text_end) {
        size_t len = text_end - text_start;
        if (len < text_out_size) {
            strncpy(text_out, text_start, len);
            text_out[len] = '\0';
            return true;
        }
    }
    
    return false;
}

/**
 * Load tokenized text into KV cache in chunks
 * Returns number of tokens loaded, or -1 on error
 */
static int load_tokens_to_kv_cache(
    struct ethervox_governor* governor,
    const llama_token* tokens,
    int n_tokens,
    const char* log_prefix
) {
    if (!governor || !tokens || n_tokens <= 0) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    int chunk_size = 512;
    for (int i = 0; i < n_tokens; i += chunk_size) {
        int chunk_len = (i + chunk_size > n_tokens) ? (n_tokens - i) : chunk_size;
        
        llama_batch batch = llama_batch_init(chunk_len, 0, 1);
        batch.n_tokens = chunk_len;
        for (int j = 0; j < chunk_len; j++) {
            batch.token[j] = tokens[i + j];
            batch.pos[j] = governor->current_kv_pos + i + j;  // Account for outer loop position
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j] = false;
        }
        
        int decode_result = llama_decode(governor->llm_ctx, batch);
        llama_batch_free(batch);
        
        if (decode_result != 0) {
            GOV_ERROR("%s: Failed to decode chunk %d/%d (error %d, tokens %d-%d/%d)", 
                      log_prefix, i / chunk_size + 1, (n_tokens + chunk_size - 1) / chunk_size,
                      decode_result, i, i + chunk_len - 1, n_tokens);
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
    }
    
    // Update position after all chunks processed
    governor->current_kv_pos += n_tokens;
    return n_tokens;
}

/**
 * Search memory for conversation summary and load into KV cache
 * Returns 0 on success (even if no summary found), -1 on error
 */
static int load_conversation_summary(
    struct ethervox_governor* governor,
    const char* tag_filter_json,
    const char* context_prefix,
    const char* log_prefix
) {
    if (!governor || !tag_filter_json || !context_prefix || !log_prefix) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Find memory_search tool
    ethervox_tool_t* memory_search_tool = NULL;
    for (uint32_t i = 0; i < governor->tool_registry->tool_count; i++) {
        if (strcmp(governor->tool_registry->tools[i].name, "memory_search") == 0) {
            memory_search_tool = &governor->tool_registry->tools[i];
            break;
        }
    }
    
    if (!memory_search_tool) {
        GOV_LOG("%s: memory_search tool not available", log_prefix);
        return ETHERVOX_SUCCESS;  // Not an error, just unavailable
    }
    
    // Build search args
    char search_args[512];
    snprintf(search_args, sizeof(search_args),
        "{\"query\":null,"
        "\"tag_filter\":%s,"
        "\"limit\":1}",
        tag_filter_json);
    
    char* search_result = NULL;
    char* search_error = NULL;
    
    int search_status = memory_search_tool->execute(search_args, &search_result, &search_error);
    
    if (search_status != 0 || !search_result) {
        GOV_LOG("%s: No previous conversation summary found", log_prefix);
        free(search_result);
        free(search_error);
        return ETHERVOX_SUCCESS;  // Not an error
    }
    
    // Parse JSON to extract text
    char summary_text[4096];
    if (!parse_memory_search_text(search_result, summary_text, sizeof(summary_text))) {
        GOV_LOG("%s: No valid summary found in memory (empty or malformed)", log_prefix);
        free(search_result);
        free(search_error);
        return ETHERVOX_SUCCESS;
    }
    
    free(search_result);
    free(search_error);
    
    // Strip metadata header if present (e.g., "[Manual Cache Clear - Context Summary]\\n\\n")
    char* actual_summary = summary_text;
    if (summary_text[0] == '[') {
        // Look for end of header (]\n\n or ]\\ n\\ n for escaped)
        char* header_end = strstr(summary_text, "]\\n\\n");
        if (header_end) {
            actual_summary = header_end + 5;  // Skip "]\n\n" (with escapes: 5 chars)
        } else {
            header_end = strstr(summary_text, "]\n\n");
            if (header_end) {
                actual_summary = header_end + 3;  // Skip "]\n\n"
            }
        }
    }
    
    // Additional validation: check summary length is reasonable
    size_t summary_len = strlen(actual_summary);
    if (summary_len < 10) {
        GOV_ERROR("%s: Summary too short (%zu chars), likely corrupt", log_prefix, summary_len);
        return ETHERVOX_SUCCESS;
    }
    if (summary_len > 3500) {
        GOV_LOG("%s: Warning - summary unusually long (%zu chars)", log_prefix, summary_len);
    }
    
    GOV_LOG("%s: Found conversation summary (%zu chars)", log_prefix, summary_len);
    
    // Build context restoration prompt
    char context_restore_prompt[8192];
    snprintf(context_restore_prompt, sizeof(context_restore_prompt),
        "%s\n%s\n\n%s",
        context_prefix,
        actual_summary,  // Use stripped summary without metadata header
        strcmp(log_prefix, "STARTUP") == 0 
            ? "Ready to continue our conversation."
            : "Continue conversation naturally based on this context.");
    
    // Tokenize
    llama_token* restore_tokens = (llama_token*)malloc(2048 * sizeof(llama_token));
    if (!restore_tokens) {
        GOV_ERROR("%s: Failed to allocate token buffer", log_prefix);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    const struct llama_vocab* vocab = llama_model_get_vocab(governor->llm_model);
    int n_restore = llama_tokenize(
        vocab,
        context_restore_prompt,
        strlen(context_restore_prompt),
        restore_tokens,
        2048,
        true,   // add_special
        false   // parse_special
    );
    
    if (n_restore <= 0) {
        GOV_ERROR("%s: Failed to tokenize summary (returned %d)", log_prefix, n_restore);
        free(restore_tokens);
        return ETHERVOX_SUCCESS;  // Not a fatal error
    }
    
    if (n_restore >= 2048) {
        GOV_ERROR("%s: Summary tokenized to maximum buffer size (%d), likely truncated", log_prefix, n_restore);
        free(restore_tokens);
        return ETHERVOX_SUCCESS;
    }
    
    // Sanity check: reasonable token count for a summary
    if (n_restore < 5) {
        GOV_ERROR("%s: Summary produced too few tokens (%d), likely corrupt", log_prefix, n_restore);
        free(restore_tokens);
        return ETHERVOX_SUCCESS;
    }
    
    GOV_LOG("%s: Tokenized summary into %d tokens", log_prefix, n_restore);
    
    // Load into KV cache
    int loaded = load_tokens_to_kv_cache(governor, restore_tokens, n_restore, log_prefix);
    free(restore_tokens);
    
    if (loaded > 0) {
        GOV_LOG("%s: Loaded %d context tokens into KV cache", log_prefix, loaded);
        GOV_LOG("%s: Context restoration complete", log_prefix);
    }
    
    return loaded > 0 ? 0 : -1;
}

#endif // ETHERVOX_WITH_LLAMA

// ============================================================================
// End Helper Function Implementations
// ============================================================================

/**
 * Extract tool calls from LLM response
 * Looks for <tool_call name="..." attr="..." /> tags
 * 
 * Returns: Number of tool calls found, fills tool_calls array
 */
static int extract_tool_calls(const char* response, char** tool_calls, int max_calls) {
    if (!response || !tool_calls) return ETHERVOX_SUCCESS;
    
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
 * Extract JSON-format tool calls from LLM response (Granite 4.0 format)
 * Looks for <tool_call>\n{"name": "...", "arguments": {...}}\n</tool_call>
 * 
 * Returns: Number of tool calls found, fills tool_calls array with JSON strings
 */
static int extract_tool_calls_json(const char* response, char** tool_calls, int max_calls) {
    if (!response || !tool_calls) return ETHERVOX_SUCCESS;
    
    int count = 0;
    const char* pos = response;
    
    while (count < max_calls) {
        // Find opening tag
        const char* start = strstr(pos, "<tool_call>");
        if (!start) break;
        
        const char* json_start = start + 11;  // Skip "<tool_call>"
        
        // Find closing tag
        const char* end = strstr(json_start, "</tool_call>");
        if (!end) break;
        
        // Skip whitespace at start of JSON
        while (json_start < end && (*json_start == ' ' || *json_start == '\n' || *json_start == '\r' || *json_start == '\t')) {
            json_start++;
        }
        
        // Find actual JSON end (skip trailing whitespace)
        const char* json_end = end;
        while (json_end > json_start && (*(json_end-1) == ' ' || *(json_end-1) == '\n' || *(json_end-1) == '\r' || *(json_end-1) == '\t')) {
            json_end--;
        }
        
        // Extract JSON content
        size_t json_len = json_end - json_start;
        if (json_len > 0) {
            tool_calls[count] = malloc(json_len + 1);
            if (!tool_calls[count]) break;
            
            strncpy(tool_calls[count], json_start, json_len);
            tool_calls[count][json_len] = '\0';
            
            count++;
        }
        
        pos = end + 12;  // Move past "</tool_call>"
    }
    
    return count;
}

// ============================================================================
// Context Management Helper Functions
// ============================================================================

/**
 * Check context health status based on current KV cache usage
 */
static context_health_t check_context_health(ethervox_governor_t* gov) {
#if defined(ETHERVOX_WITH_LLAMA) && LLAMA_HEADER_AVAILABLE
    int n_ctx = llama_n_ctx(gov->llm_ctx);
    int current_pos = gov->current_kv_pos;
    float usage = (float)current_pos / n_ctx;
    
    if (usage < 0.60f) return CTX_HEALTH_OK;
    if (usage < 0.80f) return CTX_HEALTH_WARNING;
    if (usage < 0.95f) return CTX_HEALTH_CRITICAL;
    return CTX_HEALTH_OVERFLOW;
#else
    return CTX_HEALTH_OK;  // No monitoring without llama.cpp
#endif
}

/**
 * Initialize conversation history
 */
static int init_conversation_history(conversation_history_t* history, uint32_t initial_capacity) {
    history->turns = malloc(initial_capacity * sizeof(conversation_turn_t));
    if (!history->turns) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    
    history->turn_count = 0;
    history->capacity = initial_capacity;
    return ETHERVOX_SUCCESS;
}

/**
 * Append a turn to conversation history
 */
static int append_turn(conversation_history_t* history, const conversation_turn_t* turn) {
    // Resize if needed
    if (history->turn_count >= history->capacity) {
        uint32_t new_capacity = history->capacity * 2;
        conversation_turn_t* new_turns = realloc(history->turns, 
                                                 new_capacity * sizeof(conversation_turn_t));
        if (!new_turns) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        
        history->turns = new_turns;
        history->capacity = new_capacity;
    }
    
    history->turns[history->turn_count++] = *turn;
    return ETHERVOX_SUCCESS;
}

/**
 * Cleanup conversation history
 */
static void cleanup_conversation_history(conversation_history_t* history) {
    if (history->turns) {
        free(history->turns);
        history->turns = NULL;
    }
    history->turn_count = 0;
    history->capacity = 0;
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
 * Execute a single tool call (JSON format for Granite 4.0)
 * Input: JSON string like {"name": "calculator_compute", "arguments": {"expression": "17*23"}}
 * Extracts name and arguments, calls tool
 */
static int execute_tool_call_json(
    const char* tool_call_json,
    ethervox_tool_registry_t* registry,
    char** result,
    char** error
) {
    if (!tool_call_json || !registry || !result || !error) {
        if (error) *error = strdup("Invalid parameters passed to execute_tool_call_json");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Simple JSON parsing - find "name" field
    const char* name_start = strstr(tool_call_json, "\"name\"");
    if (!name_start) {
        *error = strdup("Missing 'name' field in JSON tool call");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Find the value after "name":
    const char* name_value_start = strchr(name_start, ':');
    if (!name_value_start) {
        *error = strdup("Malformed 'name' field in JSON tool call");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    name_value_start++;
    
    // Skip whitespace and opening quote
    while (*name_value_start == ' ' || *name_value_start == '\t' || *name_value_start == '\n') {
        name_value_start++;
    }
    if (*name_value_start == '"') name_value_start++;
    
    // Find end quote
    const char* name_value_end = strchr(name_value_start, '"');
    if (!name_value_end) {
        *error = strdup("Malformed 'name' value in JSON tool call");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Extract tool name
    size_t name_len = name_value_end - name_value_start;
    char* tool_name = malloc(name_len + 1);
    if (!tool_name) {
        *error = strdup("Memory allocation failed for tool name");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    strncpy(tool_name, name_value_start, name_len);
    tool_name[name_len] = '\0';
    
    // DEBUG: List all registered tools
    GOV_LOG("Looking for tool '%s' in registry with %u tools:", tool_name, registry->tool_count);
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        GOV_LOG("  [%u] %s", i, registry->tools[i].name);
    }
    
    // Find tool in registry
    const ethervox_tool_t* tool = ethervox_tool_registry_find(registry, tool_name);
    if (!tool) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Unknown tool: %s", tool_name);
        *error = strdup(err_msg);
        free(tool_name);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Find "arguments" field
    const char* args_start = strstr(tool_call_json, "\"arguments\"");
    char json_input[65536] = "{}";  // Default empty object
    
    if (args_start) {
        const char* args_value_start = strchr(args_start, ':');
        if (args_value_start) {
            args_value_start++;
            
            // Skip whitespace
            while (*args_value_start == ' ' || *args_value_start == '\t' || *args_value_start == '\n') {
                args_value_start++;
            }
            
            // Find the opening brace
            if (*args_value_start == '{') {
                int brace_count = 0;
                const char* p = args_value_start;
                const char* args_end = NULL;
                
                // Find matching closing brace
                while (*p) {
                    if (*p == '{') brace_count++;
                    else if (*p == '}') {
                        brace_count--;
                        if (brace_count == 0) {
                            args_end = p + 1;
                            break;
                        }
                    }
                    p++;
                }
                
                if (args_end) {
                    size_t args_len = args_end - args_value_start;
                    if (args_len < sizeof(json_input)) {
                        strncpy(json_input, args_value_start, args_len);
                        json_input[args_len] = '\0';
                    }
                }
            }
        }
    }
    
    GOV_LOG("Executing tool '%s' with JSON: %s", tool->name, json_input);
    
    free(tool_name);
    
    // Safety checks before execution
    if (!tool->execute) {
        *error = strdup("Tool has NULL execute pointer");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    if (!result || !error) {
        GOV_ERROR("Invalid result or error pointers passed to execute_tool_call_json");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Resource checks for audio tools (speak/listen)
    // Note: The tools themselves handle graceful degradation, but we can provide
    // better error messages here if execution context indicates resources unavailable
    if (strcmp(tool->name, "speak") == 0 || strcmp(tool->name, "listen") == 0) {
        GOV_LOG("Audio tool '%s' requested - resource availability will be checked by tool callback", tool->name);
    }
    
    // Execute the tool
    int exec_result = tool->execute(json_input, result, error);
    
    if (exec_result != 0) {
        if (!*error) {
            *error = strdup("Tool execution failed (no error message provided)");
        }
        GOV_ERROR("Tool '%s' failed: %s", tool->name, *error);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    if (!*result) {
        *result = strdup("(no output)");
    }
    
    GOV_LOG("Tool '%s' succeeded: %s", tool->name, *result);
    return ETHERVOX_SUCCESS;
}

/**
 * Execute a single tool call (XML attribute format)
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
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Find tool in registry
    const ethervox_tool_t* tool = ethervox_tool_registry_find(registry, tool_name);
    if (!tool) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Unknown tool: %s", tool_name);
        *error = strdup(err_msg);
        free(tool_name);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
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
        "directory", "file_path", "pattern", "recursive", "path",
        "enable", "description",
        // Memory tools
        "text", "new_text", "content", "tags", "query", "limit", "window_size", "format",
        "importance", "min_importance", "max_age_hours", "is_user",
        "memory_id", "memory_ids", "filepath",
        "older_than_seconds", "importance_threshold",
        "tag_filter", "focus_topic", "correction",
        // Startup prompt tools
        "prompt_text",
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
                               strcmp(attrs[i], "path") == 0 ||
                               strcmp(attrs[i], "tags") == 0 ||
                               strcmp(attrs[i], "query") == 0 ||
                               strcmp(attrs[i], "text") == 0 ||
                               strcmp(attrs[i], "new_text") == 0 ||
                               strcmp(attrs[i], "content") == 0 ||
                               strcmp(attrs[i], "directory") == 0 ||
                               strcmp(attrs[i], "pattern") == 0 ||
                               strcmp(attrs[i], "format") == 0 ||
                               strcmp(attrs[i], "label") == 0 ||
                               strcmp(attrs[i], "description") == 0 ||
                               strcmp(attrs[i], "prompt_text") == 0 ||
                               strcmp(attrs[i], "tag_filter") == 0 ||
                               strcmp(attrs[i], "focus_topic") == 0 ||
                               strcmp(attrs[i], "correction") == 0);
            
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
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    if (!result || !error) {
        GOV_ERROR("Invalid result or error pointers passed to execute_tool_call");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Resource checks for audio tools (speak/listen)
    // Note: The tools themselves handle graceful degradation, but we can provide
    // better error messages here if execution context indicates resources unavailable
    if (strcmp(tool->name, "speak") == 0 || strcmp(tool->name, "listen") == 0) {
        GOV_LOG("Audio tool '%s' requested - resource availability will be checked by tool callback", tool->name);
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

ethervox_result_t ethervox_governor_load_model(ethervox_governor_t* governor, const char* model_path) {
    if (!governor || !model_path) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    
#if !defined(ETHERVOX_WITH_LLAMA) || !LLAMA_HEADER_AVAILABLE
    GOV_ERROR("llama.cpp not available - cannot load model");
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
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
    
    // Clear corruption flag before attempting load
    g_model_corruption_detected = false;
    
    GOV_LOG("[Governor] Loading model: %s", model_path);
    
    // Auto-detect chat template from model path
    governor->chat_template = chat_template_get(CHAT_TEMPLATE_AUTO, model_path);
    GOV_LOG("[Governor] Detected chat template type: %d", governor->chat_template->type);
    
    // Initialize llama.cpp backend (only once per process)
    if (!g_llama_backend_initialized) {
        GOV_LOG("[Governor] Initializing llama.cpp backend");
        // Set up log callback to capture llama.cpp errors
        ggml_log_set(governor_ggml_log_callback, NULL);
        
        // Enable verbose llama.cpp logging
        llama_log_set(governor_ggml_log_callback, NULL);
        
        llama_backend_init();
        
        // CRITICAL: Load ggml backends (CPU, etc.)
        GOV_LOG("[Governor] Loading ggml backends...");
        ggml_backend_load_all();
        int backend_count = ggml_backend_reg_count();
        GOV_LOG("[Governor] Loaded %d ggml backends", backend_count);
        
        if (backend_count == 0) {
            GOV_ERROR("[Governor] FATAL: No backends loaded! Cannot load models.");
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        g_llama_backend_initialized = true;
    } else {
        GOV_LOG("[Governor] llama.cpp backend already initialized, skipping");
    }
    
    // Model params - Use runtime config (with config.h fallbacks)
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = governor->config.gpu_layers;
    model_params.use_mmap = ETHERVOX_GOVERNOR_USE_MMAP;
    model_params.use_mlock = false;  // Don't lock memory (let OS manage)
    model_params.progress_callback = governor_load_progress_callback;
    model_params.progress_callback_user_data = NULL;
    
    GOV_LOG("[Governor] Model params: n_gpu_layers=%d, use_mmap=%d", model_params.n_gpu_layers, model_params.use_mmap);
    
    // Verify file exists and is readable before trying to load
    FILE* test_file = fopen(model_path, "rb");
    if (!test_file) {
        GOV_ERROR("Cannot open model file: %s (errno: %d - %s)", model_path, errno, strerror(errno));
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Get file size
    fseek(test_file, 0, SEEK_END);
    long file_size = ftell(test_file);
    fseek(test_file, 0, SEEK_SET);
    fclose(test_file);
    
    GOV_LOG("[Governor] Model file accessible: %ld bytes", file_size);
    
    // Double-check all parameters before loading
    GOV_LOG("[Governor] Final check before load:");
    GOV_LOG("  - model_path: %s", model_path);
    GOV_LOG("  - n_gpu_layers: %d", model_params.n_gpu_layers);
    GOV_LOG("  - use_mmap: %d", model_params.use_mmap);
    GOV_LOG("  - use_mlock: %d", model_params.use_mlock);
    GOV_LOG("  - vocab_only: %d", model_params.vocab_only);
    GOV_LOG("  - check_tensors: %d", model_params.check_tensors);
    
    // Load model
    // Additional debug: Check file one more time before load
    FILE* f_test = fopen(model_path, "rb");
    if (!f_test) {
        GOV_ERROR("[Governor] CRITICAL: Cannot open file just before llama load: %s", strerror(errno));
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    fclose(f_test);
    GOV_LOG("[Governor] File test just before load: SUCCESS");
    
    // Check backend count one more time
    int backend_count_preload = ggml_backend_reg_count();
    GOV_LOG("[Governor] Backend count just before load: %d", backend_count_preload);
    
    if (backend_count_preload == 0) {
        GOV_ERROR("[Governor] FATAL: Backends disappeared before load!");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    GOV_LOG("[Governor] About to call llama_model_load_from_file...");
    GOV_LOG("[Governor] Path: %s", model_path);
    GOV_LOG("[Governor] Model params address: %p", (void*)&model_params);
    
    governor->llm_model = llama_model_load_from_file(model_path, model_params);
    
    GOV_LOG("[Governor] llama_model_load_from_file returned: %p", (void*)governor->llm_model);
    
    if (!governor->llm_model) {
        // Check if backends still there after failed load
        int backend_count_postfail = ggml_backend_reg_count();
        GOV_ERROR("[Governor] Load failed, backend count after: %d", backend_count_postfail);
    }
    
    if (!governor->llm_model) {
        GOV_ERROR("Failed to load model from %s", model_path);
        GOV_ERROR("Model params used: n_gpu_layers=%d, use_mmap=%d, use_mlock=%d", 
                  model_params.n_gpu_layers, model_params.use_mmap, model_params.use_mlock);
        
        // Try to understand why it failed
        GOV_ERROR("Possible reasons:");
        GOV_ERROR("  1. Incompatible GGUF version");
        GOV_ERROR("  2. Corrupted model file");
        GOV_ERROR("  3. Insufficient memory");
        GOV_ERROR("  4. Missing backend support");
        
        // Return -2 if corruption was detected, -1 otherwise
        if (g_model_corruption_detected) {
            GOV_ERROR("[Governor] Model corruption detected - returning error code -2");
            return -2;
        }
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    GOV_LOG("[Governor] Model loaded successfully");
    
    // Set privacy mode based on config (secret mode)
    extern void ethervox_memory_set_privacy_mode(bool disable_logging);
    ethervox_memory_set_privacy_mode(governor->config.disable_memory_logging);
    
    // Context params - Use runtime config (with config.h fallbacks)
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = governor->config.context_size;
    ctx_params.n_batch = ETHERVOX_GOVERNOR_BATCH_SIZE;
    ctx_params.n_threads = governor->config.n_threads;
    ctx_params.n_threads_batch = governor->config.n_threads;
    ctx_params.flash_attn_type = ETHERVOX_GOVERNOR_FLASH_ATTN_TYPE;  // Enable flash attention for speed and quantized KV cache
    ctx_params.type_k = ETHERVOX_GOVERNOR_KV_CACHE_TYPE;
    ctx_params.type_v = ETHERVOX_GOVERNOR_KV_CACHE_TYPE;
    ctx_params.no_perf = false;  // Enable performance tracking for metrics
    
    // Create context
    governor->llm_ctx = llama_init_from_model(governor->llm_model, ctx_params);
    if (!governor->llm_ctx) {
        GOV_ERROR("Failed to create llama context");
        llama_model_free(governor->llm_model);
        governor->llm_model = NULL;
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    GOV_LOG("Context created successfully");
    
    // Log tool registry state BEFORE building system prompt
    GOV_LOG("Tool registry has %u tools registered", governor->tool_registry->tool_count);
    for (uint32_t i = 0; i < governor->tool_registry->tool_count; i++) {
        GOV_LOG("  Tool %u: %s", i, governor->tool_registry->tools[i].name);
    }
    
    // Build system prompt based on mode (full vs minimal)
    // Increased to 16KB to accommodate all tools and examples (full mode)
    char system_prompt[16384];
    
    if (governor->config.system_prompt_mode == ETHERVOX_GOVERNOR_MODE_MINIMAL) {
        // Minimal mode: brief prompt without tools for fast mobile loading
        GOV_LOG("Building MINIMAL system prompt (fast mobile mode, tools unavailable)");
        
        // Ultra-brief system prompt - optimized for mobile startup speed
        // ~50 tokens vs ~1200 tokens in full mode (96% reduction)
        const char* minimal_prompt = 
            "You are EthervoxAI, a helpful and concise voice assistant. "
            "Your tools are currently unavailable, so provide direct answers based on your knowledge. "
            "Be brief, accurate, and conversational.";
        
        strncpy(system_prompt, minimal_prompt, sizeof(system_prompt) - 1);
        system_prompt[sizeof(system_prompt) - 1] = '\0';
        
        GOV_LOG("Minimal system prompt (%zu chars) - tools disabled", strlen(system_prompt));
        
        // Mark tools as unavailable in governor state
        governor->tools_available = false;
        
    } else {
        // Full mode: complete system prompt with all tools and capabilities
        GOV_LOG("Building FULL system prompt (all tools available)");
        
        if (ethervox_tool_registry_build_system_prompt(governor->tool_registry,
                                                       governor->chat_template,
                                                       system_prompt, sizeof(system_prompt),
                                                       NULL,  // TODO: Wire memory_store for adaptive learning
                                                       governor->model_path) != 0) {
            GOV_ERROR("Failed to build system prompt");
            llama_free(governor->llm_ctx);
            llama_model_free(governor->llm_model);
            governor->llm_ctx = NULL;
            governor->llm_model = NULL;
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        GOV_LOG("Full system prompt built (%zu chars)", strlen(system_prompt));
        
        // Mark tools as available in governor state
        governor->tools_available = true;
    }
    
    GOV_LOG("System prompt (%zu chars):\n%s", strlen(system_prompt), system_prompt);
    
    // Tokenize system prompt
    const struct llama_vocab* vocab = llama_model_get_vocab(governor->llm_model);
    int n_tokens = -llama_tokenize(vocab, system_prompt, strlen(system_prompt), NULL, 0, true, false);
    
    if (n_tokens <= 0) {
        GOV_ERROR("Failed to tokenize system prompt");
        llama_free(governor->llm_ctx);
        llama_model_free(governor->llm_model);
        governor->llm_ctx = NULL;
        governor->llm_model = NULL;
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    llama_token* tokens = malloc(n_tokens * sizeof(llama_token));
    if (!tokens) {
        GOV_ERROR("Failed to allocate token buffer");
        llama_free(governor->llm_ctx);
        llama_model_free(governor->llm_model);
        governor->llm_ctx = NULL;
        governor->llm_model = NULL;
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    llama_tokenize(vocab, system_prompt, strlen(system_prompt), tokens, n_tokens, true, false);
    
    // Save a copy of the system prompt tokens for recovery after nuclear clear
    governor->system_prompt_tokens = malloc(n_tokens * sizeof(llama_token));
    if (governor->system_prompt_tokens) {
        memcpy(governor->system_prompt_tokens, tokens, n_tokens * sizeof(llama_token));
        governor->system_prompt_tokens_len = n_tokens;
        GOV_LOG("Saved system prompt tokens (%d tokens) for nuclear clear recovery", n_tokens);
    } else {
        GOV_ERROR("Failed to allocate memory for system prompt backup");
    }
    
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
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        llama_batch_free(batch);
    }
    
    free(tokens);
    
    governor->system_prompt_token_count = n_tokens;
    governor->current_kv_pos = n_tokens;  // Start after system prompt
    governor->model_path = strdup(model_path);
    
    // Pre-tokenize static tool result wrappers for speed
    const char* prefix = governor->chat_template->tool_result_start;
    const char* suffix = governor->chat_template->tool_result_end;
    
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
    
    // ========================================================================
    // STARTUP CONTEXT RESTORATION - Load most recent conversation summary
    // ========================================================================
    // Provides conversation continuity across sessions by restoring context
    // from previous conversations. This is similar to the RELIGHT sequence
    // but happens at initial model load instead of after KV cache clear.
    
    GOV_LOG("STARTUP: Checking for previous conversation context...");
    load_conversation_summary(
        governor,
        "[\"context_summary\"]",
        "[Previous Session Context]",
        "STARTUP"
    );
    
    GOV_LOG("[Governor] Model loaded and ready");;
    
    return ETHERVOX_SUCCESS;
#endif
}

/**
 * Unload the Governor model to free memory
 */
ethervox_result_t ethervox_governor_unload_model(ethervox_governor_t* governor) {
    if (!governor) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    
    if (!governor->llm_loaded) {
        GOV_LOG("[Governor] Model already unloaded");
        return ETHERVOX_SUCCESS; // Already unloaded, success
    }
    
#if !defined(ETHERVOX_WITH_LLAMA) || !LLAMA_HEADER_AVAILABLE
    GOV_ERROR("llama.cpp not available");
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
#else
    GOV_LOG("[Governor] Unloading model to free memory (keeping model path for reload)");
    
    // Free LLM context and model
    if (governor->llm_ctx) {
        llama_free(governor->llm_ctx);
        governor->llm_ctx = NULL;
    }
    if (governor->llm_model) {
        llama_model_free(governor->llm_model);
        governor->llm_model = NULL;
    }
    
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
    
    // Keep model_path for reload, but mark as unloaded
    governor->llm_loaded = false;
    governor->system_prompt_token_count = 0;
    governor->current_kv_pos = 0;
    
    // Clear conversation history since KV cache is gone
    cleanup_conversation_history(&governor->conversation_history);
    init_conversation_history(&governor->conversation_history, 32);
    governor->turn_counter = 0;
    
    GOV_LOG("[Governor] Model unloaded successfully (path preserved for reload)");
    
    return ETHERVOX_SUCCESS;
#endif
}

/**
 * Reload the Governor model using the previously saved model path
 */
ethervox_result_t ethervox_governor_reload_model(ethervox_governor_t* governor) {
    if (!governor) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    
    if (governor->llm_loaded) {
        GOV_LOG("[Governor] Model already loaded");
        return ETHERVOX_SUCCESS; // Already loaded, success
    }
    
    if (!governor->model_path) {
        GOV_ERROR("[Governor] Cannot reload - no previous model path saved");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    GOV_LOG("[Governor] Reloading model from: %s", governor->model_path);
    
    // Use the existing load_model function with the saved path
    return ethervox_governor_load_model(governor, governor->model_path);
}

/**
 * Update Governor runtime configuration
 */
ethervox_result_t ethervox_governor_update_config(ethervox_governor_t* governor, const ethervox_governor_config_t* config) {
    if (!governor || !config) {
        GOV_ERROR("Invalid governor or config");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Update configuration
    governor->config = *config;
    GOV_LOG("Governor config updated: gpu_layers=%u, context=%u, threads=%d",
            config->gpu_layers, config->context_size, config->n_threads);
    
    return ETHERVOX_SUCCESS;
}

/**
 * Check if the Governor model is currently loaded
 */
bool ethervox_governor_is_loaded(ethervox_governor_t* governor) {
    if (!governor) return false;
    return governor->llm_loaded;
}

/**
 * Initialize Governor with configuration
 */
ethervox_result_t ethervox_governor_init(
    ethervox_governor_t** governor,
    const ethervox_governor_config_t* config,
    ethervox_tool_registry_t* tool_registry
) {
    if (!governor || !tool_registry) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    
    ethervox_governor_t* gov = calloc(1, sizeof(ethervox_governor_t));
    if (!gov) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    
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
    gov->tool_execution_enabled = true;  // Enabled by default
    gov->system_prompt_lost = false;     // System prompt present until nuclear clear
    gov->tools_available = true;         // Tools available by default (changes in minimal mode)
    gov->interrupt_requested = false;    // No interrupt pending
    gov->system_prompt_tokens = NULL;
    gov->system_prompt_tokens_len = 0;
    
    // Initialize context manager
    memset(&gov->context_manager, 0, sizeof(context_manager_state_t));
    gov->context_manager.current_health = CTX_HEALTH_OK;
    gov->turn_counter = 0;
    
    // Initialize conversation history with capacity for 100 turns
    if (init_conversation_history(&gov->conversation_history, 100) != 0) {
        free(gov);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    *governor = gov;
    return ETHERVOX_SUCCESS;
}

/**
 * Execute user query with execution context for conversational awareness
 * 
 * Extended version that includes execution context (input source, capabilities, callbacks)
 * for conversational AI features. The LLM receives context about where input came from
 * and can use conversation tools (speak, listen) accordingly.
 * 
 * @param governor Governor instance
 * @param user_query User's natural language query
 * @param exec_context Execution context (NULL for legacy behavior)
 * @param response Output: Final response (caller must free)
 * @param error Output: Error message if failed (caller must free)
 * @param metrics Output: Confidence metrics (optional)
 * @param progress_callback Progress callback (optional)
 * @param token_callback Token streaming callback (optional)
 * @param user_data User data for callbacks (optional)
 * @return Governor status
 */
ethervox_governor_status_t ethervox_governor_execute_with_context(
    ethervox_governor_t* governor,
    const char* user_query,
    const ethervox_execution_context_t* exec_context,
    char** response,
    char** error,
    ethervox_confidence_metrics_t* metrics,
    ethervox_governor_progress_callback progress_callback,
    void (*token_callback)(const char* token, void* user_data),
    void* user_data
) {
    // Set conversation callbacks if provided
    if (exec_context && exec_context->callbacks) {
        // External function in conversation_tools plugin
        extern void ethervox_conversation_tools_set_callbacks(void* callbacks);
        ethervox_conversation_tools_set_callbacks((void*)exec_context->callbacks);
    }
    
    // If no execution context, just pass through to standard execute
    if (!exec_context) {
        return ethervox_governor_execute(
            governor, user_query, response, error, metrics,
            progress_callback, token_callback, user_data
        );
    }
    
    // Build enhanced query with execution context metadata
    // This gives the LLM awareness of its environment and available capabilities
    char enhanced_query[8192];
    int offset = 0;
    
    // Add input source context
    const char* source_name = "unknown";
    switch (exec_context->source) {
        case ETHERVOX_INPUT_SOURCE_CLI:
            source_name = "command-line interface";
            break;
        case ETHERVOX_INPUT_SOURCE_VOICE:
            source_name = "voice conversation";
            break;
        case ETHERVOX_INPUT_SOURCE_API:
            source_name = "API request";
            break;
        default:
            source_name = exec_context->source_description ? 
                         exec_context->source_description : "unknown";
            break;
    }
    
    offset += snprintf(enhanced_query + offset, sizeof(enhanced_query) - offset,
        "[Context: Input from %s", source_name);
    
    // Add capability information
    if (exec_context->tts_available || exec_context->microphone_available) {
        offset += snprintf(enhanced_query + offset, sizeof(enhanced_query) - offset,
            " | Available tools:");
        
        if (exec_context->tts_available) {
            offset += snprintf(enhanced_query + offset, sizeof(enhanced_query) - offset,
                " speak (text-to-speech)");
        }
        
        if (exec_context->microphone_available) {
            offset += snprintf(enhanced_query + offset, sizeof(enhanced_query) - offset,
                "%s listen (microphone input)",
                exec_context->tts_available ? "," : "");
        }
    }
    
    // Add usage requirement for voice mode
    if (exec_context->source == ETHERVOX_INPUT_SOURCE_VOICE && 
        exec_context->tts_available) {
        offset += snprintf(enhanced_query + offset, sizeof(enhanced_query) - offset,
            " | IMPORTANT: You MUST use the 'speak' tool for ALL responses. "
            "Do NOT return plain text - ALWAYS call speak(text=\"your response\")");
    }
    
    offset += snprintf(enhanced_query + offset, sizeof(enhanced_query) - offset,
        "]\n\n%s", user_query);
    
    GOV_LOG("Context-aware query (%d chars): %s", offset, enhanced_query);
    
    // Execute with enhanced query
    return ethervox_governor_execute(
        governor, enhanced_query, response, error, metrics,
        progress_callback, token_callback, user_data
    );
}

/**
 * Execute Governor reasoning loop (legacy API - for backward compatibility)
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
    
    // Reset iteration counter and interrupt flag
    governor->last_iteration_count = 0;
    governor->interrupt_requested = false;
    
    // Only clear KV cache if we're running low on space or this is explicitly requested
    // This allows conversation history to accumulate naturally
    llama_memory_t mem = llama_get_memory(governor->llm_ctx);
    int32_t max_pos = llama_memory_seq_pos_max(mem, 0);
    int n_ctx = llama_n_ctx(governor->llm_ctx);
    
    // Clear if we're past system prompt and getting close to context limit (>50% full)
    if (max_pos > governor->system_prompt_token_count && max_pos > (n_ctx / 2)) {
        GOV_LOG("KV cache clearing: removing positions %d to %d (was at %d%% capacity)", 
                governor->system_prompt_token_count, max_pos, (max_pos * 100 / n_ctx));
        
        // ========================================================================
        // CONTEXT SUMMARIZATION - Preserve conversation knowledge before clearing
        // ========================================================================
        
        // Notify UI that summarization is starting
        if (progress_callback) {
            char summary_msg[256];
            snprintf(summary_msg, sizeof(summary_msg), 
                    "Context at %d%% - summarizing conversation before clearing...",
                    (max_pos * 100 / n_ctx));
            progress_callback(ETHERVOX_GOVERNOR_EVENT_CONTEXT_SUMMARIZING, summary_msg, user_data);
        }
        
        // Generate an LLM-based summary of the recent conversation
        GOV_LOG("Generating conversation summary before clearing...");
        
        // Build conversation context from history
        char conversation_context[4096] = {0};
        int ctx_len = 0;
        
        // Include recent turns (last 10 or all if fewer)
        uint32_t start_turn = (governor->conversation_history.turn_count > 10) 
                              ? (governor->conversation_history.turn_count - 10) : 0;
        
        for (uint32_t i = start_turn; i < governor->conversation_history.turn_count; i++) {
            conversation_turn_t* turn = &governor->conversation_history.turns[i];
            int remaining = sizeof(conversation_context) - ctx_len - 1;
            
            if (remaining > 200) {  // Need space for role + preview
                int written = snprintf(conversation_context + ctx_len, remaining,
                    "%s: %s\n",
                    turn->is_user ? "User" : "Assistant",
                    turn->preview);
                if (written > 0 && written < remaining) {
                    ctx_len += written;
                }
            }
        }
        
        // Create summarization prompt
        char summary_prompt[5120];
        snprintf(summary_prompt, sizeof(summary_prompt),
            "Summarize this conversation in 2-3 concise sentences, capturing key topics, "
            "decisions, and context that should be remembered:\n\n%s\n\n"
            "Summary (2-3 sentences):",
            conversation_context);
        
        // DISABLED: LLM-based summarization (causes bus errors, needs proper batch handling)
        // For now, use simple text-based summary
        char llm_summary[1024] = {0};
        
        // Create a simple summary from conversation context
        if (ctx_len > 0) {
            snprintf(llm_summary, sizeof(llm_summary),
                "Recent conversation covered %d turns with topics discussed.",
                (int)(governor->conversation_history.turn_count - start_turn));
        }
        
        GOV_LOG("Using simple conversation summary (LLM summarization disabled)");
        
        skip_llm_summary:
        ;  // Empty statement to avoid C23 extension warning
        
        // Store the summary in memory
        ethervox_tool_t* memory_tool = NULL;
        for (uint32_t i = 0; i < governor->tool_registry->tool_count; i++) {
            if (strcmp(governor->tool_registry->tools[i].name, "memory_store") == 0) {
                memory_tool = &governor->tool_registry->tools[i];
                break;
            }
        }
        
        if (memory_tool) {
            // Use LLM summary if available, otherwise fallback to simple marker
            char summary_content[2048];
            if (llm_summary[0] != '\0') {
                snprintf(summary_content, sizeof(summary_content),
                    "[Conversation Summary - Context Cleared at %d%% capacity]\n\n%s\n\n"
                    "Turn count: %u. Full conversation history preserved in memory.",
                    (max_pos * 100 / n_ctx), llm_summary, governor->turn_counter);
            } else {
                snprintf(summary_content, sizeof(summary_content),
                    "Context cleared at position %d (%d%% full). "
                    "Conversation history up to this point preserved. "
                    "Turn count: %u. Use memory_search to recall earlier conversation if needed.",
                    max_pos, (max_pos * 100 / n_ctx), governor->turn_counter);
            }
            
            // Escape quotes for JSON
            char escaped_summary[4096];
            int esc_idx = 0;
            for (int i = 0; summary_content[i] && esc_idx < sizeof(escaped_summary) - 2; i++) {
                if (summary_content[i] == '"' || summary_content[i] == '\\') {
                    escaped_summary[esc_idx++] = '\\';
                }
                if (summary_content[i] == '\n') {
                    escaped_summary[esc_idx++] = '\\';
                    escaped_summary[esc_idx++] = 'n';
                } else {
                    escaped_summary[esc_idx++] = summary_content[i];
                }
            }
            escaped_summary[esc_idx] = '\0';
            
            // Build JSON args for memory_store
            char memory_args[8192];
            snprintf(memory_args, sizeof(memory_args),
                    "{\"text\":\"%s\","
                    "\"importance\":0.90,"
                    "\"tags\":[\"context_summary\",\"auto_generated\",\"kv_cleared\"]}",
                    escaped_summary);
            
            char* store_result = NULL;
            char* store_error = NULL;
            
            int store_status = memory_tool->execute(memory_args, &store_result, &store_error);
            if (store_status == 0) {
                GOV_LOG("Stored conversation summary in memory");
            } else {
                GOV_ERROR("Failed to store conversation summary: %s", store_error ? store_error : "unknown error");
            }
            
            free(store_result);
            free(store_error);
        } else {
            GOV_LOG("Warning: memory_store tool not available, summary not persisted");
        }
        
        // Now clear the KV cache
        llama_memory_seq_rm(mem, 0, governor->system_prompt_token_count, -1);
        
        // CRITICAL: After removal, re-query the actual max position
        // llama_memory_seq_rm doesn't immediately update internal position tracking
        // We must verify what llama.cpp thinks the position is NOW
        int32_t actual_max = llama_memory_seq_pos_max(mem, 0);
        
        // If llama.cpp still thinks we're beyond the system prompt, we have a problem
        bool needed_nuclear_clear = false;
        if (actual_max >= governor->system_prompt_token_count) {
            GOV_ERROR("KV cache removal failed: max_pos still at %d after clearing", actual_max);
            // Force-clear using llama_memory_clear (nuclear option - clears EVERYTHING)
            llama_memory_clear(mem, true);  // Clear both metadata and data
            actual_max = llama_memory_seq_pos_max(mem, 0);
            needed_nuclear_clear = true;
            GOV_LOG("Nuclear clear: completely wiped KV cache, max_pos now: %d", actual_max);
        }
        
        // Use the actual max position from llama.cpp, or system_prompt_token_count if empty
        if (needed_nuclear_clear) {
            // ========================================================================
            // RELIGHT SEQUENCE - Restore system prompt after catastrophic failure
            // ========================================================================
            // Like relighting a rocket engine after shutdown, we restore the governor
            // to full operational state by reprocessing the saved system prompt
            
            GOV_LOG("Nuclear clear wiped KV cache - initiating RELIGHT sequence...");
            
            // Attempt to restore system prompt from saved tokens
            if (governor->system_prompt_tokens && governor->system_prompt_tokens_len > 0) {
                GOV_LOG("RELIGHT: Restoring system prompt (%d tokens)...", 
                        governor->system_prompt_tokens_len);
                
                // Reprocess system prompt in chunks (same as initial load)
                int chunk_size = 1024;
                bool relight_successful = true;
                
                for (int i = 0; i < governor->system_prompt_tokens_len; i += chunk_size) {
                    int chunk_len = (i + chunk_size > governor->system_prompt_tokens_len) 
                                    ? (governor->system_prompt_tokens_len - i) 
                                    : chunk_size;
                    bool is_final_chunk = (i + chunk_size >= governor->system_prompt_tokens_len);
                    
                    // Create batch with explicit positions
                    llama_batch batch = llama_batch_init(chunk_len, 0, 1);
                    batch.n_tokens = chunk_len;
                    for (int j = 0; j < chunk_len; j++) {
                        batch.token[j] = governor->system_prompt_tokens[i + j];
                        batch.pos[j] = i + j;
                        batch.n_seq_id[j] = 1;
                        batch.seq_id[j][0] = 0;
                        batch.logits[j] = false;
                    }
                    // Compute logits only for the very last token
                    if (is_final_chunk) {
                        batch.logits[chunk_len - 1] = true;
                    }
                    
                    if (llama_decode(governor->llm_ctx, batch) != 0) {
                        GOV_ERROR("RELIGHT FAILED: Could not restore system prompt chunk at token %d", i);
                        relight_successful = false;
                        llama_batch_free(batch);
                        break;
                    }
                    
                    llama_batch_free(batch);
                }
                
                if (relight_successful) {
                    // System prompt successfully restored!
                    governor->current_kv_pos = governor->system_prompt_token_count;
                    governor->system_prompt_lost = false;
                    GOV_LOG("RELIGHT COMPLETE: System prompt restored, tools re-enabled");
                    GOV_LOG("KV cache restored to position %d (%d%% full)", 
                            governor->current_kv_pos, 
                            (governor->current_kv_pos * 100 / n_ctx));
                    
                    // Load conversation summary from memory to restore context
                    GOV_LOG("RELIGHT: Loading conversation summary from memory...");
                    load_conversation_summary(
                        governor,
                        "[\"context_summary\",\"kv_cleared\"]",
                        "[Context Restored] Previous conversation summary:",
                        "RELIGHT"
                    );
                    
                    // Notify UI that recovery was successful
                    if (progress_callback) {
                        char relight_msg[256];
                        snprintf(relight_msg, sizeof(relight_msg),
                                "System recovered - full capabilities restored with conversation context");
                        progress_callback(ETHERVOX_GOVERNOR_EVENT_CONTEXT_CLEARED, relight_msg, user_data);
                    }
                } else {
                    // Relight failed - governor remains in degraded state
                    governor->current_kv_pos = 0;
                    governor->system_prompt_lost = true;
                    GOV_ERROR("RELIGHT FAILED: System prompt could not be restored");
                    GOV_ERROR("Governor in degraded mode - tools disabled");
                }
            } else {
                // No saved system prompt - cannot relight
                governor->current_kv_pos = 0;
                governor->system_prompt_lost = true;
                GOV_ERROR("RELIGHT IMPOSSIBLE: No saved system prompt tokens");
                GOV_ERROR("Governor in degraded mode - continuing without tools");
            }
        } else {
            // Normal partial clear - system prompt is intact
            governor->current_kv_pos = (actual_max >= 0) ? actual_max + 1 : governor->system_prompt_token_count;
            GOV_LOG("KV cache cleared: kept system prompt [0..%d), resuming at position %d (verified: %d)", 
                    governor->system_prompt_token_count, governor->current_kv_pos, actual_max);
        }
        
        // Notify UI that clearing is complete
        if (progress_callback) {
            char clear_msg[256];
            snprintf(clear_msg, sizeof(clear_msg),
                    "Context cleared - conversation summary saved to memory (resuming from %d%%)",
                    (governor->current_kv_pos * 100 / n_ctx));
            progress_callback(ETHERVOX_GOVERNOR_EVENT_CONTEXT_CLEARED, clear_msg, user_data);
        }
    } else if (max_pos >= governor->system_prompt_token_count) {
        // Continue from where we left off (normal case with system prompt)
        governor->current_kv_pos = max_pos + 1;
        GOV_LOG("KV cache continuing: from position %d (%d%% full)",
                governor->current_kv_pos, (governor->current_kv_pos * 100 / n_ctx));
    } else if (governor->system_prompt_lost) {
        // After nuclear clear: system prompt is gone, continue from actual position
        // This applies to all iterations until model is reloaded
        governor->current_kv_pos = (max_pos >= 0) ? max_pos + 1 : 0;
        GOV_LOG("KV cache continuing (post-nuclear): from position %d (%d%% full, NO SYSTEM PROMPT)",
                governor->current_kv_pos, (governor->current_kv_pos * 100 / n_ctx));
    } else {
        // First query - start after system prompt
        GOV_LOG("KV position decision: max_pos=%d, current_kv_pos=%d, system_prompt=%d",
                max_pos, governor->current_kv_pos, governor->system_prompt_token_count);
        governor->current_kv_pos = governor->system_prompt_token_count;
        GOV_LOG("KV cache initialized: starting at position %d",
                governor->current_kv_pos);
    }
    
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
    
    // Add current user query using chat template
    conv_pos += snprintf(conversation + conv_pos, sizeof(conversation) - conv_pos,
        "%s%s%s%s", 
        governor->chat_template->user_start,
        user_query,
        governor->chat_template->user_end,
        governor->chat_template->assistant_start);
    
    // Track how much of conversation has been processed to avoid re-processing
    size_t processed_length = 0;
    
    for (uint32_t iteration = 0; iteration < governor->config.max_iterations; iteration++) {
        governor->last_iteration_count = iteration + 1;
        
        // Check for interrupt request
        if (governor->interrupt_requested) {
            GOV_LOG("Governor interrupted at iteration %d", iteration + 1);
            if (error) *error = strdup("Generation interrupted by user");
            return ETHERVOX_GOVERNOR_INTERRUPTED;
        }
        
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
            
            // ========================================================================
            // Context Health Monitoring - Check before decoding new tokens
            // ========================================================================
            context_health_t health = check_context_health(governor);
            
            // Update health state
            if (health != governor->context_manager.current_health) {
                GOV_LOG("[Context] Health changed: %d -> %d", 
                        governor->context_manager.current_health, health);
                governor->context_manager.current_health = health;
            }
            
            // If critical and not already managing, inject warning to LLM
            if (health == CTX_HEALTH_CRITICAL && !governor->context_manager.management_in_progress) {
                int n_ctx = llama_n_ctx(governor->llm_ctx);
                float usage_pct = (float)governor->current_kv_pos / n_ctx * 100.0f;
                
                GOV_LOG("[Context] CRITICAL: Usage at %.1f%%, injecting management warning", usage_pct);
                
                // Build warning message
                char warning_msg[512];
                snprintf(warning_msg, sizeof(warning_msg),
                        "\n<system>CRITICAL: Context usage at %.0f%%. "
                        "You MUST call context_manage tool before responding. "
                        "Recommended: action='shift_window', keep_last_n_turns=10</system>\n",
                        usage_pct);
                
                // Tokenize warning
                int n_warning = -llama_tokenize(vocab, warning_msg, strlen(warning_msg), 
                                                NULL, 0, false, false);
                if (n_warning > 0) {
                    llama_token* warning_tokens = malloc(n_warning * sizeof(llama_token));
                    if (warning_tokens) {
                        llama_tokenize(vocab, warning_msg, strlen(warning_msg), 
                                     warning_tokens, n_warning, false, false);
                        
                        // Create batch for warning
                        llama_batch warning_batch = llama_batch_init(n_warning, 0, 1);
                        warning_batch.n_tokens = n_warning;
                        for (int i = 0; i < n_warning; i++) {
                            warning_batch.token[i] = warning_tokens[i];
                            warning_batch.pos[i] = governor->current_kv_pos + i;
                            warning_batch.n_seq_id[i] = 1;
                            warning_batch.seq_id[i][0] = 0;
                            warning_batch.logits[i] = false;
                        }
                        warning_batch.logits[n_warning - 1] = true;
                        
                        // Decode warning into KV cache
                        if (llama_decode(governor->llm_ctx, warning_batch) == 0) {
                            governor->current_kv_pos += n_warning;
                            governor->context_manager.management_in_progress = true;
                            GOV_LOG("[Context] Warning injected successfully (%d tokens)", n_warning);
                        }
                        
                        llama_batch_free(warning_batch);
                        free(warning_tokens);
                    }
                }
            }
            
            // Check if we have space in the context window
            int n_ctx = llama_n_ctx(governor->llm_ctx);
            if (governor->current_kv_pos + n_tokens > n_ctx) {
                GOV_ERROR("Context window would be exceeded: current_pos=%d + n_tokens=%d > n_ctx=%d",
                         governor->current_kv_pos, n_tokens, n_ctx);
                free(tokens);
                if (error) {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), 
                            "Context window exceeded (%d + %d > %d). Try a shorter query.",
                            governor->current_kv_pos, n_tokens, n_ctx);
                    *error = strdup(err_msg);
                }
                return ETHERVOX_GOVERNOR_ERROR;
            }
            
            // Create batch with explicit positions starting at current KV position
            llama_batch batch = llama_batch_init(n_tokens, 0, 1);
            batch.n_tokens = n_tokens;
            for (int i = 0; i < n_tokens; i++) {
                batch.token[i] = tokens[i];
                batch.pos[i] = governor->current_kv_pos + i;
                batch.n_seq_id[i] = 1;
                batch.seq_id[i][0] = 0;
                batch.logits[i] = false;
            }
            batch.logits[n_tokens - 1] = true;  // Only need logits from last token
            
            GOV_LOG("Decoding batch: n_tokens=%d, pos_start=%d, pos_end=%d, n_ctx=%d", 
                    n_tokens, governor->current_kv_pos, governor->current_kv_pos + n_tokens - 1,
                    llama_n_ctx(governor->llm_ctx));
            
            // Decode tokens (append to KV cache)
            if (llama_decode(governor->llm_ctx, batch) != 0) {
                GOV_ERROR("Decode failed: n_tokens=%d, pos_start=%d, pos_end=%d", 
                         n_tokens, governor->current_kv_pos, governor->current_kv_pos + n_tokens - 1);
                llama_batch_free(batch);
                free(tokens);
                if (error) *error = strdup("Failed to decode conversation");
                return ETHERVOX_GOVERNOR_ERROR;
            }
            
            llama_batch_free(batch);
            free(tokens);
            
            // ========================================================================
            // Turn Tracking - Record user query turn
            // ========================================================================
            int user_turn_start = governor->current_kv_pos;
            
            // Update KV cache position
            governor->current_kv_pos += n_tokens;
            
            int user_turn_end = governor->current_kv_pos - 1;
            
            // Create user turn record
            conversation_turn_t user_turn = {
                .turn_number = governor->conversation_history.turn_count,
                .kv_start = user_turn_start,
                .kv_end = user_turn_end,
                .timestamp = time(NULL),
                .importance = 0.5f,  // User queries have moderate importance
                .is_user = true
            };
            
            // Copy preview (first 120 chars of user query)
            strncpy(user_turn.preview, new_content, sizeof(user_turn.preview) - 1);
            user_turn.preview[sizeof(user_turn.preview) - 1] = '\0';
            
            // Append to history
            append_turn(&governor->conversation_history, &user_turn);
            
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
        
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(governor->config.temperature));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));
        
        int generated_count = 0;
        const int max_tokens = governor->config.max_tokens_per_response;  // Use configured limit
        bool inside_tool_call = false;  // Track if we're inside a tool call
        
        GOV_LOG("Starting generation with max_tokens=%d", max_tokens);
        
        while (generated_count < max_tokens) {
            llama_token next_token = llama_sampler_sample(sampler, governor->llm_ctx, -1);
            
            // Check for EOG token
            if (llama_vocab_is_eog(vocab, next_token)) {
                if (generated_count == 0) {
                    GOV_LOG("WARNING: Model immediately generated EOG token (id=%d) - this suggests prompt/context issue", next_token);
                } else {
                    GOV_LOG("Stopping generation: EOG token detected (id=%d) after %d tokens", next_token, generated_count);
                }
                break;
            }
            
            // Decode token to text
            char token_text[128];
            int n_chars = llama_token_to_piece(vocab, next_token, token_text, sizeof(token_text), 0, false);
            if (n_chars > 0 && n_chars < (int)sizeof(token_text)) {
                token_text[n_chars] = '\0';
                
                // Add to buffer first
                strncat(llm_response_buffer, token_text, sizeof(llm_response_buffer) - strlen(llm_response_buffer) - 1);
                
                // Check for actual stop sequences (not just <| prefix)
                // Only stop if we see a complete stop sequence pattern
                size_t buf_len = strlen(llm_response_buffer);
                bool found_stop = false;
                
                // Check for common stop sequence starts (but need more than just <|)
                // Granite: <|end_of_text|>, <|start_of_role|>, <|end_of_role|>
                // Qwen/LFM: <|im_end|>, <|im_start|>
                if (buf_len >= 10) {
                    // Check for complete or near-complete stop sequences
                    if (strstr(llm_response_buffer + (buf_len >= 20 ? buf_len - 20 : 0), "<|end_of_text|>") ||
                        strstr(llm_response_buffer + (buf_len >= 20 ? buf_len - 20 : 0), "<|start_of_role|>") ||
                        strstr(llm_response_buffer + (buf_len >= 20 ? buf_len - 20 : 0), "<|end_of_role|>") ||
                        strstr(llm_response_buffer + (buf_len >= 20 ? buf_len - 20 : 0), "<|im_end|>") ||
                        strstr(llm_response_buffer + (buf_len >= 20 ? buf_len - 20 : 0), "<|im_start|>")) {
                        found_stop = true;
                        
                        // Find where the stop sequence starts and truncate there
                        char* stop_pos = strstr(llm_response_buffer, "<|end_of_text|>");
                        if (!stop_pos) stop_pos = strstr(llm_response_buffer, "<|start_of_role|>");
                        if (!stop_pos) stop_pos = strstr(llm_response_buffer, "<|end_of_role|>");
                        if (!stop_pos) stop_pos = strstr(llm_response_buffer, "<|im_end|>");
                        if (!stop_pos) stop_pos = strstr(llm_response_buffer, "<|im_start|>");
                        
                        if (stop_pos) {
                            *stop_pos = '\0';
                            GOV_LOG("Stop sequence detected, ending generation");
                        }
                        break;
                    }
                }
                
                // Check if we're entering or exiting a tool call (format-aware)
                tool_format_type_t tool_format = chat_template_get_tool_format(governor->chat_template);
                
                if (!inside_tool_call && strstr(llm_response_buffer, "<tool_call")) {
                    inside_tool_call = true;
                }
                
                // Determine if we should stream this token
                bool should_stream = false;
                if (inside_tool_call) {
                    // Inside tool call - check if we're exiting (format-specific)
                    if (tool_format == TOOL_FORMAT_JSON_IN_XML) {
                        // JSON format: wait for </tool_call>
                        if (strstr(llm_response_buffer, "</tool_call>")) {
                            inside_tool_call = false;
                        }
                    } else {
                        // XML attribute format: wait for />
                        if (strstr(llm_response_buffer, "/>")) {
                            inside_tool_call = false;
                        }
                    }
                    // Don't stream anything while inside tool call
                    should_stream = false;
                } else {
                    // Not inside tool call - check if we might be starting one
                    size_t buf_len = strlen(llm_response_buffer);
                    const char* buf_end = llm_response_buffer + buf_len;
                    
                    // Check if buffer ends with potential tool call start patterns OR stop sequence starts
                    // Be more specific - only hold back if we're building "<tool_call" or a stop sequence
                    bool might_be_tool_start = false;
                    bool might_be_stop_sequence = false;
                    
                    // Check for partial stop sequences at end of buffer
                    // Granite: <|start_of_role|>, <|end_of_role|>, <|end_of_text|>
                    if (buf_len >= 1 && buf_end[-1] == '<') {
                        might_be_stop_sequence = true;  // Might be starting <| pattern
                    } else if (buf_len >= 2 && strncmp(buf_end - 2, "<|", 2) == 0) {
                        might_be_stop_sequence = true;
                    } else if (buf_len >= 3 && (strncmp(buf_end - 3, "<|s", 3) == 0 || strncmp(buf_end - 3, "<|e", 3) == 0)) {
                        might_be_stop_sequence = true;
                    } else if (strstr(buf_end - (buf_len >= 20 ? 20 : buf_len), "<|start_of") ||
                               strstr(buf_end - (buf_len >= 20 ? 20 : buf_len), "<|end_of")) {
                        might_be_stop_sequence = true;
                    }
                    
                    // Check for tool call start patterns
                    if (buf_len >= 1 && buf_end[-1] == '<' && token_text[0] == 't') {
                        might_be_tool_start = true;  // '<' followed by 't' - might be '<tool_call'
                    } else if (buf_len >= 2 && strncmp(buf_end - 2, "<t", 2) == 0) {
                        might_be_tool_start = true;  // Building '<t'
                    } else if (buf_len >= 3 && strncmp(buf_end - 3, "<to", 3) == 0) {
                        might_be_tool_start = true;  // Building '<to'
                    } else if (buf_len >= 4 && strncmp(buf_end - 4, "<too", 4) == 0) {
                        might_be_tool_start = true;  // Building '<too'
                    } else if (buf_len >= 5 && strncmp(buf_end - 5, "<tool", 5) == 0) {
                        might_be_tool_start = true;  // Building '<tool'
                    } else if (buf_len >= 6 && strncmp(buf_end - 6, "<tool_", 6) == 0) {
                        might_be_tool_start = true;  // Building '<tool_'
                    } else if (buf_len >= 10 && strncmp(buf_end - 10, "<tool_call", 10) == 0) {
                        might_be_tool_start = true;  // Building full '<tool_call'
                    }
                    
                    // Check if token itself contains stop sequence fragments
                    bool is_stop_fragment = chat_template_has_stop_sequence(
                        governor->chat_template, token_text);
                    
                    // Only stream if we're sure it's not a tool call, no STOP sequences, and not a stop fragment
                    should_stream = !might_be_tool_start && 
                                   !might_be_stop_sequence &&
                                   !is_stop_fragment &&
                                   !strstr(llm_response_buffer, "STOP") && 
                                   !chat_template_has_stop_sequence(governor->chat_template, llm_response_buffer);
                }
                
                if (should_stream && token_callback) {
                    token_callback(token_text, user_data);
                }
            }
            
            // Check for stop sequences in the accumulated response
            if (chat_template_has_stop_sequence(governor->chat_template, llm_response_buffer) ||
                strstr(llm_response_buffer, "STOP")) {
                // Remove the stop sequence from output
                for (int i = 0; governor->chat_template->stop_sequences[i] != NULL; i++) {
                    char* stop_pos = strstr(llm_response_buffer, governor->chat_template->stop_sequences[i]);
                    if (stop_pos) *stop_pos = '\0';
                }
                char* stop_pos = strstr(llm_response_buffer, "STOP");
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
            // 1. Stop immediately after tool_call closing tag (only if tool execution is enabled)
            if (governor->tool_execution_enabled && 
                strstr(llm_response_buffer, "<tool_call") && strstr(llm_response_buffer, "/>")) {
                GOV_LOG("Early stop: Tool call completed (%d tokens)", generated_count);
                break;  // Tool call complete, stop immediately
            }
            
            // 2. Stop if we see role markers or <tool_result> (model is hallucinating examples from system prompt)
            if (strstr(llm_response_buffer, governor->chat_template->user_start) || 
                strstr(llm_response_buffer, governor->chat_template->system_start) ||
                strstr(llm_response_buffer, "<tool_result")) {
                // Truncate the buffer to remove the hallucinated content
                char* hallucination_start = strstr(llm_response_buffer, governor->chat_template->user_start);
                if (!hallucination_start) hallucination_start = strstr(llm_response_buffer, governor->chat_template->system_start);
                if (!hallucination_start) hallucination_start = strstr(llm_response_buffer, "<tool_result");
                if (hallucination_start) *hallucination_start = '\0';
                
                GOV_LOG("Early stop: Hallucination detected and truncated (%d tokens)", generated_count);
                break;
            }
        }
        
        llama_sampler_free(sampler);
        
        GOV_LOG("Generation complete: %d tokens generated (max_tokens was %d)", generated_count, max_tokens);
        
        // ========================================================================
        // Turn Tracking - Record assistant response turn
        // ========================================================================
        int assistant_turn_start = governor->current_kv_pos - generated_count;
        int assistant_turn_end = governor->current_kv_pos - 1;
        
        // Create assistant turn record
        conversation_turn_t assistant_turn = {
            .turn_number = governor->conversation_history.turn_count,
            .kv_start = assistant_turn_start,
            .kv_end = assistant_turn_end,
            .timestamp = time(NULL),
            .importance = 0.7f,  // Assistant responses have higher importance
            .is_user = false
        };
        
        // Copy preview (first 120 chars of response)
        strncpy(assistant_turn.preview, llm_response_buffer, sizeof(assistant_turn.preview) - 1);
        assistant_turn.preview[sizeof(assistant_turn.preview) - 1] = '\0';
        
        // Append to history
        append_turn(&governor->conversation_history, &assistant_turn);
        
        // Clean up any stop tokens that made it into the output
        // Check for Granite patterns: <|start_of_role|>, <|end_of_role|>, <|end_of_text|>
        char* stop_marker = strstr(llm_response_buffer, "<|start_of_role|>");
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, "<|end_of_role|>");
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, "<|end_of_text|>");
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, "<|start_of");
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, "<|end_of");
        // Check for Qwen/LFM patterns: <|im_end|>, <|im_start|>
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, " <|im");
        if (!stop_marker) stop_marker = strstr(llm_response_buffer, "<|im");
        if (stop_marker) {
            *stop_marker = '\0';
            GOV_LOG("Cleaned up stop sequence from output at position %ld", stop_marker - llm_response_buffer);
        }
        
        const char* llm_response = llm_response_buffer;
        GOV_LOG("Generated response: %s", llm_response);
        
        if (metrics) {
            metrics->iteration_count = iteration + 1;
            metrics->tool_calls_made = 0;
        }
        
        // Extract tool calls based on model's tool format
        char* tool_calls[10];
        int num_tools = 0;
        tool_format_type_t tool_format = chat_template_get_tool_format(governor->chat_template);
        
        if (tool_format == TOOL_FORMAT_JSON_IN_XML) {
            // Granite 4.0 format: <tool_call>\n{"name": "...", "arguments": {...}}\n</tool_call>
            num_tools = extract_tool_calls_json(llm_response, tool_calls, 10);
        } else {
            // XML attribute format: <tool_call name="..." attr="..." />
            num_tools = extract_tool_calls(llm_response, tool_calls, 10);
        }
        
        // Debug: Log tool extraction result
        if (num_tools > 0) {
            GOV_LOG("Found %d tool call(s) in response", num_tools);
        } else {
            // Use DEBUG level since it's normal for final responses to have no tool calls
            ETHERVOX_LOGD("No tool calls in this response (length: %zu)", strlen(llm_response));
            ETHERVOX_LOGD("LLM response without tool calls: %.200s%s", 
                          llm_response, strlen(llm_response) > 200 ? "..." : "");
        }
        
        if (metrics) {
            metrics->tool_calls_made = num_tools;
        }
        
        // Execute tools if present and tool execution is enabled
        if (num_tools > 0 && governor->tool_execution_enabled) {
            for (int i = 0; i < num_tools; i++) {
                char* tool_result = NULL;
                char* tool_error = NULL;
                
                // Extract tool name for progress notification (format-specific)
                char* tool_name = NULL;
                if (tool_format == TOOL_FORMAT_JSON_IN_XML) {
                    // Parse JSON to get name
                    const char* name_start = strstr(tool_calls[i], "\"name\"");
                    if (name_start) {
                        const char* value_start = strchr(name_start, ':');
                        if (value_start) {
                            value_start++;
                            while (*value_start == ' ' || *value_start == '"') value_start++;
                            const char* value_end = strchr(value_start, '"');
                            if (value_end) {
                                size_t len = value_end - value_start;
                                tool_name = malloc(len + 1);
                                if (tool_name) {
                                    strncpy(tool_name, value_start, len);
                                    tool_name[len] = '\0';
                                }
                            }
                        }
                    }
                } else {
                    tool_name = parse_attribute(tool_calls[i], "name");
                }
                
                if (tool_name && progress_callback) {
                    char tool_msg[256];
                    snprintf(tool_msg, sizeof(tool_msg), "Calling tool: %s", tool_name);
                    progress_callback(ETHERVOX_GOVERNOR_EVENT_TOOL_CALL, tool_msg, user_data);
                }
                
                // Execute tool using format-specific handler
                int status;
                if (tool_format == TOOL_FORMAT_JSON_IN_XML) {
                    status = execute_tool_call_json(
                        tool_calls[i],
                        governor->tool_registry,
                        &tool_result,
                        &tool_error
                    );
                } else {
                    status = execute_tool_call(
                        tool_calls[i],
                        governor->tool_registry,
                        &tool_result,
                        &tool_error
                    );
                }
                
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
                    
                    // Decode prefix: chat_template->tool_result_start
                    if (governor->tool_result_prefix_tokens && governor->tool_result_prefix_len > 0) {
                        // Check if prefix will fit in context
                        int n_ctx = llama_n_ctx(governor->llm_ctx);
                        if (governor->current_kv_pos + governor->tool_result_prefix_len > n_ctx) {
                            GOV_ERROR("Cannot add tool result prefix: would exceed context (pos=%d, prefix=%d, ctx=%d)",
                                     governor->current_kv_pos, governor->tool_result_prefix_len, n_ctx);
                        } else {
                            llama_batch prefix_batch = llama_batch_init(governor->tool_result_prefix_len, 0, 1);
                            prefix_batch.n_tokens = governor->tool_result_prefix_len;
                            for (int j = 0; j < governor->tool_result_prefix_len; j++) {
                                prefix_batch.token[j] = governor->tool_result_prefix_tokens[j];
                                prefix_batch.pos[j] = governor->current_kv_pos + j;
                                prefix_batch.n_seq_id[j] = 1;
                                prefix_batch.seq_id[j][0] = 0;
                                prefix_batch.logits[j] = false;
                            }
                            if (llama_decode(governor->llm_ctx, prefix_batch) != 0) {
                                GOV_ERROR("Failed to decode tool result prefix at pos %d", governor->current_kv_pos);
                            } else {
                                governor->current_kv_pos += governor->tool_result_prefix_len;
                            }
                            llama_batch_free(prefix_batch);
                        }
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
                    
                    // Decode suffix: chat_template->tool_result_end
                    if (governor->tool_result_suffix_tokens && governor->tool_result_suffix_len > 0) {
                        // Check if suffix will fit in context
                        int n_ctx = llama_n_ctx(governor->llm_ctx);
                        if (governor->current_kv_pos + governor->tool_result_suffix_len > n_ctx) {
                            GOV_ERROR("Cannot add tool result suffix: would exceed context (pos=%d, suffix=%d, ctx=%d)",
                                     governor->current_kv_pos, governor->tool_result_suffix_len, n_ctx);
                        } else {
                            llama_batch suffix_batch = llama_batch_init(governor->tool_result_suffix_len, 0, 1);
                            suffix_batch.n_tokens = governor->tool_result_suffix_len;
                            for (int j = 0; j < governor->tool_result_suffix_len; j++) {
                                suffix_batch.token[j] = governor->tool_result_suffix_tokens[j];
                                suffix_batch.pos[j] = governor->current_kv_pos + j;
                                suffix_batch.n_seq_id[j] = 1;
                                suffix_batch.seq_id[j][0] = 0;
                                suffix_batch.logits[j] = (j == governor->tool_result_suffix_len - 1);  // Only last needs logits
                            }
                            if (llama_decode(governor->llm_ctx, suffix_batch) != 0) {
                                GOV_ERROR("Failed to decode tool result suffix at pos %d", governor->current_kv_pos);
                            } else {
                                governor->current_kv_pos += governor->tool_result_suffix_len;
                            }
                            llama_batch_free(suffix_batch);
                        }
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
                    // Decode prefix (error is treated like a tool_result for format purposes)
                    const char* error_prefix = governor->chat_template->user_start;
                    int prefix_n_tokens = -llama_tokenize(vocab, error_prefix, strlen(error_prefix), NULL, 0, false, false);
                    if (prefix_n_tokens > 0) {
                        int n_ctx = llama_n_ctx(governor->llm_ctx);
                        if (governor->current_kv_pos + prefix_n_tokens > n_ctx) {
                            GOV_ERROR("Cannot add error prefix: would exceed context (pos=%d, prefix=%d, ctx=%d)",
                                     governor->current_kv_pos, prefix_n_tokens, n_ctx);
                        } else {
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
                                if (llama_decode(governor->llm_ctx, prefix_batch) != 0) {
                                    GOV_ERROR("Failed to decode error prefix at pos %d", governor->current_kv_pos);
                                } else {
                                    governor->current_kv_pos += prefix_n_tokens;
                                }
                                llama_batch_free(prefix_batch);
                                free(prefix_tokens);
                            }
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
                                if (llama_decode(governor->llm_ctx, error_batch) != 0) {
                                    GOV_ERROR("Failed to decode error message chunk at pos %d", governor->current_kv_pos);
                                    llama_batch_free(error_batch);
                                    break;
                                }
                                llama_batch_free(error_batch);
                                governor->current_kv_pos += batch_size;
                                tokens_processed += batch_size;
                            }
                            free(error_tokens);
                        }
                    }
                    
                    // Add suffix to return to assistant context
                    const char* error_suffix = governor->chat_template->assistant_start;
                    int suffix_n_tokens = -llama_tokenize(vocab, error_suffix, strlen(error_suffix), NULL, 0, false, false);
                    if (suffix_n_tokens > 0) {
                        int n_ctx = llama_n_ctx(governor->llm_ctx);
                        if (governor->current_kv_pos + suffix_n_tokens > n_ctx) {
                            GOV_ERROR("Cannot add error suffix: would exceed context (pos=%d, suffix=%d, ctx=%d)",
                                     governor->current_kv_pos, suffix_n_tokens, n_ctx);
                        } else {
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
                                if (llama_decode(governor->llm_ctx, suffix_batch) != 0) {
                                    GOV_ERROR("Failed to decode error suffix at pos %d", governor->current_kv_pos);
                                } else {
                                    governor->current_kv_pos += suffix_n_tokens;
                                }
                                llama_batch_free(suffix_batch);
                                free(suffix_tokens);
                            }
                        }
                    }
                    
                    free(tool_error);
                }
                
                if (tool_name) free(tool_name);
            }
            
            // Check if any executed tool was a terminal tool (speak, listen)
            // This MUST happen before freeing tool_calls
            bool has_terminal_tool = false;
            for (int i = 0; i < num_tools; i++) {
                // Parse tool name from tool call
                const char* name_start = strstr(tool_calls[i], "\"name\"");
                if (name_start) {
                    const char* value_start = strchr(name_start, ':');
                    if (value_start) {
                        value_start++;
                        while (*value_start == ' ' || *value_start == '"') value_start++;
                        
                        // Extract tool name for logging
                        char tool_name_buf[64] = {0};
                        int j = 0;
                        while (j < 63 && value_start[j] && value_start[j] != '"' && value_start[j] != ',') {
                            tool_name_buf[j] = value_start[j];
                            j++;
                        }
                        
                        ETHERVOX_LOGD("Checking tool #%d: '%s'", i, tool_name_buf);
                        
                        if (strncmp(value_start, "speak", 5) == 0 || strncmp(value_start, "listen", 6) == 0) {
                            has_terminal_tool = true;
                            GOV_LOG("Detected terminal tool: %s", tool_name_buf);
                            break;
                        }
                    }
                }
            }
            
            // Now free tool_calls AFTER checking for terminal tools
            for (int i = 0; i < num_tools; i++) {
                free(tool_calls[i]);
            }
            
            // If we executed a terminal tool (speak/listen), stop iteration
            if (has_terminal_tool) {
                GOV_LOG("Terminal tool executed (speak/listen), ending iteration");
                // Return empty response - terminal tool already handled output
                if (response) {
                    *response = strdup("");
                }
                if (progress_callback) {
                    progress_callback(ETHERVOX_GOVERNOR_EVENT_COMPLETE,
                                    "Terminal tool executed", user_data);
                }
                return ETHERVOX_GOVERNOR_SUCCESS;
            }
            
            // If we executed tools, continue iteration to let LLM incorporate results
            // (unless we're at max iterations)
            if (iteration + 1 < governor->config.max_iterations) {
                continue;
            } else {
                // Max iterations reached after tool execution - return what we have
                GOV_LOG("Max iterations reached after tool execution, returning last response");
                if (response) {
                    *response = strdup(llm_response);
                }
                if (progress_callback) {
                    progress_callback(ETHERVOX_GOVERNOR_EVENT_COMPLETE,
                                    "Max iterations reached", user_data);
                }
                return ETHERVOX_GOVERNOR_TIMEOUT;
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
    
    // Should not reach here, but handle gracefully
    if (error) {
        *error = strdup("Unexpected loop termination");
    }
    return ETHERVOX_GOVERNOR_ERROR;
    
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
    
    // Cleanup conversation history
    cleanup_conversation_history(&governor->conversation_history);
    
    free(governor);
}

ethervox_result_t ethervox_governor_reset_conversation(ethervox_governor_t* governor) {
    if (!governor) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !defined(ETHERVOX_WITH_LLAMA) || !LLAMA_HEADER_AVAILABLE
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
#else
    // Clear KV cache back to system prompt
    if (!governor->llm_ctx || governor->system_prompt_token_count == 0) {
        GOV_LOG("Cannot reset: model not loaded");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    if (!governor->system_prompt_tokens || governor->system_prompt_tokens_len == 0) {
        GOV_ERROR("Cannot reset: no saved system prompt tokens for RELIGHT");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    llama_memory_t mem = llama_get_memory(governor->llm_ctx);
    int32_t max_pos = llama_memory_seq_pos_max(mem, 0);
    
    if (max_pos <= governor->system_prompt_token_count) {
        GOV_LOG("Conversation already clean (at position %d, system prompt is %d tokens)",
                max_pos, governor->system_prompt_token_count);
        return ETHERVOX_SUCCESS;
    }
    
    int n_ctx = llama_n_ctx(governor->llm_ctx);
    GOV_LOG("Resetting conversation: max_pos=%d, system_prompt=%d (%d%% full)",
            max_pos, governor->system_prompt_token_count, (max_pos * 100 / n_ctx));
    
    // ========================================================================
    // NUCLEAR CLEAR + RELIGHT (without summary reload)
    // ========================================================================
    // We can't trust llama_memory_seq_rm due to llama.cpp bugs, so we nuclear
    // clear everything and RELIGHT with system prompt only (no conversation summary)
    
    GOV_LOG("Nuclear clear: wiping entire KV cache for clean reset...");
    llama_memory_clear(mem, true);  // Clear both metadata and data
    int32_t actual_max = llama_memory_seq_pos_max(mem, 0);
    GOV_LOG("Nuclear clear complete: KV cache at position %d", actual_max);
    
    // RELIGHT: Restore system prompt from saved tokens
    GOV_LOG("RELIGHT: Restoring system prompt (%d tokens) without conversation summary...", 
            governor->system_prompt_tokens_len);
    
    // Reprocess system prompt in chunks (same as initial load)
    int chunk_size = 1024;
    bool relight_successful = true;
    
    for (int i = 0; i < governor->system_prompt_tokens_len; i += chunk_size) {
        int chunk_len = (i + chunk_size > governor->system_prompt_tokens_len) 
                        ? (governor->system_prompt_tokens_len - i) 
                        : chunk_size;
        bool is_final_chunk = (i + chunk_size >= governor->system_prompt_tokens_len);
        
        // Create batch with explicit positions
        llama_batch batch = llama_batch_init(chunk_len, 0, 1);
        batch.n_tokens = chunk_len;
        for (int j = 0; j < chunk_len; j++) {
            batch.token[j] = governor->system_prompt_tokens[i + j];
            batch.pos[j] = i + j;
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j] = false;
        }
        // Compute logits only for the very last token
        if (is_final_chunk) {
            batch.logits[chunk_len - 1] = true;
        }
        
        if (llama_decode(governor->llm_ctx, batch) != 0) {
            GOV_ERROR("RELIGHT FAILED: Could not restore system prompt chunk at token %d", i);
            relight_successful = false;
            llama_batch_free(batch);
            break;
        }
        
        llama_batch_free(batch);
    }
    
    if (!relight_successful) {
        GOV_ERROR("Conversation reset failed: could not restore system prompt");
        governor->current_kv_pos = 0;
        governor->system_prompt_lost = true;
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // System prompt successfully restored!
    governor->current_kv_pos = governor->system_prompt_token_count;
    governor->system_prompt_lost = false;
    GOV_LOG("RELIGHT COMPLETE: System prompt restored at position %d (%d%% full)", 
            governor->current_kv_pos, (governor->current_kv_pos * 100 / n_ctx));
    
    // Clear conversation history tracking
    cleanup_conversation_history(&governor->conversation_history);
    init_conversation_history(&governor->conversation_history, 32);
    
    // Reset context manager state (compatible with moving window system)
    governor->context_manager.current_health = CTX_HEALTH_OK;
    governor->context_manager.overflow_event_count = 0;
    governor->context_manager.last_gc_position = governor->system_prompt_token_count;
    governor->context_manager.management_in_progress = false;
    
    // TODO: Handle persistent memory - currently memory survives reset
    // Future: Create new memory data file or mark old memories as archived
    GOV_LOG("Note: Persistent memory not cleared (memories from before reset remain searchable)");
    
    GOV_LOG("Conversation reset complete: clean slate with system prompt only");
    
    return ETHERVOX_SUCCESS;
#endif
}

ethervox_tool_registry_t* ethervox_governor_get_registry(ethervox_governor_t* governor) {
    return governor ? governor->tool_registry : NULL;
}

void ethervox_governor_request_interrupt(ethervox_governor_t* governor) {
    if (governor) {
        governor->interrupt_requested = true;
        GOV_LOG("[Interrupt] Interrupt requested - will abort at next iteration");
    }
}

const chat_template_t* ethervox_governor_get_chat_template(ethervox_governor_t* governor) {
    return governor ? governor->chat_template : NULL;
}

void ethervox_governor_set_tool_execution(ethervox_governor_t* governor, bool enabled) {
    if (governor) {
        governor->tool_execution_enabled = enabled;
        GOV_LOG("Tool execution %s", enabled ? "enabled" : "disabled");
    }
}

ethervox_result_t ethervox_governor_summarize_and_clear_cache(ethervox_governor_t* governor, bool force_clear) {
    if (!governor) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !defined(ETHERVOX_WITH_LLAMA) || !LLAMA_HEADER_AVAILABLE
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
#else
    if (!governor->llm_ctx || governor->system_prompt_token_count == 0) {
        GOV_LOG("Cannot summarize: model not loaded");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    llama_memory_t mem = llama_get_memory(governor->llm_ctx);
    int32_t max_pos = llama_memory_seq_pos_max(mem, 0);
    int n_ctx = llama_n_ctx(governor->llm_ctx);
    
    // Check if we need to clear
    if (!force_clear && max_pos <= governor->system_prompt_token_count) {
        GOV_LOG("Cache already clean (at position %d, system prompt is %d tokens)",
                max_pos, governor->system_prompt_token_count);
        return ETHERVOX_SUCCESS;
    }
    
    if (!force_clear && max_pos <= (n_ctx / 2)) {
        GOV_LOG("Cache only at %d%% capacity - not clearing (use force_clear=true to override)",
                (max_pos * 100 / n_ctx));
        return ETHERVOX_SUCCESS;
    }
    
    GOV_LOG("Manual cache summarization: max_pos=%d, system_prompt=%d (%d%% full)",
            max_pos, governor->system_prompt_token_count, (max_pos * 100 / n_ctx));
    
    // Build conversation context from history
    char conversation_context[4096] = {0};
    int ctx_len = 0;
    
    // Include recent turns (last 10 or all if fewer)
    uint32_t start_turn = (governor->conversation_history.turn_count > 10) 
                          ? (governor->conversation_history.turn_count - 10) : 0;
    
    for (uint32_t i = start_turn; i < governor->conversation_history.turn_count; i++) {
        conversation_turn_t* turn = &governor->conversation_history.turns[i];
        int remaining = sizeof(conversation_context) - ctx_len - 1;
        
        if (remaining > 200) {
            int written = snprintf(conversation_context + ctx_len, remaining,
                "%s: %s\n",
                turn->is_user ? "User" : "Assistant",
                turn->preview);
            if (written > 0 && written < remaining) {
                ctx_len += written;
            }
        }
    }
    
    // Create summarization prompt
    char summary_prompt[5120];
    snprintf(summary_prompt, sizeof(summary_prompt),
        "Summarize this conversation in 2-3 concise sentences, capturing key topics, "
        "decisions, and context that should be remembered:\n\n%s\n\n"
        "Summary (2-3 sentences):",
        conversation_context);
    
    // Tokenize
    llama_token* summary_tokens = (llama_token*)malloc(1024 * sizeof(llama_token));
    if (!summary_tokens) {
        GOV_ERROR("Failed to allocate summary tokens");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    const struct llama_vocab* vocab = llama_model_get_vocab(governor->llm_model);
    int n_summary_tokens = llama_tokenize(
        vocab,
        summary_prompt,
        strlen(summary_prompt),
        summary_tokens,
        1024,
        true,
        false
    );
    
    if (n_summary_tokens < 0) {
        GOV_ERROR("Failed to tokenize summary prompt");
        free(summary_tokens);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    GOV_LOG("Processing summary prompt (%d tokens)...", n_summary_tokens);
    
    // First, evaluate the prompt tokens through the model in chunks
    int chunk_size = 512;
    for (int i = 0; i < n_summary_tokens; i += chunk_size) {
        int chunk_len = (i + chunk_size > n_summary_tokens) ? (n_summary_tokens - i) : chunk_size;
        bool is_last_chunk = (i + chunk_size >= n_summary_tokens);
        
        llama_batch batch = llama_batch_init(chunk_len, 0, 1);
        batch.n_tokens = chunk_len;
        for (int j = 0; j < chunk_len; j++) {
            batch.token[j] = summary_tokens[i + j];
            batch.pos[j] = governor->current_kv_pos + i + j;
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j] = false;
        }
        // Only compute logits for the last token of the last chunk
        if (is_last_chunk) {
            batch.logits[chunk_len - 1] = true;
        }
        
        if (llama_decode(governor->llm_ctx, batch) != 0) {
            GOV_ERROR("Failed to process summary prompt chunk at token %d", i);
            llama_batch_free(batch);
            free(summary_tokens);
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        llama_batch_free(batch);
    }
    
    GOV_LOG("Summary prompt processed, generating response...");
    
    // Generate summary
    char llm_summary[1024] = {0};
    int summary_len = 0;
    bool summary_complete = false;
    
    struct llama_sampler* temp_sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(temp_sampler, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(temp_sampler, llama_sampler_init_dist(0));
    
    int current_gen_pos = governor->current_kv_pos + n_summary_tokens;
    
    for (int i = 0; i < 100 && !summary_complete; i++) {
        llama_token next_token = llama_sampler_sample(temp_sampler, governor->llm_ctx, -1);
        llama_sampler_accept(temp_sampler, next_token);
        
        const struct llama_vocab* vocab_check = llama_model_get_vocab(governor->llm_model);
        if (next_token == llama_vocab_eos(vocab_check) || 
            next_token == llama_vocab_nl(vocab_check)) {
            summary_complete = true;
        } else {
            const struct llama_vocab* vocab = llama_model_get_vocab(governor->llm_model);
            char piece[32];
            int n_chars = llama_token_to_piece(vocab, next_token, piece, sizeof(piece), 0, false);
            
            if (n_chars > 0 && summary_len + n_chars < sizeof(llm_summary) - 1) {
                memcpy(llm_summary + summary_len, piece, n_chars);
                summary_len += n_chars;
                llm_summary[summary_len] = '\0';
            }
            
            // Feed the token back for next iteration
            llama_batch batch = llama_batch_init(1, 0, 1);
            batch.n_tokens = 1;
            batch.token[0] = next_token;
            batch.pos[0] = current_gen_pos + i;
            batch.n_seq_id[0] = 1;
            batch.seq_id[0][0] = 0;
            batch.logits[0] = true;
            
            if (llama_decode(governor->llm_ctx, batch) != 0) {
                llama_batch_free(batch);
                break;
            }
            
            llama_batch_free(batch);
        }
    }
    
    llama_sampler_free(temp_sampler);
    free(summary_tokens);
    
    GOV_LOG("Generated summary: %s", llm_summary[0] ? llm_summary : "(empty)");
    
    // Store in memory
    ethervox_tool_t* memory_tool = NULL;
    for (uint32_t i = 0; i < governor->tool_registry->tool_count; i++) {
        if (strcmp(governor->tool_registry->tools[i].name, "memory_store") == 0) {
            memory_tool = &governor->tool_registry->tools[i];
            break;
        }
    }
    
    if (memory_tool && llm_summary[0] != '\0') {
        char summary_content[2048];
        snprintf(summary_content, sizeof(summary_content),
            "[Manual Cache Clear - Context Summary]\n\n%s\n\n"
            "Turn count: %u. Cache cleared manually at %d%% capacity.",
            llm_summary, governor->turn_counter, (max_pos * 100 / n_ctx));
        
        // Escape for JSON
        char escaped_summary[4096];
        int esc_idx = 0;
        for (int i = 0; summary_content[i] && esc_idx < sizeof(escaped_summary) - 2; i++) {
            if (summary_content[i] == '"' || summary_content[i] == '\\') {
                escaped_summary[esc_idx++] = '\\';
            }
            if (summary_content[i] == '\n') {
                escaped_summary[esc_idx++] = '\\';
                escaped_summary[esc_idx++] = 'n';
            } else {
                escaped_summary[esc_idx++] = summary_content[i];
            }
        }
        escaped_summary[esc_idx] = '\0';
        
        char memory_args[8192];
        snprintf(memory_args, sizeof(memory_args),
                "{\"text\":\"%s\","
                "\"importance\":0.90,"
                "\"tags\":[\"context_summary\",\"manual_clear\",\"auto_generated\"]}",
                escaped_summary);
        
        char* store_result = NULL;
        char* store_error = NULL;
        
        if (memory_tool->execute(memory_args, &store_result, &store_error) == 0) {
            GOV_LOG("Stored conversation summary in memory");
        } else {
            GOV_ERROR("Failed to store summary: %s", store_error ? store_error : "unknown");
        }
        
        free(store_result);
        free(store_error);
    }
    
    // Clear the cache
    llama_memory_seq_rm(mem, 0, governor->system_prompt_token_count, -1);
    governor->current_kv_pos = governor->system_prompt_token_count;
    
    GOV_LOG("Cache cleared: now at position %d (%d%% full)",
            governor->current_kv_pos, (governor->current_kv_pos * 100 / n_ctx));
    
    return ETHERVOX_SUCCESS;
#endif
}
