# Phase 4 inventory: moving TMB_Recomp onto BozoEngine

Written 2026-11-04 from TMB_Recomp `main` at 7f508eb (submodule PS2Recomp at 3416aa3, branch `native/renderer`) and
BozoEngine at 875d553. Read-only survey: nothing was changed in either repository. It plans phase 4 of
[`PLAN.md`](PLAN.md) (section 4) under the clean-room rules (section 6), SDL3 (section 6b) and the authorship rule
(section 7).

Method:
- **Call sites.** Counted in TMB's `runtime/` with comments and strings stripped (`*.cpp`, `*.h`, `vu1/`; 13,559
  lines).
- **Authorship.** `git blame --line-porcelain -w -C -C -M HEAD` on the fork's current tree.
  - "Team" means three identities:
    - `304581853+boubou666@users.noreply.github.com` (boubou666);
    - `thomas.epitech@gmail.com`;
    - `gitea@barrut.me`, which is **Arzaroth**. This answers PLAN section 7's open question: the fork's shortlog
      names that address "Arzaroth", and it is Arzaroth's only identity there.
  - Ranieri and Ran-j are the same person (`ran-junior@hotmail.com`), the upstream author.
  - `-w` keeps a whitespace-only change by us from taking credit for an upstream line.

## 1. The fork API TMB's runtime uses

TMB's runtime is 11 files:
- `tmb_overrides.cpp` 1831;
- `tmb_native.cpp` 3515;
- `tmb_meshes.cpp` 1833;
- `tmb_particles.cpp` 2620;
- `tmb_2d.cpp` 1044;
- `tmb_textures.cpp` 546;
- `tmb_vu0.cpp` 1069;
- `tmb_cheats.cpp` 462;
- `tmb_host.cpp` 193;
- `vu1/tmb_vu1_rain.cpp` 443;
- three headers.

It includes 23 fork headers. The tables below count call sites (identifier uses) per area. "BozoEngine" says what BozoEngine has today and what it lacks.

### 1.1 Context and ABI helpers

| API | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `getRegU32`, `setReturnS32`, `SET_GPR_U32` | `ps2_recomp_abi.h`, `ps2_runtime_macros.h` | 191 (overrides 67, 2d 58, particles 28, textures 15, meshes 14, cheats 9) | **Has**, same names (`bozo/context.h`) |
| `R5900Context` type | `ps2_recomp_abi.h` | 104 | **Has**. Field names match what TMB touches: `pc` 34, `f` 7, `vu0_vf`, `vu0_acc`, `vu0_q`, `vu0_i`, `vu0_clip_flags`, `vi`, `r` (57 sites) |
| `PS2Runtime *` as the third argument of every override | `ps2_runtime.h` | 118 | **Different**: `Runtime *` (opaque). Mechanical rename; the members TMB calls on it go to a port API (1.3) |
| Direct calls to generated functions (`name_0xaddr(rdram, ctx, rt)`) | generated `ps2_recompiled_functions.h` | 76 sites, 75 names | **Has**: all 75 names exist in `build/tmb_gen/tmb_functions.h`. Include `tmb_functions.h` instead |
| FPU and VU float helpers (`ps2_fadd/fsub/fmul/fdiv`, `ps2_vadd/vmul`, `FPU_CVT_W_S`) | `ps2_runtime_macros.h` | 7 (particles 6, meshes 1) | **Has** the same semantics in `bozo/ops_fpu.h` (`fpu::add` ...); a 6-line adapter in particles |
| SSE vector helpers (`vuAdd4`, `_mm_*` through `ps2_vu1_inline.h` / `ps2_vu1_jit.h`) | VU1 JIT headers | about 150, all in `vu1/tmb_vu1_rain.cpp` | **Not needed**: that file is dropped (section 3) |

### 1.2 Overrides registration

| API | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `runtime.registerFunction(addr, fn)` | `ps2_runtime.h` / `.cpp` | 84 (overrides 29, 2d 19, textures 12, meshes 9, particles 9, cheats 6) | **Has** `FunctionTable::setOverride`. **Lacks:** (a) a port-facing registration call; (b) the recompile-time list (see below) |
| `PS2_REGISTER_GAME_OVERRIDE(name, elf, entry, crc, fn)` | `game_overrides.h` / `.cpp` (238 lines, 100% Ranieri) | 1 | **Lacks**: needs a small port entry point, written fresh. The game is known, so no database is needed |
| Recompiler stubs from `recomp/tmb_scus97101.toml` | `ps2xRecomp` config | 10 manual `ret0` / `ret1` (Logitech wheel functions, `videoCallback`) | **Lacks**: becomes 10 TMB overrides or `stub` lines in the externals list |

On (b): `bozo-recomp --externals` must name every override address. Otherwise the generated code calls the guest
function directly and the override never runs, silently. Two things are needed:
- a generator that writes the externals file from two sources: the ELF symbols of the libraries BozoEngine
  implements, and a TMB-side list of override addresses;
- a startup check that every registered address is an external.

### 1.3 Runtime, memory access and the scheduler

