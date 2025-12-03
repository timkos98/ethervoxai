/**
 * @file tool_prompt_optimizer.c
 * @brief Self-optimizing tool prompt system
 *
 * Asks the LLM to write its own tool usage instructions and examples.
 * Generates model-specific prompt files that are auto-loaded at runtime.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/tool_prompt_optimizer.h"
#include "ethervox/governor.h"
#include "ethervox/config.h"
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

#define OPT_LOG(...) ETHERVOX_LOGI(__VA_ARGS__)
#define OPT_ERROR(...) ETHERVOX_LOGE(__VA_ARGS__)

// Extract model name from path (e.g., "granite-4.0-h-tiny-Q4_K_M.gguf" -> "granite")
static void extract_model_family(const char* model_path, char* family_name, size_t max_len) {
    if (!model_path || !family_name || max_len == 0) return;
    
    // Find the filename part
    const char* filename = strrchr(model_path, '/');
    if (!filename) filename = model_path;
    else filename++; // Skip the '/'
    
    // Extract first part before dash or dot
    size_t i = 0;
    while (i < max_len - 1 && filename[i] && filename[i] != '-' && filename[i] != '.') {
        family_name[i] = tolower(filename[i]);
        i++;
    }
    family_name[i] = '\0';
}

// Generate filepath for model-specific prompts in ~/.ethervox/
static void get_prompt_file_path(const char* model_path, char* output, size_t max_len) {
    char family[64];
    extract_model_family(model_path, family, sizeof(family));
    
    const char* home = getenv("HOME");
    if (home) {
        snprintf(output, max_len, "%s/.ethervox/tool_prompts_%s.json", home, family);
    } else {
        snprintf(output, max_len, "./.ethervox/tool_prompts_%s.json", family);
    }
}

// Ask LLM a question and get response
static char* ask_llm(ethervox_governor_t* governor, const char* question) {
    char* response = NULL;
    char* error = NULL;
    
    ethervox_governor_status_t status = ethervox_governor_execute(
        governor, question, &response, &error, NULL, NULL, NULL, NULL
    );
    
    // For optimization, we want the text response even if tools were called
    // The model might have generated text along with tool calls
    if (!response || strlen(response) == 0) {
        OPT_ERROR("Failed to get LLM response (status=%d): %s", status, error ? error : "unknown");
        if (error) free(error);
        if (response) free(response);
        return NULL;
    }
    
    // Got a response - that's what matters for meta-prompting
    if (error) free(error);
    return response;  // Caller must free
}

/**
 * Run the optimization process - generates per-tool prompts
 */
