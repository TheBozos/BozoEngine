# bozo-externals (phase 4): the --externals list a port's code is generated with, from the ELF's symbols BozoEngine
# answers natively and the port's override list (tools/externals/main.cpp). Always built; --check (a call log's stubs
# against them) when the call-log reader is (BOZO_BUILD_TOOLS).
add_executable(bozo-externals main.cpp)
target_link_libraries(bozo-externals PRIVATE bozo_recomp_lib bozo_libs bozo_runtime_core)
target_compile_options(bozo-externals PRIVATE -Wall -Wextra)
if(TARGET bozo_calllog)
  target_link_libraries(bozo-externals PRIVATE bozo_calllog)
  target_compile_definitions(bozo-externals PRIVATE BOZO_EXTERNALS_CALLLOG)
endif()
if(MINGW)
  target_link_options(bozo-externals PRIVATE -static)
endif()
