# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(PREPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")

# Release (default): no USB serial log, diagnostics HTTP still runs.
# Debug: idf.py -DZBGW_DEBUG=1 reconfigure   or   $env:ZBGW_DEBUG=1
if(NOT DEFINED CACHE{ZBGW_DEBUG})
    if(DEFINED ENV{ZBGW_DEBUG} AND NOT "$ENV{ZBGW_DEBUG}" STREQUAL "" AND NOT "$ENV{ZBGW_DEBUG}" STREQUAL "0")
        set(ZBGW_DEBUG ON CACHE BOOL "USB serial logging")
    else()
        set(ZBGW_DEBUG OFF CACHE BOOL "USB serial logging")
    endif()
endif()
if(ZBGW_DEBUG)
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.debug")
    set(_zbgw_log_profile debug)
else()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.release")
    set(_zbgw_log_profile release)
endif()
if(ZBGW_DEBUG)
    message(STATUS "ZBGW profile: debug (-Og, USB serial)")
else()
    message(STATUS "ZBGW profile: release (-O2, no serial log)")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    execute_process(
        COMMAND python "${CMAKE_SOURCE_DIR}/tools/apply_log_profile.py"
                "${_zbgw_log_profile}" "${CMAKE_SOURCE_DIR}/sdkconfig"
        RESULT_VARIABLE _zbgw_log_rc
    )
    if(_zbgw_log_rc)
        message(FATAL_ERROR "apply_log_profile.py failed (${_zbgw_log_rc})")
    endif()
endif()

set(COMPONENTS main)
set(PROJECT_VER "1.2.25")

project(zigbee-gateway)