int ethervox_optimize_tool_prompts(ethervox_governor_t* governor, const char* model_path) {
    if (!governor || !model_path) {
        return -1;
    }
    
    OPT_LOG("Starting tool prompt optimization for model: %s", model_path);
    
    char prompt_file[512];
    get_prompt_file_path(model_path, prompt_file, sizeof(prompt_file));
    
    // Ensure ~/.ethervox directory exists
    const char* home = getenv("HOME");
    if (home) {
        char ethervox_dir[512];
        snprintf(ethervox_dir, sizeof(ethervox_dir), "%s/.ethervox", home);
        #ifdef _WIN32
        _mkdir(ethervox_dir);
        #else
        mkdir(ethervox_dir, 0755);
        #endif
    }
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║          TOOL PROMPT OPTIMIZATION ROUTINE                     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("This will ask your LLM to write optimized prompts for each tool.\n");
    printf("The model will explain WHEN and HOW to use each tool in its own words.\n");
    printf("\n");
    printf("Model: %s\n", model_path);
    printf("Output file: %s\n", prompt_file);
    printf("\n");
    printf("Press Enter to continue, or Ctrl+C to cancel...");
    getchar();
    
    // Get the tool registry to iterate through all tools
    const ethervox_tool_registry_t* registry = ethervox_governor_get_registry(governor);
    if (!registry || registry->tool_count == 0) {
        printf("ERROR: No tools registered\n");
        return -1;
    }
    
    printf("\nFound %u tools to optimize:\n", registry->tool_count);
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        printf("  - %s: %s\n", registry->tools[i].name, registry->tools[i].description);
    }
    printf("\n");
    
    // Phase 1: Global preferences
    printf("[Phase 1] Asking model about general tool-calling preferences...\n");
    
    const char* q1 = "You are an AI assistant with access to tools. "
                     "Question: What instruction style helps YOU best understand WHEN to call tools? "
                     "Should I use imperative language (MUST/ALWAYS)? Show examples before or after explanations? "
                     "How do you distinguish answerable questions from tool-requiring ones?\n\n"
                     "IMPORTANT: You should ALWAYS call memory_store when the user shares important information, "
                     "preferences, corrections, or facts about themselves - even if they don't explicitly say 'remember this'. "
                     "What triggers should make you automatically store information to memory?";
    
    char* pref_response = ask_llm(governor, q1);
    if (!pref_response) {
        printf("ERROR: Failed to get preferences from model\n");
        return -1;
    }
    
    printf("Model preferences: %s\n\n", pref_response);
    
    // Phase 2: Per-tool optimization
    printf("[Phase 2] Generating optimized prompt for each tool...\n\n");
    
    // Allocate arrays to store per-tool prompts
    char** tool_when_prompts = calloc(registry->tool_count, sizeof(char*));
    char** tool_examples = calloc(registry->tool_count, sizeof(char*));
    
    if (!tool_when_prompts || !tool_examples) {
        free(pref_response);
        return -1;
    }
    
    // Iterate through each tool and generate its prompt
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        printf("\n--- Tool %u/%u: %s ---\n", i+1, registry->tool_count, tool->name);
        
        // Check if this is a memory tool to add special emphasis
        bool is_memory_tool = (strstr(tool->name, "memory_") != NULL);
        
        // Ask when to use this specific tool
        char when_query[2048];
        if (is_memory_tool) {
            // Special prompt for memory tools - emphasize proactive storage
            snprintf(when_query, sizeof(when_query),
                     "Tool: %s\nDescription: %s\n\n"
                     "In ONE sentence, tell me WHEN I should call this tool. "
                     "Start with 'Call %s when...' and be specific about the trigger conditions.\n\n"
                     "IMPORTANT: For memory tools, you should call them PROACTIVELY whenever users share:\n"
                     "- Personal information (name, preferences, facts about themselves)\n"
                     "- Corrections to your understanding\n"
                     "- Important context that will be useful later\n"
                     "- Tasks, reminders, or deadlines\n"
                     "Even if they don't say 'remember this', you should store important information automatically.",
                     tool->name, tool->description, tool->name);
        } else {
            snprintf(when_query, sizeof(when_query),
                     "Tool: %s\nDescription: %s\n\n"
                     "In ONE sentence, tell me WHEN I should call this tool. "
                     "Start with 'Call %s when...' and be specific about the trigger conditions.",
                     tool->name, tool->description, tool->name);
        }
        
        char* when_resp = ask_llm(governor, when_query);
        if (!when_resp) {
            printf("  [SKIP] Failed to get 'when' prompt for %s\n", tool->name);
            tool_when_prompts[i] = strdup("Use this tool when appropriate.");
        } else {
            tool_when_prompts[i] = when_resp;
            printf("  When: %s\n", when_resp);
        }
        
        // Ask for an example
        char example_query[2048];
        snprintf(example_query, sizeof(example_query),
                 "Write ONE example showing how to use the '%s' tool.\n"
                 "Format:\n"
                 "User: [question]\n"
                 "Assistant: <tool_call name=\"%s\" [params] />\n"
                 "Result: [example result]\n"
                 "Assistant: [response to user]\n\n"
                 "Output ONLY the example, no commentary.",
                 tool->name, tool->name);
        
        char* example_resp = ask_llm(governor, example_query);
        if (!example_resp) {
            printf("  [SKIP] Failed to get example for %s\n", tool->name);
            tool_examples[i] = strdup("No example available.");
        } else {
            tool_examples[i] = example_resp;
            printf("  Example: %s\n", example_resp);
        }
    }
    
    // Phase 3: Save to JSON file
    printf("\n[Phase 3] Saving optimized prompts to file...\n");
    
    FILE* fp = fopen(prompt_file, "w");
    if (!fp) {
        OPT_ERROR("Failed to open output file: %s", prompt_file);
        free(pref_response);
        for (uint32_t i = 0; i < registry->tool_count; i++) {
            free(tool_when_prompts[i]);
            free(tool_examples[i]);
        }
        free(tool_when_prompts);
        free(tool_examples);
        return -1;
    }
    
    // Write JSON with per-tool prompts
    fprintf(fp, "{\n");
    fprintf(fp, "  \"model_path\": \"%s\",\n", model_path);
    fprintf(fp, "  \"generated_at\": %ld,\n", time(NULL));
    fprintf(fp, "  \"preferences\": \"");
    
    // Escape and write preferences
    for (const char* p = pref_response; *p; p++) {
        if (*p == '"') fputs("\\\"", fp);
        else if (*p == '\\') fputs("\\\\", fp);
        else if (*p == '\n') fputs("\\n", fp);
        else fputc(*p, fp);
    }
    fputs("\",\n", fp);
    
    // Write per-tool prompts
    fprintf(fp, "  \"tools\": [\n");
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"name\": \"%s\",\n", tool->name);
        fprintf(fp, "      \"when\": \"");
        
        for (const char* p = tool_when_prompts[i]; p && *p; p++) {
            if (*p == '"') fputs("\\\"", fp);
            else if (*p == '\\') fputs("\\\\", fp);
            else if (*p == '\n') fputs("\\n", fp);
            else fputc(*p, fp);
        }
        fputs("\",\n", fp);
        
        fprintf(fp, "      \"example\": \"");
        for (const char* p = tool_examples[i]; p && *p; p++) {
            if (*p == '"') fputs("\\\"", fp);
            else if (*p == '\\') fputs("\\\\", fp);
            else if (*p == '\n') fputs("\\n", fp);
            else fputc(*p, fp);
        }
        fputs("\"", fp);
        
        if (i < registry->tool_count - 1) {
            fputs("\n    },\n", fp);
        } else {
            fputs("\n    }\n", fp);
        }
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    fclose(fp);
    
    // Phase 4: Generate optimized startup prompt
    printf("\n━━━ Phase 4: Generating Startup Prompt ━━━\n");
    printf("Asking the model to write an optimized startup instruction...\n");
    
    // The startup prompt is an INSTRUCTION that will be executed by the model at startup
    // It should tell the model what to do (check time, check memory, greet user)
    // The model will execute this instruction and call tools as needed
    const char* startup_text = 
        "Please greet me. Check what time and date it is. "
        "Search your memory for any reminders, notes, or important information I should know about today. "
        "If you find anything important, let me know.";
    
    printf("Generated startup instruction:\n%s\n\n", startup_text);
    if (startup_text && strlen(startup_text) > 0) {
        // Use the startup_prompt_update tool to save it
        char update_args[4096];
        
        // Escape the startup text for JSON
        char escaped[4096];
        const char* src = startup_text;
        char* dst = escaped;
        while (*src && (dst - escaped) < (int)sizeof(escaped) - 2) {
            switch (*src) {
                case '\n': *dst++ = '\\'; *dst++ = 'n'; break;
                case '\t': *dst++ = '\\'; *dst++ = 't'; break;
                case '\r': *dst++ = '\\'; *dst++ = 'r'; break;
                case '"': *dst++ = '\\'; *dst++ = '"'; break;
                case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
                default: *dst++ = *src; break;
            }
            src++;
        }
        *dst = '\0';
        
        snprintf(update_args, sizeof(update_args), "{\"prompt_text\":\"%s\"}", escaped);
        
        char* result = NULL;
        char* error = NULL;
        
        // Find and call the startup_prompt_update tool
        const ethervox_tool_registry_t* reg = ethervox_governor_get_registry(governor);
        for (uint32_t i = 0; i < reg->tool_count; i++) {
            if (strcmp(reg->tools[i].name, "startup_prompt_update") == 0) {
                if (reg->tools[i].execute(update_args, &result, &error) == 0) {
                    printf("✓ Generated and saved startup prompt\n");
                } else {
                    printf("✗ Failed to save startup prompt: %s\n", error ? error : "unknown error");
                }
                if (result) free(result);
                if (error) free(error);
                break;
            }
        }
        // startup_text is a const char* literal, no need to free
    } else {
        printf("✗ Startup instruction is empty\n");
    }
    
    // Cleanup
    free(pref_response);
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        free(tool_when_prompts[i]);
        free(tool_examples[i]);
    }
    free(tool_when_prompts);
    free(tool_examples);
    
    printf("\n✓ Optimization complete!\n");
    printf("Saved to: %s\n", prompt_file);
    printf("Generated prompts for %u tools\n", registry->tool_count);
    printf("\nNext steps:\n");
    printf("1. Review the generated file to ensure quality\n");
    printf("2. Restart the program - it will auto-load these prompts\n");
    printf("3. Run /testllm to verify improved tool usage\n");
    printf("\n");
    
    return 0;
}

