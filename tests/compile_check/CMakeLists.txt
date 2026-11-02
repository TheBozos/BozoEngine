# Compiles the C++ that bozo-recomp generated for a whole program, with the tests' stub runtime, into one executable
# (so every call between generated functions links). It does not run the game.
#
#   bozo-recomp <elf> <gen dir>
#   cmake -S tests/compile_check -B <build dir> -G Ninja -DBOZO_GENERATED_DIR=<gen dir>
#   cmake --build <build dir>
cmake_minimum_required(VERSION 3.20)
project(BozoCompileCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT BOZO_GENERATED_DIR)
  message(FATAL_ERROR "set BOZO_GENERATED_DIR to bozo-recomp's output directory")
endif()
set(BOZO_PREFIX "bozo" CACHE STRING "the --prefix given to bozo-recomp")

file(GLOB generated CONFIGURE_DEPENDS ${BOZO_GENERATED_DIR}/${BOZO_PREFIX}_*.cpp)
set(root ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_library(bozo_generated STATIC ${generated})
target_include_directories(bozo_generated PUBLIC ${root}/runtime/include ${BOZO_GENERATED_DIR})
# Generated code: labels that nothing jumps to are normal.
target_compile_options(bozo_generated PRIVATE -Wno-unused-label -Wno-unused-variable)

add_executable(compile_check main.cpp ${root}/tests/support/stub_runtime.cpp)
target_include_directories(compile_check PRIVATE ${root}/tests/support)
target_compile_definitions(compile_check PRIVATE BOZO_PREFIX=${BOZO_PREFIX})
target_link_libraries(compile_check PRIVATE bozo_generated)
if(MINGW)
  target_link_options(compile_check PRIVATE -static)
endif()
