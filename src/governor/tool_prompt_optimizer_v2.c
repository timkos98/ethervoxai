/**
 * @file tool_prompt_optimizer_v2.c
 * @brief Enhanced tool prompt optimizer with JSON output and batch processing
 *
 * Generates model-specific optimized prompts in JSON format.
 * Processes tools in batches to avoid KV cache overflow.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/tool_prompt_optimizer.h"
#include "ethervox/tool_manifest.h"
#include "ethervox/governor.h"
#include "ethervox/config.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define BATCH_SIZE 5  // Process 5 tools per batch to avoid KV overflow

// Extract clean model name (e.g., "granite-4.0-Q4_K_M.gguf" -> "granite-4.0")
static void extract_model_name(const char* model_path, char* model_name, size_t max_len) {
    if (!model_path || !model_name || max_len == 0) return;
    
    const char* filename = strrchr(model_path, '/');
    if (!filename) filename = model_path;
    else filename++;
    
    // Extract up to first dash followed by 'Q' (quantization marker)
    // or up to .gguf extension
    size_t i = 0;
    bool found_quant = false;
    
    while (i < max_len - 1 && filename[i]) {
        // Stop at .gguf
        if (filename[i] == '.' && strncmp(&filename[i], ".gguf", 5) == 0) {
            break;
        }
        
        // Stop at quantization marker (e.g., -Q4_K_M)
        if (filename[i] == '-' && i + 1 < strlen(filename) && filename[i+1] == 'Q') {
            break;
        }
        
        // Stop at -h- (helper marker in some models)
        if (filename[i] == '-' && i + 2 < strlen(filename) && 
            filename[i+1] == 'h' && filename[i+2] == '-') {
            break;
        }
        
        model_name[i] = filename[i];
        i++;
    }
    model_name[i] = '\0';
    
    // Remove trailing dash if present
    if (i > 0 && model_name[i-1] == '-') {
        model_name[i-1] = '\0';
    }
}

// Count words in a string (for token estimation)
static int count_words(const char* text) {
    int count = 0;
    bool in_word = false;
    
    for (const char* p = text; *p; p++) {
        if (isspace(*p)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            count++;
        }
    }
    
    return count;
}

// Extract optimized sentence from LLM response
static void extract_optimized_sentence(const char* response, char* output, size_t max_len) {
    if (!response || !output || max_len == 0) return;
    
    // Look for "Call <tool> when..." pattern
    const char* start = strstr(response, "Call ");
    if (!start) {
        // Fallback: take first sentence
        start = response;
        while (*start && isspace(*start)) start++;
    }
    
    // Find end of sentence
    const char* end = strchr(start, '.');
    if (!end) end = strchr(start, '\n');
    if (!end) end = start + strlen(start);
    
    size_t len = end - start;
    if (len >= max_len) len = max_len - 1;
    
    strncpy(output, start, len);
    output[len] = '\0';
    
    // Trim trailing whitespace
    while (len > 0 && isspace(output[len-1])) {
        output[--len] = '\0';
    }
}

// Ensure directory exists
static int ensure_directory(const char* path) {
    #ifdef _WIN32
    return _mkdir(path);
    #else
    return mkdir(path, 0755);
    #endif
}

// Create ~/.ethervox/tools/optimized/ directory structure
static int create_optimized_dir(void) {
    const char* home = getenv("HOME");
    if (!home) {
        ETHERVOX_LOGE("HOME environment variable not set");
        return -1;
    }
    
    char dir1[512], dir2[512], dir3[512];
    snprintf(dir1, sizeof(dir1), "%s/.ethervox", home);
    snprintf(dir2, sizeof(dir2), "%s/.ethervox/tools", home);
    snprintf(dir3, sizeof(dir3), "%s/.ethervox/tools/optimized", home);
    
    ensure_directory(dir1);
    ensure_directory(dir2);
    ensure_directory(dir3);
    
    return 0;
}

/**
 * Optimize tool prompts and write to JSON cache
 */
