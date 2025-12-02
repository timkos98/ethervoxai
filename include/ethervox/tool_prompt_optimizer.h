/**
 * @file tool_prompt_optimizer.h
 * @brief Self-optimizing tool prompt system
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#ifndef ETHERVOX_TOOL_PROMPT_OPTIMIZER_H
#define ETHERVOX_TOOL_PROMPT_OPTIMIZER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct ethervox_governor ethervox_governor_t;

/**
 * Run the tool prompt optimization routine
 * 
 * This asks the LLM to write its own tool usage instructions and examples.
 * The generated prompts are saved to .ethervox_tool_prompts_<model>.json
 * and will be auto-loaded on next startup.
 * 
 * @param governor Governor instance with loaded model
 * @param model_path Path to the model file (used for naming output file)
 * @return 0 on success, negative on error
 */
int ethervox_optimize_tool_prompts(ethervox_governor_t* governor, const char* model_path);

/**
 * Load previously optimized prompts for a model
 * 
 * Attempts to load model-specific prompts from .ethervox_tool_prompts_<model>.json
 * 
 * @param model_path Path to the model file
 * @param instruction_out Buffer to receive instruction text
 * @param instruction_size Size of instruction buffer
 * @param examples_out Buffer to receive examples text
 * @param examples_size Size of examples buffer
 * @return 0 if loaded successfully, negative if file not found or parse error
 */
int ethervox_load_optimized_prompts(
    const char* model_path,
    char* instruction_out,
    size_t instruction_size,
    char* examples_out,
    size_t examples_size
);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_TOOL_PROMPT_OPTIMIZER_H
