/**
 * @file tool_registry.c
 * @brief Tool registry implementation
 *
 * Manages the catalog of available tools that can be invoked by the Governor.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/governor.h"
#include "ethervox/tool_manifest.h"
#include "ethervox/tool_prompt_optimizer.h"
#include "ethervox/logging.h"
#include "ethervox/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ethervox_result_t ethervox_tool_registry_init(ethervox_tool_registry_t* registry, uint32_t initial_capacity) {
    if (!registry) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    if (initial_capacity == 0) {
        initial_capacity = 16; // Default capacity
    }
    
    registry->tools = (ethervox_tool_t*)calloc(initial_capacity, sizeof(ethervox_tool_t));
    if (!registry->tools) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    registry->tool_count = 0;
    registry->capacity = initial_capacity;
    
    return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_tool_registry_add(ethervox_tool_registry_t* registry, const ethervox_tool_t* tool) {
    if (!registry || !tool) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Check if tool already exists
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        if (strcmp(registry->tools[i].name, tool->name) == 0) {
            return -2; // Tool already registered
        }
    }
    
    // Expand capacity if needed
    if (registry->tool_count >= registry->capacity) {
        uint32_t new_capacity = registry->capacity * 2;
        ethervox_tool_t* new_tools = (ethervox_tool_t*)realloc(
            registry->tools,
            new_capacity * sizeof(ethervox_tool_t)
        );
        
        if (!new_tools) {
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        registry->tools = new_tools;
        registry->capacity = new_capacity;
    }
    
    // Copy tool definition
    memcpy(&registry->tools[registry->tool_count], tool, sizeof(ethervox_tool_t));
    registry->tool_count++;
    
    return ETHERVOX_SUCCESS;
}

const ethervox_tool_t* ethervox_tool_registry_find(
    const ethervox_tool_registry_t* registry,
    const char* name
) {
    if (!registry || !name) {
        return NULL;
    }
    
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        if (strcmp(registry->tools[i].name, name) == 0) {
            return &registry->tools[i];
        }
    }
    
    return NULL;
}

ethervox_result_t ethervox_tool_registry_export_manifest(
    const ethervox_tool_registry_t* registry,
    const char* binary_path
) {
    if (!registry || !binary_path) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Open output file
    FILE* fp = fopen(binary_path, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to create manifest: %s\n", binary_path);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Write header
    tool_manifest_header_t header = {0};
    header.magic = TOOL_MANIFEST_MAGIC;
    header.version = TOOL_MANIFEST_VERSION;
    header.tool_count = registry->tool_count;
    header.index_offset = sizeof(tool_manifest_header_t);
    
    // Calculate detail offset (after all index entries)
    header.detail_offset = header.index_offset + 
                          (registry->tool_count * sizeof(tool_index_entry_t));
    header.checksum_type = 1; // CRC32
    
    if (fwrite(&header, sizeof(tool_manifest_header_t), 1, fp) != 1) {
        fclose(fp);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // First pass: calculate detail sizes and write index entries
    uint32_t current_detail_offset = header.detail_offset;
    
    // Allocate temporary buffer for parsed parameters
    typedef struct {
        uint8_t param_count;
        tool_param_t params[MAX_PARAMETERS];
        uint16_t detail_size;
    } tool_export_info_t;
    
    tool_export_info_t* tool_infos = calloc(registry->tool_count, sizeof(tool_export_info_t));
    if (!tool_infos) {
        fclose(fp);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Parse all tool schemas first to calculate sizes
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        tool_export_info_t* info = &tool_infos[i];
        
        // Parse JSON schema to extract parameters
        const char* schema = tool->parameters_json_schema;
        if (schema && schema[0] != '\0') {
            // Simple JSON parsing for parameters
            const char* properties = strstr(schema, "\"properties\"");
            const char* required_arr = strstr(schema, "\"required\"");
            
            if (properties) {
                properties = strchr(properties, '{');
                if (properties) {
                    properties++; // Skip opening brace
                    
                    // Parse each property
                    const char* cursor = properties;
                    while (*cursor && info->param_count < MAX_PARAMETERS) {
                        // Find next property name
                        const char* name_start = strchr(cursor, '"');
                        if (!name_start || name_start >= strstr(cursor, "}")) break;
                        name_start++;
                        
                        const char* name_end = strchr(name_start, '"');
                        if (!name_end) break;
                        
                        // Extract parameter name
                        size_t name_len = name_end - name_start;
                        if (name_len >= 32) name_len = 31;
                        
                        tool_param_t* param = &info->params[info->param_count];
                        strncpy(param->name, name_start, name_len);
                        param->name[name_len] = '\0';
                        
                        // Find the property value object
                        const char* prop_obj = strchr(name_end, '{');
                        if (!prop_obj) break;
                        
                        // Extract type
                        const char* type_field = strstr(prop_obj, "\"type\"");
                        if (type_field && type_field < strstr(prop_obj + 1, "}")) {
                            const char* type_val = strchr(type_field, ':');
                            if (type_val) {
                                type_val = strchr(type_val, '"');
                                if (type_val) {
                                    type_val++;
                                    const char* type_end = strchr(type_val, '"');
                                    if (type_end) {
                                        size_t type_len = type_end - type_val;
                                        if (type_len >= 16) type_len = 15;
                                        strncpy(param->type, type_val, type_len);
                                        param->type[type_len] = '\0';
                                    }
                                }
                            }
                        }
                        
                        // Extract description
                        const char* desc_field = strstr(prop_obj, "\"description\"");
                        if (desc_field && desc_field < strstr(prop_obj + 1, "}")) {
                            const char* desc_val = strchr(desc_field, ':');
                            if (desc_val) {
                                desc_val = strchr(desc_val, '"');
                                if (desc_val) {
                                    desc_val++;
                                    const char* desc_end = desc_val;
                                    // Find end quote, handling escaped quotes
                                    while (*desc_end && !(*desc_end == '"' && *(desc_end - 1) != '\\')) {
                                        desc_end++;
                                    }
                                    if (*desc_end == '"') {
                                        size_t desc_len = desc_end - desc_val;
                                        if (desc_len >= 128) desc_len = 127;
                                        strncpy(param->default_value, desc_val, desc_len);
                                        param->default_value[desc_len] = '\0';
                                    }
                                }
                            }
                        }
                        
                        // Check if required
                        param->required = 0;
                        if (required_arr) {
                            char search_pattern[36];
                            snprintf(search_pattern, sizeof(search_pattern), "\"%s\"", param->name);
                            if (strstr(required_arr, search_pattern)) {
                                param->required = 1;
                            }
                        }
                        
                        info->param_count++;
                        
                        // Move cursor past this property
                        cursor = strstr(prop_obj, "}");
                        if (!cursor) break;
                        cursor++;
                    }
                }
            }
        }
        
        // Calculate detail size: header + parameters
        info->detail_size = sizeof(tool_detail_header_t) + 
                           (info->param_count * sizeof(tool_param_t));
    }
    
    // Write index entries
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        const tool_export_info_t* info = &tool_infos[i];
        
        tool_index_entry_t entry = {0};
        strncpy(entry.name, tool->name, TOOL_NAME_MAX - 1);
        strncpy(entry.one_line, tool->description, TOOL_DESC_MAX - 1);
        strncpy(entry.category, "general", TOOL_CATEGORY_MAX - 1);
        entry.priority = 5;
        entry.enabled = 1;
        entry.detail_offset = current_detail_offset;
        entry.detail_size = info->detail_size;
        
        if (fwrite(&entry, sizeof(tool_index_entry_t), 1, fp) != 1) {
            free(tool_infos);
            fclose(fp);
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        current_detail_offset += info->detail_size;
    }
    
    // Write detail sections with parameters
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        const tool_export_info_t* info = &tool_infos[i];
        
        tool_detail_header_t detail = {0};
        strncpy(detail.name, tool->name, TOOL_NAME_MAX - 1);
        strncpy(detail.description, tool->description, TOOL_DESC_MAX - 1);
        strncpy(detail.category, "general", TOOL_CATEGORY_MAX - 1);
        detail.param_count = info->param_count;
        detail.trigger_count = 0;
        
        if (fwrite(&detail, sizeof(tool_detail_header_t), 1, fp) != 1) {
            free(tool_infos);
            fclose(fp);
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        // Write parameters
        for (uint8_t p = 0; p < info->param_count; p++) {
            if (fwrite(&info->params[p], sizeof(tool_param_t), 1, fp) != 1) {
                free(tool_infos);
                fclose(fp);
                return ETHERVOX_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    
    free(tool_infos);
    
    // Write CRC32 footer
    fseek(fp, 0, SEEK_SET);
    uint8_t* file_data = malloc(current_detail_offset);
    if (file_data) {
        size_t read = fread(file_data, 1, current_detail_offset, fp);
        if (read == current_detail_offset) {
            uint32_t crc = ethervox_tool_crc32(file_data, current_detail_offset);
            fseek(fp, 0, SEEK_END);
            fwrite(&crc, sizeof(uint32_t), 1, fp);
        }
        free(file_data);
    }
    
    fclose(fp);
    
    ETHERVOX_LOG_DEBUG("Exported %u tools to manifest: %s", registry->tool_count, binary_path);
    return ETHERVOX_SUCCESS;
}

// Platform type detection helper
static bool is_mobile_platform(void) {
#if defined(__ANDROID__) || defined(ANDROID) || defined(__APPLE__)
    #ifdef __APPLE__
        #include "TargetConditionals.h"
        #if TARGET_OS_IPHONE || TARGET_OS_IOS
            return true;
        #else
            return false;  // macOS
        #endif
    #else
        return true;  // Android
    #endif
#else
    return false;  // Desktop/ESP32
#endif
}

/**
 * Check if a tool should have its full schema in the system prompt.
 * Fast-path tools are loaded upfront; others are loaded on-demand via get_tool_info.
 */
