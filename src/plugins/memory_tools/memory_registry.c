/**
 * @file memory_registry.c
 * @brief Register memory tools with Governor tool registry
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/memory_tools.h"
#include "ethervox/governor.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Global memory store pointer for tool wrappers
// Note: Not thread-safe, but sufficient for single-threaded CLI
static ethervox_memory_store_t* g_memory_store = NULL;

// JSON parsing helper (simplified - would use cJSON in production)
static int parse_json_string(const char* json, const char* key, char* value, size_t value_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    
    const char* start = strstr(json, search);
    if (!start) return -1;
    
    start += strlen(search);
    
    // Skip whitespace after colon
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }
    
    // Expect opening quote
    if (*start != '"') return -1;
    start++;
    
    const char* end = strchr(start, '"');
    if (!end) return -1;
    
    size_t len = end - start;
    if (len >= value_len) len = value_len - 1;
    
    strncpy(value, start, len);
    value[len] = '\0';
    return 0;
}

static int parse_json_float(const char* json, const char* key, float* value) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    
    const char* start = strstr(json, search);
    if (!start) return -1;
    
    start += strlen(search);
    *value = strtof(start, NULL);
    return 0;
}

static int parse_json_uint(const char* json, const char* key, uint32_t* value) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    
    const char* start = strstr(json, search);
    if (!start) return -1;
    
    start += strlen(search);
    *value = strtoul(start, NULL, 10);
    return 0;
}

static int parse_json_bool(const char* json, const char* key, bool* value) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    
    const char* start = strstr(json, search);
    if (!start) return -1;
    
    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;
    
    *value = (strncmp(start, "true", 4) == 0);
    return 0;
}

// Tool wrappers that bridge between Governor API and memory functions


// Parse a JSON array of strings: "tags": ["reminder", "recurring"]
// OR a single string: "tags": "reminder"
static int parse_json_string_array(const char* json, const char* key, char tags[][32], uint32_t* tag_count, uint32_t max_tags) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* start = strstr(json, search);
    if (!start) return -1;
    start += strlen(search);
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    
    *tag_count = 0;
    
    // Handle single string: "tags": "reminder"
    if (*start == '"') {
        start++;
        const char* end = strchr(start, '"');
        if (!end) return -1;
        size_t len = end - start;
        if (len > 0 && len < 32) {
            strncpy(tags[0], start, len);
            tags[0][len] = '\0';
            (*tag_count) = 1;
            return 0;
        }
        return -1;
    }
    
    // Handle array: "tags": ["reminder", "recurring"]
    if (*start != '[') return -1;
    start++;
    while (*start && *start != ']') {
        while (*start == ' ' || *start == '\t' || *start == ',') start++;
        if (*start == '"') {
            start++;
            const char* end = strchr(start, '"');
            if (!end) break;
            size_t len = end - start;
            if (len > 0 && len < 32 && *tag_count < max_tags) {
                strncpy(tags[*tag_count], start, len);
                tags[*tag_count][len] = '\0';
                (*tag_count)++;
            }
            start = end + 1;
        } else {
            break;
        }
    }
    return (*tag_count > 0) ? 0 : -1;
}

static int tool_memory_store_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    char text[ETHERVOX_MEMORY_MAX_TEXT_LEN];
    float importance = 0.5f;
    bool is_user = false;
    char tags[8][32];
    uint32_t tag_count = 0;

    // Parse arguments
    if (parse_json_string(args_json, "text", text, sizeof(text)) != 0) {
        *error = strdup("Missing 'text' parameter");
        return -1;
    }
    parse_json_float(args_json, "importance", &importance);
    parse_json_bool(args_json, "is_user", &is_user);
    
    // Try to parse tags; if none provided, default to "general"
    if (parse_json_string_array(args_json, "tags", tags, &tag_count, 8) != 0) {
        // Only use "general" if tags were not provided at all
        strncpy(tags[0], "general", 32);
        tag_count = 1;
    }

    // Convert to const char* array
    const char* tag_ptrs[8];
    for (uint32_t i = 0; i < tag_count; i++) tag_ptrs[i] = tags[i];

    uint64_t memory_id;
    if (ethervox_memory_store_add(store, text, tag_ptrs, tag_count,
                                  importance, is_user, &memory_id) != 0) {
        *error = strdup("Failed to store memory");
        return -1;
    }
    char* res = malloc(256);
    snprintf(res, 256, "{\"success\":true,\"memory_id\":%llu}", (unsigned long long)memory_id);
    *result = res;
    return 0;
}
// Tool: memory_reminder_list - returns all reminders (tagged 'reminder')
static int tool_memory_reminder_list_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    ethervox_memory_search_result_t* results = NULL;
    uint32_t result_count = 0;
    const char* tag_filter[] = {"reminder"};
    // Find up to 32 reminders
    if (ethervox_memory_search(store, NULL, tag_filter, 1, 32, &results, &result_count) != 0) {
        *error = strdup("Search failed");
        return -1;
    }
    // Build JSON response
    size_t res_len = 4096;
    char* res = malloc(res_len);
    int pos = snprintf(res, res_len, "{\"reminders\":[");
    for (uint32_t i = 0; i < result_count; i++) {
        const ethervox_memory_entry_t* e = &results[i].entry;
        pos += snprintf(res + pos, res_len - pos,
            "%s{\"text\":\"%s\",\"tags\":[",
            i > 0 ? "," : "",
            e->text);
        for (uint32_t t = 0; t < e->tag_count; t++) {
            pos += snprintf(res + pos, res_len - pos, "%s\"%s\"", t > 0 ? "," : "", e->tags[t]);
        }
        pos += snprintf(res + pos, res_len - pos, "],\"timestamp\":%llu}", (unsigned long long)e->timestamp);
    }
    snprintf(res + pos, res_len - pos, "],\"count\":%u}", result_count);
    free(results);
    *result = res;
    return 0;
}

static int tool_memory_search_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    
    ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__,
                "memory_search_wrapper INPUT: args_json='%s'", args_json ? args_json : "(null)");
    
    char query[512] = {0};
    uint32_t limit = 10;
    
    parse_json_string(args_json, "query", query, sizeof(query));
    parse_json_uint(args_json, "limit", &limit);
    
    ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__,
                "memory_search_wrapper: query='%s', limit=%u, query[0]=%d",
                query, limit, (int)query[0]);
    
    ethervox_memory_search_result_t* results = NULL;
    uint32_t result_count = 0;
    
    // If empty query, return most recent message instead
    if (query[0] == '\0' && store && store->entry_count > 0) {
        // Find the most recent entry
        ethervox_memory_entry_t* most_recent = &store->entries[0];
        for (uint32_t i = 1; i < store->entry_count; i++) {
            if (store->entries[i].timestamp > most_recent->timestamp) {
                most_recent = &store->entries[i];
            }
        }
        
        // Build JSON response with most recent entry
        size_t res_len = 4096;
        char* res = malloc(res_len);
        snprintf(res, res_len,
                "{\"results\":[{\"text\":\"%s\",\"relevance\":1.0,\"importance\":%.2f}],\"count\":1}",
                most_recent->text,
                most_recent->importance);
        
        *result = res;
        return 0;
    }
    
    if (ethervox_memory_search(store, query, NULL, 0, limit,
                              &results, &result_count) != 0) {
        *error = strdup("Search failed");
        return -1;
    }
    
    // Build JSON response
    size_t res_len = 4096;
    char* res = malloc(res_len);
    int pos = snprintf(res, res_len, "{\"results\":[");
    
    for (uint32_t i = 0; i < result_count && i < limit; i++) {
        pos += snprintf(res + pos, res_len - pos,
                       "%s{\"text\":\"%s\",\"relevance\":%.2f,\"importance\":%.2f}",
                       i > 0 ? "," : "",
                       results[i].entry.text,
                       results[i].relevance,
                       results[i].entry.importance);
    }
    
    snprintf(res + pos, res_len - pos, "],\"count\":%u}", result_count);
    
    free(results);
    *result = res;
    
    return 0;
}

static int tool_memory_summarize_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    
    uint32_t window_size = 10;
    char focus[256] = {0};
    
    parse_json_uint(args_json, "window_size", &window_size);
    parse_json_string(args_json, "focus_topic", focus, sizeof(focus));
    
    char* summary = NULL;
    if (ethervox_memory_summarize(store, window_size,
                                  focus[0] ? focus : NULL,
                                  &summary, NULL, NULL) != 0) {
        *error = strdup("Summarization failed");
        return -1;
    }
    
    // Return as JSON
    size_t res_len = strlen(summary) + 256;
    char* res = malloc(res_len);
    snprintf(res, res_len, "{\"summary\":\"%s\"}", summary);
    
    free(summary);
    *result = res;
    
    return 0;
}

// Tool: memory_complete_reminder - marks a reminder as completed by adding a 'completed' tag
static int tool_memory_complete_reminder_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    if (!store) {
        *error = strdup("Memory store not initialized");
        return -1;
    }
    
    uint64_t memory_id = 0;
    char id_str[32];
    if (parse_json_string(args_json, "memory_id", id_str, sizeof(id_str)) != 0) {
        *error = strdup("Missing 'memory_id' parameter");
        return -1;
    }
    memory_id = strtoull(id_str, NULL, 10);
    
    int found = 0;
    for (uint32_t i = 0; i < store->entry_count; i++) {
        ethervox_memory_entry_t* entry = &store->entries[i];
        if (entry->memory_id == memory_id) {
            int already = 0;
            for (uint32_t t = 0; t < entry->tag_count; t++) {
                if (strncmp(entry->tags[t], "completed", ETHERVOX_MEMORY_TAG_LEN) == 0) {
                    already = 1;
                    break;
                }
            }
            if (!already && entry->tag_count < ETHERVOX_MEMORY_MAX_TAGS) {
                strncpy(entry->tags[entry->tag_count], "completed", ETHERVOX_MEMORY_TAG_LEN - 1);
                entry->tags[entry->tag_count][ETHERVOX_MEMORY_TAG_LEN - 1] = '\0';
                entry->tag_count++;
            }
            found = 1;
            break;
        }
    }
    if (!found) {
        *error = strdup("Reminder not found");
        return -1;
    }
    char* res = malloc(64);
    snprintf(res, 64, "{\"success\":true,\"memory_id\":%llu}", (unsigned long long)memory_id);
    *result = res;
    return 0;
}

// Tool: memory_export_wrapper - export conversation to file
static int tool_memory_export_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    if (!store) {
        *error = strdup("Memory store not initialized");
        return -1;
    }
    
    char filepath[512] = {0};
    char format[32] = "json";
    
    if (parse_json_string(args_json, "filepath", filepath, sizeof(filepath)) != 0) {
        *error = strdup("Missing 'filepath' parameter");
        return -1;
    }
    
    parse_json_string(args_json, "format", format, sizeof(format));
    
    uint64_t bytes_written = 0;
    if (ethervox_memory_export(store, filepath, format, &bytes_written) != 0) {
        *error = strdup("Export failed");
        return -1;
    }
    
    char* res = malloc(256);
    snprintf(res, 256, "{\"success\":true,\"bytes_written\":%llu}", (unsigned long long)bytes_written);
    *result = res;
    
    return 0;
}

static int tool_memory_forget_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    
    uint64_t older_than = 0;
    float importance_threshold = 0.0f;
    
    parse_json_uint(args_json, "older_than_seconds", (uint32_t*)&older_than);
    parse_json_float(args_json, "importance_threshold", &importance_threshold);
    
    uint32_t pruned = 0;
    if (ethervox_memory_forget(store, older_than, importance_threshold, &pruned) != 0) {
        *error = strdup("Forget operation failed");
        return -1;
    }
    
    char* res = malloc(256);
    snprintf(res, 256, "{\"success\":true,\"items_pruned\":%u}", pruned);
    *result = res;
    
    return 0;
}

int ethervox_memory_tools_register(
    void* registry_ptr,
    ethervox_memory_store_t* store
) {


    ethervox_tool_registry_t* registry = (ethervox_tool_registry_t*)registry_ptr;
    if (!registry || !store) {
        return -1;
    }
    // Set global memory store for tool wrappers to access
    g_memory_store = store;
    int ret = 0;

    // Register memory_complete_reminder tool
    ethervox_tool_t tool_complete_reminder = {
        .name = "memory_complete_reminder",
        .description = "Mark a reminder as completed/done by its memory_id. Use the memory_id from memory_reminder_list to complete specific reminders.",
        .parameters_json_schema =
            "{"
            "  \"type\": \"object\","
            "  \"properties\": {"
            "    \"memory_id\": {\"type\": \"string\", \"description\": \"The unique ID of the reminder to mark complete (from memory_reminder_list)\"}"
            "  },"
            "  \"required\": [\"memory_id\"]"
            "}",
        .execute = tool_memory_complete_reminder_wrapper,
        .is_deterministic = false,
        .requires_confirmation = false,
        .is_stateful = true,
        .estimated_latency_ms = 5.0f
    };
    ret |= ethervox_tool_registry_add(registry, &tool_complete_reminder);
    
    // Register memory_store tool (single definition)
    ethervox_tool_t tool_store = {
        .name = "memory_store",
        .description = "Save a fact, event, or reminder to conversation memory. For reminders, include 'reminder' tag and deadline info in text (e.g., 'Call John at 2:30 PM today' or 'Team meeting in 5 mins'). Use importance 0.0-1.0 where 1.0 is critical.",
        .parameters_json_schema =
            "{"
            "  \"type\": \"object\","
            "  \"properties\": {"
            "    \"text\": {\"type\": \"string\", \"description\": \"Content to remember. For reminders, include deadline/time info in the text.\"},"
            "    \"tags\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Labels like 'reminder', 'important', 'urgent', etc.\"},"
            "    \"importance\": {\"type\": \"number\", \"minimum\": 0, \"maximum\": 1, \"description\": \"0.0=low to 1.0=critical. Use 0.9+ for urgent reminders.\"},"
            "    \"is_user\": {\"type\": \"boolean\", \"description\": \"True if this is user input, false if assistant generated\"}"
            "  },"
            "  \"required\": [\"text\"]"
            "}",
        .execute = tool_memory_store_wrapper,
        .is_deterministic = false,
        .requires_confirmation = false,
        .is_stateful = true,
        .estimated_latency_ms = 5.0f
    };
    ret |= ethervox_tool_registry_add(registry, &tool_store);
    
    // Register memory_search tool
    ethervox_tool_t tool_search = {
        .name = "memory_search",
        .description = "Search conversation memory by text similarity and tags",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"
            "\"tag_filter\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
            "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}"
            "}}",
        .execute = tool_memory_search_wrapper,
        .is_deterministic = true,
        .requires_confirmation = false,
        .is_stateful = false,
        .estimated_latency_ms = 10.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_search);
    


    // Register memory_reminder_list tool
    ethervox_tool_t tool_reminder_list = {
        .name = "memory_reminder_list",
        .description = "List all active reminders (entries tagged with 'reminder'). Returns reminder text, deadlines, importance, and memory_id for completing reminders.",
        .parameters_json_schema = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_memory_reminder_list_wrapper,
        .is_deterministic = true,
        .requires_confirmation = false,
        .is_stateful = false,
        .estimated_latency_ms = 10.0f
    };
    ret |= ethervox_tool_registry_add(registry, &tool_reminder_list);
    ethervox_tool_t tool_export = {
        .name = "memory_export",
        .description = "Export the entire conversation history to a file. Requires both filepath and format. Example: export to './notes.md' as markdown",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"filepath\":{\"type\":\"string\",\"description\":\"Path where to save the file (required). Example: './conversation.md' or './notes.json'\"},"
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"markdown\"],\"description\":\"File format: 'json' or 'markdown' (required)\"}"
            "},\"required\":[\"filepath\",\"format\"]}",
        .execute = tool_memory_export_wrapper,
        .is_deterministic = true,
        .requires_confirmation = true,
        .is_stateful = true,
        .estimated_latency_ms = 50.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_export);
    
    // Register memory_forget tool
    ethervox_tool_t tool_forget = {
        .name = "memory_forget",
        .description = "Prune old or low-importance memories",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"older_than_seconds\":{\"type\":\"integer\",\"minimum\":0},"
            "\"importance_threshold\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}"
            "}}",
        .execute = tool_memory_forget_wrapper,
        .is_deterministic = false,
        .requires_confirmation = true,
        .is_stateful = true,
        .estimated_latency_ms = 15.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_forget);
    
    if (ret == 0) {
        ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                    "Registered 5 memory tools with Governor");
    }
    
    return ret;
}
