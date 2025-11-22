/**
 * @file compute_tools_registry.c
 * @brief Registration helper for all compute tools
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/compute_tools.h"
#include <stdlib.h>

int ethervox_compute_tools_register_all(ethervox_tool_registry_t* registry) {
    if (!registry) {
        return -1;
    }
    
    int count = 0;
    
    // Register calculator
    const ethervox_tool_t* calc = ethervox_tool_calculator();
    if (ethervox_tool_registry_add(registry, calc) == 0) {
        count++;
    }
    
    // Register percentage calculator
    const ethervox_tool_t* percent = ethervox_tool_percentage();
    if (ethervox_tool_registry_add(registry, percent) == 0) {
        count++;
    }
    
    // Register time query tools
    const ethervox_tool_t* time_current = ethervox_tool_time_get_current();
    if (ethervox_tool_registry_add(registry, time_current) == 0) {
        count++;
    }
    
    const ethervox_tool_t* time_date = ethervox_tool_time_get_date();
    if (ethervox_tool_registry_add(registry, time_date) == 0) {
        count++;
    }
    
    const ethervox_tool_t* time_day = ethervox_tool_time_get_day_of_week();
    if (ethervox_tool_registry_add(registry, time_day) == 0) {
        count++;
    }
    
    const ethervox_tool_t* time_week = ethervox_tool_time_get_week_number();
    if (ethervox_tool_registry_add(registry, time_week) == 0) {
        count++;
    }
    
    // TODO: Register unit converter when implemented
    // const ethervox_tool_t* units = ethervox_tool_unit_converter();
    // if (ethervox_tool_registry_add(registry, units) == 0) {
    //     count++;
    // }
    
    return count;
}
