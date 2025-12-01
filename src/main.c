/**
 * @file main.c
 * @brief Main entry point for EthervoxAI command-line tool
 *
 * Interactive REPL with Governor orchestration and memory tools
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __APPLE__
#include <readline/readline.h>
#include <readline/history.h>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "ethervox/config.h"
#include "ethervox/platform.h"
#include "ethervox/governor.h"
#include "ethervox/compute_tools.h"
#include "ethervox/memory_tools.h"
#include "ethervox/file_tools.h"
#include "ethervox/logging.h"

// External debug flag from logging.c (declared in config.h)
// extern int g_ethervox_debug_enabled; // Already declared in config.h

// Global state for signal handling
static volatile bool g_running = true;
static bool g_debug_enabled = false;  // Debug logging disabled by default (opt-in)
static bool g_quiet_mode = true;      // Quiet mode by default
static bool g_markdown_enabled = true; // Markdown formatting enabled by default
static char g_loaded_model_path[512] = {0};  // Track loaded model path for /reset

// Default startup prompt text (used if no custom prompt file exists)
static const char* DEFAULT_STARTUP_PROMPT = 
    "Check for any pending reminders using memory_reminder_list. "
    "Search for important recent memories using memory_search with min_importance=0.8 (or 0.9 for critical items). "
    "Get the current date"
    "Get the current time in 24hr format."
    "Get the current week number"
    "look for notes or memos stored in memory."
    "Greet the user tell them the week number, the date, and time in 24hr format"
    "List reminders if they exist, do not make up any reminders. If the tool finds no reminders, just say that there are no reminders."
    "ONLY return reminders that came from a memory_reminder_list tool call.";

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    printf("\n\nShutting down gracefully...\n");
}

static void print_banner(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                     EthervoxAI v%s                        ║\n", ETHERVOX_VERSION_STRING);
    printf("║          Governor LLM with Memory Tools Plugin               ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
#ifdef ETHERVOX_PLATFORM_RPI
    printf("Platform: Raspberry Pi\n");
#elif defined(ETHERVOX_PLATFORM_ESP32)
    printf("Platform: ESP32\n");
#elif defined(ETHERVOX_PLATFORM_MACOS)
    printf("Platform: macOS\n");
#elif defined(ETHERVOX_PLATFORM_LINUX)
    printf("Platform: Linux\n");
#elif defined(ETHERVOX_PLATFORM_WINDOWS)
    printf("Platform: Windows\n");
#elif defined(ETHERVOX_PLATFORM_ANDROID)
    printf("Platform: Android\n");
#else
    printf("Platform: Unknown\n");
#endif
    printf("\n");
}

static void print_help(void) {
    printf("\nAvailable Commands:\n");
    printf("  /help              Show this help message\n");
    printf("  /test              Run component tests\n");
    printf("  /load <path>       Load Governor model\n");
    printf("  /tools             Show loaded Governor tools\n");
    printf("  /search <query>    Search conversation memory\n");
    printf("  /summary [n]       Summarize last n turns (default: 10)\n");
    printf("  /export <file>     Export memory to JSON file\n");
    printf("  /stats             Show memory statistics\n");
    printf("  /startup <cmd>     Manage startup prompt (edit/show/reset)\n");
    printf("  /debug             Toggle debug logging on/off\n");
    printf("  /markdown          Toggle markdown formatting on/off\n");
    printf("  /clear             Clear conversation memory\n");
    printf("  /reset             Reset conversation (reload model)\n");
    printf("  /paste             Enter paste mode for multi-line input\n");
    printf("  /quit              Exit the program\n");
    printf("\nPaste Mode: Type /paste to enter, then paste multi-line text.\n");
    printf("Exit with /end on its own line or Ctrl+D.\n");
    printf("\nOr just type a message to chat with the Governor.\n\n");
}

// Component test functions
static bool test_platform(ethervox_platform_t* platform) {
    printf("[TEST] Platform detection... ");
    int result = ethervox_platform_register_hal(platform);
    if (result != 0) {
        printf("✗ FAILED\n");
        return false;
    }
    printf("✓ OK (%s)\n", platform->info.platform_name);
    return true;
}

static bool test_memory(const char* test_dir) {
    printf("[TEST] Memory initialization... ");
    ethervox_memory_store_t test_mem;
    if (ethervox_memory_init(&test_mem, "test", test_dir) != 0) {
        printf("✗ FAILED\n");
        return false;
    }
    printf("✓ OK\n");
    
    printf("[TEST] Memory storage... ");
    const char* tags[] = {"test"};
    uint64_t id;
    if (ethervox_memory_store_add(&test_mem, "Test entry", tags, 1, 0.8f, true, &id) != 0) {
        printf("✗ FAILED\n");
        ethervox_memory_cleanup(&test_mem);
        return false;
    }
    printf("✓ OK (ID: %llu)\n", (unsigned long long)id);
    
    printf("[TEST] Memory search... ");
    ethervox_memory_search_result_t* results = NULL;
    uint32_t count = 0;
    if (ethervox_memory_search(&test_mem, "Test", NULL, 0, 5, &results, &count) != 0) {
        printf("✗ FAILED\n");
        ethervox_memory_cleanup(&test_mem);
        return false;
    }
    printf("✓ OK (%u results)\n", count);
    free(results);
    
    ethervox_memory_cleanup(&test_mem);
    return true;
}

static bool test_governor(void) {
    printf("[TEST] Governor initialization... ");
    ethervox_tool_registry_t reg;
    if (ethervox_tool_registry_init(&reg, 8) != 0) {
        printf("✗ FAILED\n");
        return false;
    }
    
    ethervox_governor_t* gov = NULL;
    if (ethervox_governor_init(&gov, NULL, &reg) != 0) {
        printf("✗ FAILED\n");
        ethervox_tool_registry_cleanup(&reg);
        return false;
    }
    printf("✓ OK\n");
    
    ethervox_governor_cleanup(gov);
    ethervox_tool_registry_cleanup(&reg);
    return true;
}

static bool test_memory_tools_registration(ethervox_governor_t* gov, ethervox_memory_store_t* mem) {
    printf("[TEST] Memory tools registration... ");
    // Memory tools are registered during init, just verify memory works
    if (!gov || !mem) {
        printf("✗ FAILED (NULL pointers)\n");
        return false;
    }
    printf("✓ OK\n");
    return true;
}

static void run_component_tests(const char* memory_dir) {
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  Component Tests\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    
    int passed = 0;
    int total = 0;
    
    ethervox_platform_t platform = {0};
    total++;
    if (test_platform(&platform)) passed++;
    
    total++;
    if (test_memory(memory_dir)) passed++;
    
    total++;
    if (test_governor()) passed++;
    
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  Results: %d/%d tests passed\n", passed, total);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
}

static void print_stats(ethervox_memory_store_t* memory) {
    printf("\n━━━ Memory Statistics ━━━\n");
    printf("Session ID:       %s\n", memory->session_id);
    printf("Current entries:  %u\n", memory->entry_count);
    printf("Total stored:     %llu\n", (unsigned long long)memory->total_memories_stored);
    printf("Total searches:   %llu\n", (unsigned long long)memory->total_searches);
    printf("Total exports:    %llu\n", (unsigned long long)memory->total_exports);
    if (memory->storage_filepath[0]) {
        printf("Storage file:     %s\n", memory->storage_filepath);
    } else {
        printf("Storage:          Memory-only (no persistence)\n");
    }
    printf("\n");
}

// Paste mode: accumulate multi-line input until /end or Ctrl+D
static char* read_paste_input(void) {
    // Allocate buffer for accumulated input (64KB max)
    size_t buffer_size = 65536;
    char* buffer = malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Memory allocation failed for paste mode\n");
        return NULL;
    }
    
    buffer[0] = '\0';
    size_t total_len = 0;
    
#if defined(__APPLE__) || defined(__linux__)
    // Readline-based paste mode
    printf("(Paste mode - type /end or press Ctrl+D to submit)\n");
    
    while (1) {
        char* line = readline("paste> ");
        
        if (!line) {
            // Ctrl+D (EOF) - exit paste mode and submit
            printf("\n");
            break;
        }
        
        // Check for /end command
        if (strcmp(line, "/end") == 0) {
            free(line);
            printf("\n");
            break;
        }
        
        // Append line to buffer with newline
        size_t line_len = strlen(line);
        if (total_len + line_len + 2 > buffer_size) {
            fprintf(stderr, "Paste buffer exceeded 64KB limit\n");
            free(line);
            free(buffer);
            return NULL;
        }
        
        if (total_len > 0) {
            buffer[total_len++] = '\n';
        }
        strcpy(buffer + total_len, line);
        total_len += line_len;
        buffer[total_len] = '\0';
        
        free(line);
    }
#else
    // Fallback paste mode (no readline)
    printf("(Paste mode - type /end or press Ctrl+D to submit)\n");
    char line[2048];
    
    while (1) {
        printf("paste> ");
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) {
            // Ctrl+D (EOF) - exit paste mode and submit
            printf("\n");
            break;
        }
        
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        
        // Check for /end command
        if (strcmp(line, "/end") == 0) {
            printf("\n");
            break;
        }
        
        // Append line to buffer with newline
        if (total_len + len + 2 > buffer_size) {
            fprintf(stderr, "Paste buffer exceeded 64KB limit\n");
            free(buffer);
            return NULL;
        }
        
        if (total_len > 0) {
            buffer[total_len++] = '\n';
        }
        strcpy(buffer + total_len, line);
        total_len += len;
        buffer[total_len] = '\0';
    }
#endif
    
    return buffer;
}

static void handle_search(ethervox_memory_store_t* memory, const char* query) {
    ethervox_memory_search_result_t* results = NULL;
    uint32_t result_count = 0;
    
    int ret = ethervox_memory_search(memory, query, NULL, 0, 10, &results, &result_count);
    
    if (ret != 0 || result_count == 0) {
        printf("No results found for: %s\n", query);
        return;
    }
    
    printf("\n━━━ Search Results (%u found) ━━━\n", result_count);
    for (uint32_t i = 0; i < result_count; i++) {
        printf("\n[%.2f] %s:\n", 
               results[i].relevance,
               results[i].entry.is_user_message ? "User" : "Assistant");
        printf("  %s\n", results[i].entry.text);
        
        if (results[i].entry.tag_count > 0) {
            printf("  Tags: ");
            for (uint32_t t = 0; t < results[i].entry.tag_count; t++) {
                printf("%s%s", results[i].entry.tags[t], 
                       t < results[i].entry.tag_count - 1 ? ", " : "");
            }
            printf("\n");
        }
    }
    printf("\n");
    
    free(results);
}

static void handle_summary(ethervox_memory_store_t* memory, int window_size) {
    char* summary = NULL;
    char** key_points = NULL;
    uint32_t kp_count = 0;
    
    int ret = ethervox_memory_summarize(memory, window_size, NULL, 
                                        &summary, &key_points, &kp_count);
    
    if (ret != 0 || !summary) {
        printf("Failed to generate summary\n");
        return;
    }
    
    printf("\n━━━ Conversation Summary ━━━\n");
    printf("%s\n", summary);
    
    if (kp_count > 0) {
        printf("\nKey Points:\n");
        for (uint32_t i = 0; i < kp_count; i++) {
            printf("  • %s\n", key_points[i]);
            free(key_points[i]);
        }
        free(key_points);
    }
    printf("\n");
    
    free(summary);
}

static void handle_export(ethervox_memory_store_t* memory, const char* filepath) {
    uint64_t bytes_written = 0;
    
    int ret = ethervox_memory_export(memory, filepath, "json", &bytes_written);
    
    if (ret != 0) {
        printf("Failed to export memory to: %s\n", filepath);
        return;
    }
    
    printf("Exported %llu bytes to: %s\n", (unsigned long long)bytes_written, filepath);
}

static void store_message(ethervox_memory_store_t* memory, const char* text, 
                         bool is_user, float importance) {
    const char* tags[] = {"conversation"};
    uint64_t memory_id;
    
    ethervox_memory_store_add(memory, text, tags, 1, importance, is_user, &memory_id);
}

// Convert markdown formatting to ANSI escape codes for terminal display
static char* markdown_to_ansi(const char* text) {
    if (!text) return NULL;
    
    size_t len = strlen(text);
    // Allocate generous buffer (3x original for escape codes)
    char* output = malloc(len * 3 + 1);
    if (!output) return strdup(text);
    
    const char* src = text;
    char* dst = output;
    bool in_bold = false;
    bool in_italic = false;
    bool in_code = false;
    
    while (*src) {
        // Bold: **text**
        if (src[0] == '*' && src[1] == '*') {
            if (in_bold) {
                // End bold - need to reapply other active styles
                strcpy(dst, "\033[22m");  // Turn off bold specifically
                dst += 5;
                if (in_italic) {
                    strcpy(dst, "\033[3m");  // Reapply italic
                    dst += 4;
                }
                in_bold = false;
            } else {
                // Start bold
                strcpy(dst, "\033[1m");
                dst += 4;
                in_bold = true;
            }
            src += 2;
            continue;
        }
        
        // Italic: *text* (but not if it's part of **)
        if (*src == '*' && !(src > text && src[-1] == '*') && !(src[1] == '*')) {
            if (in_italic) {
                // End italic/underline
                strcpy(dst, "\033[24m");  // Turn off underline
                dst += 5;
                if (in_bold) {
                    strcpy(dst, "\033[1m");  // Reapply bold
                    dst += 4;
                }
                in_italic = false;
            } else {
                // Start italic/underline (use underline for better compatibility)
                strcpy(dst, "\033[4m");
                dst += 4;
                in_italic = true;
            }
            src++;
            continue;
        }
        
        // Code/monospace: `text`
        if (*src == '`') {
            if (in_code) {
                // End code
                strcpy(dst, "\033[22m\033[39m");  // Turn off dim and restore color
                dst += 10;
                // Reapply bold/italic if needed
                if (in_bold) {
                    strcpy(dst, "\033[1m");
                    dst += 4;
                }
                if (in_italic) {
                    strcpy(dst, "\033[3m");
                    dst += 4;
                }
                in_code = false;
            } else {
                strcpy(dst, "\033[2m\033[90m");  // Dim + gray
                dst += 9;
                in_code = true;
            }
            src++;
            continue;
        }
        
        // Copy regular character
        *dst++ = *src++;
    }
    
    // Reset any remaining formatting
    if (in_bold || in_italic || in_code) {
        strcpy(dst, "\033[22m\033[24m\033[39m");  // Reset bold, underline, color
        dst += 15;
    }
    
    *dst = '\0';
    return output;
}

static void print_tools(ethervox_governor_t* governor) {
    if (!governor) {
        printf("No Governor instance available\n");
        return;
    }
    
    ethervox_tool_registry_t* registry = ethervox_governor_get_registry(governor);
    if (!registry) {
        printf("No tool registry available\n");
        return;
    }
    
    printf("\n━━━ Governor Tools (%u loaded) ━━━\n\n", registry->tool_count);
    
    // Group tools by category
    printf("Compute Tools:\n");
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        if (strstr(tool->name, "calculator_") || strstr(tool->name, "time_") || 
            strstr(tool->name, "percentage_")) {
            printf("  %-25s %s\n", tool->name, tool->description);
        }
    }
    
    printf("\nMemory Tools:\n");
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        if (strstr(tool->name, "memory_")) {
            printf("  %-25s %s\n", tool->name, tool->description);
        }
    }
    
    printf("\nFile Tools:\n");
    for (uint32_t i = 0; i < registry->tool_count; i++) {
        const ethervox_tool_t* tool = &registry->tools[i];
        if (strstr(tool->name, "file_")) {
            const char* access_note = tool->requires_confirmation ? " [WRITE ACCESS]" : "";
            printf("  %-25s %s%s\n", tool->name, tool->description, access_note);
        }
    }
    
    printf("\n");
}

static void process_command(const char* line, ethervox_memory_store_t* memory,
                           ethervox_governor_t* governor, bool* quit_flag) {
    // Trim leading/trailing whitespace
    while (*line == ' ' || *line == '\t') line++;
    
    if (line[0] == '\0') return;
    
    if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
        *quit_flag = true;
        return;
    }
    
    if (strcmp(line, "/help") == 0) {
        print_help();
        return;
    }
    
    if (strcmp(line, "/stats") == 0) {
        print_stats(memory);
        return;
    }
    
    if (strcmp(line, "/tools") == 0) {
        print_tools(governor);
        return;
    }
    
    if (strcmp(line, "/debug") == 0) {
        g_debug_enabled = !g_debug_enabled;
        g_ethervox_debug_enabled = g_debug_enabled;  // Also control legacy debug flag
        if (g_debug_enabled) {
            ethervox_log_set_level(ETHERVOX_LOG_LEVEL_INFO);
            printf("Debug logging: ENABLED\n");
        } else {
            ethervox_log_set_level(ETHERVOX_LOG_LEVEL_OFF);
            printf("Debug logging: DISABLED\n");
        }
        return;
    }
    
    if (strcmp(line, "/markdown") == 0) {
        g_markdown_enabled = !g_markdown_enabled;
        printf("Markdown formatting: %s\n", g_markdown_enabled ? "ENABLED" : "DISABLED");
        return;
    }
    
    if (strcmp(line, "/test") == 0) {
        run_component_tests(memory->storage_filepath[0] ? "./memory_data" : "/tmp");
        return;
    }
    
    if (strncmp(line, "/load ", 6) == 0) {
        const char* model_path = line + 6;
        printf("Loading model: %s\n", model_path);
        
        int ret = ethervox_governor_load_model(governor, model_path);
        if (ret == 0) {
            snprintf(g_loaded_model_path, sizeof(g_loaded_model_path), "%s", model_path);
            printf("✓ Model loaded successfully\n");
        } else {
            printf("✗ Failed to load model (code: %d)\n", ret);
        }
        return;
    }
    
    if (strncmp(line, "/search ", 8) == 0) {
        handle_search(memory, line + 8);
        return;
    }
    
    if (strncmp(line, "/summary", 8) == 0) {
        int window = 10;
        if (strlen(line) > 9) {
            window = atoi(line + 9);
        }
        handle_summary(memory, window);
        return;
    }
    
    if (strncmp(line, "/export ", 8) == 0) {
        handle_export(memory, line + 8);
        return;
    }
    
    if (strcmp(line, "/clear") == 0) {
        uint32_t pruned = 0;
        ethervox_memory_forget(memory, 0, 1.1f, &pruned); // Remove all (importance > 1.0)
        printf("Cleared %u memories\n", pruned);
        return;
    }
    
    if (strcmp(line, "/reset") == 0) {
        printf("To reset conversation history, restart the application.\n");
        printf("The KV cache maintains conversation context across queries.\n");
        return;
    }
    
    if (strncmp(line, "/startup ", 9) == 0) {
        const char* action = line + 9;
        
        // Default startup prompt file location
        const char* home_dir = getenv("HOME");
        char prompt_file[512];
        if (home_dir) {
            snprintf(prompt_file, sizeof(prompt_file), "%s/.ethervox/startup_prompt.txt", home_dir);
        } else {
            snprintf(prompt_file, sizeof(prompt_file), "./.ethervox_startup_prompt.txt");
        }
        
        if (strncmp(action, "edit", 4) == 0) {
            // Get or create editor command
            const char* editor = getenv("EDITOR");
            if (!editor) editor = "nano";  // Default to nano
            
            char cmd[768];
            snprintf(cmd, sizeof(cmd), "%s %s", editor, prompt_file);
            
            printf("Opening startup prompt in %s...\n", editor);
            printf("Default prompt: %s\n\n", DEFAULT_STARTUP_PROMPT);
            
            int ret = system(cmd);
            if (ret == 0) {
                printf("Startup prompt saved to: %s\n", prompt_file);
            } else {
                printf("Failed to open editor\n");
            }
        } else if (strncmp(action, "show", 4) == 0) {
            FILE* fp = fopen(prompt_file, "r");
            if (fp) {
                printf("Current startup prompt:\n");
                printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
                char buf[2048];
                while (fgets(buf, sizeof(buf), fp)) {
                    printf("%s", buf);
                }
                printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
                fclose(fp);
            } else {
                printf("No custom startup prompt found.\n");
                printf("Default: %s\n", DEFAULT_STARTUP_PROMPT);
            }
        } else if (strncmp(action, "reset", 5) == 0) {
            if (remove(prompt_file) == 0) {
                printf("Custom startup prompt removed. Will use default.\n");
            } else {
                printf("No custom startup prompt to remove.\n");
            }
        } else {
            printf("Usage: /startup <edit|show|reset>\n");
            printf("  edit  - Edit custom startup prompt (uses $EDITOR or nano)\n");
            printf("  show  - Display current startup prompt\n");
            printf("  reset - Remove custom prompt and use default\n");
        }
        return;
    }
    
    if (strcmp(line, "/paste") == 0) {
        printf("Entering paste mode...\n");
        char* pasted = read_paste_input();
        if (!pasted || pasted[0] == '\0') {
            printf("Paste mode cancelled (empty input).\n");
            if (pasted) free(pasted);
            return;
        }
        // Recursively process the pasted content as a command
        process_command(pasted, memory, governor, quit_flag);
        free(pasted);
        return;
    }
    
    // Not a command - treat as user message
    printf("\n");
    
    // Store user message
    store_message(memory, line, true, 0.8f);
    
    // Suppress llama.cpp/ggml stderr logs in quiet mode
    int stderr_backup = -1;
    if (g_quiet_mode) {
        stderr_backup = dup(STDERR_FILENO);
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null != -1) {
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }
    
    // Send to Governor for processing
    char* response = NULL;
    char* error = NULL;
    ethervox_confidence_metrics_t metrics;
    
    ethervox_governor_status_t status = ethervox_governor_execute(
        governor,
        line,
        &response,
        &error,
        &metrics,
        NULL,  // progress_callback
        NULL,  // token_callback
        NULL   // user_data
    );
    
    // Restore stderr if we redirected it
    if (g_quiet_mode && stderr_backup != -1) {
        fflush(stderr);  // Flush any pending stderr output before restoring
        dup2(stderr_backup, STDERR_FILENO);
        close(stderr_backup);
    }
    
    // Now apply cyan color to the response output
    printf("\033[36m");  // Cyan color for assistant response
    fflush(stdout);
    
    if (status == ETHERVOX_GOVERNOR_SUCCESS && response) {
        // Apply markdown formatting if enabled
        if (g_markdown_enabled) {
            char* formatted = markdown_to_ansi(response);
            if (formatted) {
                printf("\033[36m%s\033[0m\n\n", formatted);
                free(formatted);
            } else {
                printf("%s\033[0m\n\n", response);
            }
        } else {
            printf("%s\033[0m\n\n", response);
        }
        
        // Store assistant response
        store_message(memory, response, false, 0.8f);
        
        free(response);
    } else if (status == ETHERVOX_GOVERNOR_ERROR) {
        printf("\033[0m");  // Reset color before error
        if (error) {
            printf("[Error: %s]\n\n", error);
            free(error);
        } else {
            printf("[Error: Governor execution failed]\n\n");
        }
    } else {
        printf("\033[0m");  // Reset color before status message
        printf("[Status: %d - Response may be incomplete]\n\n", status);
        if (response) free(response);
        if (error) free(error);
    }
}

int main(int argc, char** argv) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Set default log level to OFF (quiet mode by default)
    g_ethervox_debug_enabled = 0;
    ethervox_log_set_level(ETHERVOX_LOG_LEVEL_OFF);
    
    print_banner();
    
    // Parse command-line arguments
    const char* model_path = NULL;
    const char* memory_dir = NULL;
    const char* startup_prompt_file = NULL;
    bool interactive = true;
    
    bool run_tests = false;
    bool quiet_mode = true;  // Default to quiet mode
    bool no_persist = false;
    bool skip_startup_prompt = false;

    // Dafualt to auto-load model unless --noautoload specified
    bool auto_load_model = true;
    if (!model_path) {
        model_path = "models/granite-4.0-h-tiny-Q4_K_M.gguf";  // Default model path
    }
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            memory_dir = argv[++i];
        } else if (strcmp(argv[i], "--no-persist") == 0) {
            no_persist = true;
            memory_dir = NULL; // Memory-only mode
        } else if (strcmp(argv[i], "--noautoload") == 0) {
            auto_load_model = false;
            if (model_path) {
                model_path = NULL;  // Default model path
            }
        } else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            quiet_mode = false;
            g_quiet_mode = false;
            g_debug_enabled = true;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            quiet_mode = true;
            g_quiet_mode = true;  // Set global flag
            g_debug_enabled = false;
        } else if (strcmp(argv[i], "--startup-prompt") == 0 && i + 1 < argc) {
            startup_prompt_file = argv[++i];
        } else if (strcmp(argv[i], "--no-startup-prompt") == 0) {
            skip_startup_prompt = true;
        } else if (strcmp(argv[i], "--test") == 0) {
            run_tests = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --model <path>     Path to Governor GGUF model\n");
            printf("  --noautoload       Suppress auto-loading default Governor model on startup\n");
            printf("  --memory <dir>     Directory for memory persistence\n");
            printf("  --no-persist       Run in memory-only mode (no files)\n");
            printf("  --startup-prompt <file>  Custom startup prompt file\n");
            printf("  --no-startup-prompt      Skip automatic startup prompt\n");
            printf("  --debug, -d        Enable debug logging (default: off)\n");
            printf("  --quiet, -q        Disable debug logging (explicit quiet)\n");
            printf("  --test             Run component tests before starting\n");
            printf("  --help, -h         Show this help message\n");
            printf("\n");
            return 0;
        }
    }
    
    // Set initial log level based on quiet mode
    if (!quiet_mode) {
        if (g_debug_enabled) {
            ethervox_log_set_level(ETHERVOX_LOG_LEVEL_DEBUG);
        } else {
            ethervox_log_set_level(ETHERVOX_LOG_LEVEL_INFO);
        }
        g_ethervox_debug_enabled = 1;  // Also enable legacy debug flag
    } else {
        ethervox_log_set_level(ETHERVOX_LOG_LEVEL_OFF);
        g_ethervox_debug_enabled = 0;
    }
    
    // Default memory directory
    if (memory_dir == NULL && interactive && !no_persist) {
        memory_dir = "./memory_data";
    }
    
    // Run component tests if requested
    if (run_tests) {
        run_component_tests(memory_dir ? memory_dir : "/tmp");
        if (!interactive) {
            return 0;
        }
    }
    
    // Initialize platform
    ethervox_platform_t platform = {0};
    int result = ethervox_platform_register_hal(&platform);
    
    // Initialize memory store
    ethervox_memory_store_t memory;
    if (ethervox_memory_init(&memory, NULL, memory_dir) != 0) {
        fprintf(stderr, "Failed to initialize memory store\n");
        return 1;
    }
    
    if (memory_dir) {
        printf("Memory: Persistent storage at %s\n", memory.storage_filepath);
        
        // Use platform-agnostic function to load previous session
        // This handles all the complexity of finding the most recent file,
        // preserving tags, IDs, and adding the "imported" tag
        uint32_t turns_loaded = 0;
        if (ethervox_memory_load_previous_session(&memory, &turns_loaded) == 0 && turns_loaded > 0) {
            if (!quiet_mode) {
                printf("Memory: Loaded %u previous memories from last session\n", turns_loaded);
            }
        }
    } else {
        printf("Memory: In-memory only (no persistence)\n");
    }
    
    // Initialize Governor
    ethervox_governor_t* governor = NULL;
    ethervox_tool_registry_t registry;
    
    if (ethervox_tool_registry_init(&registry, 16) != 0) {
        fprintf(stderr, "Failed to initialize tool registry\n");
        ethervox_memory_cleanup(&memory);
        return 1;
    }
    
    if (ethervox_governor_init(&governor, NULL, &registry) != 0) {
        fprintf(stderr, "Failed to initialize Governor\n");
        ethervox_tool_registry_cleanup(&registry);
        ethervox_memory_cleanup(&memory);
        return 1;
    }
    
    printf("Governor: Initialized\n");
    
    // Register compute tools with Governor
    int compute_count = ethervox_compute_tools_register_all(&registry);
    if (compute_count > 0) {
        printf("Compute Tools: Registered %d tools with Governor\n", compute_count);
    }
    
    // Register memory tools with Governor
    if (ethervox_memory_tools_register(&registry, &memory) == 0) {
        printf("Memory Tools: Registered with Governor\n");
    }
    
    // Initialize and register file tools (read-only by default)
    ethervox_file_tools_config_t file_config;
    
    // Set allowed base paths for file access
    const char* home_dir = getenv("HOME");
    const char* base_paths[4] = {NULL, NULL, NULL, NULL};
    int path_count = 0;
    
    if (home_dir) {
        // Allow home directory
        base_paths[path_count++] = home_dir;
        
        // Allow common document paths
        char docs_path[512];
        snprintf(docs_path, sizeof(docs_path), "%s/Documents", home_dir);
        base_paths[path_count++] = strdup(docs_path);  // Needs to persist
    }
    
    // Allow current directory
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        base_paths[path_count++] = strdup(cwd);  // Needs to persist
    }
    
    if (ethervox_file_tools_init(&file_config, base_paths, ETHERVOX_FILE_ACCESS_READ_WRITE) == 0) {
        // Add allowed file extensions
        ethervox_file_tools_add_filter(&file_config, ".txt");
        ethervox_file_tools_add_filter(&file_config, ".md");
        ethervox_file_tools_add_filter(&file_config, ".org");
        ethervox_file_tools_add_filter(&file_config, ".c");
        ethervox_file_tools_add_filter(&file_config, ".cpp");
        ethervox_file_tools_add_filter(&file_config, ".h");
        ethervox_file_tools_add_filter(&file_config, ".sh");
        
        // Register with Governor
        if (ethervox_file_tools_register(&registry, &file_config) == 0) {
            printf("File Tools: Registered with Governor (read-write: .txt/.md/.org/.c/.cpp/.h/.sh)\n");
        }
    }
    
    // Auto-load model if requested
    if (auto_load_model && model_path) {
        if (!quiet_mode) {
            printf("\n[INFO] [Governor] Auto-loading model: %s\n", model_path);
        }
        
        // Redirect stderr to suppress llama.cpp/ggml Metal compilation logs in quiet mode
        int stderr_backup = -1;
        if (quiet_mode) {
            stderr_backup = dup(STDERR_FILENO);
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null != -1) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
        }
        
        // Resolve model path (make it absolute if needed)
        char resolved_path[1024];
        if (model_path[0] != '/') {
            // Relative path - try multiple locations in order:
            // 1. Same directory as executable
            // 2. One level up from executable (if in build/)
            // 3. Current working directory
            
            char exe_path[512];
            char exe_dir[512];
            bool path_found = false;
            ssize_t len = -1;
            
#ifdef __APPLE__
            // macOS uses _NSGetExecutablePath
            uint32_t size = sizeof(exe_path);
            if (_NSGetExecutablePath(exe_path, &size) == 0) {
                len = strlen(exe_path);
            }
#else
            len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
#endif
            
            if (len > 0) {
                exe_path[len] = '\0';
                char* last_slash = strrchr(exe_path, '/');
                if (last_slash) {
                    size_t dir_len = last_slash - exe_path;
                    strncpy(exe_dir, exe_path, dir_len);
                    exe_dir[dir_len] = '\0';
                    
                    // Try 1: Same directory as executable
                    snprintf(resolved_path, sizeof(resolved_path), "%s/%s", exe_dir, model_path);
                    if (access(resolved_path, R_OK) == 0) {
                        path_found = true;
                    }
                    
                    // Try 2: One level up (project root from build/)
                    if (!path_found) {
                        char parent_dir[512];
                        strncpy(parent_dir, exe_dir, sizeof(parent_dir) - 1);
                        parent_dir[sizeof(parent_dir) - 1] = '\0';
                        
                        char* second_last = strrchr(parent_dir, '/');
                        if (second_last) {
                            *second_last = '\0';
                            snprintf(resolved_path, sizeof(resolved_path), "%s/%s", parent_dir, model_path);
                            if (access(resolved_path, R_OK) == 0) {
                                path_found = true;
                            }
                        }
                    }
                }
            }
            
            // Try 3: Current working directory
            if (!path_found) {
                char cwd[512];
                if (getcwd(cwd, sizeof(cwd))) {
                    snprintf(resolved_path, sizeof(resolved_path), "%s/%s", cwd, model_path);
                } else {
                    snprintf(resolved_path, sizeof(resolved_path), "%s", model_path);
                }
            }
        } else {
            snprintf(resolved_path, sizeof(resolved_path), "%s", model_path);
        }
        
        // Check if file exists
        if (access(resolved_path, R_OK) != 0) {
            fprintf(stderr, "[ERROR] Model file not found or not readable: %s\n", resolved_path);
            fprintf(stderr, "[INFO] Run './scripts/download-governor-model.sh' to download it\n");
        } else {
            int ret = ethervox_governor_load_model(governor, resolved_path);
            if (ret == 0) {
                snprintf(g_loaded_model_path, sizeof(g_loaded_model_path), "%s", resolved_path);
                if (!quiet_mode) {
                    printf("[INFO] ✓ Model loaded successfully\n");
                }
            } else {
                fprintf(stderr, "[ERROR] ✗ Failed to load model (code: %d)\n", ret);
            }
        }
        
        // Restore stderr if we redirected it
        if (quiet_mode && stderr_backup != -1) {
            dup2(stderr_backup, STDERR_FILENO);
            close(stderr_backup);
        }
        
        printf("\n");
    } else if (model_path) {
        printf("\nModel available: %s\n", model_path);
        printf("Use /load %s to load it, or restart with --load <path>\n\n", model_path);
    } else {
        printf("\nNo model specified. Use /load <path> or --load <path> flag.\n");
        printf("Recommended: models/granite-4.0-h-tiny-Q4_K_M.gguf\n\n");
    }
    
    print_help();
    
    // Execute startup prompt if model is loaded and not skipped
    if (!skip_startup_prompt && g_loaded_model_path[0] != '\0') {
        char startup_prompt[2048];
        bool has_custom_prompt = false;
        
        // Determine startup prompt file location
        const char* home_dir = getenv("HOME");
        char default_prompt_file[512];
        if (home_dir) {
            snprintf(default_prompt_file, sizeof(default_prompt_file), "%s/.ethervox/startup_prompt.txt", home_dir);
        } else {
            snprintf(default_prompt_file, sizeof(default_prompt_file), "./.ethervox_startup_prompt.txt");
        }
        
        const char* prompt_file_to_use = startup_prompt_file ? startup_prompt_file : default_prompt_file;
        
        // Try to load custom startup prompt from file
        FILE* fp = fopen(prompt_file_to_use, "r");
        if (fp) {
            size_t len = fread(startup_prompt, 1, sizeof(startup_prompt) - 1, fp);
            startup_prompt[len] = '\0';
            fclose(fp);
            has_custom_prompt = true;
            if (!quiet_mode) {
                printf("\n[INFO] Using custom startup prompt from: %s\n", prompt_file_to_use);
            }
        } else if (startup_prompt_file) {
            // Only warn if a custom file was explicitly specified but not found
            fprintf(stderr, "[WARN] Could not open startup prompt file: %s\n", startup_prompt_file);
        }
        
        // Use default startup prompt if no custom one loaded
        if (!has_custom_prompt) {
            snprintf(startup_prompt, sizeof(startup_prompt), "%s", DEFAULT_STARTUP_PROMPT);
        }
        
        if (!quiet_mode) {
            printf("\n[Executing startup prompt...]\n");
        }
        
        // Redirect stderr to suppress llama.cpp Metal shader compilation logs
        int stderr_backup = -1;
        stderr_backup = dup(STDERR_FILENO);
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null != -1) {
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
        
        // Execute the startup prompt silently
        char* response = NULL;
        char* error = NULL;
        
        int status = ethervox_governor_execute(
            governor,
            startup_prompt,
            &response,
            &error,
            NULL,  // No metrics
            NULL,  // No progress callback
            NULL,  // No token callback
            NULL   // No user data
        );
        
        // Restore stderr
        if (stderr_backup != -1) {
            dup2(stderr_backup, STDERR_FILENO);
            close(stderr_backup);
        }
        
        if (status == 0 && response) {
            // Display the startup response
            printf("\n\033[36m%s\033[0m\n\n", response);
            
            // Store the startup exchange in memory
            const char* tags[] = {"startup", "auto_generated"};
            uint64_t user_id, assistant_id;
            ethervox_memory_store_add(&memory, startup_prompt, tags, 2, 0.7f, true, &user_id);
            ethervox_memory_store_add(&memory, response, tags, 2, 0.7f, false, &assistant_id);
            
            free(response);
        } else if (error) {
            if (!quiet_mode) {
                fprintf(stderr, "[WARN] Startup prompt failed: %s\n\n", error);
            }
            free(error);
        }
    }
    
    // Main REPL loop
    bool quit = false;
    
#if defined(__APPLE__) || defined(__linux__)
    // Use readline for command history on macOS/Linux
    while (g_running && !quit) {
        char* line = readline("> ");
        
        if (!line) {
            break;  // EOF (Ctrl+D)
        }
        
        // Add non-empty lines to history
        if (line[0] != '\0') {
            add_history(line);
        }
        
        process_command(line, &memory, governor, &quit);
        free(line);
    }
#else
    // Fallback for platforms without readline
    char line[2048];
    while (g_running && !quit) {
        printf("> ");
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        process_command(line, &memory, governor, &quit);
    }
#endif
    
    // Cleanup
    printf("\n━━━ Final Statistics ━━━\n");
    print_stats(&memory);
    
    // Generate summary of this session before closing (if we have entries and storage)
    if (memory.entry_count > 2 && memory.storage_filepath[0] != '\0') {
        if (!quiet_mode) {
            printf("\nGenerating session summary...\n");
        }
        
        char* summary = NULL;
        char** key_points = NULL;
        uint32_t kp_count = 0;
        
        // Summarize the entire session
        int ret = ethervox_memory_summarize(&memory, memory.entry_count, NULL,
                                           &summary, &key_points, &kp_count);
        
        if (ret == 0 && summary) {
            // Store summary as a special memory entry for the next session
            const char* tags[] = {"session_summary", "auto_generated"};
            uint64_t summary_id;
            
            ethervox_memory_store_add(&memory, summary, tags, 2, 0.95f, false, &summary_id);
            
            if (!quiet_mode) {
                printf("Session summary saved to memory.\n");
            }
            
            free(summary);
            if (key_points) {
                for (uint32_t i = 0; i < kp_count; i++) {
                    free(key_points[i]);
                }
                free(key_points);
            }
        }
    }
    
    printf("Cleaning up...\n");
    
    // Suppress llama.cpp cleanup logs in quiet mode
    int stderr_backup = -1;
    if (g_quiet_mode) {
        stderr_backup = dup(STDERR_FILENO);
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null != -1) {
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }
    
    ethervox_governor_cleanup(governor);
    ethervox_tool_registry_cleanup(&registry);
    ethervox_memory_cleanup(&memory);
    
    // Restore stderr if we redirected it
    if (g_quiet_mode && stderr_backup != -1) {
        dup2(stderr_backup, STDERR_FILENO);
        close(stderr_backup);
    }
    
    printf("Goodbye!\n\n");
    
    return 0;
}