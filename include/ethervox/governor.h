/**
 * @file governor.h
 * @brief Governor orchestration system for LLM-driven tool execution
 *
 * The Governor is the central orchestrator that manages iterative interaction
 * between the LLM (Phi-3.5-mini-instruct) and compute/action tools until
 * confidence threshold is met.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#ifndef ETHERVOX_GOVERNOR_H
#define ETHERVOX_GOVERNOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tool execution function signature
 * 
 * @param args_json JSON string with tool parameters
 * @param result Output: JSON result string (caller must free)
 * @param error Output: Error message if execution fails (caller must free)
 * @return 0 on success, negative on error
 */
typedef int (*ethervox_tool_execute_fn)(
    const char* args_json,
    char** result,
    char** error
);

/**
 * Tool definition
 */
typedef struct {
    char name[64];                      // Tool name (e.g., "calculator_compute")
    char description[256];              // Human-readable description
    char parameters_json_schema[1024]; // JSON schema for parameters
    
    ethervox_tool_execute_fn execute;   // Execution function
    
    bool is_deterministic;              // Same input = same output
    bool requires_confirmation;         // Ask user before executing
    bool is_stateful;                   // Modifies system state
    float estimated_latency_ms;         // Expected execution time
} ethervox_tool_t;

/**
 * Tool registry
 */
typedef struct {
    ethervox_tool_t* tools;             // Array of tools
    uint32_t tool_count;                // Number of registered tools
    uint32_t capacity;                  // Allocated capacity
} ethervox_tool_registry_t;

/**
 * Governor execution status
 */
typedef enum {
    ETHERVOX_GOVERNOR_SUCCESS,            // Confidence met, response ready
    ETHERVOX_GOVERNOR_NEED_CLARIFICATION, // Need more info from user
    ETHERVOX_GOVERNOR_TIMEOUT,            // Exceeded iteration/time limits
    ETHERVOX_GOVERNOR_ERROR,              // Execution error
    ETHERVOX_GOVERNOR_USER_DENIED         // User denied tool execution
} ethervox_governor_status_t;

/**
 * Confidence metrics from LLM response
 */
typedef struct {
    float confidence;                   // 0.0-1.0
    uint32_t iteration_count;           // Iterations used
    uint32_t tool_calls_made;           // Tools called
    bool has_explicit_confidence;       // LLM provided <confidence> tag
} ethervox_confidence_metrics_t;

/**
 * Governor progress event types
 */
typedef enum {
    ETHERVOX_GOVERNOR_EVENT_ITERATION_START,   // Starting new iteration
    ETHERVOX_GOVERNOR_EVENT_THINKING,          // Generating response from LLM
    ETHERVOX_GOVERNOR_EVENT_TOOL_CALL,         // Calling a tool
    ETHERVOX_GOVERNOR_EVENT_TOOL_RESULT,       // Tool execution complete
    ETHERVOX_GOVERNOR_EVENT_TOOL_ERROR,        // Tool execution failed
    ETHERVOX_GOVERNOR_EVENT_CONFIDENCE_UPDATE, // Confidence level changed
    ETHERVOX_GOVERNOR_EVENT_COMPLETE           // Final answer ready
} ethervox_governor_event_type_t;

/**
 * Progress callback for Governor execution
 * Allows UI to display intermediate reasoning steps
 * 
 * @param event_type Type of event
 * @param message Human-readable message
 * @param user_data User data passed to execute function
 */
typedef void (*ethervox_governor_progress_callback)(
    ethervox_governor_event_type_t event_type,
    const char* message,
    void* user_data
);

/**
 * Governor configuration
 */
typedef struct {
    float confidence_threshold;         // Stop when confidence >= this (default: 0.85)
    uint32_t max_iterations;            // Max tool execution loops (default: 5)
    uint32_t max_tool_calls_per_iteration; // Max tool calls per iteration (default: 10)
    uint32_t timeout_seconds;           // Total execution timeout (default: 30)
    uint32_t max_tokens_per_response;   // Max tokens to generate per LLM response (default: 2048)
} ethervox_governor_config_t;

/**
 * Governor state (opaque)
 */
typedef struct ethervox_governor ethervox_governor_t;

// ============================================================================
// Tool Registry Functions
// ============================================================================

/**
 * Initialize tool registry
 */
int ethervox_tool_registry_init(ethervox_tool_registry_t* registry, uint32_t initial_capacity);

/**
 * Register a tool
 */
int ethervox_tool_registry_add(ethervox_tool_registry_t* registry, const ethervox_tool_t* tool);

/**
 * Find tool by name
 */
const ethervox_tool_t* ethervox_tool_registry_find(
    const ethervox_tool_registry_t* registry,
    const char* name
);

/**
 * Build system prompt with all tools
 */
int ethervox_tool_registry_build_system_prompt(
    const ethervox_tool_registry_t* registry,
    char* buffer,
    size_t buffer_size
);

/**
 * Cleanup registry
 */
void ethervox_tool_registry_cleanup(ethervox_tool_registry_t* registry);

// ============================================================================
// Governor Functions
// ============================================================================

/**
 * Initialize Governor with configuration
 * 
 * @param governor Output: Governor instance (caller must free with cleanup)
 * @param config Governor configuration (NULL for defaults)
 * @param registry Tool registry
 * @return 0 on success, negative on error
 */
int ethervox_governor_init(
    ethervox_governor_t** governor,
    const ethervox_governor_config_t* config,
    ethervox_tool_registry_t* tool_registry
);

/**
 * Load Phi-3.5-mini-instruct model and process system prompt
 * 
 * @param governor Governor instance
 * @param model_path Path to GGUF model file
 * @return 0 on success, negative on error
 */
int ethervox_governor_load_model(
    ethervox_governor_t* governor,
    const char* model_path
);

/**
 * Execute user query with tool orchestration
 * 
 * @param governor Governor instance
 * @param user_query User's natural language query
 * @param response Output: Final response (caller must free)
 * @param error Output: Error message if failed (caller must free)
 * @param metrics Output: Confidence metrics (optional, can be NULL)
 * @param progress_callback Progress callback for UI updates (optional, can be NULL)
 * @param user_data User data passed to progress callback (optional, can be NULL)
 * @return Governor status
 */
ethervox_governor_status_t ethervox_governor_execute(
    ethervox_governor_t* governor,
    const char* user_query,
    char** response,
    char** error,
    ethervox_confidence_metrics_t* metrics,
    ethervox_governor_progress_callback progress_callback,
    void (*token_callback)(const char* token, void* user_data),
    void* user_data
);

/**
 * Get iteration count from last execution (for debugging)
 */
uint32_t ethervox_governor_get_last_iteration_count(ethervox_governor_t* governor);

/**
 * Cleanup Governor (frees resources)
 */
void ethervox_governor_cleanup(ethervox_governor_t* governor);

// ============================================================================
// Default Configuration
// ============================================================================

static inline ethervox_governor_config_t ethervox_governor_default_config(void) {
    ethervox_governor_config_t config = {
        .confidence_threshold = 0.85f,
        .max_iterations = 5,
        .max_tool_calls_per_iteration = 10,
        .timeout_seconds = 30,
        .max_tokens_per_response = 2048
    };
    return config;
}

/**
 * Get the tool registry from Governor
 * 
 * @param governor Governor instance
 * @return Tool registry pointer, or NULL if invalid
 */
ethervox_tool_registry_t* ethervox_governor_get_registry(ethervox_governor_t* governor);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_GOVERNOR_H
