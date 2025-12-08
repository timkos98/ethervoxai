/**
 * @file governor_manifest_init.c
 * @brief Governor initialization with Tool Manifest System integration
 *
 * Implements the 4-level fallback chain and minimal system prompt generation.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/governor.h"
#include "ethervox/tool_manifest.h"
#include "ethervox/config.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * Initialize governor with Tool Manifest System
 * 
 * This implements the complete initialization sequence:
 * 1. Export manifest from runtime tool registry
 * 2. Load binary manifest
 * 3. Detect model name
 * 4. Load optimized JSON prompts
 * 5. Build minimal system prompt
 * 6. Graceful 4-level fallback
 * 
 * @param governor Governor instance
 * @param model_path Path to GGUF model file
 * @param manifest_registry Output: Tool manifest registry
 * @return 0 on success, negative on error
 */
int ethervox_governor_init_with_manifest(
    ethervox_governor_t* governor,
    const char* model_path,
    tool_manifest_registry_t* manifest_registry
) {
    if (!governor || !model_path || !manifest_registry) {
        return -1;
    }
    
    // Get runtime tool registry (from governor)
    const ethervox_tool_registry_t* runtime_registry = 
        ethervox_governor_get_registry(governor);
    
    if (!runtime_registry) {
        ETHERVOX_LOGE("No runtime tool registry available");
        return -1;
    }
    
    // === STEP 1: Export manifest from runtime registry ===
    const char* home = getenv("HOME");
    char manifest_path[512];
    
    if (home) {
        snprintf(manifest_path, sizeof(manifest_path),
                "%s/.ethervox/tools/tools.bin", home);
    } else {
        snprintf(manifest_path, sizeof(manifest_path),
                "./.ethervox/tools/tools.bin");
    }
    
    // Create directory if needed
    char tools_dir[512];
    snprintf(tools_dir, sizeof(tools_dir), "%s/.ethervox/tools", 
             home ? home : ".");
    
    #ifdef _WIN32
    _mkdir(tools_dir);
    #else
    mkdir(tools_dir, 0755);
    #endif
    
    ETHERVOX_LOGI("Exporting tool manifest to: %s", manifest_path);
    
    if (ethervox_tool_registry_export_manifest(runtime_registry, manifest_path) == 0) {
        ETHERVOX_LOGI("Manifest exported: %u tools", runtime_registry->tool_count);
    } else {
        ETHERVOX_LOGW("Failed to export manifest (will try to load existing)");
    }
    
    // === STEP 2: Load binary manifest ===
    if (ethervox_tool_manifest_init(manifest_registry, manifest_path) != 0) {
        // Level 2 fallback: LLM-only mode
        ETHERVOX_LOGW("═══════════════════════════════════════════════════════");
        ETHERVOX_LOGW("WARNING: Tool manifest unavailable");
        ETHERVOX_LOGW("Running in LLM-only mode with basic commands");
        ETHERVOX_LOGW("═══════════════════════════════════════════════════════");
        
        manifest_registry->tools_available = false;
        manifest_registry->fallback_level = 2;
        
        // Still functional - deterministic toolkit always available
        return 0;
    }
    
    // === STEP 3: Detect model name ===
    // Extract model name from path (e.g., "granite-4.0-Q4_K_M.gguf" -> "granite-4.0")
    const char* filename = strrchr(model_path, '/');
    if (!filename) filename = model_path;
    else filename++;
    
    char model_name[128];
    size_t i = 0;
    while (i < sizeof(model_name) - 1 && filename[i]) {
        if (filename[i] == '.' && strncmp(&filename[i], ".gguf", 5) == 0) break;
        if (filename[i] == '-' && i + 1 < strlen(filename) && filename[i+1] == 'Q') break;
        if (filename[i] == '-' && i + 2 < strlen(filename) && 
            filename[i+1] == 'h' && filename[i+2] == '-') break;
        
        model_name[i] = filename[i];
        i++;
    }
    model_name[i] = '\0';
    
    // Remove trailing dash
    if (i > 0 && model_name[i-1] == '-') {
        model_name[i-1] = '\0';
    }
    
    ETHERVOX_LOGI("Detected model: %s", model_name);
    
    // === STEP 4: Load optimized JSON prompts ===
    char optimized_path[512];
    snprintf(optimized_path, sizeof(optimized_path),
             "%s/.ethervox/tools/optimized/%s.json", 
             home ? home : ".", model_name);
    
    if (ethervox_tool_manifest_load_optimized(manifest_registry, optimized_path) == 0) {
        // Level 0: Optimal - using optimized JSON prompts
        ETHERVOX_LOGI("✓ Loaded optimized prompts: %s (~150 tokens)",
                      ethervox_tool_fallback_level_name(0));
        manifest_registry->fallback_level = 0;
    } else {
        // Level 1: Good - using binary one-liners
        ETHERVOX_LOGI("ℹ Optimized prompts not found, using binary one-liners");
        ETHERVOX_LOGI("  Run /optimize_tool_prompts to generate them");
        ETHERVOX_LOGI("✓ Fallback level: %s (~500 tokens)",
                      ethervox_tool_fallback_level_name(1));
        manifest_registry->fallback_level = 1;
    }
    
    // === STEP 5: Validate manifest ===
    if (!ethervox_tool_manifest_validate(manifest_registry)) {
        ETHERVOX_LOGW("Manifest validation failed, but continuing...");
    }
    
    ETHERVOX_LOGI("Tool Manifest System initialized successfully");
    ETHERVOX_LOGI("  Tools available: %u", manifest_registry->header.tool_count);
    ETHERVOX_LOGI("  Fallback level: %u (%s)",
                  manifest_registry->fallback_level,
                  ethervox_tool_fallback_level_name(manifest_registry->fallback_level));
    
    return 0;
}

/**
 * Build complete system prompt using Tool Manifest System
 * 
 * Generates minimal prompt with tool index, then appends base instructions.
 * 
 * @param manifest_registry Tool manifest registry
 * @param output Output buffer
 * @param output_size Buffer size
 * @return Number of bytes written, or negative on error
 */
int ethervox_governor_build_system_prompt_with_manifest(
    const tool_manifest_registry_t* manifest_registry,
    char* output,
    size_t output_size
) {
    if (!output || output_size == 0) {
        return -1;
    }
    
    int offset = 0;
    
    // Base system message
    offset = snprintf(output, output_size,
        "You are a helpful AI assistant.\n"
        "Respond naturally and conversationally to the user.\n\n");
    
    if (offset < 0 || (size_t)offset >= output_size) {
        return -1;
    }
    
    // Add tool index (minimal prompt)
    if (manifest_registry && manifest_registry->tools_available) {
        int tool_prompt_len = ethervox_tool_build_minimal_system_prompt(
            manifest_registry,
            output + offset,
            output_size - offset,
            255  // Include all tools
        );
        
        if (tool_prompt_len > 0) {
            offset += tool_prompt_len;
        }
    } else {
        // Level 2/3: No dynamic tools, show basic commands
        int cmd_len = snprintf(output + offset, output_size - offset,
            "Available commands:\n"
            "• /help - Show this help\n"
            "• /quit - Exit conversation\n"
            "• /clear - Clear history\n"
            "• /memory - Search past conversations\n"
            "• /status - System information\n\n");
        
        if (cmd_len > 0) {
            offset += cmd_len;
        }
    }
    
    ETHERVOX_LOGI("System prompt generated: %d bytes", offset);
    
    return offset;
}
