# Feature flags for optional network / mutating-file subsystems (TASK-C1.0).
#
# All default ON so existing consumers (ethervoxai-android, ethervoxai-ios) are
# unaffected. The Workspace product line configures with these OFF so that a
# WORKSPACE-profile build contains no network code and no write-capable file
# tools (AGENTS.md I1, I2).
#
# A feature guarded here must never "succeed silently" when OFF: callers get
# ETHERVOX_ERROR_FEATURE_DISABLED, not empty/stubbed data.

option(ETHERVOX_FEATURE_HTTP       "Enable generic platform HTTP client (src/common/platform_http.c)" ON)
option(ETHERVOX_FEATURE_DOWNLOADER "Enable the model downloader"                                       ON)
option(ETHERVOX_FEATURE_BUG_REPORT "Enable the GitHub-issue bug reporter"                               ON)
option(ETHERVOX_FEATURE_WEATHER    "Enable the Open-Meteo weather tool"                                 ON)
option(ETHERVOX_FEATURE_FILE_TOOLS "Enable the file_tools plugin (read, and read-write if selected)"    ON)

if(ETHERVOX_FEATURE_HTTP)
    add_definitions(-DETHERVOX_FEATURE_HTTP=1)
else()
    add_definitions(-DETHERVOX_FEATURE_HTTP=0)
endif()
if(ETHERVOX_FEATURE_DOWNLOADER)
    add_definitions(-DETHERVOX_FEATURE_DOWNLOADER=1)
else()
    add_definitions(-DETHERVOX_FEATURE_DOWNLOADER=0)
endif()
if(ETHERVOX_FEATURE_BUG_REPORT)
    add_definitions(-DETHERVOX_FEATURE_BUG_REPORT=1)
else()
    add_definitions(-DETHERVOX_FEATURE_BUG_REPORT=0)
endif()
if(ETHERVOX_FEATURE_WEATHER)
    add_definitions(-DETHERVOX_FEATURE_WEATHER=1)
else()
    add_definitions(-DETHERVOX_FEATURE_WEATHER=0)
endif()
if(ETHERVOX_FEATURE_FILE_TOOLS)
    add_definitions(-DETHERVOX_FEATURE_FILE_TOOLS=1)
else()
    add_definitions(-DETHERVOX_FEATURE_FILE_TOOLS=0)
endif()

message(STATUS "Feature flags: HTTP=${ETHERVOX_FEATURE_HTTP} DOWNLOADER=${ETHERVOX_FEATURE_DOWNLOADER} "
               "BUG_REPORT=${ETHERVOX_FEATURE_BUG_REPORT} WEATHER=${ETHERVOX_FEATURE_WEATHER} "
               "FILE_TOOLS=${ETHERVOX_FEATURE_FILE_TOOLS}")
