add_library(ethervox_governor STATIC
    governor.c
    tool_registry.c
    chat_template.c
)

target_include_directories(ethervox_governor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

# Link with math library
target_link_libraries(ethervox_governor m)
