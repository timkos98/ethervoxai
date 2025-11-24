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
    char* buffer,
    size_t buffer_size
) {
    if (!registry || !buffer || buffer_size == 0) {
        return -1;
    }
    
    bool is_mobile = is_mobile_platform();
    
    // Qwen2.5 uses <|im_start|> and <|im_end|> format
    // Desktop: More detailed with memory/file tools emphasized
    // Mobile: Concise, focused on voice interaction
    const char* platform_context = is_mobile
        ? "You are Ethervox, a concise voice assistant optimized for mobile. Keep responses brief and actionable."
        : "You are Ethervox, an intelligent assistant. You MUST use tools - you cannot answer from memory alone.\n\n"
          "CRITICAL: When user asks about past information (name, preferences, previous topics), you MUST call memory_search tool. "
          "Do NOT try to answer without searching memory first.";
    
    int written = snprintf(buffer, buffer_size,
        "<|im_start|>system\n"
        "%s\n\n"
        "Available tools:\n",
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
        // Mobile: Ultra-concise, voice-focused
        ? "\nUSAGE: <tool_call name=\"TOOL\" param=\"value\" />\n"
          "EXAMPLES:\n"
          "Input: What time?\n"
          "Output: <tool_call name=\"time_get_current\" />\n"
          "Result: {\"time_12hr\": \"3:45 PM\"}\n"
          "Output: 3:45 PM.\n\n"
          "Input: 17 divided by 12?\n"
          "Output: <tool_call name=\"calculator_compute\" expression=\"17/12\" />\n"
          "Result: {\"result\": 1.42}\n"
          "Output: 1.42.\n\n"
          "RULES:\n"
          "1. Keep responses brief (1-2 sentences max)\n"
          "2. Use tools for math/dates/time\n"
          "3. Voice-friendly formatting<|im_end|>\n"
        
        // Desktop: Comprehensive with memory/file context
        : "\n\nTOOL USAGE:\n"
          "When you need to use a tool, output: <tool_call name=\"tool_name\" param=\"value\" />\n"
          "After tool execution, you'll receive: <tool_result>JSON data</tool_result>\n"
          "Then respond naturally using that data.\n\n"
          "EXAMPLES:\n"
          "User: Hi\n"
          "Assistant: <tool_call name=\"memory_search\" query=\"\" limit=\"5\" />\n"
          "<tool_result>{\"results\":[{\"text\":\"Discussed project timeline\"}]}</tool_result>\n"
          "Assistant: Hi! I see we were discussing the project timeline. How's that going?\n\n"
          "User: My name is Tim\n"
          "Assistant: <tool_call name=\"memory_store\" content=\"User's name is Tim\" tags=\"personal\" importance=\"0.95\" />\n"
          "<tool_result>{\"success\":true}</tool_result>\n"
          "Assistant: Nice to meet you, Tim!\n\n"
          "User: What's my name?\n"
          "Assistant: <tool_call name=\"memory_search\" query=\"name\" limit=\"5\" />\n"
          "<tool_result>{\"results\":[{\"text\":\"User's name is Tim\"}]}</tool_result>\n"
          "Assistant: Your name is Tim!\n\n"
          "User: What time is it?\n"
          "Assistant: <tool_call name=\"time_get_current\" />\n\n"
          "User: Calculate 17 divided by 12\n"
          "Assistant: <tool_call name=\"calculator_compute\" expression=\"17/12\" />\n\n"
          "User: List files here\n"
          "Assistant: <tool_call name=\"file_list\" directory=\".\" recursive=\"false\" />\n\n"
          "User: Read semester_project_TUM.org\n"
          "Assistant: <tool_call name=\"file_read\" file_path=\"semester_project_TUM.org\" />\n\n"
          "MANDATORY MEMORY RULES:\n"
          "1. GREETING (hi/hello/hey) → ALWAYS use memory_search first\n"
          "2. QUESTIONS ABOUT PAST (name/preferences/previous topics) → ALWAYS use memory_search\n"
          "3. USER SHARES INFO → IMMEDIATELY use memory_store BEFORE responding\n"
          "4. NEVER answer 'I don't know' or 'you haven't told me' - search memory first!\n"
          "5. Store importance: 0.95 personal facts, 0.8 questions, 0.7 preferences\n\n"
          "WRONG: 'You have not shared your name with me'\n"
          "RIGHT: <tool_call name=\"memory_search\" query=\"name\" limit=\"5\" />\n\n"
          "OTHER RULES:\n"
          "1. Respond conversationally - NO role labels\n"
          "2. Use calculator_compute for ALL math\n"
          "3. Use file tools for document access<|im_end|>\n";
    
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
