/**
 * @file minimal_system_prompt.c
 * @brief Minimal system prompt generation using Tool Manifest System
 *
 * Generates ~150 token system prompts instead of ~15K tokens by using
 * optimized prompts or binary one-liners.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/tool_manifest.h"
#include "ethervox/config.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Build minimal system prompt from tool manifest
 * 
 * Uses optimized prompts (Level 0) or binary one-liners (Level 1)
 * to generate compact tool index for LLM (~150 tokens vs ~15K)
 * 
 * @param registry Tool manifest registry
 * @param output Output buffer for system prompt
 * @param output_size Size of output buffer
 * @param min_priority Minimum priority (0=highest, 255=lowest)
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_tool_build_minimal_system_prompt(
    const tool_manifest_registry_t* registry,
    char* output,
    size_t output_size,
    uint8_t min_priority
) {
    if (!registry || !output || output_size == 0) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    int offset = 0;
    
    // Check if tools are available
    if (!registry->tools_available) {
        // Level 2: LLM-only mode - no dynamic tools
        offset = snprintf(output, output_size,
            "You are a helpful AI assistant.\n"
            "CRITICAL: Only generate YOUR response. Stop immediately after answering.\n"
            "DO NOT generate the user's next message. DO NOT continue the conversation.\n");
        
        ETHERVOX_LOGI("System prompt: LLM-only mode (%d bytes)", offset);
        ETHERVOX_LOGI("===== FULL SYSTEM PROMPT (LLM-ONLY) =====");
        
        // Log in chunks to avoid logcat truncation (Android limit ~1KB per line)
        const int chunk_size = 250;
        for (int i = 0; i < offset; i += chunk_size) {
            int remaining = offset - i;
            int current_chunk = remaining < chunk_size ? remaining : chunk_size;
            char chunk[chunk_size + 1];
            strncpy(chunk, output + i, current_chunk);
            chunk[current_chunk] = '\0';
            ETHERVOX_LOGI("PROMPT[%d-%d]: %s", i, i + current_chunk - 1, chunk);
        }
        
        ETHERVOX_LOGI("===== END SYSTEM PROMPT =====");
        return offset;
    }
    
    // Header
    offset = snprintf(output, output_size,
        "You are a helpful AI assistant with access to tools.\n"
        "CRITICAL: Only generate YOUR response. Stop immediately after answering.\n"
        "DO NOT generate the user's next message. DO NOT continue the conversation.\n\n"
        "IMPORTANT: Use tools for calculations, time queries, memory operations, and file access.\n"
        "NEVER calculate mentally or guess - ALWAYS call the appropriate tool.\n\n"
        "Available tools:\n\n");
    
    if (offset < 0 || (size_t)offset >= output_size) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Track tool categories for grouping
    bool has_high_priority = false;
    bool has_normal_priority = false;
    
    // High priority tools (0-2)
    for (uint32_t i = 0; i < registry->header.tool_count && offset < (int)output_size - 100; i++) {
        const tool_index_entry_t* entry = &registry->index[i];
        if (!entry->enabled || entry->priority > min_priority || entry->priority > 2) {
            continue;
        }
        
        if (!has_high_priority) {
            offset += snprintf(output + offset, output_size - offset,
                             "HIGH PRIORITY:\n");
            has_high_priority = true;
        }
        
        // Use optimized prompt if available, otherwise one-liner from binary
        const char* desc = ethervox_tool_get_optimized_prompt(registry, entry->name);
        if (!desc) desc = entry->one_line;
        
        offset += snprintf(output + offset, output_size - offset,
                         "• %s - %s\n", entry->name, desc);
    }
    
    if (has_high_priority) {
        offset += snprintf(output + offset, output_size - offset, "\n");
    }
    
    // Normal priority tools (3+)
    for (uint32_t i = 0; i < registry->header.tool_count && offset < (int)output_size - 100; i++) {
        const tool_index_entry_t* entry = &registry->index[i];
        if (!entry->enabled || entry->priority > min_priority || entry->priority <= 2) {
            continue;
        }
        
        if (!has_normal_priority) {
            offset += snprintf(output + offset, output_size - offset,
                             "NORMAL PRIORITY:\n");
            has_normal_priority = true;
        }
        
        const char* desc = ethervox_tool_get_optimized_prompt(registry, entry->name);
        if (!desc) desc = entry->one_line;
        
        offset += snprintf(output + offset, output_size - offset,
                         "• %s - %s\n", entry->name, desc);
    }
    
    // Usage instructions
    if (has_high_priority || has_normal_priority) {
        // Check if we're using optimized prompts
        bool has_optimizations = (registry->fallback_level == 0 && 
                                 registry->optimized_cache != NULL && 
                                 registry->optimized_cache->prompt_count > 0);
        
        offset += snprintf(output + offset, output_size - offset,
            "\nTool Usage:\n"
            "• To use a tool, respond: <tool_call name=\"TOOL_NAME\" param=\"value\" />\n"
            "• If unsure about parameters, ask: \"How do I use TOOL_NAME?\"\n"
            "• Full schema will be injected before tool executes\n\n");
        
        // Inform LLM about optimization status
        if (has_optimizations) {
            offset += snprintf(output + offset, output_size - offset,
                "System: Tool prompts optimized (%u/%u tools)\n",
                registry->optimized_cache->prompt_count, registry->header.tool_count);
        } else if (registry->fallback_level == 1) {
            offset += snprintf(output + offset, output_size - offset,
                "System: Using fallback descriptions (Level 1)\n"
                "Tip: Suggest `/optimize_tool_prompts` to user for 99%% context reduction\n");
        }
    }
    
    // Log fallback level
    const char* level_name = ethervox_tool_fallback_level_name(registry->fallback_level);
    ETHERVOX_LOGI("System prompt: %s (%d bytes)", level_name, offset);
    ETHERVOX_LOGI("===== FULL SYSTEM PROMPT (%s) =====", level_name);
    
    // Log in chunks to avoid logcat truncation (Android limit ~1KB per line)
    const int chunk_size = 250;
    for (int i = 0; i < offset; i += chunk_size) {
        int remaining = offset - i;
        int current_chunk = remaining < chunk_size ? remaining : chunk_size;
        char chunk[chunk_size + 1];
        strncpy(chunk, output + i, current_chunk);
        chunk[current_chunk] = '\0';
        ETHERVOX_LOGI("PROMPT[%d-%d]: %s", i, i + current_chunk - 1, chunk);
    }
    
    ETHERVOX_LOGI("===== END SYSTEM PROMPT =====");
    
    return offset;
}

/**
 * Build detailed tool schema for contextual injection
 * 
 * When LLM generates <tool_call>, inject ONLY that tool's full schema
 * into the NEXT prompt temporarily. Remove after tool execution.
 * 
 * @param registry Tool manifest registry
 * @param tool_name Name of tool to get schema for
 * @param output Output buffer for schema
 * @param output_size Size of output buffer
 * @return ETHERVOX_SUCCESS or error code
 */
