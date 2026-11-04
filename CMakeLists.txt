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

# The checkers and log readers (tools/replay, tools/run) need zlib; a port building BozoEngine as part of its own tree
# without zlib (the build kit) turns them off. The recompiler and the runtime have no dependency.
option(BOZO_BUILD_TOOLS "Build the call-log and capture tools (bozo-run, bozo-replay; need zlib)" ON)

add_subdirectory(recompiler)
add_subdirectory(runtime)
if(BOZO_BUILD_TOOLS)
  add_subdirectory(tools/replay)
  add_subdirectory(tools/run)
endif()
add_subdirectory(tools/externals)

include(CTest)
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
  add_subdirectory(tests)
endif()
