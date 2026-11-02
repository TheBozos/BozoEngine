# bozo-recomp: R5900 (EE) to C++.
add_library(bozo_recomp_lib STATIC
  src/decoder.cpp
  src/elf.cpp
  src/program.cpp
  src/analysis.cpp
  src/emitter.cpp)
target_include_directories(bozo_recomp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(bozo_recomp_lib PRIVATE -Wall -Wextra)

add_executable(bozo-recomp src/main.cpp)
target_link_libraries(bozo-recomp PRIVATE bozo_recomp_lib)
target_compile_options(bozo-recomp PRIVATE -Wall -Wextra)
if(MINGW)
  # A self-contained executable: no dependency on the toolchain's DLLs being on PATH.
  target_link_options(bozo-recomp PRIVATE -static)
endif()