int ethervox_optimize_tool_prompts_v2(
    ethervox_governor_t* governor,
    const char* model_path,
    tool_manifest_registry_t* manifest_registry
) {
    if (!governor || !model_path || !manifest_registry) {
        return -1;
    }
    
    if (!manifest_registry->tools_available) {
        ETHERVOX_LOGE("No tools available in manifest");
        return -1;
    }
    
    // Extract model name
    char model_name[128];
    extract_model_name(model_path, model_name, sizeof(model_name));
    
    ETHERVOX_LOGI("Starting tool prompt optimization for: %s", model_name);
    
    // Create output directory
    if (create_optimized_dir() != 0) {
        ETHERVOX_LOGW("Failed to create optimized directory");
    }
    
    // Build output path
    const char* home = getenv("HOME");
    char output_path[512];
    snprintf(output_path, sizeof(output_path),
             "%s/.ethervox/tools/optimized/%s.json", home, model_name);
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║          TOOL PROMPT OPTIMIZATION (JSON OUTPUT)               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("Model: %s\n", model_name);
    printf("Tools: %u\n", manifest_registry->header.tool_count);
    printf("Output: %s\n", output_path);
    printf("Batch size: %d tools/batch\n", BATCH_SIZE);
    printf("\n");
    printf("This will generate model-specific optimized prompts (~15 tokens/tool).\n");
    printf("Estimated time: ~%d seconds\n", 
           (manifest_registry->header.tool_count / BATCH_SIZE) * 10);
    printf("\n");
    
    // Open output file
    FILE* fp = fopen(output_path, "w");
    if (!fp) {
        ETHERVOX_LOGE("Failed to open output file: %s", output_path);
        return -1;
    }
    
    // Write JSON header
    fprintf(fp, "{\n");
    fprintf(fp, "  \"model_name\": \"%s\",\n", model_name);
    fprintf(fp, "  \"optimized_at\": %ld,\n", (long)time(NULL));
    fprintf(fp, "  \"tools\": [\n");
    
    uint32_t total_tools = manifest_registry->header.tool_count;
    uint32_t tools_processed = 0;
    
    // Process tools in batches
    for (uint32_t batch_start = 0; batch_start < total_tools; batch_start += BATCH_SIZE) {
        uint32_t batch_end = batch_start + BATCH_SIZE;
        if (batch_end > total_tools) batch_end = total_tools;
        
        printf("\n[Batch %u/%u] Processing tools %u-%u...\n",
               (batch_start / BATCH_SIZE) + 1,
               (total_tools + BATCH_SIZE - 1) / BATCH_SIZE,
               batch_start + 1, batch_end);
        
        // Reset conversation to clear KV cache between batches
        ethervox_governor_reset_conversation(governor);
        
        for (uint32_t i = batch_start; i < batch_end; i++) {
            const tool_index_entry_t* tool_idx = &manifest_registry->index[i];
            
            if (!tool_idx->enabled) {
                printf("  ⊘ %s: disabled, skipping\n", tool_idx->name);
                continue;
            }
            
            // Get full tool detail
            tool_detail_header_t detail;
            tool_param_t params[MAX_PARAMETERS];
            uint8_t param_count;
            
            if (ethervox_tool_get_detail(manifest_registry, tool_idx->name, 
                                        &detail, params, &param_count) != 0) {
                printf("  ✗ %s: failed to load detail\n", tool_idx->name);
                continue;
            }
            
            // Build optimization query
            char query[4096];
            snprintf(query, sizeof(query),
                    "Tool: %s\n"
                    "Category: %s\n"
                    "Description: %s\n"
                    "Parameters: %u\n\n"
                    "In ONE concise sentence (max 15 words), explain WHEN to call this tool.\n"
                    "Start with 'Call %s when...' and be specific about triggering scenarios.\n"
                    "Focus on USER INTENT that triggers this tool, not implementation details.",
                    tool_idx->name, tool_idx->category, detail.description,
                    detail.param_count, tool_idx->name);
            
            // Ask LLM
            char* response = NULL;
            char* error = NULL;
            
            if (ethervox_governor_execute(governor, query, &response, &error,
                                         NULL, NULL, NULL, NULL) == 0 && response) {
                // Extract optimized sentence
                char optimized[256];
                extract_optimized_sentence(response, optimized, sizeof(optimized));
                
                // Estimate tokens (~0.75 tokens per word)
                int word_count = count_words(optimized);
                int token_estimate = (int)(word_count * 0.75);
                
                // Write to JSON
                fprintf(fp, "    {\n");
                fprintf(fp, "      \"name\": \"%s\",\n", tool_idx->name);
                fprintf(fp, "      \"optimized_prompt\": \"%s\",\n", optimized);
                fprintf(fp, "      \"token_count\": %d\n", token_estimate);
                fprintf(fp, "    }%s\n", (i < total_tools - 1) ? "," : "");
                
                printf("  ✓ %s: %s (%d tokens)\n", tool_idx->name, optimized, token_estimate);
                
                tools_processed++;
            } else {
                printf("  ✗ %s: optimization failed - %s\n", 
                       tool_idx->name, error ? error : "unknown");
                       
                // Write fallback entry using one-liner
                fprintf(fp, "    {\n");
                fprintf(fp, "      \"name\": \"%s\",\n", tool_idx->name);
                fprintf(fp, "      \"optimized_prompt\": \"%s\",\n", tool_idx->one_line);
                fprintf(fp, "      \"token_count\": %d\n", count_words(tool_idx->one_line));
                fprintf(fp, "    }%s\n", (i < total_tools - 1) ? "," : "");
            }
            
            free(response);
            free(error);
        }
    }
    
    // Close JSON
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    fclose(fp);
    
    printf("\n✓ Optimization complete!\n");
    printf("  Tools processed: %u/%u\n", tools_processed, total_tools);
    printf("  Output file: %s\n", output_path);
    printf("\nRestart EthervoxAI to use optimized prompts.\n");
    
    return 0;
}