static bool is_fast_path_tool(const char* tool_name) {
    return (strcmp(tool_name, "calculator_compute") == 0 ||
            strcmp(tool_name, "get_time") == 0 ||
            strcmp(tool_name, "get_date") == 0 ||
            strcmp(tool_name, "get_tool_info") == 0 ||
            strcmp(tool_name, "speak") == 0 ||
            strcmp(tool_name, "listen") == 0);
}

/**
 * Build JSON tool definition for Granite 4.0 format
 * Format: {"type": "function", "function": {"name": "...", "description": "...", "parameters": {...}}}
 */
static int build_json_tool_definition(
    const ethervox_tool_t* tool,
    char* buffer,
    size_t buffer_size
) {
    // Parse the JSON schema to extract just the parameters object
    const char* schema = tool->parameters_json_schema;
    if (!schema || schema[0] == '\0') {
        schema = "{}";
    }
    
    return snprintf(buffer, buffer_size,
        "{\"type\": \"function\", \"function\": {\"name\": \"%s\", \"description\": \"%s\", \"parameters\": %s}}",
        tool->name,
        tool->description,
        schema);
}

ethervox_result_t ethervox_tool_registry_build_system_prompt(
    const ethervox_tool_registry_t* registry,
    const chat_template_t* chat_template,
    char* buffer,
    size_t buffer_size,
    void* memory_store,
    const char* model_path  // Added parameter for optimized prompts
) {
    if (!registry || !chat_template || !buffer || buffer_size == 0) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    bool is_mobile = is_mobile_platform();
    tool_format_type_t tool_format = chat_template_get_tool_format(chat_template);
    
    // Try to load model-specific optimized prompts first
    char custom_instructions[2048] = {0};
    char custom_examples[4096] = {0};
    bool has_custom_prompts = false;
    
    if (model_path && ethervox_load_optimized_prompts(
        model_path,
        custom_instructions,
        sizeof(custom_instructions),
        custom_examples,
        sizeof(custom_examples)
    ) == 0) {
        has_custom_prompts = true;
        ETHERVOX_LOG_INFO("Using optimized prompts for this model");
    } else {
        ETHERVOX_LOG_WARN("No optimized prompts found - tool usage may be inefficient");
        ETHERVOX_LOG_WARN("Run optimization via Settings to improve performance");
    }
    
    int written = 0;
    size_t remaining = buffer_size;
    char* ptr = buffer;
    
    // Format differs significantly between XML and JSON tool formats
    if (tool_format == TOOL_FORMAT_JSON_IN_XML) {
        // Granite format: system prompt with JSON tools in <tools></tools>
        // NOTE: Do NOT include chat template markers (system_start/end) as visible text!
        // The tokenizer handles these automatically as special tokens.
        written = snprintf(ptr, remaining,
            "You are a helpful assistant with access to tools. "
            "When generating stories or creative content, write complete narratives with proper endings. "
            "For conversations: Only generate YOUR response, then STOP. DO NOT generate the user's next message. "
            "You are provided with function signatures within <tools></tools> XML tags:\n<tools>\n");
        
        if (written < 0 || (size_t)written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        ptr += written;
        remaining -= written;
        
        // Add each fast-path tool as JSON
        for (uint32_t i = 0; i < registry->tool_count; i++) {
            const ethervox_tool_t* tool = &registry->tools[i];
            if (!is_fast_path_tool(tool->name)) continue;
            
            ETHERVOX_LOG_INFO("Adding fast-path tool to system prompt: %s", tool->name);
            
            char json_tool[1024];
            int json_len = build_json_tool_definition(tool, json_tool, sizeof(json_tool));
            if (json_len < 0 || json_len >= (int)sizeof(json_tool)) {
                ETHERVOX_LOG_WARN("Tool %s JSON too large (%d bytes), skipping from fast-path", 
                                 tool->name, json_len);
                continue;
            }
            
            int tool_written = snprintf(ptr, remaining, "%s\n", json_tool);
            if (tool_written < 0 || (size_t)tool_written >= remaining) {
                ETHERVOX_LOG_ERROR("Buffer overflow adding tool %s (needed %d, have %zu)", 
                                  tool->name, tool_written, remaining);
                return ETHERVOX_ERROR_INVALID_ARGUMENT;
            }
            ptr += tool_written;
            remaining -= tool_written;
        }
        
        // Close tools and add list of ALL available tools
        int tools_list_written = snprintf(ptr, remaining, "</tools>\n\nOther available tools (use get_tool_info to learn about these):\n");
        if (tools_list_written < 0 || (size_t)tools_list_written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        ptr += tools_list_written;
        remaining -= tools_list_written;
        
        // List all non-fast-path tools by name only
        for (uint32_t i = 0; i < registry->tool_count; i++) {
            const ethervox_tool_t* tool = &registry->tools[i];
            if (is_fast_path_tool(tool->name)) continue;  // Skip fast-path tools already listed
            
            int name_written = snprintf(ptr, remaining, "  - %s\n", tool->name);
            if (name_written < 0 || (size_t)name_written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
            ptr += name_written;
            remaining -= name_written;
        }
        
        // Add calling instructions
        int instr_written = snprintf(ptr, remaining,
            "\nFor each tool call, return a json object with function name and arguments within <tool_call></tool_call> XML tags:\n"
            "<tool_call>\n"
            "{\"name\": <function-name>, \"arguments\": <args-json-object>}\n"
            "</tool_call>\n\n"
            "CRITICAL RULES:\n"
            "1. For ALL math/calculations, you MUST call calculator_compute - never calculate mentally\n"
            "2. For time/date queries, you MUST use get_date or get_time\n"
            "3. For tools in the <tools> section above, call them directly with their parameters\n"
            "4. For other tools listed, call get_tool_info first to learn their parameters, then call the tool\n"
            "5. Always call tools before answering - do not guess or answer from memory\n\n"
            "TOOL CALL FORMAT EXAMPLES (these are examples only, not conversation history):\n\n"
            "Example 1 - Math calculation:\n"
            "<tool_call>\n{\"name\": \"calculator_compute\", \"arguments\": {\"expression\": \"17*23\"}}\n</tool_call>\n\n"
            "Example 2 - Get current date:\n"
            "<tool_call>\n{\"name\": \"get_date\", \"arguments\": {}}\n</tool_call>\n\n"
            "Example 3 - Voice response:\n"
            "<tool_call>\n{\"name\": \"speak\", \"arguments\": {\"text\": \"Hello! How can I help you?\"}}\n</tool_call>\n\n"
            "Example 4 - Learn about a tool:\n"
            "<tool_call>\n{\"name\": \"get_tool_info\", \"arguments\": {\"tool_name\": \"startup_prompt_get_current\"}}\n</tool_call>\n\n"
            "Remember: These are FORMAT examples. When responding to the ACTUAL user query below, generate your OWN tool call based on THEIR request.\n\n");
            
        // NOTE: Do NOT append system_end marker - tokenizer handles it automatically
            
        if (instr_written < 0 || (size_t)instr_written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        
    } else {
        // XML attribute format (Qwen, Phi, Llama, etc.)
        const char* platform_context = has_custom_prompts ? custom_instructions :
            (is_mobile
            ? "You are Ethervox. ALWAYS use tools - never answer from memory alone.\n"
              "Tool syntax: <tool_call name=\"tool_name\" param=\"value\" />\n"
            : "You are Ethervox, a helpful AI assistant.\n"
              "IMPORTANT: You MUST use tools for all calculations, time queries, and memory operations.\n"
              "NEVER calculate mentally or guess - ALWAYS call the appropriate tool.\n"
              "Tool call format: <tool_call name=\"tool_name\" param=\"value\" />\n");
        
        // NOTE: Do NOT include system_start marker - tokenizer handles it automatically
        written = snprintf(ptr, remaining,
            "%s\n",
            platform_context
        );
        
        if (written < 0 || (size_t)written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        ptr += written;
        remaining -= written;
        
        // Strong instruction about tool usage
        int instruction_written = snprintf(ptr, remaining,
            "\nYou are a tool-using assistant. CRITICAL RULES:\n"
            "1. For ALL math/calculations, you MUST call calculator_compute - never calculate mentally\n"
            "2. For time/date queries, call get_date or get_time\n"
            "3. For tools without schemas below, call get_tool_info first to learn parameters\n\n");
        if (instruction_written < 0 || (size_t)instruction_written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        ptr += instruction_written;
        remaining -= instruction_written;
        
        // Tool list header - minimal mode for KV cache efficiency
        int tools_header = snprintf(ptr, remaining, 
            "Available Tools:\n");
        if (tools_header < 0 || (size_t)tools_header >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
        ptr += tools_header;
        remaining -= tools_header;
        
        // Add each tool - minimal listing for non-fast-path, full schema for fast-path
        for (uint32_t i = 0; i < registry->tool_count; i++) {
            const ethervox_tool_t* tool = &registry->tools[i];
            int tool_written;
            
            if (is_fast_path_tool(tool->name)) {
                // Fast-path tools: include full description and schema
                const char* schema = (tool->parameters_json_schema[0] != '\0') 
                    ? tool->parameters_json_schema 
                    : "{}";
                tool_written = snprintf(ptr, remaining,
                    "- %s: %s\n  Schema: %s\n",
                    tool->name,
                    tool->description,
                    schema);
            } else {
                // Non-fast-path tools: name only, call get_tool_info for details
                tool_written = snprintf(ptr, remaining,
                    "- %s\n",
                    tool->name);
            }
            
            if (tool_written < 0 || (size_t)tool_written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
            
            ptr += tool_written;
            remaining -= tool_written;
        }
        
        // Use custom examples if available, otherwise defaults with get_tool_info
        const char* usage_section = has_custom_prompts ? custom_examples :
            (is_mobile
            ? "\nTOOL FORMAT EXAMPLES (format only, not conversation):\n"
              "Math: <tool_call name=\"calculator_compute\" expression=\"5+5\" />\n"
              "Memory: <tool_call name=\"memory_store\" text=\"Call John\" tags=\"reminder\" />\n"
            : "\nTOOL CALL FORMAT EXAMPLES (these are examples only, not actual conversation):\n\n"
              "Fast-path tools (calculator, time, date, get_tool_info) - call directly:\n\n"
              "Example 1 - Math calculation:\n"
              "<tool_call name=\"calculator_compute\" expression=\"15*8\" />\n\n"
              "Example 2 - Get current date:\n"
              "<tool_call name=\"get_date\" />\n\n"
              "Other tools - call get_tool_info first to learn parameters:\n\n"
              "Example 3 - Unit conversion (learn params first):\n"
              "<tool_call name=\"get_tool_info\" tool_name=\"unit_convert\" />\n"
              "Then: <tool_call name=\"unit_convert\" value=\"100\" from_unit=\"celsius\" to_unit=\"fahrenheit\" />\n\n"
              "Example 4 - Store memory (learn params first):\n"
              "<tool_call name=\"get_tool_info\" tool_name=\"memory_store\" />\n"
              "Then: <tool_call name=\"memory_store\" text=\"User's name is Tim\" tags=\"personal\" importance=\"0.9\" />\n\n"
              "CRITICAL: These are FORMAT examples. Respond to the ACTUAL user query with appropriate tool calls.\n\n"
              "For math, ALWAYS use calculator_compute. For other tools, call get_tool_info first to learn parameters.\n");
        
        int instr_written = snprintf(ptr, remaining, "%s", usage_section);
        if (instr_written < 0 || (size_t)instr_written >= remaining) return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Validate final buffer integrity
    size_t final_length = strlen(buffer);
    size_t expected_written = buffer_size - remaining;
    
    ETHERVOX_LOG_DEBUG("System prompt built: %zu chars written, %zu bytes remaining of %zu total", 
                       final_length, remaining, buffer_size);
    
    if (final_length > buffer_size - 1) {
        ETHERVOX_LOG_ERROR("Buffer overflow detected! Length %zu exceeds buffer size %zu", 
                          final_length, buffer_size);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Debug: Print system prompt for verification (first 500 chars)
    ETHERVOX_LOG_DEBUG("System prompt (tool_format=%d, length=%zu, first 500 chars): %.500s...", 
                       tool_format, strlen(buffer), buffer);
    
    return ETHERVOX_SUCCESS;
}

void ethervox_tool_registry_cleanup(ethervox_tool_registry_t* registry) {
    if (registry && registry->tools) {
        free(registry->tools);
        registry->tools = NULL;
        registry->tool_count = 0;
        registry->capacity = 0;
    }
}
