/**
 * @file test_tool_catalogue.c
 * @brief Golden + behaviour tests for the tool catalogue loader (TASK-C2.6a)
 *
 * Golden test: the compute_tools group's name/description/schema, now loaded
 * from tools/catalogue/compute_tools.json, must be byte-identical to the
 * hardcoded C literals they replaced (16-TOOLS.md's whole point - description
 * text is the prompt, and it must not drift).
 *
 * Copyright (c) 2024-2026 EthervoxAI Team
 * SPDX-License-Identifier: LicenseRef-EthervoxAI-Proprietary
 */

#include "ethervox/compute_tools.h"
#include "ethervox/system_info_tools.h"
#include "ethervox/tool_catalogue.h"
#include "ethervox/error.h"
#include "unit/test_utils.h"
#include <stdio.h>
#include <string.h>

static void test_golden_calculator(void) {
    const ethervox_tool_t* t = ethervox_tool_calculator();
    CHECK(strcmp(t->name, "calculator_compute") == 0);
    CHECK(strcmp(t->description,
        "Compute ANY math calculation - use for all arithmetic, don't calculate mentally. "
        "Supports +, -, *, /, ^, sqrt, abs, parentheses.") == 0);
    CHECK(strcmp(t->parameters_json_schema,
        "{\"type\":\"object\",\"properties\":{"
        "\"expression\":{\"type\":\"string\",\"description\":\"Mathematical expression to evaluate\"},"
        "\"decimal_places\":{\"type\":\"integer\",\"description\":\"Number of decimal places (0-15, default: 2)\",\"default\":2}"
        "},\"required\":[\"expression\"]}") == 0);
}

static void test_golden_percentage(void) {
    const ethervox_tool_t* t = ethervox_tool_percentage();
    CHECK(strcmp(t->name, "percentage_calculate") == 0);
    CHECK(strcmp(t->description,
        "Calculate percentages for tips, tax, discounts (operations: of, increase, decrease, is_what_percent)") == 0);
    CHECK(strcmp(t->parameters_json_schema,
        "{\"type\":\"object\",\"properties\":{"
        "\"value\":{\"type\":\"number\",\"description\":\"The value to calculate percentage on\"},"
        "\"percentage\":{\"type\":\"number\",\"description\":\"The percentage amount\"},"
        "\"operation\":{\"type\":\"string\",\"enum\":[\"of\",\"increase\",\"decrease\",\"is_what_percent\"],\"description\":\"Operation to perform\"},"
        "\"decimal_places\":{\"type\":\"integer\",\"description\":\"Number of decimal places (0-15, default: 2)\",\"default\":2}"
        "},\"required\":[\"value\",\"percentage\",\"operation\"]}") == 0);
}

static void test_golden_time_tools(void) {
    const ethervox_tool_t* t;

    t = ethervox_tool_time_get_current();
    CHECK(strcmp(t->name, "get_time") == 0);
    CHECK(strcmp(t->description,
        "Get current time. Use when user asks 'what time is it' or needs to know the current time.") == 0);
    CHECK(strcmp(t->parameters_json_schema, "{}") == 0);

    t = ethervox_tool_time_get_date();
    CHECK(strcmp(t->name, "get_date") == 0);
    CHECK(strcmp(t->description,
        "Get current date (day/month/year). Use to get today's date. Use when user asks 'what's the date', "
        "needs today's date for calculations, or asks about days until/since an event.") == 0);
    CHECK(strcmp(t->parameters_json_schema, "{}") == 0);

    t = ethervox_tool_time_get_day_of_week();
    CHECK(strcmp(t->name, "get_day") == 0);
    CHECK(strcmp(t->description,
        "Get the current day of the week. Use when user asks 'what day is it' or 'what day of the week'.") == 0);
    CHECK(strcmp(t->parameters_json_schema, "{}") == 0);

    t = ethervox_tool_time_get_week_number();
    CHECK(strcmp(t->name, "time_get_week_number") == 0);
    CHECK(strcmp(t->description,
        "Get the current week number of the year. Use when user asks 'what week is it' or 'what week number'.") == 0);
    CHECK(strcmp(t->parameters_json_schema, "{}") == 0);
}

