# BozoEngine's tests.
#
# Instruction tests: bozo-test-gen recompiles the cases of instr/cases.cpp into C++ (through the real analysis and
# emitter); bozo-instr-tests compiles that C++ with the stub runtime, runs each case and checks the results.
#   cmake --build build && ctest --test-dir build

set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/instr_gen)
set(gen_files ${gen_dir}/instr_000.cpp ${gen_dir}/instr_table.cpp ${gen_dir}/instr_functions.h)

add_executable(bozo-test-gen instr/gen_main.cpp instr/cases.cpp)
target_include_directories(bozo-test-gen PRIVATE instr support)
target_link_libraries(bozo-test-gen PRIVATE bozo_recomp_lib bozo_runtime_headers)

add_custom_command(
  OUTPUT ${gen_files}
  COMMAND bozo-test-gen ${gen_dir}
  DEPENDS bozo-test-gen
  COMMENT "Recompiling the instruction test cases")

add_executable(bozo-instr-tests instr/run_main.cpp instr/cases.cpp support/stub_runtime.cpp
  ${gen_dir}/instr_000.cpp ${gen_dir}/instr_table.cpp)
target_include_directories(bozo-instr-tests PRIVATE instr support ${gen_dir})
target_link_libraries(bozo-instr-tests PRIVATE bozo_runtime_headers)
set_source_files_properties(${gen_dir}/instr_000.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")

if(MINGW)
  target_link_options(bozo-test-gen PRIVATE -static)
  target_link_options(bozo-instr-tests PRIVATE -static)
endif()

add_test(NAME instructions COMMAND bozo-instr-tests)

# Optional: with -DBOZO_TEST_ELF=<a game's executable> (never part of the repository), check that every word of its
# code decodes.
if(BOZO_TEST_ELF)
  add_test(NAME elf_decodes COMMAND bozo-recomp ${BOZO_TEST_ELF} --census)
endif()

# The runtime core (runtime/): guest memory views, the function table and overrides, COP0 Status.
add_executable(bozo-runtime-tests runtime/runtime_tests.cpp)
target_link_libraries(bozo-runtime-tests PRIVATE bozo_runtime_core)
if(MINGW)
  target_link_options(bozo-runtime-tests PRIVATE -static)
endif()
add_test(NAME runtime COMMAND bozo-runtime-tests)
