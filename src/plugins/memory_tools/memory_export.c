/**
 * @file memory_export.c
 * @brief Memory export/import in JSON and Markdown formats
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/memory_tools.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Format timestamp for display
static void format_timestamp(time_t timestamp, char* buf, size_t len) {
    struct tm* tm_info = localtime(&timestamp);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

// Write JSON-escaped string
static void write_json_escaped(FILE* fp, const char* str) {
    fputc('"', fp);
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:   fputc(*p, fp); break;
        }
    }
    fputc('"', fp);
}

int ethervox_memory_export(
    ethervox_memory_store_t* store,
    const char* filepath,
    const char* format,
    uint64_t* bytes_written
) {
    if (!store || !store->is_initialized || !filepath || !format) {
        return -1;
    }
    
    FILE* fp = fopen(filepath, "w");
    if (!fp) {
        ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                    "Failed to open export file: %s", filepath);
        return -1;
    }
    
    uint64_t bytes = 0;
    
    if (strcmp(format, "json") == 0) {
        // Export as JSON
        fprintf(fp, "{\n");
        fprintf(fp, "  \"session_id\": \"%s\",\n", store->session_id);
        fprintf(fp, "  \"session_started\": %ld,\n", store->session_started);
        fprintf(fp, "  \"total_turns\": %llu,\n", (unsigned long long)store->current_turn_id);
        fprintf(fp, "  \"entry_count\": %u,\n", store->entry_count);
        fprintf(fp, "  \"entries\": [\n");
        
        for (uint32_t i = 0; i < store->entry_count; i++) {
            ethervox_memory_entry_t* entry = &store->entries[i];
            
            fprintf(fp, "    {\n");
            fprintf(fp, "      \"memory_id\": %llu,\n", (unsigned long long)entry->memory_id);
            fprintf(fp, "      \"turn_id\": %llu,\n", (unsigned long long)entry->turn_id);
            fprintf(fp, "      \"timestamp\": %ld,\n", entry->timestamp);
            fprintf(fp, "      \"is_user\": %s,\n", entry->is_user_message ? "true" : "false");
            fprintf(fp, "      \"importance\": %.2f,\n", entry->importance);
            fprintf(fp, "      \"text\": ");
            write_json_escaped(fp, entry->text);
            fprintf(fp, ",\n");
            
            fprintf(fp, "      \"tags\": [");
            for (uint32_t t = 0; t < entry->tag_count; t++) {
                fprintf(fp, "%s\"%s\"", t > 0 ? ", " : "", entry->tags[t]);
            }
            fprintf(fp, "]");
            
            if (entry->tools_called_count > 0) {
                fprintf(fp, ",\n      \"tools_called\": [");
                for (uint32_t t = 0; t < entry->tools_called_count; t++) {
                    fprintf(fp, "%s\"%s\"", t > 0 ? ", " : "", entry->tools_called[t]);
                }
                fprintf(fp, "]");
            }
            
            fprintf(fp, "\n    }%s\n", i < store->entry_count - 1 ? "," : "");
        }
        
        fprintf(fp, "  ],\n");
        fprintf(fp, "  \"statistics\": {\n");
        fprintf(fp, "    \"total_memories\": %llu,\n", (unsigned long long)store->total_memories_stored);
        fprintf(fp, "    \"total_searches\": %llu,\n", (unsigned long long)store->total_searches);
        fprintf(fp, "    \"total_exports\": %llu\n", (unsigned long long)store->total_exports);
        fprintf(fp, "  }\n");
        fprintf(fp, "}\n");
        
    } else if (strcmp(format, "markdown") == 0) {
        // Export as Markdown
        char time_buf[64];
        format_timestamp(store->session_started, time_buf, sizeof(time_buf));
        
        fprintf(fp, "# Conversation Memory Export\n\n");
        fprintf(fp, "**Session ID:** %s  \n", store->session_id);
        fprintf(fp, "**Started:** %s  \n", time_buf);
        fprintf(fp, "**Total Turns:** %llu  \n", (unsigned long long)store->current_turn_id);
        fprintf(fp, "**Entries:** %u  \n\n", store->entry_count);
        
        fprintf(fp, "---\n\n");
        
        uint64_t last_turn = 0;
        for (uint32_t i = 0; i < store->entry_count; i++) {
            ethervox_memory_entry_t* entry = &store->entries[i];
            
            // Turn separator
            if (entry->turn_id != last_turn) {
                fprintf(fp, "\n## Turn %llu\n\n", (unsigned long long)entry->turn_id);
                last_turn = entry->turn_id;
            }
            
            // Format timestamp
            format_timestamp(entry->timestamp, time_buf, sizeof(time_buf));
            
            // Message
            fprintf(fp, "**%s** `[%s]` _importance: %.2f_\n\n",
                   entry->is_user_message ? "User" : "Assistant",
                   time_buf, entry->importance);
            
            fprintf(fp, "%s\n\n", entry->text);
            
            // Tags
            if (entry->tag_count > 0) {
                fprintf(fp, "_Tags:_ ");
                for (uint32_t t = 0; t < entry->tag_count; t++) {
                    fprintf(fp, "`%s`%s", entry->tags[t],
                           t < entry->tag_count - 1 ? ", " : "");
                }
                fprintf(fp, "\n\n");
            }
            
            // Tools called
            if (entry->tools_called_count > 0) {
                fprintf(fp, "_Tools:_ ");
                for (uint32_t t = 0; t < entry->tools_called_count; t++) {
                    fprintf(fp, "`%s`%s", entry->tools_called[t],
                           t < entry->tools_called_count - 1 ? ", " : "");
                }
                fprintf(fp, "\n\n");
            }
            
            fprintf(fp, "---\n\n");
        }
        
        // Statistics
        fprintf(fp, "\n## Statistics\n\n");
        fprintf(fp, "- Total memories stored: %llu\n", (unsigned long long)store->total_memories_stored);
        fprintf(fp, "- Total searches performed: %llu\n", (unsigned long long)store->total_searches);
        fprintf(fp, "- Total exports: %llu\n", (unsigned long long)store->total_exports);
        
    } else {
        fclose(fp);
        ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                    "Unknown export format: %s", format);
        return -1;
    }
    
    bytes = ftell(fp);
    fclose(fp);
    
    store->total_exports++;
    
    if (bytes_written) {
        *bytes_written = bytes;
    }
    
    ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                "Exported %u entries to %s (%s format, %llu bytes)",
                store->entry_count, filepath, format, (unsigned long long)bytes);
    
    return 0;
}

int ethervox_memory_import(
    ethervox_memory_store_t* store,
    const char* filepath,
    uint32_t* turns_loaded
) {
    if (!store || !store->is_initialized || !filepath) {
        return -1;
    }
    
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                    "Failed to open import file: %s", filepath);
        return -1;
    }
    
    // Simple JSONL import (one entry per line)
    char line[ETHERVOX_MEMORY_MAX_TEXT_LEN + 512];
    uint32_t loaded = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        // Parse basic JSON fields (simplified parser)
        uint64_t id, turn_id;
        long timestamp;
        float importance;
        char text[ETHERVOX_MEMORY_MAX_TEXT_LEN];
        bool is_user = false;
        
        // Parse numeric fields
        char* id_ptr = strstr(line, "\"id\":");
        char* turn_ptr = strstr(line, "\"turn\":");
        char* ts_ptr = strstr(line, "\"ts\":");
        char* imp_ptr = strstr(line, "\"imp\":");
        char* user_ptr = strstr(line, "\"user\":");
        
        if (!id_ptr || !turn_ptr || !ts_ptr || !imp_ptr || !user_ptr) {
            continue;  // Skip malformed lines
        }
        
        id = strtoull(id_ptr + 5, NULL, 10);
        turn_id = strtoull(turn_ptr + 7, NULL, 10);
        timestamp = strtol(ts_ptr + 5, NULL, 10);
        importance = strtof(imp_ptr + 6, NULL);
        
        // Check if user:true or user:false
        if (strstr(user_ptr, "true")) {
            is_user = true;
        }
        
        // Extract text field (between "text":" and next ",)
        char* text_start = strstr(line, "\"text\":\"");
        if (text_start) {
            text_start += 8;  // Skip "text":"
            char* text_end = text_start;
            
            // Find the closing quote, handling escaped quotes
            while (*text_end) {
                if (*text_end == '\"' && (text_end == text_start || *(text_end - 1) != '\\')) {
                    break;
                }
                text_end++;
            }
            
            if (*text_end == '\"') {
                size_t len = text_end - text_start;
                if (len > 0 && len < sizeof(text)) {
                    strncpy(text, text_start, len);
                    text[len] = '\0';
                    
                    // Add to memory (simplified - doesn't parse tags)
                    const char* tags[] = {"imported"};
                    uint64_t memory_id;
                    
                    if (ethervox_memory_store_add(store, text, tags, 1,
                                                 importance, is_user,
                                                 &memory_id) == 0) {
                        loaded++;
                    }
                }
            }
        }
    }
    
    fclose(fp);
    
    if (turns_loaded) {
        *turns_loaded = loaded;
    }
    
    ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                "Imported %u entries from %s", loaded, filepath);
    
    return 0;
}
