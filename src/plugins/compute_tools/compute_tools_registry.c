/**
 * @file compute_tools_registry.c
 * @brief Registration helper for all compute tools
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/compute_tools.h"
#include "ethervox/error.h"
#include <stdlib.h>

ethervox_result_t ethervox_compute_tools_register_all(ethervox_tool_registry_t* registry) {
    ETHERVOX_CHECK_PTR(registry);
    
    // Register calculator
    const ethervox_tool_t* calc = ethervox_tool_calculator();
    ETHERVOX_CHECK(ethervox_tool_registry_add(registry, calc));
    
    // Register percentage calculator
    const ethervox_tool_t* percent = ethervox_tool_percentage();
    ETHERVOX_CHECK(ethervox_tool_registry_add(registry, percent));
    
    // Register time query tools
    const ethervox_tool_t* time_current = ethervox_tool_time_get_current();
    ETHERVOX_CHECK(ethervox_tool_registry_add(registry, time_current));
    
    const ethervox_tool_t* time_date = ethervox_tool_time_get_date();
    ETHERVOX_CHECK(ethervox_tool_registry_add(registry, time_date));
    
    const ethervox_tool_t* time_day = ethervox_tool_time_get_day_of_week();
    ETHERVOX_CHECK(ethervox_tool_registry_add(registry, time_day));
    
    const ethervox_tool_t* time_week = ethervox_tool_time_get_week_number();
    ETHERVOX_CHECK(ethervox_tool_registry_add(registry, time_week));
    
    // TODO: Register unit converter when implemented
    // const ethervox_tool_t* units = ethervox_tool_unit_converter();
    // ETHERVOX_CHECK(ethervox_tool_registry_add(registry, units));
    
    return ETHERVOX_SUCCESS;
}
