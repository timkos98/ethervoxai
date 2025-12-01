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

# Plugin manager tests
add_executable(test_plugin_manager unit/test_plugin_manager.c)
target_link_libraries(test_plugin_manager ethervoxai)
target_include_directories(test_plugin_manager PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME PluginManager COMMAND test_plugin_manager)
set_tests_properties(PluginManager PROPERTIES TIMEOUT 30 LABELS "unit")
