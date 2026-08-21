cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(PREPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")

set(COMPONENTS main)

project(halite-p4-hub)
