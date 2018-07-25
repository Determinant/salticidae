# Copyright (c) 2018 Cornell University.
#
# Author: Ted Yin <tederminant@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
# of the Software, and to permit persons to whom the Software is furnished to do
# so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

cmake_minimum_required(VERSION 3.9)
project(Salticidae)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

find_package(Libevent REQUIRED)
find_package(OpenSSL REQUIRED)

include_directories(include)
add_library(salticidae
    OBJECT
    src/util.cpp
    src/conn.cpp)

option(BUILD_SHARED "build shared library." OFF)
if(BUILD_SHARED)
    set_property(TARGET salticidae PROPERTY POSITION_INDEPENDENT_CODE 1)
    add_library(salticidae_shared SHARED $<TARGET_OBJECTS:salticidae>)
    set_target_properties(salticidae_shared PROPERTIES OUTPUT_NAME "salticidae")
    target_link_libraries(salticidae_shared event crypto)
endif()
add_library(salticidae_static STATIC $<TARGET_OBJECTS:salticidae>)
set_target_properties(salticidae_static PROPERTIES OUTPUT_NAME "salticidae")
target_link_libraries(salticidae_static event crypto)

add_subdirectory(test)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release")
endif()

option(SALTICIDAE_ENABLE_DEBUG_LOG "enable debug log" OFF)
option(SALTICIDAE_ENABLE_NORMAL_LOG "enable regular log" ON)
if (SALTICIDAE_ENABLE_DEBUG_LOG)
    add_definitions(-DSALTICIDAE_DEBUG_LOG)
elseif(SALTICIDAE_ENABLE_NORMAL_LOG)
    add_definitions(-DSALTICIDAE_NORMAL_LOG)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -W -Wall -Wextra -pedantic -Wsuggest-override")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -W -Wall -Wextra -pedantic -Wsuggest-override")