| API | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `rdram` (guest RAM pointer) | ABI | 998 | **Has**, same |
| `memory().getScratchpad()`, `read32/read64/write32` | `ps2_memory.h` | 13 (overrides 9, meshes 4) | **Has**: `GuestMemory::scratchpad()`, `hostPointer()` |
| `memory().gsLag/setGsLag/setVif1Threaded/vif1Threaded/queueVif1Callback/getGSVRAM` | `ps2_memory.h` | 7 | **Not needed** (GS and VIF1 emulation: section 3) |
| `hasGs`, `gsForPresentation().setDrawRates/requestParallelRenderer`, `GSCpuBackend::SetAsyncRasterization`, `GsDrawEstimate` | `ps2_runtime.h`, `gs/*` | 10 | **Not needed** |
| `iopDebugSnapshot` | `ps2_runtime.h` | 1 | **Not needed** (no IOP) |
| `PS2Runtime::getIoPaths/setIoPaths` (the ELF path, the disc image) | `ps2_runtime.h` | 4 | **Partly**: the CD library takes an ISO path (`--iso`, `BOZO_ISO`). Needs the port's call to set it (TMB's `findDiscImage` stays TMB's) |
| `eeScheduler()`: `currentThread`, `thread`, `changePriority`, `rotateReadyQueue`, `transferIfRequested`, `currentVSyncTick` (9), `snapshot`, `currentCycle`, `accountCycles`, `presentGuestFrame`, `setNativeFrames`, `setVBlankHold*`, `vblanksHeld`, `setEqualPrioritySlicing`, `setIdleObserver`, `idleWaitNanoseconds`, `kEeClockHz` | `ee_scheduler.h`, `EeScheduler.cpp` | 23 member calls, 12 type uses, all in `tmb_overrides.cpp` | **Partly.** `ee::Kernel` has `changePriority`, `rotateReadyQueue`, `transfer`, `thread`, `threads()`, `vblankCount()`. **Lacks** "native frames": the frame-end point that charges the EE the real time it did not account and presents (`accountCycles`, `presentGuestFrame`, `setNativeFrames`). The vblank hold, slicing and idle-observer toggles are needed only by experiments that drop |
| `framePacer()`, `FramePacer::setCap/frameEnd`, `noteGameFrame` | `frame_pacer.h` (header-only), `ps2_runtime.cpp` | 3 | **Lacks**. `frame_pacer.h` moves (100% team). `noteGameFrame` belongs to the FPS overlay (1.9) |
| `ps2x::setStorePatch` (a hook on the game's stores to hardware registers) | `ps2_store_patch.h`, `ps2_memory.cpp` | 1 (`tmb_2d` dialog kick) | **Lacks**: a port hook in `bozo_io_write` / the `io` library |
| `padBackend().setMultitap/multitap` | `ps2_pad.h` | 3 | **Lacks**: BozoEngine's libpad has ports 1 and 2, slot 0; multitap slots 1-3 are needed for 4 players (TMB's `mtap*` overrides call it) |
| `ps2_stubs::setMpegMaxPicturesAhead/abortMpegPlayback/endMpegPlaybackInput/mpegDebugSummary` | `Stubs/MPEG.h` / `.cpp` (12% team) | 5 | **To check against BozoEngine's native libmpeg**: these tune the fork's HLE. TMB's movie overrides (`pcmCallback`, `audioDec*`, `videoDecFlush/Abort`, `switchThread`) must be re-checked for overlap with BozoEngine's libmpeg |
| `ps2x::profiling::setSampleGate` | `sampling_profiler_gate.h` | 2 | Optional. The profiler (385 lines, 100% team) can move |

**Risk to settle first.** About 20 of TMB's overrides call guest code: the 76 direct calls, and `particleDraw`
continuing in the game's own chain loop. So far BozoEngine has run every override from the call log, never natively.

BozoEngine stops a guest thread by unwinding to the dispatcher. So guest code that a native override calls may need
the inner-call rule (`noStop` / events taken in place, `kernel.md` section 3), applied to port overrides.

The pinned check of milestone M0 (section 5) is where this is proved.

### 1.4 The native renderer

| API | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `Renderer::instance()` + methods: `setKey` 16, `beginGroup` 5, `publishOverlay` 4, `drawOverlay` 2, `draw` 3, `drawTracked` 1, `depthAt` 2, `wantDepth` 2, `beginFrame` 3, `endFrame` 1, `setClip/clearClip` 3, `setBackdrop` 1, `setClearColour` 1, `setParts` 1, `interpolating` 1, `gsZScale` 2, `setEnabled/setStreamDraws/setPresentsAlone` 3 | `native_renderer.h` / `.cpp` | 44 type uses, about 50 method calls (native 29, 2d 7, particles 7, meshes 1) | **Lacks** all of it. The code moves (100% team) |
| `Vertex` | `native_renderer.h` | 111 (native 61, particles 22, meshes 11, 2d 8, shading 8) | Moves with the renderer |
| `Material`, `Material::Blend` | `native_renderer.h` | 28 | Moves |
| `Screen` | `native_renderer.h` | 10 | Moves |
| `TextureCache::Texture` (the image struct only; TMB never calls the cache) | `native_texture_cache.h` | 14 | Only the struct and `csm1Position` move. The cache (`.cpp`) needs `GSMem` (foreign, section 2) and is not needed |
| Movie frames: `setMoviePictureHook`, `setDisplayPicture`, `clearDisplayPicture` | `native_movie.h` (100% team) | 3 (2d) | **Lacks** a picture hook in BozoEngine's libmpeg (`libraries.md` 9.7 plans it) |
| Post effects: `Material::sampleTarget` (heat haze, the frame as a texture), depth readback for the lens flares | inside `native_renderer.cpp` | through `Material` and `depthAt` | Moves. Both need GPU readback and copy (section 5, renderer choice) |
| GS front end and stream draws: `setGsFrontEndAbsent`, `gsFrontEndAbsent`, `setGsHeadless`, `setGsDmaSkipped`, `packetStats`, `forEachGsTouch` | `native_display.h`, `native_gs_hook.h` | 13 (native 12, overrides 1) | **Not needed** (section 3) |

