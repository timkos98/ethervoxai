#ifndef ETHERVOX_GET_TOOL_INFO_H
#define ETHERVOX_GET_TOOL_INFO_H

#include "governor.h"
#include "tool_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the manifest registry for get_tool_info to query
 */
void ethervox_get_tool_info_set_manifest(tool_manifest_registry_t* manifest);

/**
 * Register the get_tool_info meta-tool
 * This tool allows the LLM to query tool schemas on-demand
 */
int ethervox_get_tool_info_register(ethervox_tool_registry_t* registry);

#ifdef __cplusplus
}
#endif

#endif // ETHERVOX_GET_TOOL_INFO_H
