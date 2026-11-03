# BozoEngine's runtime core (bozo/runtime.h): guest memory, the function table and overrides, the runtime_api.h entry
# points recompiled code calls, routed to a RuntimeServices implementation. The start of phase 2's runtime.
add_library(bozo_runtime_core STATIC core/runtime_core.cpp)
target_link_libraries(bozo_runtime_core PUBLIC bozo_runtime_headers)
target_compile_options(bozo_runtime_core PRIVATE -Wall -Wextra)
