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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ethervox_tool_registry_init(ethervox_tool_registry_t* registry, uint32_t initial_capacity) {
    if (!registry) {
        return -1;
    }
    
    if (initial_capacity == 0) {
        initial_capacity = 16; // Default capacity
    }
    
    registry->tools = (ethervox_tool_t*)calloc(initial_capacity, sizeof(ethervox_tool_t));
    if (!registry->tools) {
        return -1;
    }
    
    registry->tool_count = 0;
    registry->capacity = initial_capacity;
    
    return 0;
}

int ethervox_tool_registry_add(ethervox_tool_registry_t* registry, const ethervox_tool_t* tool) {
    if (!registry || !tool) {
        return -1;
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
            return -1;
        }
        
        registry->tools = new_tools;
        registry->capacity = new_capacity;
    }
    
    // Copy tool definition
    memcpy(&registry->tools[registry->tool_count], tool, sizeof(ethervox_tool_t));
    registry->tool_count++;
    
    return 0;
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

int ethervox_tool_registry_export_manifest(
    const ethervox_tool_registry_t* registry,
    const char* binary_path
) {
    if (!registry || !binary_path) {
        return -1;
    }
    
    // Open output file
    FILE* fp = fopen(binary_path, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to create manifest: %s\n", binary_path);
        return -1;
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
        return -1;
    }
    
    // Write index entries and collect detail info
    uint32_t current_detail_offset = header.detail_offset;
    
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        
        tool_index_entry_t entry = {0};
        strncpy(entry.name, tool->name, TOOL_NAME_MAX - 1);
        strncpy(entry.one_line, tool->description, TOOL_DESC_MAX - 1);
        strncpy(entry.category, "general", TOOL_CATEGORY_MAX - 1);
        entry.priority = 5;
        entry.enabled = 1;
        
        // Calculate detail size
        tool_detail_header_t detail_hdr = {0};
        uint16_t detail_size = sizeof(tool_detail_header_t);
        
        entry.detail_offset = current_detail_offset;
        entry.detail_size = detail_size;
        
        if (fwrite(&entry, sizeof(tool_index_entry_t), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
        
        current_detail_offset += detail_size;
    }
    
    // Write detail sections
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        
        tool_detail_header_t detail = {0};
        strncpy(detail.name, tool->name, TOOL_NAME_MAX - 1);
        // Note: description and example are variable-length in the actual format
        // For now just store basic info
        detail.param_count = 0;
        detail.trigger_count = 0;
        
        if (fwrite(&detail, sizeof(tool_detail_header_t), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }
    
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
    return 0;
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
            strcmp(tool_name, "get_tool_info") == 0);
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

int ethervox_tool_registry_build_system_prompt(
    const ethervox_tool_registry_t* registry,
    const chat_template_t* chat_template,
    char* buffer,
    size_t buffer_size,
    void* memory_store,
    const char* model_path  // Added parameter for optimized prompts
) {
    if (!registry || !chat_template || !buffer || buffer_size == 0) {
        return -1;
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
    }
    
    int written = 0;
    size_t remaining = buffer_size;
    char* ptr = buffer;
    
    // Format differs significantly between XML and JSON tool formats
    if (tool_format == TOOL_FORMAT_JSON_IN_XML) {
        // Granite 4.0 format: system prompt with JSON tools in <tools></tools>
        written = snprintf(ptr, remaining,
            "%sYou are a helpful assistant with access to the following tools. "
            "You are provided with function signatures within <tools></tools> XML tags:\n<tools>\n",
            chat_template->system_start);
        
        if (written < 0 || (size_t)written >= remaining) return -1;
        ptr += written;
        remaining -= written;
        
        // Add each fast-path tool as JSON
        for (uint32_t i = 0; i < registry->tool_count; i++) {
            const ethervox_tool_t* tool = &registry->tools[i];
            if (!is_fast_path_tool(tool->name)) continue;
            
            char json_tool[1024];
            int json_len = build_json_tool_definition(tool, json_tool, sizeof(json_tool));
            if (json_len < 0 || json_len >= (int)sizeof(json_tool)) continue;
            
            int tool_written = snprintf(ptr, remaining, "%s\n", json_tool);
            if (tool_written < 0 || (size_t)tool_written >= remaining) return -1;
            ptr += tool_written;
            remaining -= tool_written;
        }
        
        // Close tools and add calling instructions
        int instr_written = snprintf(ptr, remaining,
            "</tools>\n\n"
            "For each tool call, return a json object with function name and arguments within <tool_call></tool_call> XML tags:\n"
            "<tool_call>\n"
            "{\"name\": <function-name>, \"arguments\": <args-json-object>}\n"
            "</tool_call>\n\n"
            "CRITICAL RULES:\n"
            "1. For ALL math/calculations, you MUST call calculator_compute - never calculate mentally\n"
            "2. For time/date queries, you MUST use get_date or get_time\n"
            "3. Always call tools before answering - do not guess or answer from memory\n\n"
            "Examples:\n\n"
            "User: What's 17 times 23?\n"
            "Assistant: <tool_call>\n{\"name\": \"calculator_compute\", \"arguments\": {\"expression\": \"17*23\"}}\n</tool_call>\n\n"
            "User: What's the current date?\n"
            "Assistant: <tool_call>\n{\"name\": \"get_date\", \"arguments\": {}}\n</tool_call>\n"
            "%s",
            chat_template->system_end);
            
        if (instr_written < 0 || (size_t)instr_written >= remaining) return -1;
        
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
        
        written = snprintf(ptr, remaining,
            "%s"
            "%s\n",
            chat_template->system_start,
            platform_context
        );
        
        if (written < 0 || (size_t)written >= remaining) return -1;
        ptr += written;
        remaining -= written;
        
        // Strong instruction about tool usage
        int instruction_written = snprintf(ptr, remaining,
            "\nYou are a tool-using assistant. CRITICAL RULES:\n"
            "1. For ALL math/calculations, you MUST call calculator_compute - never calculate mentally\n"
            "2. For time/date queries, call get_date or get_time\n"
            "3. For tools without schemas below, call get_tool_info first to learn parameters\n\n");
        if (instruction_written < 0 || (size_t)instruction_written >= remaining) return -1;
        ptr += instruction_written;
        remaining -= instruction_written;
        
        // Tool list header - minimal mode for KV cache efficiency
        int tools_header = snprintf(ptr, remaining, 
            "Available Tools:\n");
        if (tools_header < 0 || (size_t)tools_header >= remaining) return -1;
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
            
            if (tool_written < 0 || (size_t)tool_written >= remaining) return -1;
            
            ptr += tool_written;
            remaining -= tool_written;
        }
        
        // Use custom examples if available, otherwise defaults with get_tool_info
        const char* usage_section = has_custom_prompts ? custom_examples :
            (is_mobile
            ? "\nExamples (ALWAYS start with tool call):\n"
              "Q: 5+5? A: <tool_call name=\"calculator_compute\" expression=\"5+5\" />\n"
              "Q: Remember call John. A: <tool_call name=\"memory_store\" text=\"Call John\" tags=\"reminder\" />\n"
            : "\nIMPORTANT: You MUST use tools. Never calculate mentally.\n\n"
              "Fast-path tools (calculator, time, date, get_tool_info) - call directly:\n\n"
              "User: What's 15 * 8?\n"
              "Assistant: <tool_call name=\"calculator_compute\" expression=\"15*8\" />\n"
              "Result: 120\n"
              "Assistant: 15 times 8 equals 120.\n\n"
              "User: What's the date?\n"
              "Assistant: <tool_call name=\"get_date\" />\n"
              "Result: December 2, 2025\n"
              "Assistant: It's December 2, 2025.\n\n"
              "Other tools - call get_tool_info first to learn parameters:\n\n"
              "User: Convert 100 celsius to fahrenheit\n"
              "Assistant: <tool_call name=\"get_tool_info\" tool_name=\"unit_convert\" />\n"
              "Result: {\"name\":\"unit_convert\",\"parameters\":{\"value\":\"number\",\"from_unit\":\"string\",\"to_unit\":\"string\"}}\n"
              "Assistant: <tool_call name=\"unit_convert\" value=\"100\" from_unit=\"celsius\" to_unit=\"fahrenheit\" />\n"
              "Result: 212\n"
              "Assistant: 100°C is 212°F.\n\n"
              "User: Remember my name is Tim\n"
              "Assistant: <tool_call name=\"get_tool_info\" tool_name=\"memory_store\" />\n"
              "Result: {\"name\":\"memory_store\",\"parameters\":{\"text\":\"string\",\"tags\":\"string\",\"importance\":\"number\"}}\n"
              "Assistant: <tool_call name=\"memory_store\" text=\"User's name is Tim\" tags=\"personal\" importance=\"0.9\" />\n"
              "Result: Stored\n"
              "Assistant: Got it, Tim!\n\n"
              "CRITICAL: For math, ALWAYS use calculator_compute. For other tools, call get_tool_info first.\n");
        
        int instr_written = snprintf(ptr, remaining, "%s", usage_section);
        if (instr_written < 0 || (size_t)instr_written >= remaining) return -1;
    }
    
    // Debug: Print system prompt for verification (first 500 chars)
    ETHERVOX_LOG_DEBUG("System prompt (tool_format=%d, length=%zu, first 500 chars): %.500s...", 
                       tool_format, strlen(buffer), buffer);
    
    return 0;
}

void ethervox_tool_registry_cleanup(ethervox_tool_registry_t* registry) {
    if (registry && registry->tools) {
        free(registry->tools);
        registry->tools = NULL;
        registry->tool_count = 0;
        registry->capacity = 0;
    }
}
