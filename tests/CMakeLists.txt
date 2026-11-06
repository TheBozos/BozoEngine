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

# The native libraries (runtime/libs): what no call log checks yet (the memory card's save path), and the heap and
# rand rules.
add_executable(bozo-libs-tests libs/libs_tests.cpp)
target_link_libraries(bozo-libs-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-libs-tests PRIVATE -static)
endif()
add_test(NAME libs COMMAND bozo-libs-tests)

# The native mixer and libsd (runtime/audio): the voice-status rules, the clock, libsd's entries, and the reference's
# samples for a fixed scenario (audio/mixer_scenario.h).
add_executable(bozo-audio-tests audio/audio_tests.cpp)
target_link_libraries(bozo-audio-tests PRIVATE bozo_audio)
if(MINGW)
  target_link_options(bozo-audio-tests PRIVATE -static)
endif()
add_test(NAME audio COMMAND bozo-audio-tests)
add_executable(bozo-compiled-library-tests native/compiled_libraries_tests.cpp)
target_link_libraries(bozo-compiled-library-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-compiled-library-tests PRIVATE -static)
endif()
add_test(NAME compiled_libraries COMMAND bozo-compiled-library-tests)
add_executable(bozo-library-semaphore-tests native/library_semaphore_tests.cpp)
target_link_libraries(bozo-library-semaphore-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-library-semaphore-tests PRIVATE -static)
endif()
add_test(NAME library_semaphores COMMAND bozo-library-semaphore-tests)
set_tests_properties(library_semaphores PROPERTIES TIMEOUT 10)
add_executable(bozo-kernel-services-tests runtime/kernel_services_tests.cpp)
target_link_libraries(bozo-kernel-services-tests PRIVATE bozo_runtime_core)
if(MINGW)
  target_link_options(bozo-kernel-services-tests PRIVATE -static)
endif()
add_test(NAME kernel_services COMMAND bozo-kernel-services-tests)
add_executable(bozo-thread-sleep-tests runtime/thread_sleep_tests.cpp)
target_link_libraries(bozo-thread-sleep-tests PRIVATE bozo_runtime_core)
if(MINGW)
  target_link_options(bozo-thread-sleep-tests PRIVATE -static)
endif()
add_test(NAME thread_sleep COMMAND bozo-thread-sleep-tests)
add_executable(bozo-deadline-tests runtime/deadline_tests.cpp)
target_link_libraries(bozo-deadline-tests PRIVATE bozo_runtime_core)
if(MINGW)
  target_link_options(bozo-deadline-tests PRIVATE -static)
endif()
add_test(NAME deadlines COMMAND bozo-deadline-tests)
set_tests_properties(deadlines PROPERTIES TIMEOUT 10)
add_executable(bozo-cd-callback-tests libs/cd_callback_tests.cpp)
target_link_libraries(bozo-cd-callback-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-cd-callback-tests PRIVATE -static)
endif()
add_test(NAME cd_callbacks COMMAND bozo-cd-callback-tests)
set_tests_properties(cd_callbacks PROPERTIES TIMEOUT 10)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME reference_records COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/native/test_reference.py)
add_test(NAME explicit_entries COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/native/test_entries.py $<TARGET_FILE:bozo-recomp>)
set(entry_gen "${CMAKE_CURRENT_BINARY_DIR}/explicit_entry_gen")
add_custom_command(
  OUTPUT "${entry_gen}/entries_000.cpp" "${entry_gen}/entries_table.cpp" "${entry_gen}/entries_functions.h"
  COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/native/test_entries.py" --emit "${entry_gen}"
  COMMAND $<TARGET_FILE:bozo-recomp> "${entry_gen}/fixture.elf" "${entry_gen}" --prefix entries --files 1
    --entries "${entry_gen}/entries.txt" --patches "${entry_gen}/patches.txt" --no-comments
  DEPENDS bozo-recomp "${CMAKE_CURRENT_SOURCE_DIR}/native/test_entries.py")
add_executable(bozo-entry-tests instr/entries_run.cpp "${entry_gen}/entries_000.cpp" "${entry_gen}/entries_table.cpp")
target_include_directories(bozo-entry-tests PRIVATE "${entry_gen}")
target_link_libraries(bozo-entry-tests PRIVATE bozo_runtime_core)
if(MINGW)
  target_link_options(bozo-entry-tests PRIVATE -static)
endif()
add_test(NAME explicit_entries_adaptations COMMAND bozo-entry-tests "${entry_gen}/fixture.elf")
add_test(NAME strict_irx_generation COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/native/test_irx.py)

if(BOZO_APP)
  add_executable(bozo-render-target-tests render/render_targets_tests.cpp)
  target_link_libraries(bozo-render-target-tests PRIVATE bozo_render SDL3::SDL3-static)
  if(MINGW)
    target_link_options(bozo-render-target-tests PRIVATE -static)
  endif()
  add_test(NAME native_render_targets COMMAND bozo-render-target-tests)
  set_tests_properties(native_render_targets PROPERTIES LABELS "gpu")
endif()

# Unused native image definitions must not write packets; generic SDK output is retained.
add_executable(bozo-image-builders-tests libs/image_builders_tests.cpp)
target_link_libraries(bozo-image-builders-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-image-builders-tests PRIVATE -static)
endif()
add_test(NAME native_image_builders COMMAND bozo-image-builders-tests)

# Native graphics services retain no GS register/transfer bank or packet readers.
add_executable(bozo-native-graphics-tests libs/native_graphics_tests.cpp)
target_link_libraries(bozo-native-graphics-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-native-graphics-tests PRIVATE -static)
endif()
add_test(NAME native_graphics_boundary COMMAND bozo-native-graphics-tests)

# Validate the whole adaptation batch before changing the offline analysis image.
add_executable(bozo-instruction-patch-tests instr/patch_tests.cpp)
target_link_libraries(bozo-instruction-patch-tests PRIVATE bozo_recomp_lib)
if(MINGW)
  target_link_options(bozo-instruction-patch-tests PRIVATE -static)
endif()
add_test(NAME guarded_instruction_adaptations COMMAND bozo-instruction-patch-tests)
add_executable(bozo-library-binding-tests native/library_binding_tests.cpp)
target_link_libraries(bozo-library-binding-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-library-binding-tests PRIVATE -static)
endif()
add_test(NAME library_bindings COMMAND bozo-library-binding-tests)
set_tests_properties(library_bindings PROPERTIES TIMEOUT 10)
add_executable(bozo-library-memory-tests native/library_memory_tests.cpp)
target_link_libraries(bozo-library-memory-tests PRIVATE bozo_libs)
if(MINGW)
  target_link_options(bozo-library-memory-tests PRIVATE -static)
endif()
add_test(NAME library_memory COMMAND bozo-library-memory-tests)
set_tests_properties(library_memory PROPERTIES TIMEOUT 10)