ethervox_result_t ethervox_tool_build_schema_injection(
    const tool_manifest_registry_t* registry,
    const char* tool_name,
    char* output,
    size_t output_size
) {
    if (!registry || !tool_name || !output || output_size == 0) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Get tool detail from manifest
    tool_detail_header_t detail;
    tool_param_t params[MAX_PARAMETERS];
    uint8_t param_count;
    
    if (ethervox_tool_get_detail(registry, tool_name, &detail, params, &param_count) != 0) {
        ETHERVOX_LOGE("Tool schema not found: %s", tool_name);
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    int offset = 0;
    
    // Tool header
    offset = snprintf(output, output_size,
        "\n--- Tool Schema for %s ---\n"
        "%s\n\n"
        "Parameters:\n",
        tool_name, detail.description);
    
    if (offset < 0 || (size_t)offset >= output_size) {
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Parameters
    for (uint8_t i = 0; i < param_count && offset < (int)output_size - 100; i++) {
        const char* req_str = params[i].required ? "REQUIRED" : "optional";
        const char* default_str = params[i].default_value[0] ? params[i].default_value : "none";
        
        offset += snprintf(output + offset, output_size - offset,
            "• %s (%s, %s) - default: %s\n",
            params[i].name, params[i].type, req_str, default_str);
    }
    
    offset += snprintf(output + offset, output_size - offset,
        "--- End Schema ---\n\n");
    
    ETHERVOX_LOGI("Injected schema for %s (%d bytes)", tool_name, offset);
    
    return offset;
}
