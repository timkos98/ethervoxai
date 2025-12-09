# SPDX-License-Identifier: CC-BY-NC-SA-4.0

# Error handling tests
add_executable(test_error unit/test_error.c)
target_link_libraries(test_error ethervoxai)
target_include_directories(test_error PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ErrorHandling COMMAND test_error)
set_tests_properties(ErrorHandling PROPERTIES TIMEOUT 30 LABELS "unit")

# Context overflow management tests
add_executable(test_context_overflow unit/test_context_overflow.c)
target_link_libraries(test_context_overflow ethervoxai)
target_include_directories(test_context_overflow PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ContextOverflow COMMAND test_context_overflow)
set_tests_properties(ContextOverflow PROPERTIES TIMEOUT 30 LABELS "unit")

# Audio core tests
add_executable(test_audio_core unit/test_audio_core.c)
target_link_libraries(test_audio_core ethervoxai)
target_include_directories(test_audio_core PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME AudioCore COMMAND test_audio_core)
set_tests_properties(AudioCore PROPERTIES TIMEOUT 30 LABELS "unit")

# Config tests
add_executable(test_config unit/test_config.c)
target_link_libraries(test_config ethervoxai)
target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME Config COMMAND test_config)
set_tests_properties(Config PROPERTIES TIMEOUT 30 LABELS "unit")

# Memory tools tests
add_executable(test_memory_tools unit/test_memory_tools.c)
target_link_libraries(test_memory_tools ethervoxai)
target_include_directories(test_memory_tools PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME MemoryTools COMMAND test_memory_tools)
set_tests_properties(MemoryTools PROPERTIES TIMEOUT 30 LABELS "unit")

# Secret mode / privacy mode tests
add_executable(test_secret_mode unit/test_secret_mode.c)
target_link_libraries(test_secret_mode ethervoxai)
target_include_directories(test_secret_mode PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME SecretMode COMMAND test_secret_mode)
set_tests_properties(SecretMode PROPERTIES TIMEOUT 30 LABELS "unit")

# Adaptive memory tests (corrections and patterns)
add_executable(test_adaptive_memory unit/test_adaptive_memory.c)
target_link_libraries(test_adaptive_memory ethervoxai)
target_include_directories(test_adaptive_memory PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME AdaptiveMemory COMMAND test_adaptive_memory)
set_tests_properties(AdaptiveMemory PROPERTIES TIMEOUT 30 LABELS "unit")

# Plugin manager tests
add_executable(test_plugin_manager unit/test_plugin_manager.c)
target_link_libraries(test_plugin_manager ethervoxai)
target_include_directories(test_plugin_manager PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME PluginManager COMMAND test_plugin_manager)
set_tests_properties(PluginManager PROPERTIES TIMEOUT 30 LABELS "unit")

# KV cache management tests (conversation reset, position tracking)
add_executable(test_kv_cache_management unit/test_kv_cache_management.c)
target_link_libraries(test_kv_cache_management ethervoxai)
target_include_directories(test_kv_cache_management PRIVATE ${CMAKE_SOURCE_DIR}/include)
# This test requires a model file, so it's not added as a ctest
# Run manually: ./tests/test_kv_cache_management [model_path]

# Memory archive tests
add_executable(test_memory_archive unit/test_memory_archive.c)
target_link_libraries(test_memory_archive ethervoxai)
target_include_directories(test_memory_archive PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME MemoryArchive COMMAND test_memory_archive)
set_tests_properties(MemoryArchive PROPERTIES TIMEOUT 30 LABELS "unit")

# File tools tests (Phase 1: Binary detection, token estimation, error handling)
add_executable(test_file_tools unit/test_file_tools.c)
target_link_libraries(test_file_tools ethervoxai)
target_include_directories(test_file_tools PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME FileTools COMMAND test_file_tools)
set_tests_properties(FileTools PROPERTIES TIMEOUT 60 LABELS "unit")

# File append tool tests
add_executable(test_file_append unit/test_file_append.c)
target_link_libraries(test_file_append ethervoxai)
target_include_directories(test_file_append PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME FileAppend COMMAND test_file_append)
set_tests_properties(FileAppend PROPERTIES TIMEOUT 30 LABELS "unit;file_tools")

# Unit conversion tests
add_executable(test_unit_conversion unit/test_unit_conversion.c)
target_link_libraries(test_unit_conversion ethervoxai m)
target_include_directories(test_unit_conversion PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME UnitConversion COMMAND test_unit_conversion)
set_tests_properties(UnitConversion PROPERTIES TIMEOUT 30 LABELS "unit;unit_conversion")

# Tool Manifest System tests
add_executable(test_tool_manifest test_tool_manifest.c)
target_link_libraries(test_tool_manifest ethervoxai)
target_include_directories(test_tool_manifest PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ToolManifest COMMAND test_tool_manifest)
set_tests_properties(ToolManifest PROPERTIES TIMEOUT 30 LABELS "unit;tool_manifest")

# Tool Manifest System benchmarks (not a test, run manually)
add_executable(benchmark_tool_manifest benchmark_tool_manifest.c)
target_link_libraries(benchmark_tool_manifest ethervoxai)
target_include_directories(benchmark_tool_manifest PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Mobile optimization features tests (minimal mode, secret mode)
add_executable(test_mobile_optimization unit/test_mobile_optimization.c)
target_link_libraries(test_mobile_optimization ethervoxai)
target_include_directories(test_mobile_optimization PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME MobileOptimization COMMAND test_mobile_optimization)
set_tests_properties(MobileOptimization PROPERTIES TIMEOUT 30 LABELS "unit;mobile")
