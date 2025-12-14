/**
 * @file settings_menu.h
 * @brief Terminal-based settings menu using ncurses
 *
 * Provides an arrow-key navigable settings interface similar to BIOS/raspi-config.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#ifndef ETHERVOX_SETTINGS_MENU_H
#define ETHERVOX_SETTINGS_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Settings menu context - holds all settings state
 */
typedef struct {
    bool debug_enabled;
    bool quiet_mode;
    bool streaming_enabled;
    bool engineering_mode;
    char model_path[512];
    char whisper_model_path[512];
    char memory_dir[512];
    char audio_device[256];
    char wake_word[64];
    bool wake_word_enabled;
    int log_level;
    char git_commit[64];
    char git_branch[64];
} ethervox_settings_t;

/**
 * Launch the interactive settings menu
 * 
 * @param settings Current settings to display/edit
 * @return 0 on success, -1 on error
 */
int ethervox_settings_menu_show(ethervox_settings_t* settings);

/**
 * Check if ncurses is available on this platform
 * 
 * @return true if settings menu can be used
 */
bool ethervox_settings_menu_available(void);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_SETTINGS_MENU_H
