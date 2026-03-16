# sst_rematch - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(sst_rematch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SSTREMATCH_SOURCES
    src/main.cpp
)

if(WIN32)
    list(APPEND SSTREMATCH_SOURCES icon/icon.rc)
    add_executable(sst_rematch WIN32 ${SSTREMATCH_SOURCES})
else()
    add_executable(sst_rematch ${SSTREMATCH_SOURCES})
endif()

rexglue_setup_target(sst_rematch)

