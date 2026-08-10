/**
 * @file test_grammar.c
 * @brief Tests for grammar-constrained decoding (C2.3)
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/grammar.h"
#include "ethervox/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define CHECK_SUCCESS(expr) do { \
    ethervox_result_t _r = (expr); \
    if (_r != ETHERVOX_SUCCESS) { \
        fprintf(stderr, "FAIL: %s:%d: %s returned %d\n", __FILE__, __LINE__, #expr, _r); \
        return 1; \
    } \
} while (0)

static int test_compile_simple_gbnf(void) {
    printf("test_compile_simple_gbnf...\n");
    
    const char* gbnf = 
        "root ::= \"hello\" \" \" \"world\"\n";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_compile(gbnf, &grammar));
    CHECK(grammar != NULL);
    
    // Verify source is stored
    const char* source = ethervox_grammar_get_source(grammar);
    CHECK(source != NULL);
    CHECK(strcmp(source, gbnf) == 0);
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_compile_json_gbnf(void) {
    printf("test_compile_json_gbnf...\n");
    
    const char* gbnf = 
        "root ::= object\n"
        "object ::= \"{\" ws members ws \"}\"\n"
        "members ::= pair (\",\" ws pair)*\n"
        "pair ::= string \":\" ws value\n"
        "string ::= \"\\\"\" [^\"]* \"\\\"\"\n"
        "value ::= string | number\n"
        "number ::= [0-9]+\n"
        "ws ::= [ \\t\\n]*\n";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_compile(gbnf, &grammar));
    CHECK(grammar != NULL);
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_json_schema_simple_object(void) {
    printf("test_json_schema_simple_object...\n");
    
    const char* schema = 
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"name\": {\"type\": \"string\"},"
        "    \"age\": {\"type\": \"integer\"}"
        "  },"
        "  \"required\": [\"name\"]"
        "}";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_from_json_schema(schema, &grammar));
    CHECK(grammar != NULL);
    
    // Verify GBNF was generated
    const char* gbnf = ethervox_grammar_get_source(grammar);
    CHECK(gbnf != NULL);
    CHECK(strlen(gbnf) > 0);
    
    printf("    Generated GBNF (%zu bytes)\n", strlen(gbnf));
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_json_schema_string_enum(void) {
    printf("test_json_schema_string_enum...\n");
    
    const char* schema = 
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"status\": {"
        "      \"type\": \"string\","
        "      \"enum\": [\"pending\", \"approved\", \"rejected\"]"
        "    }"
        "  },"
        "  \"required\": [\"status\"]"
        "}";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_from_json_schema(schema, &grammar));
    CHECK(grammar != NULL);
    
    const char* gbnf = ethervox_grammar_get_source(grammar);
    CHECK(gbnf != NULL);
    
    // Verify enum values appear in GBNF
    CHECK(strstr(gbnf, "pending") != NULL);
    CHECK(strstr(gbnf, "approved") != NULL);
    CHECK(strstr(gbnf, "rejected") != NULL);
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_json_schema_nested_object(void) {
    printf("test_json_schema_nested_object...\n");
    
    const char* schema = 
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"user\": {"
        "      \"type\": \"object\","
        "      \"properties\": {"
        "        \"name\": {\"type\": \"string\"},"
        "        \"email\": {\"type\": \"string\"}"
        "      },"
        "      \"required\": [\"name\"]"
        "    },"
        "    \"timestamp\": {\"type\": \"integer\"}"
        "  },"
        "  \"required\": [\"user\"]"
        "}";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_from_json_schema(schema, &grammar));
    CHECK(grammar != NULL);
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_json_schema_array(void) {
    printf("test_json_schema_array...\n");
    
    const char* schema = 
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"tags\": {"
        "      \"type\": \"array\","
        "      \"items\": {\"type\": \"string\"},"
        "      \"maxItems\": 5"
        "    }"
        "  },"
        "  \"required\": [\"tags\"]"
        "}";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_from_json_schema(schema, &grammar));
    CHECK(grammar != NULL);
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_json_schema_oneof(void) {
    printf("test_json_schema_oneof...\n");
    
    const char* schema = 
        "{"
        "  \"oneOf\": ["
        "    {"
        "      \"type\": \"object\","
        "      \"properties\": {"
        "        \"type\": {\"const\": \"text\"},"
        "        \"content\": {\"type\": \"string\"}"
        "      },"
        "      \"required\": [\"type\", \"content\"]"
        "    },"
        "    {"
        "      \"type\": \"object\","
        "      \"properties\": {"
        "        \"type\": {\"const\": \"number\"},"
        "        \"value\": {\"type\": \"integer\"}"
        "      },"
        "      \"required\": [\"type\", \"value\"]"
        "    }"
        "  ]"
        "}";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_from_json_schema(schema, &grammar));
    CHECK(grammar != NULL);
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

static int test_null_safety(void) {
    printf("test_null_safety...\n");
    
    ethervox_grammar_t* grammar = NULL;
    
    // NULL source
    CHECK(ethervox_grammar_compile(NULL, &grammar) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL output
    CHECK(ethervox_grammar_compile("root ::= \"test\"", NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL schema
    CHECK(ethervox_grammar_from_json_schema(NULL, &grammar) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL output
    CHECK(ethervox_grammar_from_json_schema("{}", NULL) == ETHERVOX_ERROR_INVALID_ARGUMENT);
    
    // NULL grammar free is safe
    ethervox_grammar_free(NULL);
    
    // NULL grammar get_source returns NULL
    CHECK(ethervox_grammar_get_source(NULL) == NULL);
    
    printf("  PASS\n");
    return 0;
}

static int test_invalid_gbnf(void) {
    printf("test_invalid_gbnf...\n");
    
    ethervox_grammar_t* grammar = NULL;
    
    // Empty GBNF
    ethervox_result_t result = ethervox_grammar_compile("", &grammar);
    CHECK(result == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(grammar == NULL);
    
    printf("  PASS\n");
    return 0;
}

static int test_invalid_json_schema(void) {
    printf("test_invalid_json_schema...\n");
    
    ethervox_grammar_t* grammar = NULL;
    
    // Malformed JSON
    ethervox_result_t result = ethervox_grammar_from_json_schema("{invalid json}", &grammar);
    CHECK(result == ETHERVOX_ERROR_INVALID_ARGUMENT);
    CHECK(grammar == NULL);
    
    // Empty JSON
    result = ethervox_grammar_from_json_schema("{}", &grammar);
    // Empty schema should either work or fail gracefully
    // llama.cpp may handle it, so just check it doesn't crash
    if (result == ETHERVOX_SUCCESS && grammar != NULL) {
        ethervox_grammar_free(grammar);
    }
    
    printf("  PASS\n");
    return 0;
}

static int test_tool_call_schema(void) {
    printf("test_tool_call_schema...\n");
    
    // Realistic tool call schema (calculator tool)
    const char* schema = 
        "{"
        "  \"type\": \"object\","
        "  \"properties\": {"
        "    \"expression\": {"
        "      \"type\": \"string\","
        "      \"description\": \"Mathematical expression to evaluate\""
        "    }"
        "  },"
        "  \"required\": [\"expression\"]"
        "}";
    
    ethervox_grammar_t* grammar = NULL;
    CHECK_SUCCESS(ethervox_grammar_from_json_schema(schema, &grammar));
    CHECK(grammar != NULL);
    
    const char* gbnf = ethervox_grammar_get_source(grammar);
    CHECK(gbnf != NULL);
    printf("    Tool call GBNF (%zu bytes)\n", strlen(gbnf));
    
    ethervox_grammar_free(grammar);
    
    printf("  PASS\n");
    return 0;
}

int main(void) {
    printf("Running grammar tests...\n\n");
    
    int failed = 0;
    
    failed += test_compile_simple_gbnf();
    failed += test_compile_json_gbnf();
    failed += test_json_schema_simple_object();
    failed += test_json_schema_string_enum();
    failed += test_json_schema_nested_object();
    failed += test_json_schema_array();
    failed += test_json_schema_oneof();
    failed += test_null_safety();
    failed += test_invalid_gbnf();
    failed += test_invalid_json_schema();
    failed += test_tool_call_schema();
    
    printf("\n");
    if (failed == 0) {
        printf("All tests PASSED\n");
    } else {
        printf("%d test(s) FAILED\n", failed);
    }
    
    return failed;
}
