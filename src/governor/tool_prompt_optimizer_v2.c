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
#include "ethervox/chat_template.h"
#include "ethervox/config.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Forward declaration for Android files directory getter
#ifdef ETHERVOX_PLATFORM_ANDROID
extern const char* ethervox_android_get_files_dir(void);
#endif

#define BATCH_SIZE 5  // Process 5 tools per batch to avoid KV overflow

// ANSI color codes
#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

// Global cancellation flag
static volatile sig_atomic_t g_optimization_cancelled = 0;

// Test report file
static FILE* g_report_file = NULL;
static char g_report_path[512] = {0};

static void handle_sigint(int sig) {
    (void)sig;
    g_optimization_cancelled = 1;
    printf("\n\n⚠️  Optimization cancelled by user (Ctrl+C)\n");
    if (g_report_file) {
        fprintf(g_report_file, "\n\n⚠️  Optimization cancelled by user (Ctrl+C)\n");
        fflush(g_report_file);
    }
}

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
// Parse existing JSON file to get list of already optimized tools
typedef struct {
    char name[64];
    char optimized_prompt[256];
    int token_count;
} optimized_tool_entry_t;

static int parse_existing_optimizations(
    const char* json_path,
    optimized_tool_entry_t** entries_out,
    uint32_t* count_out
) {
    FILE* fp = fopen(json_path, "r");
    if (!fp) {
        *entries_out = NULL;
        *count_out = 0;
        return -1;  // File doesn't exist yet
    }
    
    // Read entire file
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 1024 * 1024) {  // Max 1MB
        fclose(fp);
        *entries_out = NULL;
        *count_out = 0;
        return -1;
    }
    
    char* content = malloc(file_size + 1);
    if (!content) {
        fclose(fp);
        return -1;
    }
    
    fread(content, 1, file_size, fp);
    content[file_size] = '\0';
    fclose(fp);
    
    // Count tools in JSON (count occurrences of '"name":')
    uint32_t tool_count = 0;
    const char* p = content;
    while ((p = strstr(p, "\"name\":")) != NULL) {
        tool_count++;
        p += 7;
    }
    
    if (tool_count == 0) {
        free(content);
        *entries_out = NULL;
        *count_out = 0;
        return 0;
    }
    
    // Allocate entries
    optimized_tool_entry_t* entries = calloc(tool_count, sizeof(optimized_tool_entry_t));
    if (!entries) {
        free(content);
        return -1;
    }
    
    // Parse each tool entry
    uint32_t idx = 0;
    p = content;
    while ((p = strstr(p, "\"name\":")) != NULL && idx < tool_count) {
        p += 7;  // Skip past '"name":'
        while (*p && isspace(*p)) p++;
        if (*p != '"') continue;
        p++;  // Skip opening quote
        
        // Extract name
        const char* name_end = strchr(p, '"');
        if (!name_end) continue;
        size_t name_len = name_end - p;
        if (name_len >= sizeof(entries[idx].name)) name_len = sizeof(entries[idx].name) - 1;
        strncpy(entries[idx].name, p, name_len);
        entries[idx].name[name_len] = '\0';
        
        // Extract optimized_prompt (optional)
        const char* prompt_start = strstr(name_end, "\"optimized_prompt\":");
        if (prompt_start && prompt_start < name_end + 500) {  // Must be in same object
            prompt_start += 20;  // Skip past '"optimized_prompt":'
            while (*prompt_start && isspace(*prompt_start)) prompt_start++;
            if (*prompt_start == '"') {
                prompt_start++;
                const char* prompt_end = strchr(prompt_start, '"');
                if (prompt_end) {
                    size_t prompt_len = prompt_end - prompt_start;
                    if (prompt_len >= sizeof(entries[idx].optimized_prompt)) {
                        prompt_len = sizeof(entries[idx].optimized_prompt) - 1;
                    }
                    strncpy(entries[idx].optimized_prompt, prompt_start, prompt_len);
                    entries[idx].optimized_prompt[prompt_len] = '\0';
                }
            }
        }
        
        // Extract token_count (optional)
        const char* token_start = strstr(name_end, "\"token_count\":");
        if (token_start && token_start < name_end + 500) {
            token_start += 15;  // Skip past '"token_count":'
            while (*token_start && isspace(*token_start)) token_start++;
            entries[idx].token_count = atoi(token_start);
        }
        
        idx++;
        p = name_end + 1;
    }
    
    free(content);
    *entries_out = entries;
    *count_out = idx;
    return 0;
}

