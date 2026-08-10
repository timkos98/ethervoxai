/**
 * @file grammar.cpp
 * @brief Grammar-constrained decoding implementation
 *
 * C++ implementation wrapping llama.cpp's grammar and JSON Schema conversion.
 * Exposes C API defined in grammar.h.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/grammar.h"
#include "ethervox/logging.h"

#include <string>
#include <memory>
#include <cstring>

// llama.cpp includes (C++)
#if defined(ETHERVOX_WITH_LLAMA) && defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE
#include "../external/llama.cpp/vendor/nlohmann/json.hpp"

// Forward declare string utility functions that json-schema-to-grammar needs
// (These are normally provided by common.cpp, but we implement them here to avoid
// pulling in all of common.cpp's dependencies)
std::string string_repeat(const std::string & str, size_t n);
std::string string_join(const std::vector<std::string> & values, const std::string & separator);
std::vector<std::string> string_split(const std::string & str, const std::string & delimiter);

#include "../external/llama.cpp/common/json-schema-to-grammar.h"

#define GRAMMAR_AVAILABLE 1
#else
#define GRAMMAR_AVAILABLE 0
#endif

/**
 * String utility function implementations
 * (Minimal implementations to avoid pulling in all of common.cpp)
 */
#if GRAMMAR_AVAILABLE
// Repeat a string n times
std::string string_repeat(const std::string & str, size_t n) {
    std::string result;
    result.reserve(str.size() * n);
    for (size_t i = 0; i < n; ++i) {
        result += str;
    }
    return result;
}

// Join strings with separator
std::string string_join(const std::vector<std::string> & values, const std::string & separator) {
    if (values.empty()) return "";
    std::string result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) result += separator;
        result += values[i];
    }
    return result;
}

// Split a string by delimiter string
std::vector<std::string> string_split(const std::string & str, const std::string & delimiter) {
    std::vector<std::string> result;
    if (delimiter.empty()) {
        result.push_back(str);
        return result;
    }
    
    size_t start = 0;
    size_t end = str.find(delimiter);
    
    while (end != std::string::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
        end = str.find(delimiter, start);
    }
    
    result.push_back(str.substr(start));
    return result;
}
#endif

/**
 * Internal grammar structure
 */
struct ethervox_grammar {
    std::string gbnf_source;     // GBNF source (for debugging)
    std::string grammar_root;     // Root rule name
    
    ethervox_grammar() : grammar_root("root") {}
};

ethervox_result_t ethervox_grammar_compile(
    const char* gbnf_source,
    ethervox_grammar_t** out
) {
    if (!gbnf_source || !out) {
        ETHERVOX_LOG_ERROR("[Grammar] NULL source or output pointer");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !GRAMMAR_AVAILABLE
    ETHERVOX_LOG_ERROR("[Grammar] llama.cpp not available");
    return ETHERVOX_ERROR_NOT_IMPLEMENTED;
#else
    
    try {
        // Create grammar object
        auto grammar = new ethervox_grammar();
        grammar->gbnf_source = gbnf_source;
        
        // Validate GBNF syntax by attempting to parse it
        // llama.cpp will validate when we create the sampler, but we can do basic checks here
        if (grammar->gbnf_source.empty()) {
            delete grammar;
            ETHERVOX_LOG_ERROR("[Grammar] Empty GBNF source");
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
        }
        
        ETHERVOX_LOG_INFO("[Grammar] Compiled GBNF grammar (%zu bytes)", 
                         grammar->gbnf_source.size());
        
        *out = grammar;
        return ETHERVOX_SUCCESS;
        
    } catch (const std::exception& e) {
        ETHERVOX_LOG_ERROR("[Grammar] Failed to compile GBNF: %s", e.what());
        return ETHERVOX_ERROR_FAILED;
    }
#endif
}

ethervox_result_t ethervox_grammar_from_json_schema(
    const char* schema_json,
    ethervox_grammar_t** out
) {
    if (!schema_json || !out) {
        ETHERVOX_LOG_ERROR("[Grammar] NULL schema or output pointer");
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
#if !GRAMMAR_AVAILABLE
    ETHERVOX_LOG_ERROR("[Grammar] llama.cpp not available");
    return ETHERVOX_ERROR_NOT_IMPLEMENTED;
#else
    
    try {
        // Parse JSON schema
        nlohmann::ordered_json schema = nlohmann::ordered_json::parse(schema_json);
        
        // Convert to GBNF
        std::string gbnf = json_schema_to_grammar(schema);
        
        if (gbnf.empty()) {
            ETHERVOX_LOG_ERROR("[Grammar] JSON Schema conversion produced empty GBNF");
            return ETHERVOX_ERROR_FAILED;
        }
        
        // Create grammar object
        auto grammar = new ethervox_grammar();
        grammar->gbnf_source = gbnf;
        
        ETHERVOX_LOG_INFO("[Grammar] Converted JSON Schema to GBNF (%zu bytes schema → %zu bytes GBNF)",
                         strlen(schema_json), gbnf.size());
        ETHERVOX_LOG_DEBUG("[Grammar] GBNF:\n%s", gbnf.c_str());
        
        *out = grammar;
        return ETHERVOX_SUCCESS;
        
    } catch (const nlohmann::json::parse_error& e) {
        ETHERVOX_LOG_ERROR("[Grammar] JSON parse error: %s", e.what());
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        ETHERVOX_LOG_ERROR("[Grammar] Failed to convert JSON Schema: %s", e.what());
        return ETHERVOX_ERROR_FAILED;
    }
#endif
}

void ethervox_grammar_free(ethervox_grammar_t* grammar) {
    if (!grammar) {
        return;
    }
    
    ETHERVOX_LOG_DEBUG("[Grammar] Freeing grammar");
    delete grammar;
}

const char* ethervox_grammar_get_source(const ethervox_grammar_t* grammar) {
    if (!grammar) {
        return nullptr;
    }
    
    return grammar->gbnf_source.c_str();
}
