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

# The native sound mixer and libsd at its API (bozo/audio/, docs/specs/libraries.md section 15.14). The interpolation
# computes in single precision, never fused (the samples must stay the reference's).
add_library(bozo_audio STATIC
  audio/mixer.cpp
  audio/libsd.cpp)
target_include_directories(bozo_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(bozo_audio PRIVATE -Wall -Wextra -ffp-contract=off)

# The native libraries (bozo/libs/, docs/specs/libraries.md): the game's library calls answered natively.
add_library(bozo_libs STATIC
  libs/libraries.cpp
  libs/libc.cpp
  libs/libm.cpp
  libs/gs.cpp
  libs/pad.cpp
  libs/mc.cpp
  libs/cd.cpp
  libs/vu0.cpp
  libs/io.cpp
  libs/mpeg.cpp
  libs/mpeg_ffmpeg.cpp
  libs/sif.cpp
  libs/snd.cpp)
target_link_libraries(bozo_libs PUBLIC bozo_runtime_core)
target_compile_options(bozo_libs PRIVATE -Wall -Wextra)
# The VU0 programs compute in single precision rounded toward zero, as the reference does: the host's float operations
# under that rounding mode, never folded or fused (libs/vu0.cpp).
set_source_files_properties(libs/vu0.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-ffp-contract=off")

# libmpeg's video decoder (libraries.md section 9): FFmpeg's libavcodec (mpeg2video) and libavutil, optional. Found in
# BOZO_FFMPEG_ROOT (a folder with include/ and lib/: the TMB build kit's static FFmpeg 9.0.2), else through pkg-config.
# Without it libmpeg stays answered from a log.
set(BOZO_FFMPEG_ROOT "" CACHE PATH "FFmpeg folder (include/, lib/) for libmpeg's decoder")
set(BOZO_FFMPEG_FOUND OFF)
if(BOZO_FFMPEG_ROOT)
  find_library(BOZO_AVCODEC avcodec PATHS ${BOZO_FFMPEG_ROOT}/lib NO_DEFAULT_PATH)
  find_library(BOZO_AVUTIL avutil PATHS ${BOZO_FFMPEG_ROOT}/lib NO_DEFAULT_PATH)
  if(BOZO_AVCODEC AND BOZO_AVUTIL)
    target_include_directories(bozo_libs PRIVATE ${BOZO_FFMPEG_ROOT}/include)
    target_link_libraries(bozo_libs PUBLIC ${BOZO_AVCODEC} ${BOZO_AVUTIL})
    if(WIN32)
      target_link_libraries(bozo_libs PUBLIC bcrypt ole32 user32)
    endif()
    set(BOZO_FFMPEG_FOUND ON)
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(BOZO_FFMPEG IMPORTED_TARGET libavcodec libavutil)
    if(BOZO_FFMPEG_FOUND)
      target_link_libraries(bozo_libs PUBLIC PkgConfig::BOZO_FFMPEG)
    endif()
  endif()
endif()
if(BOZO_FFMPEG_FOUND)
  set_source_files_properties(libs/mpeg_ffmpeg.cpp PROPERTIES COMPILE_DEFINITIONS BOZO_HAVE_FFMPEG)
  message(STATUS "libmpeg: FFmpeg found (native movies)")
else()
  message(STATUS "libmpeg: no FFmpeg (movies answered from a log)")
endif()
