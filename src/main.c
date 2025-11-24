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
static char g_loaded_model_path[512] = {0};  // Track loaded model path for /reset

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
    printf("  /debug             Toggle debug logging on/off\n");
    printf("  /clear             Clear conversation memory\n");
    printf("  /reset             Reset conversation (reload model)\n");
    printf("  /quit              Exit the program\n");
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
    printf("\033[36m");  // Cyan color for assistant response
    fflush(stdout);
    
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
        dup2(stderr_backup, STDERR_FILENO);
        close(stderr_backup);
    }
    
    if (status == ETHERVOX_GOVERNOR_SUCCESS && response) {
        printf("%s\033[0m\n\n", response);  // Print response and reset color
        
        // Store assistant response
        store_message(memory, response, false, 0.8f);
        
        free(response);
    } else if (status == ETHERVOX_GOVERNOR_ERROR) {
        printf("\033[0m");  // Reset color
        if (error) {
            printf("[Error: %s]\n\n", error);
            free(error);
        } else {
            printf("[Error: Governor execution failed]\n\n");
        }
    } else {
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
    bool interactive = true;
    
    bool auto_load_model = false;
    bool run_tests = false;
    bool quiet_mode = true;  // Default to quiet mode
    bool no_persist = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            memory_dir = argv[++i];
        } else if (strcmp(argv[i], "--no-persist") == 0) {
            no_persist = true;
            memory_dir = NULL; // Memory-only mode
        } else if (strcmp(argv[i], "--govautoload") == 0) {
            auto_load_model = true;
            if (!model_path) {
                model_path = "models/Qwen2.5-3B-Instruct-Q4_K_M.gguf";
            }
        } else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            quiet_mode = false;
            g_quiet_mode = false;
            g_debug_enabled = true;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            quiet_mode = true;
            g_quiet_mode = true;  // Set global flag
            g_debug_enabled = false;
        } else if (strcmp(argv[i], "--test") == 0) {
            run_tests = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --model <path>     Path to Governor GGUF model\n");
            printf("  --govautoload      Auto-load default Governor model on startup\n");
            printf("  --memory <dir>     Directory for memory persistence\n");
            printf("  --no-persist       Run in memory-only mode (no files)\n");
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
        ethervox_log_set_level(ETHERVOX_LOG_LEVEL_INFO);
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
    if (result == 0) {
        printf("Platform: %s\n", platform.info.platform_name);
    }
    
    // Initialize memory store
    ethervox_memory_store_t memory;
    if (ethervox_memory_init(&memory, NULL, memory_dir) != 0) {
        fprintf(stderr, "Failed to initialize memory store\n");
        return 1;
    }
    
    if (memory_dir) {
        printf("Memory: Persistent storage at %s\n", memory.storage_filepath);
        
        // Auto-load most recent session file (excluding current session)
        // This allows continuing from previous conversations
        char latest_session[512] = {0};
        DIR* dir = opendir(memory_dir);
        if (dir) {
            struct dirent* entry;
            time_t latest_time = 0;
            
            // Get basename of current session for comparison
            const char* current_basename = strrchr(memory.storage_filepath, '/');
            if (!current_basename) current_basename = memory.storage_filepath;
            else current_basename++;  // Skip the /
            
            while ((entry = readdir(dir)) != NULL) {
                // Look for .jsonl files
                size_t len = strlen(entry->d_name);
                if (len > 6 && strcmp(entry->d_name + len - 6, ".jsonl") == 0) {
                    // Skip current session file by name
                    if (strcmp(entry->d_name, current_basename) == 0) {
                        continue;
                    }
                    
                    char fullpath[512];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", memory_dir, entry->d_name);
                    
                    struct stat st;
                    if (stat(fullpath, &st) == 0 && st.st_mtime > latest_time) {
                        latest_time = st.st_mtime;
                        snprintf(latest_session, sizeof(latest_session), "%s", fullpath);
                    }
                }
            }
            closedir(dir);
            
            // Load the latest previous session if found
            if (latest_session[0] != '\0') {
                uint32_t turns_loaded = 0;
                if (ethervox_memory_import(&memory, latest_session, &turns_loaded) == 0 && turns_loaded > 0) {
                    if (!quiet_mode) {
                        printf("Memory: Loaded %u previous memories from %s\n", turns_loaded, latest_session);
                    }
                }
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
    
    if (ethervox_file_tools_init(&file_config, base_paths, ETHERVOX_FILE_ACCESS_READ_ONLY) == 0) {
        // Add allowed file extensions
        ethervox_file_tools_add_filter(&file_config, ".txt");
        ethervox_file_tools_add_filter(&file_config, ".md");
        ethervox_file_tools_add_filter(&file_config, ".org");
        
        // Register with Governor
        if (ethervox_file_tools_register(&registry, &file_config) == 0) {
            printf("File Tools: Registered with Governor (read-only: .txt/.md/.org)\n");
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
        printf("Use /load %s to load it, or restart with --govautoload\n\n", model_path);
    } else {
        printf("\nNo model specified. Use /load <path> or --govautoload flag.\n");
        printf("Recommended: models/Qwen2.5-3B-Instruct-Q4_K_M.gguf\n\n");
    }
    
    print_help();
    
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