// Loader behaviour, independent of the embedded compute_tools.json above.
static void test_golden_system_info(void) {
    ethervox_tool_registry_t registry;
    CHECK(ethervox_is_success(ethervox_tool_registry_init(&registry, 8)));
    CHECK(ethervox_is_success(ethervox_system_info_tools_register(&registry)));

    const ethervox_tool_t* t = ethervox_tool_registry_find(&registry, "system_version");
    CHECK(t != NULL);
    CHECK(strcmp(t->description,
        "Get EthervoxAI version and build information including version number, git commit hash, "
        "build type, and platform.") == 0);
    CHECK(strcmp(t->parameters_json_schema, "{\"type\":\"object\",\"properties\":{},\"required\":[]}") == 0);

    t = ethervox_tool_registry_find(&registry, "system_capabilities");
    CHECK(t != NULL);
    CHECK(strcmp(t->description,
        "Get system capabilities and configuration limits including max languages, plugins, audio "
        "settings, and platform type.") == 0);
    CHECK(strcmp(t->parameters_json_schema, "{\"type\":\"object\",\"properties\":{},\"required\":[]}") == 0);

    ethervox_tool_registry_cleanup(&registry);
}
static void test_loader_not_found(void) {
    ethervox_tool_t tool = {0};
    ethervox_result_t r = ethervox_tool_catalogue_load(
        "[{\"name\":\"a\",\"profiles\":[\"DESKTOP\"],\"description\":\"d\",\"schema\":{}}]",
        "does_not_exist", "DESKTOP", &tool);
    CHECK(r == ETHERVOX_ERROR_NOT_FOUND);
}

static void test_loader_profile_excluded(void) {
    ethervox_tool_t tool = {0};
    ethervox_result_t r = ethervox_tool_catalogue_load(
        "[{\"name\":\"a\",\"profiles\":[\"EDGE\"],\"description\":\"d\",\"schema\":{}}]",
        "a", "DESKTOP", &tool);
    CHECK(r == ETHERVOX_ERROR_NOT_SUPPORTED);
}

static void test_loader_unknown_profile_is_error(void) {
    ethervox_tool_t tool = {0};
    ethervox_result_t r = ethervox_tool_catalogue_load(
        "[{\"name\":\"a\",\"profiles\":[\"NOT_A_PROFILE\"],\"description\":\"d\",\"schema\":{}}]",
        "a", "DESKTOP", &tool);
    CHECK(r == ETHERVOX_ERROR_INVALID_ARGUMENT);
}

static void test_loader_success(void) {
    ethervox_tool_t tool = {0};
    ethervox_result_t r = ethervox_tool_catalogue_load(
        "[{\"name\":\"a\",\"profiles\":[\"DESKTOP\",\"WORKSPACE\"],\"description\":\"hello\","
        "\"schema\":{\"type\":\"object\"}}]",
        "a", "DESKTOP", &tool);
    CHECK(ethervox_is_success(r));
    CHECK(strcmp(tool.name, "a") == 0);
    CHECK(strcmp(tool.description, "hello") == 0);
    CHECK(strcmp(tool.parameters_json_schema, "{\"type\":\"object\"}") == 0);
}

int main(void) {
    RUN_TEST(test_golden_calculator);
    RUN_TEST(test_golden_percentage);
    RUN_TEST(test_golden_time_tools);
    RUN_TEST(test_golden_system_info);
    RUN_TEST(test_loader_not_found);
    RUN_TEST(test_loader_profile_excluded);
    RUN_TEST(test_loader_unknown_profile_is_error);
    RUN_TEST(test_loader_success);
    printf("\nAll tool_catalogue tests passed.\n");
    return 0;
}