### 1.5 Texture and mesh packs, and their loaders

| API | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `packs::TexturePack`, `TextureRecord`, `decodeRecord`, `recordColour`, `psmIndexed/psmIsH/psmEightBit`, `csm1Palette` | `native_texture_pack.h` / `.cpp` | 10 (textures) | **Lacks**. Moves (100% team, 4 trivial lines) |
| `packs::MeshPack`, `MeshFile`, `MeshObject`, `MeshSection`, `LoadedImages`, `RegisterBlock`, `fourcc`, `kMagic` | `native_mesh_pack.h` / `.cpp` | 15 (meshes) | **Lacks**. Moves |
| `MeshDraw`, `MeshSubmitter`, `MeshViewList`, `MeshBatch`, `MeshGsState`, `MeshLiveToggles` | `native_mesh_draw.h` / `.cpp` | 9 (meshes) | **Lacks**. Moves |
| Offline pack writers: `native_pack/texture.py`, `mesh.py`, `ps2.py`, `ps2_disc.py` | fork `tools/` | imported by `tools/tmb_texture_pack.py`, `tmb_mesh_pack.py`, `tmb_build.py` | Move (100% team) |

### 1.6 Native mixer and audio

| API | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| Native mixer and libsd | `ps2xIOP/.../native_mixer.*`, `native_libsd.cpp` | 0 direct (env defaults only) | **Has** (`bozo::audio::Mixer`, `Libsd`; already moved), driven by `snd989::Engine` |
| Audio device output and its time stretcher | `ps2_audio_spu2.cpp`, `ps2_audio_stretch.h` (100% team) | 0 | **Lacks** the device. An SDL3 audio stream pulls `Mixer::render` at 48 kHz. The stretcher can move if it is still wanted |
| Movie sound | fork libmpeg to 989SND's stream | through TMB's `pcmCallback` / `audioDec*` overrides | **Has** `MovieSoundSink`; needs wiring to the mixer |
| 989SND in real time | the fork's IOP on the wall clock (`PS2X_IOP_WALLCLOCK`) | env defaults only | **Partly**: BozoEngine's 989SND runs on a modelled IOP clock. Free mode needs its 240 Hz tick and the mixer driven from real time |

### 1.7 Frame pacer and timing

| Piece | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `FramePacer` (cap at the field rate, real step) | `frame_pacer.h` + `ps2_determinism.h` (100% team) | via `nativeFrameEnd` | Moves |
| Native frames: `timer1Handler` and `sceGsSyncV` overrides end the frame on the PC clock | TMB code + `EeScheduler::accountCycles/presentGuestFrame/setNativeFrames` | 5 | **Lacks** these dispatcher hooks; to specify in `kernel.md` |
| Frame interpolation (renderer `setFrameRate`, keys) | `native_renderer.cpp` | `setKey`, `setParts`, `drawTracked` | Moves with the renderer |

### 1.8 Pad and input bindings

| Piece | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| Pad data and gamepad discovery | `ps2_pad.cpp` (90% team) | 3 (multitap) | libpad is native. **Lacks** a PC `PadInput` (SDL3 gamepads, hot-plug, rumble) and multitap |
| Key and button bindings, `input.ini`, mouse | `ps2_input_bindings.h` / `.cpp` (100% team but 3 trivial lines; raylib calls) | 0 direct | **Lacks**. Moves, ported from raylib key and gamepad codes to SDL3 |

### 1.9 Debug panel, ImGui and overlays

| Piece | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `ps2x::debugui::registerTab` + ImGui in the Cheats tab | `ps2_debug_ui.h` / `.cpp` (100% team but 3 trivial lines), Dear ImGui | 1 + 23 ImGui calls (cheats) | **Lacks**. Moves; ImGui as a dependency through `imgui_impl_sdl3` |
| Settings window (F2): Controls and Video tabs, the port's tabs | `ps2_debug_panel.cpp` lines 2230-2519 | (UI) | **Lacks**. Those 290 lines are 280 team and 10 foreign (`{`, `#if`, `ImGui::TableNextColumn()`): extract and move |
| Runtime Debugger (F1: CPU, threads, IOP, GS, memory tabs) | `ps2_debug_panel.cpp` rest (2,358 lines, mostly Ranieri) | 0 | Not moved. Write a small one of our own later if wanted |
| FPS overlay (F3), capture sequences (F8), F9 bursts, window captures | `ps2_runtime.cpp` 518-600 (44 boubou666, 44 Arzaroth, 13 Ranieri) and 3990-4230 (213 team, 27 Ranieri, 1 other) | 0 | **Rewrite** from this description; mostly ours, but in a file that cannot move. F9 bursts are part of the live-repro workflow, so keep them |

### 1.10 Video settings, user folder, config

