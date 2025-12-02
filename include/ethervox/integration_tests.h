/**
 * @file integration_tests.h
 * @brief Interactive /test command for integration testing
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#ifndef ETHERVOX_INTEGRATION_TESTS_H
#define ETHERVOX_INTEGRATION_TESTS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run comprehensive integration tests
 * 
 * Tests major features:
 * - Memory store operations (add, retrieve, search)
 * - Adaptive memory (corrections, patterns)
 * - Context window management
 * - Hash table indexing
 * - System prompt generation
 * - Memory export/import
 * 
 * Can be invoked via /test command in the main program
 */
void run_integration_tests(void);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_INTEGRATION_TESTS_H
