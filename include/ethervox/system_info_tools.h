/**
 * @file system_info_tools.h
 * @brief System information tools API
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#ifndef ETHERVOX_SYSTEM_INFO_TOOLS_H
#define ETHERVOX_SYSTEM_INFO_TOOLS_H

#include "ethervox/governor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register system info tools with the tool registry
 * 
 * Registers the following tools:
 * - system_version: Get version, build info, and git commit hash
 * - system_capabilities: Get system capabilities and limits
 * 
 * @param registry Tool registry to register with
 * @return 0 on success, -1 on failure
 */
int ethervox_system_info_tools_register(ethervox_tool_registry_t* registry);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_SYSTEM_INFO_TOOLS_H
