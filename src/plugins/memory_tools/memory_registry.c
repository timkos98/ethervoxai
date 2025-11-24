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

// Global memory store pointer for tool wrappers
// Note: Not thread-safe, but sufficient for single-threaded CLI
static ethervox_memory_store_t* g_memory_store = NULL;

// JSON parsing helper (simplified - would use cJSON in production)
static int parse_json_string(const char* json, const char* key, char* value, size_t value_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    
    const char* start = strstr(json, search);
    if (!start) return -1;
    
    start += strlen(search);
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

static int tool_memory_store_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    
    char text[ETHERVOX_MEMORY_MAX_TEXT_LEN];
    float importance = 0.5f;
    bool is_user = false;
    
    // Parse arguments
    if (parse_json_string(args_json, "text", text, sizeof(text)) != 0) {
        *error = strdup("Missing 'text' parameter");
        return -1;
    }
    
    parse_json_float(args_json, "importance", &importance);
    parse_json_bool(args_json, "is_user", &is_user);
    
    // TODO: Parse tags array
    const char* tags[] = {"general"};
    uint32_t tag_count = 1;
    
    uint64_t memory_id;
    if (ethervox_memory_store_add(store, text, tags, tag_count,
                                  importance, is_user, &memory_id) != 0) {
        *error = strdup("Failed to store memory");
        return -1;
    }
    
    // Return result
    char* res = malloc(256);
    snprintf(res, 256, "{\"success\":true,\"memory_id\":%llu}", (unsigned long long)memory_id);
    *result = res;
    
    return 0;
}

static int tool_memory_search_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    
    char query[512] = {0};
    uint32_t limit = 10;
    
    parse_json_string(args_json, "query", query, sizeof(query));
    parse_json_uint(args_json, "limit", &limit);
    
    ethervox_memory_search_result_t* results = NULL;
    uint32_t result_count = 0;
    
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

static int tool_memory_export_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_memory_store_t* store = g_memory_store;
    
    char filepath[512];
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
    
    // Register memory_store tool
    ethervox_tool_t tool_store = {
        .name = "memory_store",
        .description = "Save a fact or event to conversation memory",
        .parameters_json_schema = 
            "{\"type\":\"object\",\"properties\":{"
            "\"text\":{\"type\":\"string\",\"description\":\"Content to remember\"},"
            "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
            "\"importance\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
            "\"is_user\":{\"type\":\"boolean\"}"
            "},\"required\":[\"text\"]}",
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
    
    // Register memory_summarize tool
    ethervox_tool_t tool_summarize = {
        .name = "memory_summarize",
        .description = "Generate summary of recent conversation",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"window_size\":{\"type\":\"integer\",\"minimum\":1},"
            "\"focus_topic\":{\"type\":\"string\"}"
            "}}",
        .execute = tool_memory_summarize_wrapper,
        .is_deterministic = true,
        .requires_confirmation = false,
        .is_stateful = false,
        .estimated_latency_ms = 20.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_summarize);
    
    // Register memory_export tool
    ethervox_tool_t tool_export = {
        .name = "memory_export",
        .description = "Export conversation to JSON or Markdown file",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"filepath\":{\"type\":\"string\"},"
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"markdown\"]}"
            "},\"required\":[\"filepath\"]}",
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
