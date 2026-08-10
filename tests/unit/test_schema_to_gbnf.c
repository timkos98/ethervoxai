// SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary

/**
 * @file test_schema_to_gbnf.c
 * @brief Golden tests for JSON Schema → GBNF converter
 * 
 * Tests each schema type against its expected GBNF output.
 * Golden test format:
 *   tests/golden/schema_to_gbnf/<type>.json → <type>.gbnf
 * 
 * Implementation progress (C2.3a):
 * ✅ Type 1: Boolean
 * ✅ Type 2: Integer
 * ✅ Type 3: Number
 * ✅ Type 4: String (plain)
 * ✅ Type 5: String enum
 * ✅ Type 6: String maxLength
 * ✅ Type 7: String pattern
 * ✅ Type 8: Object (empty)
 * ✅ Type 9: Object (required fields)
 * ✅ Type 10: Object (optional fields)
 * ✅ Type 11: Array
 * ✅ Type 12: Array maxItems
 * ⬜ Type 13: oneOf
 * ⬜ Type 14: Nested objects
 */

#include "ethervox/grammar.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Test helpers
// =============================================================================

static char* read_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* content = (char*)malloc(size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }
    
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);
    
    return content;
}

static void test_golden(const char* name) {
    char json_path[512], gbnf_path[512];
    snprintf(json_path, sizeof(json_path), "tests/golden/schema_to_gbnf/%s.json", name);
    snprintf(gbnf_path, sizeof(gbnf_path), "tests/golden/schema_to_gbnf/%s.gbnf", name);
    
    // Read input schema
    char* schema_json = read_file(json_path);
    if (!schema_json) {
        fprintf(stderr, "FAIL: Could not read %s\n", json_path);
        exit(1);
    }
    
    // Read expected GBNF output
    char* expected_gbnf = read_file(gbnf_path);
    if (!expected_gbnf) {
        fprintf(stderr, "FAIL: Could not read %s\n", gbnf_path);
        free(schema_json);
        exit(1);
    }
    
    // Convert schema to GBNF
    ethervox_grammar_t* grammar = NULL;
    ethervox_result_t result = ethervox_grammar_from_json_schema(schema_json, &grammar);
    
    if (result != ETHERVOX_SUCCESS) {
        fprintf(stderr, "FAIL: %s - conversion failed with error %d\n", name, result);
        free(schema_json);
        free(expected_gbnf);
        exit(1);
    }
    
    // Get generated GBNF
    const char* actual_gbnf = ethervox_grammar_get_source(grammar);
    if (!actual_gbnf) {
        fprintf(stderr, "FAIL: %s - no GBNF source returned\n", name);
        ethervox_grammar_free(grammar);
        free(schema_json);
        free(expected_gbnf);
        exit(1);
    }
    
    // Compare (exact match)
    if (strcmp(actual_gbnf, expected_gbnf) != 0) {
        fprintf(stderr, "FAIL: %s - GBNF mismatch\n", name);
        fprintf(stderr, "Expected:\n%s\n", expected_gbnf);
        fprintf(stderr, "Actual:\n%s\n", actual_gbnf);
        ethervox_grammar_free(grammar);
        free(schema_json);
        free(expected_gbnf);
        exit(1);
    }
    
    printf("PASS: %s\n", name);
    
    ethervox_grammar_free(grammar);
    free(schema_json);
    free(expected_gbnf);
}

// =============================================================================
// Unit tests
// =============================================================================

static void test_null_safety(void) {
    ethervox_grammar_t* grammar = NULL;
    
    // NULL schema
    assert(ethervox_grammar_from_json_schema(NULL, &grammar) == ETHERVOX_ERROR_NULL_POINTER);
    
    // NULL output pointer
    assert(ethervox_grammar_from_json_schema("{\"type\": \"boolean\"}", NULL) == ETHERVOX_ERROR_NULL_POINTER);
    
    // NULL grammar operations
    assert(ethervox_grammar_get_source(NULL) == NULL);
    assert(ethervox_grammar_get_root(NULL) == NULL);
    ethervox_grammar_free(NULL);  // Should not crash
    
    printf("PASS: null_safety\n");
}

static void test_malformed_json(void) {
    ethervox_grammar_t* grammar = NULL;
    
    // Not JSON
    assert(ethervox_grammar_from_json_schema("not json", &grammar) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // Missing type field
    assert(ethervox_grammar_from_json_schema("{\"foo\": \"bar\"}", &grammar) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    printf("PASS: malformed_json\n");
}

static void test_compile_direct_gbnf(void) {
    ethervox_grammar_t* grammar = NULL;
    const char* gbnf = "root ::= \"hello\" | \"world\"\n";
    
    ethervox_result_t result = ethervox_grammar_compile(gbnf, "root", &grammar);
    assert(result == ETHERVOX_SUCCESS);
    assert(grammar != NULL);
    
    const char* source = ethervox_grammar_get_source(grammar);
    const char* root = ethervox_grammar_get_root(grammar);
    
    assert(source != NULL);
    assert(strcmp(source, gbnf) == 0);
    assert(strcmp(root, "root") == 0);
    
    ethervox_grammar_free(grammar);
    printf("PASS: compile_direct_gbnf\n");
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
    printf("=== JSON Schema → GBNF Converter Tests ===\n\n");
    
    // Unit tests
    printf("--- Unit Tests ---\n");
    test_null_safety();
    test_malformed_json();
    test_compile_direct_gbnf();
    
    // Golden tests (implemented types only)
    printf("\n--- Golden Tests ---\n");
    test_golden("boolean");
    test_golden("integer");
    test_golden("number");
    test_golden("string_plain");
    test_golden("string_enum");
    test_golden("string_max_length");
    test_golden("string_pattern");
    test_golden("object_empty");
    test_golden("object_required");
    test_golden("object_optional");
    test_golden("array");
    test_golden("array_max_items");
    
    printf("\n✅ All tests passed!\n");
    return 0;
}
