cmake_minimum_required(VERSION 3.16.0)

# PlatformIO passes EXTRA_COMPONENT_DIRS itself; plain ESP-IDF (idf.py / VS Code
# ESP-IDF extension) needs to be told that the app lives in src/ instead of main/.
if(NOT EXTRA_COMPONENT_DIRS)
    set(EXTRA_COMPONENT_DIRS src)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ham_decoder)
