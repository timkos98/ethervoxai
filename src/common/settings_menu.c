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
#include "ethervox/logging.h"
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
    MENU_ITEM_NUMERIC,     // Numeric value
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
static void draw_menu(menu_item_t* items, int item_count, int selected) {
    int start_y = 4;
    
    for (int i = 0; i < item_count; i++) {
        int y = start_y + (i * 2);
        
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
            int* val = (int*)items[i].value_ptr;
            mvprintw(y, 40, "[%d]", *val);
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
int ethervox_settings_menu_show(ethervox_settings_t* settings) {
    if (!settings) return -1;
    
    if (init_display() != 0) {
        fprintf(stderr, "Failed to initialize ncurses\n");
        return -1;
    }
    
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
            .label = "─── Audio ───",
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
            .label = "─── Actions ───",
            .description = "",
            .type = MENU_ITEM_ACTION,
            .value_ptr = NULL,
            .action = NULL,
            .action_data = NULL
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
    bool done = false;
    
    while (!done) {
        clear();
        draw_header("ETHERVOX SETTINGS");
        draw_menu(items, item_count, selected);
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
                    int* val = (int*)items[selected].value_ptr;
                    if (*val > 0) (*val)--;
                    show_status("Value decreased");
                }
                break;
                
            case KEY_RIGHT:
                if (items[selected].type == MENU_ITEM_TOGGLE && items[selected].value_ptr) {
                    bool* val = (bool*)items[selected].value_ptr;
                    *val = true;
                    show_status("Setting enabled");
                } else if (items[selected].type == MENU_ITEM_NUMERIC && items[selected].value_ptr) {
                    int* val = (int*)items[selected].value_ptr;
                    if (*val < 6) (*val)++;
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
    
    printf("\n✓ Settings saved\n\n");
    
    return 0;
}

bool ethervox_settings_menu_available(void) {
    return true;
}

#else // No ncurses available

int ethervox_settings_menu_show(ethervox_settings_t* settings) {
    fprintf(stderr, "Settings menu not available on this platform (ncurses not found)\n");
    return -1;
}

bool ethervox_settings_menu_available(void) {
    return false;
}

#endif
