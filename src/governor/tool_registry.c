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

int ethervox_tool_registry_build_system_prompt(
    const ethervox_tool_registry_t* registry,
    char* buffer,
    size_t buffer_size
) {
    if (!registry || !buffer || buffer_size == 0) {
        return -1;
    }
    
    // Qwen2.5 uses <|im_start|> and <|im_end|> format - COMPACT but with examples
    int written = snprintf(buffer, buffer_size,
        "<|im_start|>system\n"
        "You are Ethervox voice assistant with tools:\n"
    );
    
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }
    
    size_t remaining = buffer_size - written;
    char* ptr = buffer + written;
    
    // Add each tool - COMPACT format
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        
        int tool_written = snprintf(ptr, remaining,
            "%s - %s\n",
            tool->name,
            tool->description
        );
        
        if (tool_written < 0 || (size_t)tool_written >= remaining) {
            return -1;
        }
        
        ptr += tool_written;
        remaining -= tool_written;
    }
    
    // Add COMPACT usage instructions with key examples
    int instr_written = snprintf(ptr, remaining,
        "\nUSAGE: <tool_call name=\"TOOL\" param=\"value\" />\n"
        "EXAMPLES:\n"
        "User: What time is it?\n"
        "Assistant: <tool_call name=\"time_get_current\" />\n"
        "User: <tool_result>{\"time_12hr\": \"3:45 PM\"}</tool_result>\n"
        "Assistant: It's 3:45 PM.\n\n"
        "User: What's 17/12?\n"
        "Assistant: <tool_call name=\"calculator_compute\" expression=\"17/12\" />\n"
        "User: <tool_result>{\"result\": 1.42}</tool_result>\n"
        "Assistant: 17 divided by 12 equals 1.42.\n\n"
        "User: How many days until Christmas?\n"
        "Assistant: <tool_call name=\"time_get_date\" />\n"
        "User: <tool_result>{\"date\": \"Friday, November 22, 2024\", \"month\": \"November\", \"day\": 22}</tool_result>\n"
        "Assistant: <tool_call name=\"calculator_compute\" expression=\"(30-22)+25\" />\n"
        "User: <tool_result>{\"result\": 33}</tool_result>\n"
        "Assistant: Christmas is in 33 days.\n\n"
        "User: What's 5 times 7?\n"
        "Assistant: <tool_call name=\"calculator_compute\" expression=\"5*7\" />\n"
        "User: <tool_result>{\"result\": 35}</tool_result>\n"
        "Assistant: 5 times 7 equals 35.\n\n"
        "User: Tell me about cats\n"
        "Assistant: Cats are independent animals known for their playful nature.\n\n"
        "CRITICAL RULES (MUST FOLLOW):\n"
        "1. For ANY math/arithmetic/calculation - MUST use calculator_compute tool - NEVER compute mentally\n"
        "2. For date questions or 'days until/since' - MUST use time_get_date tool first\n"
        "3. When user asks date-related questions, use time_get_date then calculator_compute\n"
        "4. After receiving tool results, give brief natural answer\n"
        "5. Non-tool questions: be concise and direct<|im_end|>\n"
    );
    
    if (instr_written < 0 || (size_t)instr_written >= remaining) {
        return -1;
    }
    
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