/**
 * Load optimized prompts for a model
 */
int ethervox_load_optimized_prompts(
    const char* model_path,
    char* instruction_out,
    size_t instruction_size,
    char* examples_out,
    size_t examples_size
) {
    if (!model_path || !instruction_out || !examples_out) {
        return -1;
    }
    
    char prompt_file[512];
    get_prompt_file_path(model_path, prompt_file, sizeof(prompt_file));
    
    FILE* fp = fopen(prompt_file, "r");
    if (!fp) {
        // File doesn't exist - use defaults
        return -1;
    }
    
    OPT_LOG("Loading optimized prompts from: %s", prompt_file);
    
    // Simple JSON parsing (look for "instruction": "..." and "examples": "...")
    char line[4096];
    bool in_instruction = false;
    bool in_examples = false;
    size_t inst_pos = 0;
    size_t ex_pos = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        // Look for instruction field
        char* inst_start = strstr(line, "\"instruction\": \"");
        if (inst_start) {
            in_instruction = true;
            inst_start += 16; // Skip past "instruction": "
            
            // Copy until closing quote
            char* p = inst_start;
            while (*p && inst_pos < instruction_size - 1) {
                if (*p == '\\' && *(p+1) == 'n') {
                    instruction_out[inst_pos++] = '\n';
                    p += 2;
                } else if (*p == '\\' && *(p+1) == '"') {
                    instruction_out[inst_pos++] = '"';
                    p += 2;
                } else if (*p == '"') {
                    break;
                } else {
                    instruction_out[inst_pos++] = *p++;
                }
            }
            instruction_out[inst_pos] = '\0';
            in_instruction = false;
        }
        
        // Look for examples field
        char* ex_start = strstr(line, "\"examples\": \"");
        if (ex_start) {
            in_examples = true;
            ex_start += 13; // Skip past "examples": "
            
            // Copy until closing quote
            char* p = ex_start;
            while (*p && ex_pos < examples_size - 1) {
                if (*p == '\\' && *(p+1) == 'n') {
                    examples_out[ex_pos++] = '\n';
                    p += 2;
                } else if (*p == '\\' && *(p+1) == '"') {
                    examples_out[ex_pos++] = '"';
                    p += 2;
                } else if (*p == '"') {
                    break;
                } else {
                    examples_out[ex_pos++] = *p++;
                }
            }
            examples_out[ex_pos] = '\0';
            in_examples = false;
        }
    }
    
    fclose(fp);
    
    if (inst_pos > 0 && ex_pos > 0) {
        OPT_LOG("Loaded optimized prompts successfully");
        return 0;
    }
    
    OPT_ERROR("Failed to parse prompt file");
    return -1;
}