| Piece | Fork files | TMB sites | BozoEngine |
|---|---|---|---|
| `video.ini`, window modes, F11 / Alt+Enter, render scale, monitor sizes | `ps2_video_settings.h` / `.cpp` (98-99% team, the rest `#include`s) | 0 direct (`ps2_video::loadAtStartup` in a comment) | **Lacks**. Moves; raylib monitor and window calls become SDL3 display calls |
| User folder (`%APPDATA%\TMB_Recomp`), logs, crash reports, migration, game data folder | `ps2_app.h` / `.cpp` (99.5% team, 4 `#include` lines) | 2 (`executableDirectory`, `configured`) | **Lacks**. Moves as is (no raylib in it) |
| VFS (`ps2_vfs`) | 100% Ranieri | 0 (TMB's `open`/`close`/`stat` are POSIX) | Not needed |
| Memory cards | the fork's MemoryCard stub | 0 | **Has** native libmc with host folders `<dir>/mc0`, `<dir>/mc1`, the same layout as the fork's user folder. Confirm with a real save |

### 1.11 Anything else

- **Store patch, multitap, movie picture hook, IoPaths.** Listed above. All are small port hooks BozoEngine lacks.
- **The reference recorder** (`ps2_reference.cpp`, 98.8% team). It stays on the fork as the reference;
  BozoEngine has its own (`runtime/harness`). Its `draws` and `sound` fields are filled once the renderer and mixer
  exist (milestone M5).
- **`ThreadNaming.h`** (Ranieri). TMB uses only Windows macros through it (`HANDLE`, `NOMINMAX` in `tmb_host.cpp`).
  Not needed.

## 2. Authorship of the fork files TMB needs

Per file, lines by author. Class:
- **moves**: 100% team, or foreign lines that are only standard `#include`s, blank lines or braces. Retype those
  few lines; nothing copyrightable is foreign.
- **mostly ours**: the foreign share and where it is given.
- **rewrite**: foreign code of substance.

### 2.1 Needed, and they move

| File | Lines | boubou666 | thomas.epitech | Arzaroth | Foreign | Class |
|---|---|---|---|---|---|---|
| `include/runtime/native/native_renderer.h` | 399 | 288 | 101 | 7 | 3 (Aslan Hud: `#include <memory>`, `<mutex>`, `<unordered_map>`) | moves |
| `src/lib/native/native_renderer.cpp` | 1802 | 887 | 902 | 13 | 0 | moves |
| `include/runtime/native/native_texture_pack.h` | 84 | 6 | 74 | 0 | 4 (Ranieri: 3 `#include`s, a blank line) | moves |
| `src/lib/native/native_texture_pack.cpp` | 113 | 4 | 109 | 0 | 0 | moves |
| `include/runtime/native/native_mesh_pack.h` | 205 | 0 | 199 | 0 | 6 (Ranieri 5, Antonio Guastella 1: `#include`s) | moves |
| `src/lib/native/native_mesh_pack.cpp` | 241 | 0 | 241 | 0 | 0 | moves |
| `include/runtime/native/native_mesh_draw.h` | 185 | 4 | 181 | 0 | 0 | moves |
| `src/lib/native/native_mesh_draw.cpp` | 244 | 4 | 240 | 0 | 0 | moves |
| `include/runtime/native/native_movie.h` | 50 | 50 | 0 | 0 | 0 | moves |
| `include/runtime/native/native_texture_cache.h` (only `Texture` and `csm1Position` are needed) | 100 | 94 | 0 | 6 | 0 | moves (that part) |
| `include/runtime/frame_pacer.h` | 120 | 114 | 6 | 0 | 0 | moves |
| `include/runtime/ps2_determinism.h` | 63 | 0 | 63 | 0 | 0 | moves |
| `include/ps2_app.h` | 55 | 0 | 55 | 0 | 0 | moves |
| `src/lib/ps2_app.cpp` | 753 | 7 | 742 | 0 | 4 (Ranieri: 3 `#include`s, a blank line) | moves |
| `include/runtime/ps2_input_bindings.h` | 101 | 101 | 0 | 0 | 0 | moves |
| `src/lib/ps2_input_bindings.cpp` | 570 | 563 | 4 | 0 | 3 (Ranieri: `#include`s) | moves |
| `include/runtime/ps2_video_settings.h` | 63 | 0 | 60 | 0 | 3 (`#include`s) | moves |
| `src/lib/ps2_video_settings.cpp` | 372 | 22 | 346 | 0 | 4 (`#include`s) | moves |
| `include/runtime/ps2_debug_ui.h` | 25 | 0 | 22 | 0 | 3 (Ran-j: `#include`s) | moves |
| `src/lib/ps2_debug_ui.cpp` | 30 | 0 | 30 | 0 | 0 | moves |
| `src/lib/ps2_debug_panel.cpp` 2230-2519 (Settings window, Video and Controls tabs) | 290 | 111 | 169 | 0 | 10 (Ranieri: `{`, `#if PS2X_ENABLE_DEBUG_UI`, `TableNextRow/Column`) | moves (extract) |
| `src/lib/ps2_audio_spu2.cpp` (device output, WAV record) | 244 | 208 | 21 | 15 | 0 | moves (optional) |
| `src/lib/ps2_audio_stretch.h` (WSOLA stretcher) | 223 | 199 | 0 | 24 | 0 | moves (optional) |
| `include/runtime/ps2_store_patch.h` | 21 | 21 | 0 | 0 | 0 | moves |
| `include/runtime/sampling_profiler_gate.h` | 12 | 12 | 0 | 0 | 0 | moves (optional) |
| `src/profiling/sampling_profiler.cpp` | 385 | 173 | 0 | 212 | 0 | moves (optional) |
| `tools/native_pack/__init__.py`, `mesh.py`, `ps2.py`, `texture.py` | 452 | 0 | 452 | 0 | 0 | moves |
| `tools/ps2_disc.py` | 248 | 227 | 0 | 21 | 0 | moves |
| Already moved: `native_mixer.h`, `native_mixer.cpp`, `native_libsd.cpp` | 1604 | 1396 | 208 | 0 | 0 | moved (BozoEngine d8bec0f) |

Before porting, raylib calls must be replaced in three of these files:
- `native_renderer.cpp`: about 99 `rlgl` calls plus raylib's `LoadRenderTexture`, `BeginTextureMode` and
  `DrawTexturePro`;
- `ps2_input_bindings.cpp`;
- `ps2_video_settings.cpp`.

### 2.2 Needed, foreign: rewrite from a spec

| File | Lines | Team | Foreign | What TMB needs from it | Class |
|---|---|---|---|---|---|
| `include/game_overrides.h`, `src/lib/game_overrides.cpp` | 238 | 0 | 238 (Ranieri) | the registration entry point | rewrite (trivial; BozoEngine's `FunctionTable` exists) |
| `include/ps2_runtime.h` | 440 | 123 | 317 (Ranieri 228, Ran-j 68, others) | `registerFunction`, `IoPaths`, accessors | rewrite as BozoEngine's port API |
| `src/lib/ps2_runtime.cpp` | 4258 | 1822 | 2436 | window init, the present loop, FPS overlay, captures | rewrite. The present loop (3990-4230) is 89% team but sits in a file that cannot move: write it fresh on SDL3 |
| `include/runtime/ee_scheduler.h`, `src/lib/Kernel/EeScheduler.cpp` | 3495 | 922 | 2573 (Ranieri) | native frames, `accountCycles`, `presentGuestFrame` | rewrite as BozoEngine dispatcher features (BozoEngine's kernel exists) |
| `include/ps2_recomp_abi.h` | 301 | 58 | 243 | context and helpers | **already rewritten** (`bozo/context.h`) |
| `include/ps2_runtime_macros.h` | 1497 | 429 | 1068 | 7 FPU helper calls | **already rewritten** (`bozo/ops_fpu.h`) |
| `include/runtime/ps2_pad.h`, `src/lib/ps2_pad.cpp` | 478 | 418 | 60 | gamepad discovery, multitap flag | **mostly ours**: 12.5% foreign. Aslan Hud's lines are the pad data layout (button bit constants, lines 17-37; the 32-byte report fill, 384-447). BozoEngine's libpad has its own; the SDL3 input side is new |
| `include/ps2_debug_panel.h` | 38 | 13 | 25 | the panel's class | rewrite (tiny) |
| `src/lib/ps2_debug_panel.cpp` minus 2230-2519 | 2358 | 50 | 2308 | the F1 debugger: not needed | not moved |
| `src/lib/Kernel/Stubs/MPEG.cpp` / `.h` | 2986 | 379 | 2607 | libmpeg: BozoEngine's own exists | not moved |

### 2.3 Fork native files TMB does not need on BozoEngine (dropped, listed for completeness)

All are 94-100% team, but they serve emulation or the GS front end:

| File | Lines | Team % |
|---|---|---|
| `native_renderer_gs.cpp` (draws inside the GS stream, probes) | 747 | 100 |
| `native_texture_cache.cpp` (depends on foreign `GSMem`, below) | 375 | 100 |
| `native_gif.h/.cpp` | 543 | 99.3 |
| `native_vif1.h/.cpp` | 186 | 100 |
| `native_vu1_stream.h/.cpp` | 590 | 99.5 |
| `native_vu1_sandbox.h/.cpp` | 78 | 97.4 |
| `native_vu1_hle.h` | 40 | 100 |
| `native_vu0_hle.h` | 45 | 100 |
| `native_display.h` | 107 | 100 |
| `native_gs_hook.h` | 80 | 100 |

They could move if some later port wants them.

### 2.4 Code that may derive from third-party projects

- **No markers.** None of the needed files mention PCSX2, DuckStation, GSdx, Play!, MAME or ps2sdk, and none
  say "ported from" or "adapted from".
- **GS memory swizzle (`GSMem`).** `native_texture_cache.cpp` calls `GSMem::Write*` and `InitLookupTables` in
  `gs/ps2_gs_memory.cpp`: 446 lines, tealalchemist 286, Ranieri 116, ours 44. Its tables are the classic GS
  block/column layout every emulator has. **Do not move the texture cache**: TMB's packs give images directly, and
  TMB uses only the `Texture` struct.
- **The mixer.** Its envelope (ADSR) and interpolation follow the SPU2's documented behaviour. BozoEngine already
  rewrote `envelopeStep` from psx-spx (4e16155); the rest moved in d8bec0f.
- **The rest looks original.**
  - The renderer maps GS `ALPHA`/`TEST`/`CLAMP` to GPU state from the GS manual's definitions, in our own style.
  - The WSOLA stretcher (`ps2_audio_stretch.h`) is a textbook algorithm in our own code, with nothing that matches
    SoundTouch's structure.
  - The mesh-pack tools decode VIF `UNPACK` formats from the VIF documentation.
- **VU1 and VIF emulation.** `ps2_vu1_inline.h` (44% team, 1,541 Ranieri lines) and `ps2_vu1_jit.h` (98.5% team)
  are VU1 emulation and drop with section 3. The fork's `recomp.sh` calls the fast VU1 path "PCSX2-like floats":
  that behaviour model goes away with VU1.

### 2.5 Totals

| Class | Lines | Team | Foreign |
|---|---|---|---|
| Moves (2.1, without the already-moved mixer) | **7,450** (7,160 by file + the 290-line Settings window) | 7,410 | 40 (all `#include`s, braces, `#if`) |
| Already moved (mixer, libsd) | 1,604 | 1,604 | 0 |
| Rewrite, of what TMB needs (2.2) | **~1,000 new lines** to write (estimated): the port API and registration, scheduler native-frame hooks, present loop, FPS overlay and captures, panel host, SDL3 pad input | | sources: 2,152 lines in the small headers, plus the relevant parts of `ps2_runtime.cpp` / `EeScheduler.cpp` |
| Not moved, not needed | the rest of the fork | | |

## 3. TMB's pieces that depend on PS2 emulation (they drop)

BozoEngine has no GS, VIF, VU1, IOP or SPU2. What goes, with its toggles:

| Piece | TMB lines (est.) | Toggles that disappear |
|---|---|---|
| `tmb_native.cpp`'s VIF1 stream adapter: VU1 data mirror, per-program translations, GIF DIRECT reading, gate decisions, translation checks, `runVif`, `onVif1`, `logGsTouches` | ~2,700 of 3,515. Kept: `enabled`, `effectsInStream`, `nextStreamGroup`, `menusDrawn`, view clip, fog colour, the flare depth (`patchFlareDepth`, `flareDepthBlock`), `onFrameEnd`, `onMenuFrameEnd`, `install` | `TMB_NATIVE_VIF`, `TMB_NATIVE_FRAME=0`, `TMB_NATIVE_VU1`, `TMB_NATIVE_VU1_PCS`, `TMB_NATIVE_CHECK`, `TMB_NATIVE_DUMP_CODE`, `TMB_NATIVE_DUMP_MIRROR`, `TMB_NATIVE_LOG_VU1`, `TMB_NATIVE_LOG_DIRECT(_TAGS)`, `TMB_NATIVE_LOG_GATE`, `TMB_NATIVE_LOG_PRE`, `TMB_NATIVE_LOG_FRAMETEX`, `TMB_NATIVE_LOG_REGION`, `TMB_NATIVE_HIDE`, `TMB_NATIVE_MENUS`, `TMB_TRACE_PACKETS` |
| GS front end, headless GS, DMA reading, GS renderers | in `tmb_native.cpp::install`, `gsFrontEndAbsent` and `tmb_overrides.cpp` | `TMB_GS_FRONTEND`, `TMB_GS_HEADLESS`, `TMB_GS_PACKETS`, `TMB_GS_SERIAL`, `TMB_GS_LAG`, `TMB_GS_LAG_AB`, `PS2X_GS_RENDERER`, `PS2X_GS_RASTER_THREADS`, `TMB_LOG_STOREIMAGE` (GS readback) |
| VU1 microcode compiled ahead of time and the lifted rain loop | `vu1/tmb_vu1_rain.cpp` 443; CMake `tmb_vu1_aot`, `tmb_vu1_replay` | `TMB_RAIN_LIFT_AB`, `TMB_VU1_EXACT`, `PS2X_VU_AOT_FAST`, `TMB_VU1_AOT_LINES` |
| VU0 microprograms in C++ (replaced by BozoEngine's `libs/vu0.cpp`, the hierarchy image, bit-exact) and the VIF0 path | `tmb_vu0.cpp` 1,069. The 2D image's programs are debug draws only and are not ported | `TMB_VU0_NATIVE`, `PS2X_VU0_HLE_CHECK`, `TMB_VIF0_PACKETS` |
| VIF1 on its own thread, and scheduler experiments for it (vblank hold, slicing, A/B) | ~250-350 in `tmb_overrides.cpp` | `TMB_VIF1_SERIAL`, `TMB_VBLANK_HOLD`, `TMB_VBLANK_HOLD_AB`, `TMB_SLICE_AB`, `TMB_AB` (hold, slice, lag, holdlimit, fill). With native frames on (the default) the hold was already off |
| IOP, its hardware model, emulated SPU2, IOP recompilation | env defaults in `tmb_overrides.cpp`, `iopDebugSnapshot` in the thread trace; `scripts/iop_recomp.sh`, `recomp/tmb_iop_modules.json`, generated `iop/` | `PS2X_IOP_NATIVE`, `PS2X_IOP_WALLCLOCK`, `PS2X_IOP_HARDWARE`, `TMB_IOP_HARDWARE`, `PS2X_SPU2_WALLCLOCK`, `PS2X_NATIVE_AUDIO`, `TMB_NATIVE_AUDIO=0`, `TMB_TRACE_IOP` |
| Comparison modes against the packet path | `tmb_meshes.cpp` and `tmb_particles.cpp` check code (~100-200) | `TMB_NATIVE_MESHES_CHECK`, `TMB_NATIVE_PARTICLES_CHECK` |
| Opt-outs into the game's own packets: nothing would draw them | none to delete; the toggles lose meaning | `TMB_NATIVE=0`, `TMB_NATIVE_2D=0`, `TMB_NATIVE_MESHES=0`, `TMB_NATIVE_PARTICLES=0`, `TMB_NATIVE_PARTICLE_TYPES` (types 0 and 5 are not native, and TMB has none) |
| GS sync timeline hooks | `sceGsSyncPathTimeline` / `sceGsSyncVTimeline` | `TMB_TRACE_TIMELINE*`: keep, re-pointed at the native frame end |

Estimate: about **4,500-4,700 of TMB's 13,559 runtime lines drop**, leaving about 9,000 to adapt.

The adaptation is mechanical:
- rename `PS2Runtime *` to `Runtime *` (118 sites);
- route `registerFunction` through the port API (84 sites);
- change the renderer and pack includes and their namespace (`ps2x::native` to BozoEngine's);
- move the scheduler and pacer calls to new hooks (about 30 sites).

`launcher/` (MIT, 100% boubou666 in TMB's history) and TMB's `tools/` are the team's.

## 4. The launcher and build kit on BozoEngine

| Area | Today (fork) | On BozoEngine |
|---|---|---|
| Submodule | `PS2Recomp` (GPL), `native/renderer` | `BozoEngine` (MIT). Releases then carry no GPL code. The release workflow checks out submodules, so **BozoEngine must be public or reachable with a token** (PLAN section 9) |
| Recompile step | `ps2_recomp recomp/tmb_scus97101.toml` (1,292-line config), then `ps2_vu_aot` for VU1, through `build/recomp-staging` + `rsync -c` | `bozo-recomp <elf> <out> --prefix tmb --files N --externals tmb_externals.txt`. It rewrites only changed files itself, so the rsync staging goes. **New**: write the externals file (BozoEngine's library symbols from the ELF, TMB's override list, the 10 manual stubs). No `ps2_vu_aot`, no TOML config, no analyzer |
| Tools build | ps2_recomp / ps2_analyzer / ps2_vu_aot with ELFIO, toml11, fmt, libdwarf, rabbitizer, nlohmann_json | `bozo-recomp` alone: no dependencies, static. The kit's `deps/` loses those six |
| IOP step | `irx_load.py` + `iop_recomp.py`, then `generated/iop` | **Removed** (`scripts/iop_recomp.sh`, the `iop` step of `tmb_build.py`, `recomp/tmb_iop_modules.json`) |
| Packs | `tools/tmb_texture_pack.py`, `tmb_mesh_pack.py` importing `PS2Recomp/tools/native_pack` | Unchanged formats. `native_pack/` and `ps2_disc.py` move (to BozoEngine `tools/` or TMB `tools/`); the `sys.path` lines change. The renderer's pack loaders move with them, so the files stay byte-compatible |
| Disc | `disc/` extracted folder plus `disc.iso` | BozoEngine reads the ISO only (`libraries.md` 8). The extracted files are still needed at build time for the packs and the ELF, but the staged game can drop `disc/` |
| FFmpeg | kit's static LGPL 9.0.2 (avcodec, swscale, avutil), `-DPS2X_FFMPEG_ROOT` | the same build, `-DBOZO_FFMPEG_ROOT=<kit>/ffmpeg`. BozoEngine needs only avcodec and avutil, so swscale can leave the kit's configure |
| Window, input, audio device | raylib 5.5 (FetchContent) | **SDL3**, built static from a pinned release tag in the kit's `deps/` (no DLL, as today). Pinning MSYS2's `sdl3` package is the alternative, but it brings a DLL |
| ImGui | v1.92.7-docking with rlImGui | ImGui's own `imgui_impl_sdl3` and `imgui_impl_opengl3` backends (in the same repository, used as a dependency). rlImGui drops |
| GS on Vulkan | paraLLEl-GS + granite, already off in player builds | gone |
| MSYS2 kit pins | `build_kit_packages.json`: gcc 16.2, binutils, crt, headers, winpthreads, cmake 4.4.4, ninja + dependencies | Roots unchanged. BozoEngine needs GCC 13+ for `<format>`, and 16.2 qualifies. **One catch**: BozoEngine's `tools/run` and `tools/replay` `find_package(ZLIB REQUIRED)` at configure, and the stripped kit has no zlib headers. Add a `BOZO_BUILD_TOOLS` option (off when TMB includes BozoEngine) |
| `tmb_build.py` | steps verify, extract, tools, recompile, packs, iop, configure, compile, stage | verify, extract, tools (bozo-recomp), recompile (with externals), packs, configure, compile, stage. Other changes: `FETCH_NAMES` becomes `sdl3`, `imgui`; `LAUNCH_ENV` empties (every fork env default goes); fingerprints hash the BozoEngine tree; the exe path changes |
| Runner CMake | `add_subdirectory(PS2Recomp)` builds `ps2EntryRunner`; unity batches, PCH; `PS2X_APP_*` cache vars | TMB's own `add_executable(tmb ...)` links `bozo_runtime_core`, `bozo_libs`, `bozo_audio` and a new `bozo_app` (SDL3 window, renderer, input, audio, settings, panel). The app identity (`PS2X_APP_TITLE/FOLDER/...`) becomes `ps2_app`'s compile definitions, moved with it |
| PGO (`scripts/pgo.sh`, `runtime/pgo/tmb_pgo_train.cpp`) | targets `ps2_runtime`, `ps2_iop`, `tmb_vu1_aot` | targets `bozo_runtime_core`, `bozo_libs`, `bozo_audio`, `bozo_app` |
| Launcher | raylib + rlImGui + ImGui; about 35 raylib calls (window, monitor size, DPI, `LoadImageFromScreen`, a texture) | SDL3 + `imgui_impl_sdl3` + `imgui_impl_sdlrenderer3` (no GL needed). The UI code stays; the platform glue changes (about 2-3 days) |
| `make_build_kit.py` `DEPS` | raylib, imgui, rlimgui, elfio, toml11, fmt, libdwarf, rabbitizer | sdl3, imgui. `tools/` holds `bozo-recomp.exe` only; `licenses/` gains SDL3 (zlib) and loses libdwarf (LGPL) and the others |
| `release.yml` | builds the kit, `make_release.py`, smoke checks | Same steps. The submodule checkout needs access to BozoEngine; the sources zip carries BozoEngine; the licence notes drop the GPL fork |

## 5. Phase 4 breakdown

Sizes are focused work. PLAN.md's "1-2 weeks, mechanical" holds for TMB's own `runtime/` (section 3). The platform
layer BozoEngine still lacks (window, presentation, real-time pacing, PC input, audio device, settings and panel) and
the build kit bring the total to about **6-8 weeks of work**, or **4-5 weeks of calendar time** with the parallel
tracks below.

| # | Milestone | What | Check that proves it | Size | Parallel | User test |
|---|---|---|---|---|---|---|
| M0 | **TMB's runtime on BozoEngine, headless** | Port API (overrides, IoPaths, store hook, scratchpad, scheduler hooks for native frames), the externals generator plus a startup check, a log-free native host (the kernel and every library bound by ELF symbol, the vblank on a real or fixed clock), TMB's `runtime/` adapted with section 3 removed, renderer and mixer as null sinks | (1) `bozo-run boot` and `match1p --pinned` with **TMB's overrides running natively** instead of from the log: every frame still matches on ram, scratchpad, calls and threads. This proves the override and inner-guest-call path (1.3). (2) A log-free headless free run reaches the title's shell loop | 1.5-2 weeks | first; blocks M1-M5 | no |
| M1 | **Boots to the title in a window, free mode, real time** | `bozo_app`: SDL3 window, GL 3.3 context, present thread, the renderer moved (rlgl to plain GL), `FramePacer`, video settings, F11, the user folder and logs (`ps2_app`) | The title and menus shown at 60 fps on the US build. Window captures compared pixel by pixel with the fork build at the same frames (title, main menu) | 1-1.5 weeks | needs M0 | **yes**: menus look right |
| M2 | **A match is playable with pads** | SDL3 `PadInput` (hot-plug, mapping database, rumble), input bindings ported (`input.ini` compatible), multitap (4 players) in libpad | 1P Challenge match on keyboard and on a pad; 4P split screen with 4 pads; the scripted run `ch1p_frames` reaches the same end state as the fork | 0.5-1 week | after M1; parallel with M3 and M6 | **yes**: feel, rumble |
| M3 | **Sound through the mixer** | SDL3 audio stream pulls `Mixer::render`; 989SND's tick and the mixer on real time in free mode; the movie sound sink; `PS2X_AUDIO_RECORD` WAV | Pinned runs: the PS2XREF1 `sound` field (mixer output hash) equals the fork's for boot and match1p. User listens to the menus, a match and the intro movie | 1 week | after M0 (headless hash) and M1 (device); parallel with M2 | **yes**: by ear |
| M4 | **Movies, menus and the F2 panel** | libmpeg's picture hook to `setBackdrop`; the Settings window (Controls, Video, the port's Cheats tab) on `imgui_impl_sdl3`; FPS overlay (F3); F8 sequences and F9 bursts; crash reports | Intro and story movies play in sync; every F2 tab works; the cheats toggle; F9 burst PNGs are written | 1 week | after M1; parallel with M3 | **yes** |
| M5 | **Proven unchanged** | The PS2XREF1 `draws` field from the renderer's frames on both builds; every stage and arena captured and compared; timed FPS runs | `draws` and `sound` match the fork for the scripted runs; screenshots of every stage and arena pixel-identical; a busy 1P frame at or under the fork's ~5 ms (`PS2X_FRAME_STATS`) | 1 week | after M2-M4; screenshots can start after M1 | review of the differences |
| M6 | **The launcher builds it from the disc** | `tmb_build.py` steps (section 4), `make_build_kit.py` deps (SDL3, imgui; zlib-free BozoEngine configure), launcher on SDL3, PGO targets, `release.yml` | On a clean Windows PC with only the kit (no MSYS2, git or network), the launcher builds from the user's ISO and the game runs | 1 week | from M0 on, independent of M1-M5 | one run on a clean PC |
| M7 | **Release** | Both builds side by side for a release cycle (a launcher choice of engine, or two zips), then the default switches and the fork is retired for TMB | A release built on BozoEngine; every stage and arena clean; a few players report no difference | 1-2 weeks calendar, little work | last | **yes**: players |

### Renderer backend recommendation: OpenGL 3.3 core through SDL3, now

What the renderer uses today:
- `native_renderer.cpp` draws with GL 3.3 core: GLSL `#version 330` shaders, VBOs, FBO render targets, scissor,
  separate blend factors. It reaches GL through raylib's `rlgl` (about 99 calls) and five raylib render-texture
  and blit calls.
- `native_renderer_gs.cpp` is raw GL but drops.
- Two features need the GPU's readback and copy:
  - the lens flares read the frame's depth, as tiles read back one frame late;
  - `Material::sampleTarget` (heat haze) samples a copy of the target being drawn into.

Why GL:
- **It moves with almost no change.** The renderer keeps its GL 3.3 shaders and logic; only the rlgl and raylib
  calls become plain GL calls (about 100 call sites). SDL3 provides the context (`SDL_GL_CreateContext`, swap,
  vsync) and the loader (`SDL_GL_GetProcAddress`).
- **Load the functions with a small table we write.** The renderer uses about 60 GL functions. That avoids
  vendoring a generated loader.
- **ImGui has official SDL3 and OpenGL3 backends.**
- **This is the shortest path to "no change a player can see".** Any other backend changes rasterisation details
  that M5's pixel comparison would then have to explain.

When to consider SDL_GPU (Vulkan, D3D12, Metal):
- **Later, behind the renderer's existing GPU split.** The GPU-specific code is already in `struct Gpu`, about 750
  of the file's 1,802 lines.
- **The cost.** The shaders would go through SDL_shadercross or per-backend sources. The depth tiles and target
  copies become `SDL_DownloadFromGPUTexture` and `SDL_CopyGPUTextureToTexture` passes.
- **The gain.** No dependence on GL drivers (macOS, some handhelds) and native D3D12 on Windows.
- **When.** It earns its cost when Linux and macOS or the Steam Deck become goals. Not in phase 4.

Plain Vulkan is not recommended: it is the largest rewrite. TMB's frame costs about 5 ms of CPU against a light GPU
load, so Vulkan gains nothing measurable. The fork's only Vulkan code (paraLLEl-GS) belongs to the GS emulation that
is being dropped.
