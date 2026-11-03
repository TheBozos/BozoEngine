# BozoEngine's runtime (bozo/runtime.h): guest memory, the function table and overrides, the runtime_api.h entry points
# recompiled code calls, routed to a RuntimeServices implementation; the EE kernel and the dispatcher (bozo/ee/); the
# per-frame reference record (bozo/reference.h).
add_library(bozo_runtime_core STATIC
  core/runtime_core.cpp
  ee/kernel.cpp
  ee/dispatcher.cpp
  harness/reference.cpp)
target_link_libraries(bozo_runtime_core PUBLIC bozo_runtime_headers)
target_compile_options(bozo_runtime_core PRIVATE -Wall -Wextra)
