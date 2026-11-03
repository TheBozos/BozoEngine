# Phase 2 (docs/specs/kernel.md, tools/run/README.md).
#
# bozo-calllog-tool (always built): lists a call log's external addresses for the generator.
# bozo-run (built when BOZO_TMB_ELF and BOZO_CALLLOGS are set): TMB's generated code on BozoEngine's EE kernel and
# dispatcher, every other out-call answered from a call log, checked record by record, PS2XREF1 written and compared.
#   cmake -S . -B build -G Ninja -DBOZO_TMB_ELF=<SCUS_971.01> -DBOZO_CALLLOGS=<folder of .clg files>
#   cmake --build build && build/tools/run/bozo-run <log.clg> --ref reference/tmb/records/boot.ref

find_package(ZLIB REQUIRED)

add_library(bozo_calllog STATIC calllog.cpp)
target_include_directories(bozo_calllog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bozo_calllog PUBLIC ZLIB::ZLIB bozo_runtime_headers)
target_compile_options(bozo_calllog PRIVATE -Wall -Wextra)

add_executable(bozo-calllog-tool calllog_tool.cpp)
target_link_libraries(bozo-calllog-tool PRIVATE bozo_calllog)
if(MINGW)
  target_link_options(bozo-calllog-tool PRIVATE -static)
endif()

if(BOZO_TMB_ELF AND BOZO_CALLLOGS)
  add_executable(bozo-run main.cpp log_host.cpp)
  target_link_libraries(bozo-run PRIVATE bozo_calllog bozo_libs bozo_runtime_core bozo_tmb_generated)
  target_compile_options(bozo-run PRIVATE -Wall -Wextra)
  target_compile_definitions(bozo-run PRIVATE BOZO_RUN_ELF="${BOZO_TMB_ELF}")
  if(MINGW)
    target_link_options(bozo-run PRIVATE -static -Wl,--stack,268435456)
  endif()
endif()
