/**
 * @file file_registry.c
 * @brief Register file tools with Governor tool registry
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/file_tools.h"
#include "ethervox/governor.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global file config pointer for tool wrappers
static ethervox_file_tools_config_t* g_file_config = NULL;

// JSON parsing helpers (same as memory_registry.c)
static int parse_json_string(const char* json, const char* key, char* value, size_t value_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    
    const char* start = strstr(json, search);
    if (!start) {
        // Try with space after colon: "key": "value"
        snprintf(search, sizeof(search), "\"%s\": \"", key);
        start = strstr(json, search);
        if (!start) return -1;
    }
    
    start += strlen(search);
    const char* end = strchr(start, '"');
    if (!end) return -1;
    
    size_t len = end - start;
    if (len >= value_len) len = value_len - 1;
    
    strncpy(value, start, len);
    value[len] = '\0';
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

// Tool wrapper: file_list
static int tool_file_list_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_file_tools_config_t* config = g_file_config;
    
    char directory[ETHERVOX_FILE_MAX_PATH];
    bool recursive = false;
    
    if (parse_json_string(args_json, "directory", directory, sizeof(directory)) != 0) {
        *error = strdup("Missing 'directory' parameter");
        return -1;
    }
    
    parse_json_bool(args_json, "recursive", &recursive);
    
    ethervox_file_entry_t* entries = NULL;
    uint32_t entry_count = 0;
    
    if (ethervox_file_list(config, directory, recursive, &entries, &entry_count) != 0) {
        *error = strdup("Failed to list directory");
        return -1;
    }
    
    // Build JSON response
    size_t res_len = entry_count * 512 + 256;
    char* res = malloc(res_len);
    int pos = snprintf(res, res_len, "{\"entries\":[");
    
    for (uint32_t i = 0; i < entry_count; i++) {
        pos += snprintf(res + pos, res_len - pos,
                       "%s{\"name\":\"%s\",\"path\":\"%s\",\"is_directory\":%s,\"size\":%llu}",
                       i > 0 ? "," : "",
                       entries[i].name,
                       entries[i].path,
                       entries[i].is_directory ? "true" : "false",
                       (unsigned long long)entries[i].size);
    }
    
    snprintf(res + pos, res_len - pos, "],\"count\":%u}", entry_count);
    
    free(entries);
    *result = res;
    
    return 0;
}

// Tool wrapper: file_read
static int tool_file_read_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_file_tools_config_t* config = g_file_config;
    
    char file_path[ETHERVOX_FILE_MAX_PATH];
    
    if (parse_json_string(args_json, "file_path", file_path, sizeof(file_path)) != 0) {
        *error = strdup("Missing 'file_path' parameter");
        return -1;
    }
    
    char* content = NULL;
    uint64_t size = 0;
    
    if (ethervox_file_read(config, file_path, &content, &size) != 0) {
        *error = strdup("Failed to read file");
        return -1;
    }
    
    // Build JSON response (escape content)
    // Allocate 2x size for escaping + extra for JSON structure
    size_t res_len = (size * 2) + 1024;
    char* res = malloc(res_len);
    if (!res) {
        free(content);
        *error = strdup("Failed to allocate response buffer");
        return -1;
    }
    
    int pos = snprintf(res, res_len, "{\"content\":\"");
    
    // Simple JSON escaping - check bounds carefully
    for (uint64_t i = 0; i < size && pos < (int)res_len - 200; i++) {
        if (content[i] == '"' || content[i] == '\\') {
            res[pos++] = '\\';
            res[pos++] = content[i];
        } else if (content[i] == '\n') {
            res[pos++] = '\\';
            res[pos++] = 'n';
        } else if (content[i] == '\r') {
            res[pos++] = '\\';
            res[pos++] = 'r';
        } else if (content[i] == '\t') {
            res[pos++] = '\\';
            res[pos++] = 't';
        } else if ((unsigned char)content[i] >= 32) {
            res[pos++] = content[i];
        }
    }
    
    snprintf(res + pos, res_len - pos, "\",\"size\":%llu}", (unsigned long long)size);
    
    free(content);
    *result = res;
    
    return 0;
}

// Tool wrapper: file_search
static int tool_file_search_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_file_tools_config_t* config = g_file_config;
    
    char directory[ETHERVOX_FILE_MAX_PATH];
    char pattern[512];
    
    if (parse_json_string(args_json, "directory", directory, sizeof(directory)) != 0) {
        *error = strdup("Missing 'directory' parameter");
        return -1;
    }
    
    if (parse_json_string(args_json, "pattern", pattern, sizeof(pattern)) != 0) {
        *error = strdup("Missing 'pattern' parameter");
        return -1;
    }
    
    char** matches = NULL;
    uint32_t match_count = 0;
    
    if (ethervox_file_search(config, directory, pattern, &matches, &match_count) != 0) {
        *error = strdup("Search failed");
        return -1;
    }
    
    // Build JSON response
    size_t res_len = match_count * 256 + 256;
    char* res = malloc(res_len);
    int pos = snprintf(res, res_len, "{\"matches\":[");
    
    for (uint32_t i = 0; i < match_count; i++) {
        pos += snprintf(res + pos, res_len - pos,
                       "%s\"%s\"",
                       i > 0 ? "," : "",
                       matches[i]);
        free(matches[i]);
    }
    
    snprintf(res + pos, res_len - pos, "],\"count\":%u}", match_count);
    
    free(matches);
    *result = res;
    
    return 0;
}

// Tool wrapper: file_write
static int tool_file_write_wrapper(
    const char* args_json,
    char** result,
    char** error
) {
    ethervox_file_tools_config_t* config = g_file_config;
    
    char file_path[ETHERVOX_FILE_MAX_PATH];
    char content[ETHERVOX_FILE_MAX_SIZE];
    
    if (parse_json_string(args_json, "file_path", file_path, sizeof(file_path)) != 0) {
        *error = strdup("Missing 'file_path' parameter");
        return -1;
    }
    
    if (parse_json_string(args_json, "content", content, sizeof(content)) != 0) {
        *error = strdup("Missing 'content' parameter");
        return -1;
    }
    
    if (ethervox_file_write(config, file_path, content) != 0) {
        *error = strdup("Write failed (check permissions and access mode)");
        return -1;
    }
    
    char* res = malloc(64);
    snprintf(res, 64, "{\"success\":true}");
    *result = res;
    
    return 0;
}

int ethervox_file_tools_register(
    void* registry_ptr,
    ethervox_file_tools_config_t* config
) {
    ethervox_tool_registry_t* registry = (ethervox_tool_registry_t*)registry_ptr;
    if (!registry || !config) {
        return -1;
    }
    
    // Set global config for tool wrappers
    g_file_config = config;
    
    int ret = 0;
    
    // Register file_list tool
    ethervox_tool_t tool_list = {
        .name = "file_list",
        .description = "List files and directories in a given path. Only reads .txt, .md, and .org files.",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"directory\":{\"type\":\"string\",\"description\":\"Directory path to list\"},"
            "\"recursive\":{\"type\":\"boolean\",\"description\":\"Recurse into subdirectories\"}"
            "},\"required\":[\"directory\"]}",
        .execute = tool_file_list_wrapper,
        .is_deterministic = true,
        .requires_confirmation = false,
        .is_stateful = false,
        .estimated_latency_ms = 50.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_list);
    
    // Register file_read tool
    ethervox_tool_t tool_read = {
        .name = "file_read",
        .description = "Read contents of a text file (.txt, .md, .org). Maximum 10MB.",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"file_path\":{\"type\":\"string\",\"description\":\"Path to file to read\"}"
            "},\"required\":[\"file_path\"]}",
        .execute = tool_file_read_wrapper,
        .is_deterministic = true,
        .requires_confirmation = false,
        .is_stateful = false,
        .estimated_latency_ms = 100.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_read);
    
    // Register file_search tool
    ethervox_tool_t tool_search = {
        .name = "file_search",
        .description = "Search for text pattern in all allowed files within a directory",
        .parameters_json_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"directory\":{\"type\":\"string\",\"description\":\"Directory to search in\"},"
            "\"pattern\":{\"type\":\"string\",\"description\":\"Text pattern to search for\"}"
            "},\"required\":[\"directory\",\"pattern\"]}",
        .execute = tool_file_search_wrapper,
        .is_deterministic = true,
        .requires_confirmation = false,
        .is_stateful = false,
        .estimated_latency_ms = 500.0f
    };
    
    ret |= ethervox_tool_registry_add(registry, &tool_search);
    
    // Register file_write tool (only if write access enabled)
    if (config->access_mode == ETHERVOX_FILE_ACCESS_READ_WRITE) {
        ethervox_tool_t tool_write = {
            .name = "file_write",
            .description = "Write content to a file. WRITE ACCESS ENABLED - use with caution.",
            .parameters_json_schema =
                "{\"type\":\"object\",\"properties\":{"
                "\"file_path\":{\"type\":\"string\",\"description\":\"Path to file to write\"},"
                "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
                "},\"required\":[\"file_path\",\"content\"]}",
            .execute = tool_file_write_wrapper,
            .is_deterministic = false,
            .requires_confirmation = true,
            .is_stateful = true,
            .estimated_latency_ms = 100.0f
        };
        
        ret |= ethervox_tool_registry_add(registry, &tool_write);
        
        ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                    "Registered 4 file tools (including write access)");
    } else {
        ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                    "Registered 3 file tools (read-only mode)");
    }
    
    return ret;
}
