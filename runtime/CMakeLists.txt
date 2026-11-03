# BozoEngine's runtime (bozo/runtime.h): guest memory, the function table and overrides, the runtime_api.h entry points
# recompiled code calls, routed to a RuntimeServices implementation; the EE kernel and the dispatcher (bozo/ee/); the
# per-frame reference record (bozo/reference.h); the native libraries (bozo/libs/).
add_library(bozo_runtime_core STATIC
  core/runtime_core.cpp
  ee/kernel.cpp
  ee/dispatcher.cpp
  harness/reference.cpp)
target_link_libraries(bozo_runtime_core PUBLIC bozo_runtime_headers)
target_compile_options(bozo_runtime_core PRIVATE -Wall -Wextra)

# The native libraries (bozo/libs/, docs/specs/libraries.md): the game's library calls answered natively.
add_library(bozo_libs STATIC
  libs/libraries.cpp
  libs/libc.cpp
  libs/libm.cpp
  libs/gs.cpp
  libs/pad.cpp
  libs/mc.cpp
  libs/cd.cpp)
target_link_libraries(bozo_libs PUBLIC bozo_runtime_core)
target_compile_options(bozo_libs PRIVATE -Wall -Wextra)
