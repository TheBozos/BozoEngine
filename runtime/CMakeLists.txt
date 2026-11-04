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
  libs/snd.cpp
  libs/snd989.cpp)
target_link_libraries(bozo_libs PUBLIC bozo_runtime_core bozo_audio)
target_compile_options(bozo_libs PRIVATE -Wall -Wextra)

# The native renderer (bozo/render/: moved from the team's fork code, phase 4): its game side (frames published, their
# draw lists hashed), its present side on OpenGL 3.3 core (present_gl.cpp; the functions loaded at run time through
# gl.h's table, so this library needs no GL headers or libraries: headless builds never present), and the offline
# packs' loaders.
add_library(bozo_render STATIC
  render/renderer.cpp
  render/present_gl.cpp
  render/gl.cpp
  render/texture_pack.cpp
  render/mesh_pack.cpp
  render/mesh_draw.cpp)
target_link_libraries(bozo_render PUBLIC bozo_runtime_headers)
target_compile_options(bozo_render PRIVATE -Wall -Wextra)

# The port API (bozo/port.h): what a game port's overrides build on (phase 4).
add_library(bozo_port STATIC
  port/port.cpp)
target_link_libraries(bozo_port PUBLIC bozo_libs)
target_compile_options(bozo_port PRIVATE -Wall -Wextra)

# The free host (bozo/host/free_host.h): the game with no call log, every out-call answered by BozoEngine.
add_library(bozo_host STATIC
  host/free_host.cpp)
target_link_libraries(bozo_host PUBLIC bozo_port bozo_render bozo_libs bozo_runtime_core bozo_recomp_lib)
target_compile_options(bozo_host PRIVATE -Wall -Wextra)
# The VU0 programs compute in single precision rounded toward zero, as the reference does: the host's float operations
# under that rounding mode, never folded or fused (libs/vu0.cpp).
set_source_files_properties(libs/vu0.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-ffp-contract=off")

# The real-time platform layer (bozo/app/: window.h, device_pad.h, input_bindings.h, app.h, png.h) on SDL3, static,
# pinned to release 3.4.20 (zlib licence). Downloaded at configure unless FETCHCONTENT_SOURCE_DIR_SDL3 names a source
# tree of that release.
if(BOZO_APP)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_AUDIO ON CACHE BOOL "" FORCE)  # the audio device (milestone M3)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  set(SDL_GPU OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER OFF CACHE BOOL "" FORCE)
  set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(SDL3)

  add_library(bozo_app STATIC
    app/window.cpp
    app/device_pad.cpp
    app/input_bindings.cpp
    app/app.cpp
    app/png.cpp)
  target_link_libraries(bozo_app PUBLIC bozo_render bozo_libs SDL3::SDL3-static)
  target_compile_options(bozo_app PRIVATE -Wall -Wextra)
endif()

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
