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

int ethervox_tool_registry_build_system_prompt(
    const ethervox_tool_registry_t* registry,
    const chat_template_t* chat_template,
    char* buffer,
    size_t buffer_size,
    void* memory_store
) {
    if (!registry || !chat_template || !buffer || buffer_size == 0) {
        return -1;
    }
    
    bool is_mobile = is_mobile_platform();
    
    // Optimized for IBM Granite models - they prefer:
    // 1. Clear role definition
    // 2. XML-style tool syntax (which we use)
    // 3. Concise, structured instructions
    // 4. Direct examples over verbose explanations
    const char* platform_context = is_mobile
        ? "You are Ethervox, a helpful assistant. Use tools via XML tags when needed.\n"
        : "You are Ethervox, a helpful AI assistant.\n"
          "When tools can help, use: <tool_call name=\"tool_name\" param=\"value\" />\n";
    
    int written = snprintf(buffer, buffer_size,
        "%s"
        "%s\n",
        chat_template->system_start,
        platform_context
    );
    
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }
    
    size_t remaining = buffer_size - written;
    char* ptr = buffer + written;
    
    // Skip adaptive learning section to save context tokens
    // (Granite handles this well without explicit injection)
    
    // Tool list header - concise
    int tools_header = snprintf(ptr, remaining, "Tools:\n");
    if (tools_header < 0 || (size_t)tools_header >= remaining) {
        return -1;
    }
    ptr += tools_header;
    remaining -= tools_header;
    
    // Add each tool - keep descriptions concise for Granite
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        
        int tool_written = snprintf(ptr, remaining,
            "- %s: %s\n",
            tool->name,
            tool->description
        );
        
        if (tool_written < 0 || (size_t)tool_written >= remaining) {
            return -1;
        }
        
        ptr += tool_written;
        remaining -= tool_written;
    }
    
    // Granite-optimized instructions: concise with clear examples
    const char* usage_section = is_mobile
        ? "\nExamples:\n"
          "Math: <tool_call name=\"calculator_compute\" expression=\"5+5\" />\n"
          "Remember: <tool_call name=\"memory_store\" text=\"Call John\" tags=\"reminder\" />\n"
        : "\nExamples:\n"
          "User: Good morning, what's the date?\n"
          "<tool_call name=\"time_get_current\" />\n"
          "Good morning! It's December 2, 2025.\n\n"
          "User: What's 15 * 8?\n"
          "<tool_call name=\"calculator_compute\" expression=\"15*8\" />\n"
          "120\n\n"
          "User: Remember my name is Tim\n"
          "<tool_call name=\"memory_store\" text=\"User's name is Tim\" tags=\"personal\" importance=\"0.9\" />\n"
          "Got it, Tim!\n\n"
          "User: What's my name?\n"
          "<tool_call name=\"memory_search\" query=\"name\" limit=\"3\" />\n"
          "Your name is Tim!\n";
    
    int instr_written = snprintf(ptr, remaining, "%s", usage_section);
    
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
