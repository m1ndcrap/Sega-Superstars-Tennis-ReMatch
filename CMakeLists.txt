# segasuperstarstennisrecomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(segasuperstarstennisrecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SEGASUPERSTARSTENNISRECOMP_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(segasuperstarstennisrecomp WIN32 ${SEGASUPERSTARSTENNISRECOMP_SOURCES})
else()
    add_executable(segasuperstarstennisrecomp ${SEGASUPERSTARSTENNISRECOMP_SOURCES})
endif()

rexglue_setup_target(segasuperstarstennisrecomp)

