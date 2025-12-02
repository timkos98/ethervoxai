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
    size_t buffer_size
) {
    if (!registry || !chat_template || !buffer || buffer_size == 0) {
        return -1;
    }
    
    bool is_mobile = is_mobile_platform();
    
    // Mobile: Concise voice-focused. Desktop: More detailed.
    const char* platform_context = is_mobile
        ? "You are Ethervox, a voice assistant. Use tools when needed.\n\n"
          "IMPORTANT: For reminders (\"remind me\", \"don't forget\", \"call later\"), ALWAYS use memory_store with 'reminder' tag.\n\n"
          "When user shares personal info (name, preferences, plans), save it with memory_store.\n"
          "Use memory_search when asked about past conversations.\n"
          "Use calculator_compute for math. Use time tools for dates/time.\n"
          "Keep responses brief (1-2 sentences)."
        : "You are Ethervox. Use tools when they help answer the user's request.\n\n"
          "For reminders, use memory_store with 'reminder' tag.\n"
          "For past info, use memory_search.\n"
          "For math, use calculator_compute.\n"
          "Be conversational and helpful.";
    
    int written = snprintf(buffer, buffer_size,
        "%s"
        "%s\n\n"
        "Available tools:\n",
        chat_template->system_start,
        platform_context
    );
    
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }
    
    size_t remaining = buffer_size - written;
    char* ptr = buffer + written;
    
    // Add each tool with enhanced descriptions for desktop
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        
        // Highlight memory and file tools on desktop
        const char* emphasis = "";
        if (!is_mobile) {
            if (strstr(tool->name, "memory_") || strstr(tool->name, "file_")) {
                emphasis = " [KEY]";
            }
        }
        
        int tool_written = snprintf(ptr, remaining,
            "%s - %s%s\n",
            tool->name,
            tool->description,
            emphasis
        );
        
        if (tool_written < 0 || (size_t)tool_written >= remaining) {
            return -1;
        }
        
        ptr += tool_written;
        remaining -= tool_written;
    }
    
    // Platform-specific usage instructions
    const char* usage_section = is_mobile
        // Mobile: Simple, focused examples
        ? "\nUSAGE: <tool_call name=\"TOOL\" param=\"value\" />\n"
          "Ex: What time? → <tool_call name=\"time_get_current\" />\n"
          "Ex: 5+5? → <tool_call name=\"calculator_compute\" expression=\"5+5\" />\n"
          "Ex: Remind me to call John → <tool_call name=\"memory_store\" text=\"Call John\" tags=\"reminder\" importance=\"0.9\" />\n"
        
        // Desktop: More examples but still concise
        : "\nUSAGE: <tool_call name=\"tool_name\" param=\"value\" />\n\n"
          "Examples:\n"
          "User: What time is it?\n"
          "Assistant: <tool_call name=\"time_get_current\" />\n"
          "User: Calculate 17/12\n"
          "Assistant: <tool_call name=\"calculator_compute\" expression=\"17/12\" />\n"
          "User: My name is Tim\n"
          "Assistant: <tool_call name=\"memory_store\" text=\"User's name is Tim\" tags=\"personal\" importance=\"0.95\" />\n"
          "User: Remind me to call mom\n"
          "Assistant: <tool_call name=\"memory_store\" text=\"Call mom\" tags=\"reminder\" importance=\"0.9\" />\n";
    
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
