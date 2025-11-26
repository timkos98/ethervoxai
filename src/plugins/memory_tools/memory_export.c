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

// Internal function from memory_core.c for importing with explicit ID/timestamp
extern int memory_store_add_internal(
    ethervox_memory_store_t* store,
    const char* text,
    const char* tags[],
    uint32_t tag_count,
    float importance,
    bool is_user_message,
    uint64_t memory_id,
    uint64_t turn_id,
    time_t timestamp,
    uint64_t* memory_id_out
);

// Internal function from memory_search.c for deleting without persisting to log
extern int memory_delete_by_ids_internal(
    ethervox_memory_store_t* store,
    const uint64_t* memory_ids,
    uint32_t id_count,
    bool persist_to_log,
    uint32_t* items_deleted
);

// Internal function from memory_core.c for updating tags with optional persistence
extern int memory_update_tags_internal(
    ethervox_memory_store_t* store,
    uint64_t memory_id,
    const char* tags[],
    uint32_t tag_count,
    bool persist_to_log
);

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
        // Check if this is a DELETE operation
        char* delete_op = strstr(line, "\"op\":\"delete\"");
        if (delete_op) {
            // Handle DELETE record - remove memory by ID
            char* id_ptr = strstr(line, "\"id\":");
            if (!id_ptr) continue;
            
            uint64_t memory_id = strtoull(id_ptr + 5, NULL, 10);
            
            // Delete the memory entry without persisting (we're already reading from file)
            uint32_t deleted = 0;
            memory_delete_by_ids_internal(store, &memory_id, 1, false, &deleted);
            
            // Write DELETE record to new session file to preserve operation history
            // NOTE: This may be redundant - memories are already added with final state,
            // so preserving operation history isn't strictly necessary for correctness.
            // Kept for auditability and potential future replay/undo features.
            if (store->append_log && deleted > 0) {
                fprintf(store->append_log, "{\"op\":\"delete\",\"id\":%llu}\n", memory_id);
                fflush(store->append_log);
            }
            
            continue;  // Skip to next line
        }
        
        // Check if this is an UPDATE operation
        char* op_ptr = strstr(line, "\"op\":\"update\"");
        if (op_ptr) {
            // Handle UPDATE record - update tags for existing memory
            char* id_ptr = strstr(line, "\"id\":");
            if (!id_ptr) continue;
            
            uint64_t memory_id = strtoull(id_ptr + 5, NULL, 10);
            
            // Parse updated tags
            const char* tag_array[ETHERVOX_MEMORY_MAX_TAGS];
            uint32_t tag_count = 0;
            char tag_storage[ETHERVOX_MEMORY_MAX_TAGS][ETHERVOX_MEMORY_TAG_LEN];
            
            char* tags_start = strstr(line, "\"tags\":[");
            if (tags_start) {
                tags_start += 8;  // Skip "tags":[
                char* tags_end = strchr(tags_start, ']');
                
                if (tags_end) {
                    char* tag_cursor = tags_start;
                    while (tag_cursor < tags_end && tag_count < ETHERVOX_MEMORY_MAX_TAGS) {
                        char* tag_open = strchr(tag_cursor, '"');
                        if (!tag_open || tag_open >= tags_end) break;
                        
                        tag_open++;
                        char* tag_close = strchr(tag_open, '"');
                        if (!tag_close || tag_close >= tags_end) break;
                        
                        size_t tag_len = tag_close - tag_open;
                        if (tag_len > 0 && tag_len < ETHERVOX_MEMORY_TAG_LEN) {
                            strncpy(tag_storage[tag_count], tag_open, tag_len);
                            tag_storage[tag_count][tag_len] = '\0';
                            tag_array[tag_count] = tag_storage[tag_count];
                            tag_count++;
                        }
                        
                        tag_cursor = tag_close + 1;
                    }
                }
            }
            
            // Apply the tag update to the memory entry
            if (tag_count > 0) {
                // Don't persist during update (we're reading from old file)
                int update_result = memory_update_tags_internal(store, memory_id, tag_array, tag_count, false);
                
                // Write UPDATE record to new session file to preserve operation history
                // NOTE: This may be redundant - when memories are later imported, they're
                // added with their current tag state, so operation history isn't needed.
                // Kept for auditability and potential future replay/undo features.
                if (store->append_log && update_result == 0) {
                    fprintf(store->append_log, "{\"op\":\"update\",\"id\":%llu,\"tags\":[", memory_id);
                    for (uint32_t i = 0; i < tag_count; i++) {
                        fprintf(store->append_log, "\"%s\"", tag_array[i]);
                        if (i < tag_count - 1) {
                            fprintf(store->append_log, ",");
                        }
                    }
                    fprintf(store->append_log, "]}\n");
                    fflush(store->append_log);
                }
            }
            
            continue;  // Skip to next line
        }
        
        // Parse basic JSON fields (simplified parser) for regular ADD records
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
                    
                    // Parse tags from JSON (format: "tags":["tag1","tag2"])
                    const char* tag_array[ETHERVOX_MEMORY_MAX_TAGS];
                    uint32_t tag_count = 0;
                    char tag_storage[ETHERVOX_MEMORY_MAX_TAGS][ETHERVOX_MEMORY_TAG_LEN];
                    
                    char* tags_start = strstr(line, "\"tags\":[");
                    if (tags_start) {
                        tags_start += 8;  // Skip "tags":[
                        char* tags_end = strchr(tags_start, ']');
                        
                        if (tags_end) {
                            // Parse each tag in the array
                            char* tag_cursor = tags_start;
                            while (tag_cursor < tags_end && tag_count < ETHERVOX_MEMORY_MAX_TAGS) {
                                // Find next quoted tag
                                char* tag_open = strchr(tag_cursor, '"');
                                if (!tag_open || tag_open >= tags_end) break;
                                
                                tag_open++;
                                char* tag_close = strchr(tag_open, '"');
                                if (!tag_close || tag_close >= tags_end) break;
                                
                                // Copy tag
                                size_t tag_len = tag_close - tag_open;
                                if (tag_len > 0 && tag_len < ETHERVOX_MEMORY_TAG_LEN) {
                                    strncpy(tag_storage[tag_count], tag_open, tag_len);
                                    tag_storage[tag_count][tag_len] = '\0';
                                    tag_array[tag_count] = tag_storage[tag_count];
                                    tag_count++;
                                }
                                
                                tag_cursor = tag_close + 1;
                            }
                        }
                    }
                    
                    // Add "imported" tag if not already present AND if there's room
                    bool has_imported = false;
                    for (uint32_t i = 0; i < tag_count; i++) {
                        if (strcmp(tag_array[i], "imported") == 0) {
                            has_imported = true;
                            break;
                        }
                    }
                    
                    if (!has_imported && tag_count < ETHERVOX_MEMORY_MAX_TAGS) {
                        strncpy(tag_storage[tag_count], "imported", ETHERVOX_MEMORY_TAG_LEN - 1);
                        tag_storage[tag_count][ETHERVOX_MEMORY_TAG_LEN - 1] = '\0';
                        tag_array[tag_count] = tag_storage[tag_count];
                        tag_count++;
                    }
                    
                    // If no tags were parsed, use just "imported"
                    if (tag_count == 0) {
                        strncpy(tag_storage[0], "imported", ETHERVOX_MEMORY_TAG_LEN - 1);
                        tag_storage[0][ETHERVOX_MEMORY_TAG_LEN - 1] = '\0';
                        tag_array[0] = tag_storage[0];
                        tag_count = 1;
                    }
                    
                    // Add to memory with original ID, timestamp, and tags + "imported"
                    uint64_t memory_id_out;
                    
                    if (memory_store_add_internal(store, text, tag_array, tag_count,
                                                 importance, is_user, id, turn_id,
                                                 timestamp, &memory_id_out) == 0) {
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
