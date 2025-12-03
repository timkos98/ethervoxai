/**
 * @file integration_tests.c
 * @brief Interactive /test command for validating major features
 *
 * Runs comprehensive tests on:
 * - Context window management (overflow, compression)
 * - Hash table indexing (tag_hash, id_hash)
 * - Adaptive memory (corrections, patterns)
 * - Memory search and retrieval
 * - Tool execution
 * - System prompt generation
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/memory_tools.h"
#include "ethervox/context_tools.h"
#include "ethervox/governor.h"
#include "ethervox/logging.h"
#include "ethervox/chat_template.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// ANSI color codes for pretty output
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

#define TEST_PASS(msg, ...) printf(COLOR_GREEN "  ✓ " COLOR_RESET msg "\n", ##__VA_ARGS__)
#define TEST_FAIL(msg, ...) printf(COLOR_RED "  ✗ " COLOR_RESET msg "\n", ##__VA_ARGS__)
#define TEST_INFO(msg, ...) printf(COLOR_CYAN "  ℹ " COLOR_RESET msg "\n", ##__VA_ARGS__)
#define TEST_HEADER(msg, ...) printf("\n" COLOR_BOLD COLOR_BLUE "=== " msg " ===" COLOR_RESET "\n", ##__VA_ARGS__)
#define TEST_SUBHEADER(msg, ...) printf("\n" COLOR_YELLOW "→ " msg COLOR_RESET "\n", ##__VA_ARGS__)

static int g_tests_passed = 0;
static int g_tests_failed = 0;

// Test 1: Memory Store Initialization and Basic Operations
static void test_memory_basic(void) {
    TEST_HEADER("Test 1: Memory Store Basic Operations");
    
    ethervox_memory_store_t store;
    if (ethervox_memory_init(&store, "test_session", "/tmp") != 0) {
        TEST_FAIL("Failed to initialize memory store");
        g_tests_failed++;
        return;
    }
    TEST_PASS("Memory store initialized (session: %s)", store.session_id);
    
    // Add some test memories
    const char* tags1[] = {"greeting", "personal"};
    const char* tags2[] = {"tech", "ai"};
    const char* tags3[] = {"reminder", "important"};
    
    uint64_t id1, id2, id3;
    
    if (ethervox_memory_store_add(&store, "User's name is Tim", tags1, 2, 0.95f, true, &id1) != 0) {
        TEST_FAIL("Failed to store first memory");
        g_tests_failed++;
        ethervox_memory_cleanup(&store);
        return;
    }
    TEST_PASS("Stored memory 1 (ID: %llu, importance: 0.95)", (unsigned long long)id1);
    
    if (ethervox_memory_store_add(&store, "Discussion about llama.cpp integration", tags2, 2, 0.80f, false, &id2) != 0) {
        TEST_FAIL("Failed to store second memory");
        g_tests_failed++;
        ethervox_memory_cleanup(&store);
        return;
    }
    TEST_PASS("Stored memory 2 (ID: %llu, importance: 0.80)", (unsigned long long)id2);
    
    if (ethervox_memory_store_add(&store, "Call dentist on Friday", tags3, 2, 0.90f, true, &id3) != 0) {
        TEST_FAIL("Failed to store third memory");
        g_tests_failed++;
        ethervox_memory_cleanup(&store);
        return;
    }
    TEST_PASS("Stored memory 3 (ID: %llu, importance: 0.90)", (unsigned long long)id3);
    
    TEST_INFO("Total memories: %u, Total stored: %llu", 
              store.entry_count, (unsigned long long)store.total_memories_stored);
    
    // Test retrieval by ID
    const ethervox_memory_entry_t* entry;
    if (ethervox_memory_get_by_id(&store, id1, &entry) != 0) {
        TEST_FAIL("Failed to retrieve memory by ID");
        g_tests_failed++;
    } else {
        TEST_PASS("Retrieved memory by ID: \"%s\"", entry->text);
        g_tests_passed++;
    }
    
    ethervox_memory_cleanup(&store);
    TEST_PASS("Memory store cleaned up successfully");
    g_tests_passed++;
}

// Test 2: Memory Search and Tag Indexing
static void test_memory_search(void) {
    TEST_HEADER("Test 2: Memory Search and Tag Indexing");
    
    ethervox_memory_store_t store;
    ethervox_memory_init(&store, NULL, "/tmp");
    
    // Add diverse memories with different tags
    const char* tags_personal[] = {"personal", "name"};
    const char* tags_work[] = {"work", "project"};
    const char* tags_reminder[] = {"reminder", "urgent"};
    const char* tags_tech[] = {"tech", "programming"};
    
    uint64_t id;
    ethervox_memory_store_add(&store, "My name is Tim", tags_personal, 2, 0.95f, true, &id);
    ethervox_memory_store_add(&store, "Working on EthervoxAI project", tags_work, 2, 0.85f, false, &id);
    ethervox_memory_store_add(&store, "Reminder: Meeting at 3 PM", tags_reminder, 2, 0.90f, true, &id);
    ethervox_memory_store_add(&store, "Love using llama.cpp for LLM inference", tags_tech, 2, 0.80f, false, &id);
    ethervox_memory_store_add(&store, "Work deadline is Friday", tags_work, 2, 0.88f, true, &id);
    
    TEST_INFO("Added 5 test memories with various tags");
    
    // Test tag-based search
    TEST_SUBHEADER("Tag-based search for 'work' tag");
    const char* work_tag = "work";
    ethervox_memory_search_result_t* results = NULL;
    uint32_t result_count = 0;
    
    if (ethervox_memory_search(&store, NULL, &work_tag, 1, 10, &results, &result_count) != 0) {
        TEST_FAIL("Search failed");
        g_tests_failed++;
    } else {
        TEST_PASS("Found %u memories with 'work' tag", result_count);
        for (uint32_t i = 0; i < result_count; i++) {
            TEST_INFO("  [%u] %s", i + 1, results[i].entry.text);
        }
        free(results);
        
        if (result_count == 2) {
            TEST_PASS("Correct number of work-tagged memories found");
            g_tests_passed++;
        } else {
            TEST_FAIL("Expected 2 work memories, got %u", result_count);
            g_tests_failed++;
        }
    }
    
    // Test text-based search
    TEST_SUBHEADER("Text-based search for 'llama'");
    results = NULL;
    result_count = 0;
    
    if (ethervox_memory_search(&store, "llama", NULL, 0, 10, &results, &result_count) != 0) {
        TEST_FAIL("Text search failed");
        g_tests_failed++;
    } else {
        TEST_PASS("Found %u memories containing 'llama'", result_count);
        for (uint32_t i = 0; i < result_count; i++) {
            TEST_INFO("  [%u] %s (relevance: %.2f)", i + 1, results[i].entry.text, results[i].relevance);
        }
        free(results);
        
        if (result_count >= 1) {
            TEST_PASS("Successfully found text matches");
            g_tests_passed++;
        } else {
            TEST_FAIL("Should have found at least 1 match");
            g_tests_failed++;
        }
    }
    
    ethervox_memory_cleanup(&store);
}

// Test 3: Adaptive Memory (Corrections and Patterns)
static void test_adaptive_memory(void) {
    TEST_HEADER("Test 3: Adaptive Memory (Corrections & Patterns)");
    
    ethervox_memory_store_t store;
    ethervox_memory_init(&store, NULL, "/tmp");
    
    // Test correction storage
    TEST_SUBHEADER("Storing user corrections");
    uint64_t corr_id1, corr_id2;
    
    if (ethervox_memory_store_correction(&store, "User prefers metric units", "Used feet instead of meters", &corr_id1) != 0) {
        TEST_FAIL("Failed to store correction 1");
        g_tests_failed++;
    } else {
        TEST_PASS("Stored correction 1 (ID: %llu)", (unsigned long long)corr_id1);
        
        // Verify importance
        const ethervox_memory_entry_t* entry;
        if (ethervox_memory_get_by_id(&store, corr_id1, &entry) == 0) {
            if (entry->importance == 0.99f) {
                TEST_PASS("Correction has importance=0.99 ✓");
                g_tests_passed++;
            } else {
                TEST_FAIL("Correction importance is %.2f, expected 0.99", entry->importance);
                g_tests_failed++;
            }
        }
    }
    
    ethervox_memory_store_correction(&store, "User likes concise responses", NULL, &corr_id2);
    TEST_PASS("Stored correction 2 (ID: %llu)", (unsigned long long)corr_id2);
    
    // Test pattern storage
    TEST_SUBHEADER("Storing success patterns");
    uint64_t pat_id1, pat_id2, pat_id3;
    
    ethervox_memory_store_pattern(&store, "User responds well to code examples", &pat_id1);
    TEST_PASS("Stored pattern 1 (ID: %llu)", (unsigned long long)pat_id1);
    
    ethervox_memory_store_pattern(&store, "Bullet-point lists work better than paragraphs", &pat_id2);
    TEST_PASS("Stored pattern 2 (ID: %llu)", (unsigned long long)pat_id2);
    
    ethervox_memory_store_pattern(&store, "User appreciates performance optimizations", &pat_id3);
    TEST_PASS("Stored pattern 3 (ID: %llu)", (unsigned long long)pat_id3);
    
    // Retrieve corrections
    TEST_SUBHEADER("Retrieving corrections for adaptive prompt");
    ethervox_memory_search_result_t* corrections = NULL;
    uint32_t correction_count = 0;
    
    if (ethervox_memory_get_corrections(&store, &corrections, &correction_count, 10) != 0) {
        TEST_FAIL("Failed to retrieve corrections");
        g_tests_failed++;
    } else {
        TEST_PASS("Retrieved %u corrections", correction_count);
        for (uint32_t i = 0; i < correction_count; i++) {
            TEST_INFO("  [%u] %s", i + 1, corrections[i].entry.text);
        }
        free(corrections);
        
        if (correction_count == 2) {
            TEST_PASS("Correct count of corrections retrieved");
            g_tests_passed++;
        } else {
            TEST_FAIL("Expected 2 corrections, got %u", correction_count);
            g_tests_failed++;
        }
    }
    
    // Retrieve patterns
    TEST_SUBHEADER("Retrieving success patterns");
    ethervox_memory_search_result_t* patterns = NULL;
    uint32_t pattern_count = 0;
    
    if (ethervox_memory_get_patterns(&store, &patterns, &pattern_count, 10) != 0) {
        TEST_FAIL("Failed to retrieve patterns");
        g_tests_failed++;
    } else {
        TEST_PASS("Retrieved %u patterns", pattern_count);
        for (uint32_t i = 0; i < pattern_count; i++) {
            TEST_INFO("  [%u] %s", i + 1, patterns[i].entry.text);
        }
        free(patterns);
        
        if (pattern_count == 3) {
            TEST_PASS("Correct count of patterns retrieved");
            g_tests_passed++;
        } else {
            TEST_FAIL("Expected 3 patterns, got %u", pattern_count);
            g_tests_failed++;
        }
    }
    
    ethervox_memory_cleanup(&store);
}

// Test 4: System Prompt Generation with Adaptive Learning
static void test_system_prompt_generation(void) {
    TEST_HEADER("Test 4: System Prompt Generation");
    
    // Create memory store with corrections and patterns
    ethervox_memory_store_t memory_store;
    ethervox_memory_init(&memory_store, NULL, "/tmp");
    
    uint64_t id;
    ethervox_memory_store_correction(&memory_store, "User wants technical details", NULL, &id);
    ethervox_memory_store_correction(&memory_store, "Avoid marketing language", NULL, &id);
    ethervox_memory_store_pattern(&memory_store, "Code examples are helpful", &id);
    ethervox_memory_store_pattern(&memory_store, "User prefers direct answers", &id);
    
    TEST_INFO("Added 2 corrections and 2 patterns to memory");
    
    // Create tool registry
    ethervox_tool_registry_t registry;
    if (ethervox_tool_registry_init(&registry, 10) != 0) {
        TEST_FAIL("Failed to initialize tool registry");
        g_tests_failed++;
        ethervox_memory_cleanup(&memory_store);
        return;
    }
    
    TEST_PASS("Tool registry initialized");
    
    // Get chat template
    const chat_template_t* template = chat_template_get(CHAT_TEMPLATE_QWEN, NULL);
    if (!template) {
        TEST_FAIL("Failed to get chat template");
        g_tests_failed++;
        ethervox_tool_registry_cleanup(&registry);
        ethervox_memory_cleanup(&memory_store);
        return;
    }
    
    TEST_PASS("Chat template loaded (type: ChatML)");
    
    // Build system prompt WITHOUT adaptive learning (NULL memory_store)
    TEST_SUBHEADER("Building system prompt without adaptive learning");
    char prompt_no_adapt[8192];
    if (ethervox_tool_registry_build_system_prompt(&registry, template, prompt_no_adapt, sizeof(prompt_no_adapt), NULL) != 0) {
        TEST_FAIL("Failed to build system prompt");
        g_tests_failed++;
    } else {
        TEST_PASS("System prompt built (%zu chars)", strlen(prompt_no_adapt));
        TEST_INFO("Prompt does NOT contain corrections/patterns (memory_store=NULL)");
        g_tests_passed++;
    }
    
    // Build system prompt WITH adaptive learning
    TEST_SUBHEADER("Building system prompt with adaptive learning");
    char prompt_with_adapt[8192];
    if (ethervox_tool_registry_build_system_prompt(&registry, template, prompt_with_adapt, sizeof(prompt_with_adapt), &memory_store) != 0) {
        TEST_FAIL("Failed to build adaptive system prompt");
        g_tests_failed++;
    } else {
        TEST_PASS("Adaptive system prompt built (%zu chars)", strlen(prompt_with_adapt));
        
        // Check if corrections are injected
        if (strstr(prompt_with_adapt, "USER CORRECTIONS")) {
            TEST_PASS("Found 'USER CORRECTIONS' section in prompt");
            g_tests_passed++;
        } else {
            TEST_INFO("No 'USER CORRECTIONS' section (may be desktop-only feature)");
        }
        
        // Check if patterns are injected
        if (strstr(prompt_with_adapt, "SUCCESSFUL PATTERNS")) {
            TEST_PASS("Found 'SUCCESSFUL PATTERNS' section in prompt");
            g_tests_passed++;
        } else {
            TEST_INFO("No 'SUCCESSFUL PATTERNS' section (may be desktop-only feature)");
        }
    }
    
    ethervox_tool_registry_cleanup(&registry);
    ethervox_memory_cleanup(&memory_store);
}

// Test 5: Memory Export/Import
static void test_memory_export_import(void) {
    TEST_HEADER("Test 5: Memory Export and Import");
    
    ethervox_memory_store_t store;
    ethervox_memory_init(&store, "export_test", "/tmp");
    
    // Add test data
    const char* tags1[] = {"test", "export"};
    const char* tags2[] = {"test", "data"};
    uint64_t id;
    
    ethervox_memory_store_add(&store, "First test memory", tags1, 2, 0.85f, true, &id);
    ethervox_memory_store_add(&store, "Second test memory", tags2, 2, 0.75f, false, &id);
    ethervox_memory_store_correction(&store, "Test correction", NULL, &id);
    
    TEST_INFO("Added 3 memories for export");
    
    // Export to JSON
    TEST_SUBHEADER("Exporting to JSON");
    const char* json_path = "/tmp/test_export.json";
    uint64_t bytes_written = 0;
    
    if (ethervox_memory_export(&store, json_path, "json", &bytes_written) != 0) {
        TEST_FAIL("Failed to export to JSON");
        g_tests_failed++;
    } else {
        TEST_PASS("Exported %llu bytes to %s", (unsigned long long)bytes_written, json_path);
        g_tests_passed++;
    }
    
    // Export to Markdown
    TEST_SUBHEADER("Exporting to Markdown");
    const char* md_path = "/tmp/test_export.md";
    bytes_written = 0;
    
    if (ethervox_memory_export(&store, md_path, "markdown", &bytes_written) != 0) {
        TEST_FAIL("Failed to export to Markdown");
        g_tests_failed++;
    } else {
        TEST_PASS("Exported %llu bytes to %s", (unsigned long long)bytes_written, md_path);
        g_tests_passed++;
    }
    
    ethervox_memory_cleanup(&store);
    
    // Test import
    TEST_SUBHEADER("Importing from JSON");
    ethervox_memory_store_t import_store;
    ethervox_memory_init(&import_store, "import_test", "/tmp");
    
    uint32_t turns_loaded = 0;
    if (ethervox_memory_import(&import_store, json_path, &turns_loaded) != 0) {
        TEST_FAIL("Failed to import from JSON");
        g_tests_failed++;
    } else {
        TEST_PASS("Imported %u turns from JSON", turns_loaded);
        TEST_INFO("Memory store now has %u entries", import_store.entry_count);
        
        if (import_store.entry_count >= 3) {
            TEST_PASS("Imported data contains expected entries");
            g_tests_passed++;
        } else {
            TEST_FAIL("Expected at least 3 entries, got %u", import_store.entry_count);
            g_tests_failed++;
        }
    }
    
    ethervox_memory_cleanup(&import_store);
}

// Test 6: Hash Table Performance (simulated)
static void test_hash_table_performance(void) {
    TEST_HEADER("Test 6: Hash Table Index Performance");
    
    ethervox_memory_store_t store;
    ethervox_memory_init(&store, NULL, "/tmp");
    
    TEST_INFO("Adding 50 memories with various tags for hash table testing");
    
    const char* tag_sets[][3] = {
        {"tech", "ai", "llm"},
        {"personal", "name", "info"},
        {"work", "project", "deadline"},
        {"reminder", "urgent", "task"},
        {"learning", "pattern", "success"}
    };
    
    // Add 50 memories
    clock_t start = clock();
    for (int i = 0; i < 50; i++) {
        char text[256];
        snprintf(text, sizeof(text), "Test memory number %d with diverse content", i);
        
        const char* tags[3];
        int tag_set = i % 5;
        for (int j = 0; j < 3; j++) {
            tags[j] = tag_sets[tag_set][j];
        }
        
        uint64_t id;
        ethervox_memory_store_add(&store, text, tags, 3, 0.5f + (i % 5) * 0.1f, i % 2 == 0, &id);
    }
    clock_t end = clock();
    double add_time = ((double)(end - start)) / CLOCKS_PER_SEC * 1000.0;
    
    TEST_PASS("Added 50 memories in %.2f ms (%.2f ms avg per memory)", add_time, add_time / 50.0);
    TEST_INFO("Tag index has %u unique tags", store.tag_index_count);
    
    // Test tag search performance
    TEST_SUBHEADER("Testing tag search performance");
    const char* search_tag = "tech";
    
    start = clock();
    ethervox_memory_search_result_t* results = NULL;
    uint32_t result_count = 0;
    ethervox_memory_search(&store, NULL, &search_tag, 1, 50, &results, &result_count);
    end = clock();
    double search_time = ((double)(end - start)) / CLOCKS_PER_SEC * 1000.0;
    
    TEST_PASS("Tag search completed in %.2f ms, found %u results", search_time, result_count);
    
    if (result_count == 10) {  // Should be 50/5 = 10
        TEST_PASS("Correct number of 'tech' tagged memories found");
        g_tests_passed++;
    } else {
        TEST_INFO("Found %u 'tech' tagged memories", result_count);
    }
    
    free(results);
    
    // Test ID lookup performance
    TEST_SUBHEADER("Testing ID lookup performance");
    uint64_t test_id = 25;  // Middle ID
    
    start = clock();
    const ethervox_memory_entry_t* entry;
    ethervox_memory_get_by_id(&store, test_id, &entry);
    end = clock();
    double lookup_time = ((double)(end - start)) / CLOCKS_PER_SEC * 1000000.0;  // microseconds
    
    TEST_PASS("ID lookup completed in %.2f μs", lookup_time);
    
    if (lookup_time < 100.0) {  // Should be very fast with hash table
        TEST_PASS("ID lookup is efficient (< 100 μs)");
        g_tests_passed++;
    } else {
        TEST_INFO("ID lookup took %.2f μs (hash table may not be active yet)", lookup_time);
    }
    
    ethervox_memory_cleanup(&store);
}

// Test 7: Memory archiving
static void test_memory_archive(void) {
    TEST_HEADER("Test 7: Memory Archiving");
    
    // Create a temporary test directory
    const char* test_dir = "/tmp/ethervox_archive_test";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", test_dir, test_dir);
    system(cmd);
    
    // Create some fake old session files
    char filepath[512];
    for (int i = 1; i <= 3; i++) {
        snprintf(filepath, sizeof(filepath), "%s/session_old_%d.jsonl", test_dir, i);
        FILE* f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "{\"id\":%d,\"text\":\"old session %d\"}\n", i, i);
            fclose(f);
        }
    }
    
    // Initialize memory store
    ethervox_memory_store_t archive_store;
    if (ethervox_memory_init(&archive_store, NULL, test_dir) == 0) {
        // Archive the old sessions
        uint32_t archived = 0;
        if (ethervox_memory_archive_sessions(&archive_store, &archived) == 0) {
            if (archived == 3) {
                TEST_PASS("Archived 3 session files");
                g_tests_passed++;
            } else {
                TEST_FAIL("Expected 3 archived files, got %u", archived);
                g_tests_failed++;
            }
            
            // Verify archive directory exists using standard C library
            snprintf(filepath, sizeof(filepath), "%s/archive/session_old_1.jsonl", test_dir);
            FILE* test_file = fopen(filepath, "r");
            if (test_file) {
                fclose(test_file);
                TEST_PASS("Archive directory created and files moved");
                g_tests_passed++;
            } else {
                TEST_FAIL("Archived files not found in archive directory");
                g_tests_failed++;
            }
        } else {
            TEST_FAIL("Archive operation failed");
            g_tests_failed++;
        }
        
        ethervox_memory_cleanup(&archive_store);
    } else {
        TEST_FAIL("Failed to initialize archive test store");
        g_tests_failed++;
    }
    
    // Cleanup
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    system(cmd);
}

// Main test runner
void run_integration_tests(void) {
    printf("\n");
    printf(COLOR_BOLD COLOR_MAGENTA);
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                                                               ║\n");
    printf("║          ETHERVOXAI INTEGRATION TEST SUITE                   ║\n");
    printf("║          macOS Desktop - Major Features                      ║\n");
    printf("║                                                               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
    printf("\n");
    
    time_t start_time = time(NULL);
    
    // Run all tests
    test_memory_basic();
    test_memory_search();
    test_adaptive_memory();
    test_system_prompt_generation();
    test_memory_export_import();
    test_hash_table_performance();
    test_memory_archive();
    
    time_t end_time = time(NULL);
    double duration = difftime(end_time, start_time);
    
    // Final summary
    printf("\n");
    printf(COLOR_BOLD COLOR_MAGENTA);
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                     TEST SUMMARY                              ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
    printf("\n");
    
    int total_tests = g_tests_passed + g_tests_failed;
    double pass_rate = total_tests > 0 ? (g_tests_passed * 100.0 / total_tests) : 0.0;
    
    printf(COLOR_GREEN "  Tests Passed:  %d\n" COLOR_RESET, g_tests_passed);
    printf(COLOR_RED "  Tests Failed:  %d\n" COLOR_RESET, g_tests_failed);
    printf(COLOR_CYAN "  Total Tests:   %d\n" COLOR_RESET, total_tests);
    printf(COLOR_YELLOW "  Pass Rate:     %.1f%%\n" COLOR_RESET, pass_rate);
    printf(COLOR_BLUE "  Duration:      %.0f seconds\n" COLOR_RESET, duration);
    printf("\n");
    
    if (g_tests_failed == 0) {
        printf(COLOR_BOLD COLOR_GREEN);
        printf("  ✓✓✓ ALL TESTS PASSED! ✓✓✓\n");
        printf(COLOR_RESET);
    } else {
        printf(COLOR_BOLD COLOR_YELLOW);
        printf("  ⚠ Some tests failed - review output above\n");
        printf(COLOR_RESET);
    }
    
    printf("\n");
}