// Check if a tool is already optimized
static bool is_tool_optimized(
    const char* tool_name,
    const optimized_tool_entry_t* entries,
    uint32_t entry_count
) {
    for (uint32_t i = 0; i < entry_count; i++) {
        if (strcmp(tool_name, entries[i].name) == 0) {
            return true;
        }
    }
    return false;
}

// Create ~/.ethervox/tools/optimized/ directory structure
static int create_optimized_dir(void) {
#ifdef ETHERVOX_PLATFORM_ANDROID
    // On Android, use app files directory
    const char* android_files_dir = ethervox_android_get_files_dir();
    if (!android_files_dir || android_files_dir[0] == '\0') {
        ETHERVOX_LOGE("Android files directory not set");
        return -1;
    }
    
    char dir1[512], dir2[512];
    snprintf(dir1, sizeof(dir1), "%s/tools", android_files_dir);
    snprintf(dir2, sizeof(dir2), "%s/tools/optimized", android_files_dir);
    
    ensure_directory(dir1);
    ensure_directory(dir2);
#else
    // On desktop, use HOME environment variable
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
#endif
    
    return 0;
}

/**
 * Optimize tool prompts and write to JSON cache
 */
int ethervox_optimize_tool_prompts_v2(
    ethervox_governor_t* governor,
    const char* model_path,
    tool_manifest_registry_t* manifest_registry,
    bool optimize_new_only
) {
    if (!governor || !model_path || !manifest_registry) {
        return -1;
    }
    
    if (!manifest_registry->tools_available) {
        ETHERVOX_LOGE("No tools available in manifest");
        return -1;
    }
    
    // Set up signal handler for Ctrl+C
    g_optimization_cancelled = 0;
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    // Create test report file
    char ethervox_dir[512];
    if (ethervox_get_runtime_path("reports", ethervox_dir, sizeof(ethervox_dir)) == 0) {
        // Ensure reports directory exists
        #ifdef _WIN32
        _mkdir(ethervox_dir);
        #else
        mkdir(ethervox_dir, 0755);
        #endif
        
        time_t now = time(NULL);
        snprintf(g_report_path, sizeof(g_report_path),
                "%s/tool_optimization_%ld.txt", ethervox_dir, (long)now);
        g_report_file = fopen(g_report_path, "w");
        if (g_report_file) {
            printf("Report: %s\n", g_report_path);
            fprintf(g_report_file, "═══════════════════════════════════════════════════════════════\n");
            fprintf(g_report_file, " Tool Prompt Optimization Report\n");
            fprintf(g_report_file, "═══════════════════════════════════════════════════════════════\n\n");
            fprintf(g_report_file, "Timestamp: %s", ctime(&now));
            fprintf(g_report_file, "Model: %s\n\n", model_path);
            fflush(g_report_file);
        } else {
            fprintf(stderr, "Warning: Failed to create report file at %s\n", g_report_path);
        }
    } else {
        fprintf(stderr, "Warning: Failed to get reports directory path\n");
    }
    
    // Detect chat template from model path to get tool call format
    chat_template_type_t template_type = chat_template_detect(model_path);
    const chat_template_t* template = chat_template_get(template_type, model_path);
    if (!template) {
        ETHERVOX_LOGW("Could not detect chat template, using default XML format");
    }
    
    // Extract tool call format example from template's usage examples
    // This ensures we're model-agnostic
    char tool_call_example[256] = "<tool_call name=\"TOOL_NAME\" param=\"value\" />";
    if (template && template->tool_usage_examples) {
        // Try to extract the actual format used in examples
        const char* example_start = strstr(template->tool_usage_examples, "<tool_call");
        if (example_start) {
            const char* example_end = strstr(example_start, "/>");
            if (example_end) {
                size_t len = (example_end - example_start) + 2;
                if (len < sizeof(tool_call_example)) {
                    strncpy(tool_call_example, example_start, len);
                    tool_call_example[len] = '\0';
                }
            }
        }
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
    char output_path[512];
#ifdef ETHERVOX_PLATFORM_ANDROID
    const char* android_files_dir = ethervox_android_get_files_dir();
    snprintf(output_path, sizeof(output_path),
             "%s/tools/optimized/%s.json", android_files_dir, model_name);
#else
    const char* home = getenv("HOME");
    snprintf(output_path, sizeof(output_path),
             "%s/.ethervox/tools/optimized/%s.json", home, model_name);
#endif
    
    // Parse existing optimizations if in incremental mode
    optimized_tool_entry_t* existing_entries = NULL;
    uint32_t existing_count = 0;
    uint32_t tools_to_optimize = manifest_registry->header.tool_count;
    uint32_t tools_skipped = 0;
    
    if (optimize_new_only) {
        int parse_result = parse_existing_optimizations(output_path, &existing_entries, &existing_count);
        if (parse_result == 0 && existing_count > 0) {
            printf("\nIncremental mode: Found %u existing optimizations\n", existing_count);
            if (g_report_file) {
                fprintf(g_report_file, "\nIncremental mode: Found %u existing optimizations\n", existing_count);
                fprintf(g_report_file, "Will only optimize new tools\n\n");
                fflush(g_report_file);
            }
            
            // Count tools that need optimization
            tools_to_optimize = 0;
            for (uint32_t i = 0; i < manifest_registry->header.tool_count; i++) {
                const tool_index_entry_t* tool_idx = &manifest_registry->index[i];
                if (tool_idx->enabled && !is_tool_optimized(tool_idx->name, existing_entries, existing_count)) {
                    tools_to_optimize++;
                }
            }
            tools_skipped = manifest_registry->header.tool_count - tools_to_optimize;
        } else {
            printf("\nIncremental mode: No existing optimizations found, will optimize all tools\n");
        }
    }
    
    printf("\n");
    printf("Starting optimization...\n");
    printf("Model: %s\n", model_name);
    printf("Total tools: %u\n", manifest_registry->header.tool_count);
    if (optimize_new_only && existing_count > 0) {
        printf("Already optimized: %u\n", tools_skipped);
        printf("To optimize: %u\n", tools_to_optimize);
    } else {
        printf("To optimize: %u\n", tools_to_optimize);
    }
    printf("Output: %s\n", output_path);
    printf("Batch size: %d tools/batch\n", BATCH_SIZE);
    printf("\n");
    printf("This will generate model-specific optimized prompts (~15 tokens/tool).\n");
    if (tools_to_optimize > 0) {
        printf("Estimated time: ~%d seconds\n", 
               (tools_to_optimize / BATCH_SIZE + 1) * 10);
    }
    printf("\n");
    
    // If no new tools to optimize, we're done
    if (tools_to_optimize == 0) {
        printf(COLOR_GREEN "✓" COLOR_RESET " All tools already optimized!\n");
        if (existing_entries) free(existing_entries);
        if (g_report_file) {
            fprintf(g_report_file, "\nAll tools already optimized - nothing to do\n");
            fclose(g_report_file);
        }
        return 0;
    }
    
    // Open output file
    FILE* fp = fopen(output_path, "w");
    if (!fp) {
        ETHERVOX_LOGE("Failed to open output file: %s", output_path);
        if (existing_entries) free(existing_entries);
        return -1;
    }
    
    // Write JSON header
    fprintf(fp, "{\n");
    fprintf(fp, "  \"model_name\": \"%s\",\n", model_name);
    fprintf(fp, "  \"optimized_at\": %ld,\n", (long)time(NULL));
    fprintf(fp, "  \"tools\": [\n");
    
    // First, write existing optimized tools (if in incremental mode)
    bool first_entry = true;
    if (optimize_new_only && existing_entries && existing_count > 0) {
        for (uint32_t i = 0; i < existing_count; i++) {
            if (!first_entry) fprintf(fp, ",\n");
            fprintf(fp, "    {\n");
            fprintf(fp, "      \"name\": \"%s\",\n", existing_entries[i].name);
            fprintf(fp, "      \"optimized_prompt\": \"%s\",\n", existing_entries[i].optimized_prompt);
            fprintf(fp, "      \"token_count\": %d\n", existing_entries[i].token_count);
            fprintf(fp, "    }");
            first_entry = false;
        }
    }
    
    uint32_t total_tools = manifest_registry->header.tool_count;
    uint32_t tools_processed = 0;
    
    // Process tools in batches
    for (uint32_t batch_start = 0; batch_start < total_tools; batch_start += BATCH_SIZE) {
        if (g_optimization_cancelled) {
            printf("\n⚠️  Optimization cancelled - cleaning up...\n");
            if (g_report_file) {
                fprintf(g_report_file, "\n⚠️  Optimization cancelled before completion\n");
                fprintf(g_report_file, "Tools processed: %u/%u\n", tools_processed, total_tools);
                fflush(g_report_file);
                fclose(g_report_file);
                printf("📄 Partial report saved: %s\n", g_report_path);
            }
            fclose(fp);
            return -2;  // Special code for user cancellation
        }
        
        uint32_t batch_end = batch_start + BATCH_SIZE;
        if (batch_end > total_tools) batch_end = total_tools;
        
        printf("\n[Batch %u/%u] Processing tools %u-%u... (Ctrl+C to cancel)\n",
               (batch_start / BATCH_SIZE) + 1,
               (total_tools + BATCH_SIZE - 1) / BATCH_SIZE,
               batch_start + 1, batch_end);
        
        if (g_report_file) {
            fprintf(g_report_file, "\n[Batch %u/%u] Processing tools %u-%u\n",
                   (batch_start / BATCH_SIZE) + 1,
                   (total_tools + BATCH_SIZE - 1) / BATCH_SIZE,
                   batch_start + 1, batch_end);
            fflush(g_report_file);
        }
        
        // Reset conversation to clear KV cache between batches
        ethervox_governor_reset_conversation(governor);
        
        for (uint32_t i = batch_start; i < batch_end; i++) {
            if (g_optimization_cancelled) {
                printf("\n⚠️  Optimization cancelled\n");
                if (g_report_file) {
                    fprintf(g_report_file, "\n⚠️  Optimization cancelled\n");
                    fprintf(g_report_file, "Tools processed: %u/%u\n", tools_processed, total_tools);
                    fflush(g_report_file);
                    fclose(g_report_file);
                }
                fclose(fp);
                return -2;
            }
            const tool_index_entry_t* tool_idx = &manifest_registry->index[i];
            
            if (!tool_idx->enabled) {
                printf("  ⊘ %s: disabled, skipping\n", tool_idx->name);
                continue;
            }
            
            // Skip if already optimized (in incremental mode)
            if (optimize_new_only && existing_entries && 
                is_tool_optimized(tool_idx->name, existing_entries, existing_count)) {
                printf("  " COLOR_CYAN "↻" COLOR_RESET " %s: already optimized, skipping\n", tool_idx->name);
                if (g_report_file) {
                    fprintf(g_report_file, "  ↻ %s: already optimized, skipping\n", tool_idx->name);
                    fflush(g_report_file);
                }
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
            
            // Build optimization query - model-agnostic
            char query[4096];
            int qoff = snprintf(query, sizeof(query),
                    "Tool: %s\n"
                    "Category: %s\n"
                    "Description: %s\n"
                    "Parameters (%u):\n",
                    tool_idx->name, tool_idx->category, detail.description, detail.param_count);
            
            // Add parameter list
            for (uint8_t p = 0; p < param_count && qoff < (int)sizeof(query) - 500; p++) {
                qoff += snprintf(query + qoff, sizeof(query) - qoff,
                               "  - %s (%s, %s)\n",
                               params[p].name, params[p].type,
                               params[p].required ? "required" : "optional");
            }
            
            // Build tool call format example using actual parameter names
            char tool_format[512];
            int foff = snprintf(tool_format, sizeof(tool_format),
                              "<tool_call name=\"%s\"", tool_idx->name);
            for (uint8_t p = 0; p < param_count && foff < (int)sizeof(tool_format) - 50; p++) {
                foff += snprintf(tool_format + foff, sizeof(tool_format) - foff,
                               " %s=\"value\"", params[p].name);
            }
            snprintf(tool_format + foff, sizeof(tool_format) - foff, " />");
            
            qoff += snprintf(query + qoff, sizeof(query) - qoff,
                    "\nGenerate a concise optimized prompt (under 30 words) that includes:\n"
                    "1. WHEN to call this tool (user intent/trigger scenarios)\n"
                    "2. HOW to call it: %s\n\n"
                    "Format: Start with 'Call %s when...' then show the exact tool call format.\n"
                    "Be specific about triggering scenarios.",
                    tool_format, tool_idx->name);
            
            // Disable tool execution so the LLM's example tool call isn't executed
            ethervox_governor_set_tool_execution(governor, false);
            
            // Ask LLM
            char* response = NULL;
            char* error = NULL;
            
            if (ethervox_governor_execute(governor, query, &response, &error,
                                         NULL, NULL, NULL, NULL) == 0 && response) {
                // Re-enable tool execution
                ethervox_governor_set_tool_execution(governor, true);
                
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
                
                printf("  " COLOR_GREEN "✓" COLOR_RESET " %s: %s (%d tokens)\n", 
                       tool_idx->name, optimized, token_estimate);
                
                if (g_report_file) {
                    fprintf(g_report_file, "  ✓ %s\n", tool_idx->name);
                    fprintf(g_report_file, "    Optimized: %s\n", optimized);
                    fprintf(g_report_file, "    Tokens: %d\n", token_estimate);
                    fflush(g_report_file);
                }
                
                tools_processed++;
            } else {
                // Re-enable tool execution even on failure
                ethervox_governor_set_tool_execution(governor, true);
                
                printf("  ✗ %s: optimization failed - %s\n", 
                       tool_idx->name, error ? error : "unknown");
                       
                if (g_report_file) {
                    fprintf(g_report_file, "  ✗ %s: optimization failed\n", tool_idx->name);
                    if (error) {
                        fprintf(g_report_file, "    Error: %s\n", error);
                    }
                    fflush(g_report_file);
                }
                       
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
    
    printf("\n" COLOR_GREEN "✓" COLOR_RESET " Optimization complete!\n");
    printf("  Tools processed: %u/%u\n", tools_processed, total_tools);
    printf("  Output file: %s\n", output_path);
    
    // Write final statistics to report
    if (g_report_file) {
        fprintf(g_report_file, "\n═══════════════════════════════════════════════════════════════\n");
        fprintf(g_report_file, " Optimization Summary\n");
        fprintf(g_report_file, "═══════════════════════════════════════════════════════════════\n\n");
        fprintf(g_report_file, "Total tools: %u\n", total_tools);
        fprintf(g_report_file, "Successfully optimized: %u\n", tools_processed);
        fprintf(g_report_file, "Failed: %u\n", total_tools - tools_processed);
        fprintf(g_report_file, "Success rate: %.1f%%\n\n", 
               (tools_processed * 100.0) / total_tools);
        fprintf(g_report_file, "Output file: %s\n", output_path);
        fprintf(g_report_file, "\n═══════════════════════════════════════════════════════════════\n");
        fprintf(g_report_file, " Optimization Complete\n");
        fprintf(g_report_file, "═══════════════════════════════════════════════════════════════\n");
        fflush(g_report_file);
        fclose(g_report_file);
        printf("\n" COLOR_CYAN "📄 Report saved: %s" COLOR_RESET "\n", g_report_path);
    }
    
    printf("\nRestart EthervoxAI to use optimized prompts.\n");
    
    // Cleanup
    if (existing_entries) {
        free(existing_entries);
    }
    
    return 0;
}
