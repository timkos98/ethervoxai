/**
 * @file settings_menu.c
 * @brief Terminal-based settings menu implementation
 *
 * Cross-platform TUI settings interface using ncurses (macOS/Linux) or PDCurses (Windows).
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/settings_menu.h"
#include "ethervox/config.h"
#include "ethervox/settings.h"
#include "ethervox/logging.h"
#include "ethervox/governor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Check if ncurses is available
#ifdef __ANDROID__
    // Android: ncurses not available
    #define HAVE_NCURSES 0
#elif defined(__APPLE__)
    #define HAVE_NCURSES 1
    #include <ncurses.h>
#elif defined(__linux__)
    #define HAVE_NCURSES 1
    #include <ncurses.h>
#elif defined(_WIN32)
    // Windows: would use PDCurses if available
    #define HAVE_NCURSES 0
#else
    #define HAVE_NCURSES 0
#endif

#if HAVE_NCURSES

// Menu item types
typedef enum {
    MENU_ITEM_TOGGLE,      // Boolean on/off
    MENU_ITEM_NUMERIC,     // Integer numeric value
    MENU_ITEM_FLOAT,       // Float numeric value
    MENU_ITEM_ACTION,      // Execute action
    MENU_ITEM_SUBMENU,     // Open submenu
    MENU_ITEM_BACK         // Go back
} menu_item_type_t;

// Menu item definition
typedef struct {
    const char* label;
    const char* description;
    menu_item_type_t type;
    void* value_ptr;       // Pointer to value for toggles/numerics
    int (*action)(void*);  // Action callback
    void* action_data;     // Data for action
} menu_item_t;

// Colors
#define COLOR_HEADER    1
#define COLOR_SELECTED  2
#define COLOR_NORMAL    3
#define COLOR_DISABLED  4

static WINDOW* main_win = NULL;
static WINDOW* status_win = NULL;

// Initialize ncurses display
static int init_display(void) {
    main_win = initscr();
    if (!main_win) {
        return -1;
    }
    
    cbreak();              // Disable line buffering
    noecho();              // Don't echo keypresses
    keypad(main_win, TRUE); // Enable arrow keys
    curs_set(0);           // Hide cursor
    
    // Initialize colors if available
    if (has_colors()) {
        start_color();
        init_pair(COLOR_HEADER, COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_NORMAL, COLOR_WHITE, COLOR_BLACK);
        init_pair(COLOR_DISABLED, COLOR_BLACK, COLOR_BLACK);
    }
    
    return 0;
}

// Cleanup ncurses
static void cleanup_display(void) {
    if (status_win) {
        delwin(status_win);
        status_win = NULL;
    }
    if (main_win) {
        endwin();
        main_win = NULL;
    }
}

// Draw header
static void draw_header(const char* title) {
    int width = getmaxx(main_win);
    
    attron(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
    mvprintw(0, 0, "╔");
    for (int i = 1; i < width - 1; i++) printw("═");
    printw("╗");
    
    int title_pos = (width - strlen(title)) / 2;
    mvprintw(1, title_pos, "%s", title);
    
    mvprintw(2, 0, "╚");
    for (int i = 1; i < width - 1; i++) printw("═");
    printw("╝");
    attroff(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
}

// Draw footer with help
static void draw_footer(void) {
    int height = getmaxy(main_win);
    int width = getmaxx(main_win);
    
    attron(COLOR_PAIR(COLOR_HEADER));
    mvprintw(height - 2, 2, "↑↓: Navigate  ←→: Change  Enter: Select  Q: Quit");
    attroff(COLOR_PAIR(COLOR_HEADER));
}

// Draw menu items
static void draw_menu(menu_item_t* items, int item_count, int selected, int scroll_offset) {
    int start_y = 4;
    int height = getmaxy(main_win);
    int visible_items = (height - 8) / 2;  // Account for header/footer/status
    
    for (int i = scroll_offset; i < item_count && i < scroll_offset + visible_items; i++) {
        int y = start_y + ((i - scroll_offset) * 2);
        
        // Check if this is a section header (starts with box drawing character)
        bool is_header = (strncmp(items[i].label, "─", 3) == 0);
        
        if (is_header) {
            // Draw section header with special styling
            attron(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
            move(y, 2);
            clrtoeol();
            mvprintw(y, 4, "%s", items[i].label);
            attroff(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
            continue;
        }
        
        if (i == selected) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(COLOR_NORMAL));
        }
        
        // Clear line
        move(y, 2);
        clrtoeol();
        
        // Draw item
        mvprintw(y, 4, "%s", items[i].label);
        
        // Draw value based on type
        if (items[i].type == MENU_ITEM_TOGGLE && items[i].value_ptr) {
            bool* val = (bool*)items[i].value_ptr;
            mvprintw(y, 40, "[%s]", *val ? "ON " : "OFF");
        } else if (items[i].type == MENU_ITEM_NUMERIC && items[i].value_ptr) {
            uint32_t* val = (uint32_t*)items[i].value_ptr;
            // Show signed values properly (for n_threads = -1)
            if (*val == (uint32_t)-1) {
                mvprintw(y, 40, "[auto]");
            } else if (*val > 1000000) {
                mvprintw(y, 40, "[%uk]", *val / 1024);
            } else {
                mvprintw(y, 40, "[%u]", *val);
            }
        } else if (items[i].type == MENU_ITEM_FLOAT && items[i].value_ptr) {
            float* val = (float*)items[i].value_ptr;
            mvprintw(y, 40, "[%.2f]", *val);
        } else if (items[i].type == MENU_ITEM_ACTION && items[i].action) {
            mvprintw(y, 40, "→");
        }
        
        if (i == selected) {
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(COLOR_NORMAL));
        }
        
        // Draw description
        if (items[i].description && items[i].description[0]) {
            attron(COLOR_PAIR(COLOR_NORMAL) | A_DIM);
            mvprintw(y + 1, 6, "%s", items[i].description);
            attroff(COLOR_PAIR(COLOR_NORMAL) | A_DIM);
        }
    }
}

// Status message
static void show_status(const char* message) {
    int height = getmaxy(main_win);
    attron(COLOR_PAIR(COLOR_HEADER));
    mvprintw(height - 4, 2, "Status: %-60s", message);
    attroff(COLOR_PAIR(COLOR_HEADER));
    refresh();
}

// Action: Export configuration
static int action_export_config(void* data) {
    cleanup_display();
    
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                    EXPORT CONFIGURATION                       ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    // Load current settings
    ethervox_persistent_settings_t settings = ethervox_settings_get_defaults();
    if (ethervox_settings_load(&settings, NULL) != 0) {
        printf("Warning: Could not load current settings, using defaults\n\n");
    }
    
    // Prompt for export path
    char export_path[512];
    printf("Enter export path (or press Enter for default 'ethervox_config.json'): ");
    if (fgets(export_path, sizeof(export_path), stdin)) {
        // Remove newline
        export_path[strcspn(export_path, "\n")] = 0;
        
        if (export_path[0] == '\0') {
            strncpy(export_path, "ethervox_config.json", sizeof(export_path) - 1);
        }
        
        // Export to JSON string
        char* json_string = ethervox_settings_export(&settings);
        if (json_string) {
            // Write to file
            FILE* f = fopen(export_path, "w");
            if (f) {
                fprintf(f, "%s", json_string);
                fclose(f);
                printf("\n✓ Configuration exported to: %s\n", export_path);
            } else {
                printf("\n✗ Failed to write to file: %s\n", export_path);
            }
            free(json_string);
        } else {
            printf("\n✗ Failed to export configuration\n");
        }
    }
    
    printf("\nPress Enter to return...\n");
    getchar();
    
    init_display();
    return 0;
}

// Action: Import configuration
static int action_import_config(void* data) {
    cleanup_display();
    
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                    IMPORT CONFIGURATION                       ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    printf("⚠️  WARNING: This will overwrite your current settings!\n\n");
    
    // Prompt for import path
    char import_path[512];
    printf("Enter import path (or press Enter to cancel): ");
    if (fgets(import_path, sizeof(import_path), stdin)) {
        // Remove newline
        import_path[strcspn(import_path, "\n")] = 0;
        
        if (import_path[0] != '\0') {
            // Load from specified path
            ethervox_persistent_settings_t settings;
            if (ethervox_settings_load(&settings, import_path) == 0) {
                // Save to default location
                if (ethervox_settings_save(&settings, NULL) == 0) {
                    printf("\n✓ Configuration imported from: %s\n", import_path);
                    printf("✓ Saved to: %s\n", ethervox_settings_get_default_path());
                    printf("\nRestart the application for changes to take effect.\n");
                } else {
                    printf("\n✗ Failed to save imported configuration\n");
                }
            } else {
                printf("\n✗ Failed to load configuration from: %s\n", import_path);
            }
        } else {
            printf("\nImport cancelled.\n");
        }
    }
    
    printf("\nPress Enter to return...\n");
    getchar();
    
    init_display();
    return 0;
}

// Action: Run tool optimization
static int action_optimize_tools(void* data) {
    ethervox_settings_t* settings = (ethervox_settings_t*)data;
    
    cleanup_display();
    
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║          TOOL PROMPT OPTIMIZATION ROUTINE                     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    printf("This will optimize tool prompts for your model.\n");
    printf("This may take several minutes...\n\n");
    printf("Press Ctrl+C to cancel, or Enter to continue...\n");
    getchar();
    
    // TODO: Call ethervox_optimize_tool_prompts() here when Governor is available
    printf("\nOptimization would run here (needs Governor integration)\n");
    printf("Press Enter to return to settings...\n");
    getchar();
    
    init_display();
    return 0;
}

// Action: View system info
static int action_view_info(void* data) {
    ethervox_settings_t* settings = (ethervox_settings_t*)data;
    
    cleanup_display();
    
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                     SYSTEM INFORMATION                        ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    printf("Version:        0.0.6\n");
    printf("Branch:         %s\n", settings->git_branch[0] ? settings->git_branch : "unknown");
    printf("Commit:         %s\n\n", settings->git_commit[0] ? settings->git_commit : "unknown");
    
    printf("Governor Model: %s\n", settings->model_path);
    printf("Whisper Model:  %s\n", settings->whisper_model_path[0] ? settings->whisper_model_path : "(not loaded)");
    printf("Memory Dir:     %s\n\n", settings->memory_dir);
    
    printf("Audio Device:   %s\n", settings->audio_device[0] ? settings->audio_device : "(default)");
    printf("Wake Word:      %s [%s]\n\n", 
           settings->wake_word[0] ? settings->wake_word : "(not set)",
           settings->wake_word_enabled ? "ENABLED" : "DISABLED");
    
    printf("Log Level:      %d\n", settings->log_level);
    printf("Debug:          %s\n", settings->debug_enabled ? "ON" : "OFF");
    printf("Engineering:    %s\n", settings->engineering_mode ? "ON" : "OFF");
    printf("Quiet Mode:     %s\n", settings->quiet_mode ? "ON" : "OFF");
    
    printf("\nPress Enter to return...\n");
    getchar();
    
    init_display();
    return 0;
}

// Main settings menu
int ethervox_settings_menu_show(ethervox_settings_t* settings, const char* model_path,
                                 ethervox_model_reload_callback_t reload_callback, void* user_data) {
    if (!settings) return -1;
    
    if (init_display() != 0) {
        fprintf(stderr, "Failed to initialize ncurses\n");
        return -1;
    }
    
    // Load persistent settings
    ethervox_persistent_settings_t persistent = ethervox_settings_get_defaults();
    ethervox_settings_load(&persistent, NULL);
    
    // Store initial values of parameters that require model reload
    uint32_t initial_gov_gpu_layers = persistent.governor.gpu_layers;
    uint32_t initial_gov_context_size = persistent.governor.context_size;
    int initial_gov_n_threads = persistent.governor.n_threads;
    uint32_t initial_llm_gpu_layers = persistent.llm.gpu_layers;
    uint32_t initial_llm_context = persistent.llm.context_length;
    int initial_llm_threads = persistent.llm.n_threads;
    
    // Define menu items
    menu_item_t items[] = {
        {
            .label = "─── General ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Debug Mode",
            .description = "Enable detailed logging and debug output",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &settings->debug_enabled,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Quiet Mode",
            .description = "Suppress non-essential output",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &settings->quiet_mode,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Token Streaming",
            .description = "Display LLM response as tokens are generated (real-time)",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &settings->streaming_enabled,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Engineering Mode",
            .description = "Enable advanced features and detailed stats",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &settings->engineering_mode,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Log Level",
            .description = "Logging verbosity (0=TRACE to 6=OFF)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &settings->log_level,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "─── Whisper STT ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Whisper Temperature",
            .description = "Sampling temperature 0.0-1.0 (lower = more deterministic)",
            .type = MENU_ITEM_FLOAT,
            .value_ptr = &persistent.whisper.temperature,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Whisper Beam Size",
            .description = "Beam search width 1-10 (higher = more accurate, slower)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.whisper.beam_size,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Translate to English",
            .description = "Automatically translate input to English",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &persistent.whisper.translate_to_english,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Whisper GPU Acceleration",
            .description = "Use GPU if available (experimental)",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &persistent.whisper.use_gpu,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "─── Conversation ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Filter Hallucinations",
            .description = "Remove known Whisper hallucination patterns",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &persistent.conversation.filter_hallucinations,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "─── LLM (Language Model) ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "LLM Max Tokens",
            .description = "Maximum response length 64-2048 tokens",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.llm.max_tokens,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "LLM Context Length",
            .description = "Context window size 512-8192 tokens (affects memory)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.llm.context_length,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "LLM Temperature",
            .description = "Creativity 0.0-2.0 (0=deterministic, higher=creative)",
            .type = MENU_ITEM_FLOAT,
            .value_ptr = &persistent.llm.temperature,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "LLM Top-P",
            .description = "Nucleus sampling 0.0-1.0 (lower=focused, higher=diverse)",
            .type = MENU_ITEM_FLOAT,
            .value_ptr = &persistent.llm.top_p,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "LLM GPU Layers",
            .description = "Layers offloaded to GPU 0-999 (0=CPU only, 999=all)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.llm.gpu_layers,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "LLM Threads",
            .description = "CPU threads 1-32 (-1=auto-detect)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.llm.n_threads,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "─── Governor (Tool Orchestration) ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Max Iterations",
            .description = "Maximum reasoning loop iterations 1-50",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.governor.max_iterations,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Timeout",
            .description = "Maximum execution time in seconds 10-600",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.governor.timeout_seconds,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Temperature",
            .description = "Tool calling precision 0.0-1.0 (lower=deterministic)",
            .type = MENU_ITEM_FLOAT,
            .value_ptr = &persistent.governor.temperature,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Max Tokens/Iter",
            .description = "Tokens per reasoning step 16-256",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.governor.max_tokens_per_iteration,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Confidence",
            .description = "Minimum confidence to execute tools 0.0-1.0",
            .type = MENU_ITEM_FLOAT,
            .value_ptr = &persistent.governor.confidence_threshold,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor GPU Layers",
            .description = "Layers offloaded to GPU 0-999 (999=all)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.governor.gpu_layers,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Context Size",
            .description = "Context window 2048-16384 tokens",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.governor.context_size,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Governor Threads",
            .description = "CPU threads 1-32 (-1=auto)",
            .type = MENU_ITEM_NUMERIC,
            .value_ptr = &persistent.governor.n_threads,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "─── Wake Word ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Wake Word Detection",
            .description = "Enable voice activation (wake word)",
            .type = MENU_ITEM_TOGGLE,
            .value_ptr = &settings->wake_word_enabled,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Wake Word Threshold",
            .description = "Detection sensitivity 0.0-1.0 (higher = stricter)",
            .type = MENU_ITEM_FLOAT,
            .value_ptr = &persistent.wake_word.detection_threshold,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "─── Actions ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        },
        {
            .label = "Export Configuration",
            .description = "Save current settings to a backup file",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = action_export_config,
            .action_data = settings
        },
        {
            .label = "Import Configuration",
            .description = "Load settings from a backup file (overwrites current)",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = action_import_config,
            .action_data = settings
        },
        {
            .label = "Optimize Tool Prompts",
            .description = "Run LLM-based tool optimization (improves performance)",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = action_optimize_tools,
            .action_data = settings
        },
        {
            .label = "View System Info",
            .description = "Display current configuration and version",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = action_view_info,
            .action_data = settings
        },
        {
            .label = "Exit Settings",
            .description = "Return to main application",
            .type = MENU_ITEM_BACK,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
        }
    };
    
    int item_count = sizeof(items) / sizeof(items[0]);
    int selected = 0;
    int scroll_offset = 0;
    bool done = false;
    
    int height = getmaxy(main_win);
    int visible_items = (height - 8) / 2;  // Account for header/footer/status
    
    while (!done) {
        // Auto-scroll to keep selected item visible
        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + visible_items) {
            scroll_offset = selected - visible_items + 1;
        }
        
        clear();
        draw_header("ETHERVOX SETTINGS");
        draw_menu(items, item_count, selected, scroll_offset);
        draw_footer();
        refresh();
        
        int ch = getch();
        
        switch (ch) {
            case KEY_UP:
                do {
                    selected = (selected - 1 + item_count) % item_count;
                } while (strncmp(items[selected].label, "─", 3) == 0);
                break;
                
            case KEY_DOWN:
                do {
                    selected = (selected + 1) % item_count;
                } while (strncmp(items[selected].label, "─", 3) == 0);
                break;
                
            case KEY_LEFT:
                if (items[selected].type == MENU_ITEM_TOGGLE && items[selected].value_ptr) {
                    bool* val = (bool*)items[selected].value_ptr;
                    *val = false;
                    show_status("Setting disabled");
                } else if (items[selected].type == MENU_ITEM_NUMERIC && items[selected].value_ptr) {
                    uint32_t* val = (uint32_t*)items[selected].value_ptr;
                    // Smart decrement based on value magnitude
                    uint32_t decrement = (*val > 1000) ? 128 : (*val > 100) ? 16 : (*val > 10) ? 1 : 1;
                    if (*val > decrement) {
                        *val -= decrement;
                    } else {
                        *val = 0;
                    }
                    show_status("Value decreased");
                } else if (items[selected].type == MENU_ITEM_FLOAT && items[selected].value_ptr) {
                    float* val = (float*)items[selected].value_ptr;
                    if (*val > 0.0f) {
                        *val -= 0.05f;
                        if (*val < 0.0f) *val = 0.0f;
                        show_status("Value decreased");
                    }
                }
                break;
                
            case KEY_RIGHT:
                if (items[selected].type == MENU_ITEM_TOGGLE && items[selected].value_ptr) {
                    bool* val = (bool*)items[selected].value_ptr;
                    *val = true;
                    show_status("Setting enabled");
                } else if (items[selected].type == MENU_ITEM_NUMERIC && items[selected].value_ptr) {
                    uint32_t* val = (uint32_t*)items[selected].value_ptr;
                    // Smart increment based on value magnitude
                    uint32_t increment = (*val >= 1000) ? 128 : (*val >= 100) ? 16 : (*val >= 10) ? 1 : 1;
                    *val += increment;
                    
                    // Apply field-specific caps
                    if (val == &persistent.llm.gpu_layers || val == &persistent.governor.gpu_layers) {
                        if (*val > 999) *val = 999;  // GPU layers cap
                    } else if (val == &persistent.governor.context_size) {
                        if (*val > 16384) *val = 16384;  // Context size cap
                    } else {
                        if (*val > 16384) *val = 16384;  // Default cap
                    }
                    show_status("Value increased");
                } else if (items[selected].type == MENU_ITEM_FLOAT && items[selected].value_ptr) {
                    float* val = (float*)items[selected].value_ptr;
                    *val += 0.05f;
                    // Cap at reasonable maximum
                    if (*val > 2.0f) *val = 2.0f;
                    show_status("Value increased");
                }
                break;
                
            case 10: // Enter
            case KEY_ENTER:
                if (items[selected].type == MENU_ITEM_TOGGLE && items[selected].value_ptr) {
                    bool* val = (bool*)items[selected].value_ptr;
                    *val = !(*val);
                    show_status(*val ? "Enabled" : "Disabled");
                } else if (items[selected].type == MENU_ITEM_ACTION && items[selected].action) {
                    items[selected].action(items[selected].action_data);
                    show_status("Action completed");
                } else if (items[selected].type == MENU_ITEM_BACK) {
                    done = true;
                }
                break;
                
            case 'q':
            case 'Q':
            case 27: // ESC
                done = true;
                break;
        }
    }
    
    cleanup_display();
    
    // Check if any model reload parameters changed
    bool reload_required = false;
    reload_required |= (persistent.governor.gpu_layers != initial_gov_gpu_layers);
    reload_required |= (persistent.governor.context_size != initial_gov_context_size);
    reload_required |= (persistent.governor.n_threads != initial_gov_n_threads);
    reload_required |= (persistent.llm.gpu_layers != initial_llm_gpu_layers);
    reload_required |= (persistent.llm.context_length != initial_llm_context);
    reload_required |= (persistent.llm.n_threads != initial_llm_threads);
    
    // Save persistent settings to disk
    if (ethervox_settings_save(&persistent, NULL) == 0) {
        printf("\n✓ Settings saved to %s\n\n", ethervox_settings_get_default_path());
    } else {
        printf("\n⚠ Warning: Failed to save settings\n\n");
    }
    
    // Prompt for model reload if needed (only if model is loaded)
    if (reload_required && model_path && model_path[0] != '\0') {
        printf("╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                   MODEL RELOAD REQUIRED                       ║\n");
        printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
        printf("The following settings require reloading the model:\n");
        if (persistent.governor.gpu_layers != initial_gov_gpu_layers) {
            printf("  • Governor GPU Layers: %u → %u\n", initial_gov_gpu_layers, persistent.governor.gpu_layers);
        }
        if (persistent.governor.context_size != initial_gov_context_size) {
            printf("  • Governor Context Size: %u → %u\n", initial_gov_context_size, persistent.governor.context_size);
        }
        if (persistent.governor.n_threads != initial_gov_n_threads) {
            printf("  • Governor Threads: %d → %d\n", initial_gov_n_threads, persistent.governor.n_threads);
        }
        if (persistent.llm.gpu_layers != initial_llm_gpu_layers) {
            printf("  • LLM GPU Layers: %u → %u\n", initial_llm_gpu_layers, persistent.llm.gpu_layers);
        }
        if (persistent.llm.context_length != initial_llm_context) {
            printf("  • LLM Context Length: %u → %u\n", initial_llm_context, persistent.llm.context_length);
        }
        if (persistent.llm.n_threads != initial_llm_threads) {
            printf("  • LLM Threads: %d → %d\n", initial_llm_threads, persistent.llm.n_threads);
        }
        printf("\nCurrent model: %s\n\n", model_path);
        printf("Would you like to reload the model now? (y/n): ");
        fflush(stdout);
        
        char response[10];
        if (fgets(response, sizeof(response), stdin)) {
            if (response[0] == 'y' || response[0] == 'Y') {
                printf("\n🔄 Reloading model...\n");
                
                // Trigger model reload via callback
                if (reload_callback) {
                    int ret = reload_callback(model_path, user_data);
                    if (ret == 0) {
                        printf("✓ Model reloaded successfully with new settings\n\n");
                    } else {
                        printf("✗ Failed to reload model (code: %d)\n", ret);
                        printf("  You can manually reload with: /load %s\n\n", model_path);
                    }
                } else {
                    printf("⚠ No reload callback available\n");
                    printf("  Reload manually with: /load %s\n\n", model_path);
                }
            } else {
                printf("\n⚠ Model not reloaded. New settings will apply on next model load.\n");
                printf("  Reload manually with: /load %s\n\n", model_path);
            }
        }
    }
    
    return 0;
}

bool ethervox_settings_menu_available(void) {
    return true;
}

#else // No ncurses available

int ethervox_settings_menu_show(ethervox_settings_t* settings, const char* model_path,
                                 ethervox_model_reload_callback_t reload_callback, void* user_data) {
    (void)model_path;
    (void)reload_callback;
    (void)user_data;
    fprintf(stderr, "Settings menu not available on this platform (ncurses not found)\n");
    return -1;
}

bool ethervox_settings_menu_available(void) {
    return false;
}

#endif
