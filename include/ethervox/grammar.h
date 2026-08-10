/**
 * @file grammar.h
 * @brief Grammar-constrained decoding for structured LLM outputs
 *
 * Provides GBNF (Grammar-Based Next-token Filtering) support to ensure LLM outputs
 * are valid JSON/structured formats. This eliminates ~5-15% of generations that
 * produce unparseable JSON for tool calls.
 *
 * Key features:
 * - Compile GBNF grammars from source strings
 * - Convert JSON Schema to GBNF (killer feature for tool calls)
 * - Lazy grammars: produce free text then constrained output
 * - Grammar deadlock detection (empty candidate set)
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#ifndef ETHERVOX_GRAMMAR_H
#define ETHERVOX_GRAMMAR_H

#include "ethervox/error.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque grammar handle
 *
 * Represents a compiled GBNF grammar that can be used to constrain generation.
 * Thread-safe for read-only operations after compilation.
 */
typedef struct ethervox_grammar ethervox_grammar_t;

/**
 * Compile a GBNF grammar from source
 *
 * Compiles a GBNF (Grammar-Based Next-token Filtering) grammar from a string.
 * See external/llama.cpp/grammars/README.md for GBNF syntax.
 *
 * Example GBNF:
 * ```
 * root ::= object
 * object ::= "{" ws members ws "}"
 * members ::= pair ("," ws pair)*
 * pair ::= string ":" ws value
 * string ::= "\"" [^"]* "\""
 * value ::= string | number | object
 * number ::= [0-9]+
 * ws ::= [ \t\n]*
 * ```
 *
 * @param gbnf_source GBNF grammar source string (null-terminated)
 * @param out Receives compiled grammar (caller must free via ethervox_grammar_free)
 * @return ETHERVOX_SUCCESS, ETHERVOX_ERROR_INVALID_ARGUMENT if source is NULL/invalid,
 *         ETHERVOX_ERROR_FAILED if grammar compilation fails
 *
 * @note The compiled grammar is thread-safe for read-only use
 */
ethervox_result_t ethervox_grammar_compile(
    const char* gbnf_source,
    ethervox_grammar_t** out
);

/**
 * Convert JSON Schema to GBNF grammar
 *
 * Converts a JSON Schema to a GBNF grammar that constrains generation to valid JSON
 * matching the schema. This is the primary way to ensure tool calls produce valid JSON.
 *
 * Supported JSON Schema features:
 * - Objects with required/optional properties
 * - Strings with maxLength, enum, pattern (basic regex subset)
 * - Integers, numbers, booleans
 * - Arrays with maxItems
 * - oneOf for union types (e.g., tool-call variant selection)
 *
 * Example schema:
 * ```json
 * {
 *   "type": "object",
 *   "properties": {
 *     "name": {"type": "string"},
 *     "age": {"type": "integer"}
 *   },
 *   "required": ["name"]
 * }
 * ```
 *
 * @param schema_json JSON Schema string (null-terminated)
 * @param out Receives compiled grammar (caller must free via ethervox_grammar_free)
 * @return ETHERVOX_SUCCESS, ETHERVOX_ERROR_INVALID_ARGUMENT if schema is NULL/invalid,
 *         ETHERVOX_ERROR_FAILED if conversion fails
 *
 * @note Only a subset of JSON Schema is supported (sufficient for tool calls)
 * @note Complex regex patterns in "pattern" are not fully supported
 */
ethervox_result_t ethervox_grammar_from_json_schema(
    const char* schema_json,
    ethervox_grammar_t** out
);

/**
 * Free a compiled grammar
 *
 * Releases all resources associated with a compiled grammar.
 *
 * @param grammar Grammar to free (NULL is safe)
 */
void ethervox_grammar_free(ethervox_grammar_t* grammar);

/**
 * Get GBNF source from compiled grammar (for debugging)
 *
 * Returns the GBNF source that was used to compile this grammar.
 * Useful for debugging grammar conversion issues.
 *
 * @param grammar Compiled grammar
 * @return GBNF source string (owned by grammar, do not free), or NULL if grammar is NULL
 */
const char* ethervox_grammar_get_source(const ethervox_grammar_t* grammar);

#ifdef __cplusplus
}
#endif

#endif /* ETHERVOX_GRAMMAR_H */
