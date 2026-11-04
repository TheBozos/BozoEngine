# Phase 4, milestone M1: TMB's title in an SDL3 window, OpenGL 3.3, real time

Written 2026-11-04 on BozoEngine `phase4/m1` and TMB_Recomp `bozo/m0`. M1 of
[`phase4-inventory.md`](phase4-inventory.md) section 5.

## Building and running

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build/bozo_m1 -G Ninja -DTMB_BOZO=ON -DBOZO_ENGINE_DIR=<BozoEngine> -DTMB_ELF=<SCUS_971.01> \
      -DTMB_ISO="<TMB.iso>" -DBOZO_FFMPEG_ROOT=D:/tmb_kit/kit/ffmpeg [-DFETCHCONTENT_SOURCE_DIR_SDL3=<SDL3 3.4.20 sources>]
cmake --build build/bozo_m1
build/bozo_m1/tmb --iso "<TMB.iso>"          # the window; F8 saves a PNG, F11 / Alt+Enter fullscreen
```

`tmb` options: `--capture-dir`, `--capture-frames n,n,...`, `--quit-after n`, `--width/--height`, `--fullscreen`,
`--no-vsync`, `--render-scale`, `--card-dir`, `--pad-script`. `PS2X_FRAME_STATS=1`: the frames' work times.

## What moved, what is new

- **Moved (team code):** the renderer's GPU/present half (`runtime/render/present_gl.cpp`): `struct Gpu`, `present()`,
  the overlay's drawing, the display picture and backdrop, the clips, the depth tiles' `depthAt`, the 330 shaders;
  rlgl / raylib calls replaced by plain GL 3.3 core. `tools/native_pack/` (texture.py, mesh.py, ps2.py, ps2_disc.py).
- **Written fresh:** `gl.h` / `gl.cpp` (our table of ~70 GL functions loaded through `SDL_GL_GetProcAddress`; no
  generated loader); the 2D shader replacing raylib's default one (same arithmetic, same a,b,e / a,e,c quad split, same
  blend modes); the depth readback (pixel-pack buffer, mapped one present later); `Material::sampleTarget` (target
  copied before the batch; the fork's native present path never sampled it); `bozo/app/window.h` (SDL3 window, game
  thread with a 256 MB stack, present loop, stats, PNG captures), `device_pad.h` (keyboard with the fork's default keys
  and the first gamepad as port 1), `png.h` (stored-deflate PNG writer); `FreeOptions::realTime`,
  `FreeHost::requestStop/framesEnded`; the FramePacer keeps the cap's schedule (it ran at 59.4 instead of 60).
- **Not moved yet:** frame interpolation (Video settings' frame rate), `ps2_app` (user folder, logs), video settings,
  input bindings / `input.ini`, the F2 panel.

## SDL3

FetchContent of release 3.4.20 (`SDL3-3.4.20.tar.gz`, SHA256 f3bb06f5...581f), static, behind `BOZO_APP` (off by
default: headless builds and the checkers never need it). The kit (M6) needs the SDL3 3.4.20 source tree in `deps/`
passed as `FETCHCONTENT_SOURCE_DIR_SDL3`, and SDL3's zlib licence in `licenses/`.

## The draws check

The packs are built from the ISO (`TMB_ISO`) into `<build>/packs`. To give the reference's draw lists, TMB's runtime
must be compiled as the fork compiled it: `-O2 -mavx2 -mfma` for `tmb_runtime`, `-O3 -mavx2 -mfma` for `bozo_render`
(GCC contracts a*b+c into FMA; without FMA match1p differs from frame 5593, with -O3 for the runtime from 5601, with -O2
for both from 5595). With those flags boot and match1p match on every frame but the log's last (frame 2999 / 15999):
there the log's final FRAME record comes inside a library call, before TMB's frame end publishes, so the record has no
list (hash 0) where the reference's has the frame's.

## M2 / M3 need

- M2: input bindings (`input.ini`), SDL3 gamepads hot-plug / mapping / rumble, multitap slots 1-3 for 4 players;
  `ps2_app` user folder and memory cards there.
- M3: an SDL3 audio stream pulling `Mixer::render` at 48 kHz; 989SND's tick and the mixer on real time in free mode;
  the movie sound sink to the mixer (SDL_AUDIO is already built in).
