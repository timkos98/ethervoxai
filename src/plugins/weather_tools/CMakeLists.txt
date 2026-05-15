# Weather tools plugin CMakeLists.txt
# Requires libcurl for HTTP requests

if(HAVE_LIBCURL)
    add_library(ethervox_weather_tools STATIC
        weather_core.c
        weather_cache.c
        weather_registry.c
    )
    
    target_include_directories(ethervox_weather_tools PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CURL_INCLUDE_DIRS}
    )
    
    target_link_libraries(ethervox_weather_tools
        ${CURL_LIBRARIES}
        cJSON
    )
    
    message(STATUS "Weather tools plugin enabled (libcurl available)")
else()
    message(STATUS "Weather tools plugin disabled (libcurl not available)")
    message(STATUS "  Install libcurl development packages to enable weather forecasts")
endif()
