# BozoEngine: The Bozos' static recompiler and runtime for PlayStation 2 games.
cmake_minimum_required(VERSION 3.20)
project(BozoEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The headers recompiled code includes (bozo/recomp.h and what it pulls in).
add_library(bozo_runtime_headers INTERFACE)
target_include_directories(bozo_runtime_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)

add_subdirectory(recompiler)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
