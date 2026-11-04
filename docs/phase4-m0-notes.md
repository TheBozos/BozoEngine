# Phase 4, milestone M0: TMB's runtime on BozoEngine, headless

Written 2026-11-04 on BozoEngine `phase4/m0` and TMB_Recomp `bozo/m0` (from `main` 7f508eb). M0 of
[`phase4-inventory.md`](phase4-inventory.md) section 5: TMB's `runtime/`, minus what drops with the emulation, builds
against BozoEngine instead of the PS2Recomp fork, headless, proven by the pinned check with TMB's overrides native and
by a log-free free run to the title screen.

## Building

TMB_Recomp (`bozo/m0`), BozoEngine by path (`cmake/bozo.cmake`):

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build/bozo -G Ninja -DTMB_BOZO=ON -DBOZO_ENGINE_DIR=<BozoEngine> -DTMB_ELF=<SCUS_971.01> \
      -DBOZO_FFMPEG_ROOT=D:/tmb_kit/kit/ffmpeg
cmake --build build/bozo
build/bozo/tmb-check D:/BozoCaptures/tmb/calllog/boot.clg --ref <BozoEngine>/reference/tmb/records/boot.ref --pinned \
      --iso "<TMB.iso>"
build/bozo/tmb-check D:/BozoCaptures/tmb/calllog/match1p.clg --ref <BozoEngine>/reference/tmb/records/match1p.ref \
      --pinned --iso "<TMB.iso>" --pad-script <BozoEngine>/reference/tmb/scripts/ch1p_frames.txt
build/bozo/tmb-headless --iso "<TMB.iso>" --frames 8000 --report 1000
```

`TMB_ELF` is the disc's `SCUS_971.01` (extracted from the ISO's root directory). The generated code comes from
`bozo-recomp` with the externals `bozo-externals` writes (the ELF's symbols BozoEngine answers, `recomp/tmb_stubs.txt`,
TMB's override list from `tools/tmb_overrides_list.py`). The texture and mesh packs are looked for where the fork build
put them (`generated_us/`); without them the native draws are untextured and the hier objects not drawn, which changes
nothing the game computes.

## What the port API is

`runtime/include/bozo/port.h` (`bozo_port`): `Registry::registerFunction` (TMB's install functions take it as
`runtime`, so their 84 registration lines are unchanged), `BOZO_PORT` (the fork's `PS2_REGISTER_GAME_OVERRIDE`),
`port::undeclared` (the startup check: a registered override that is not an external of the generated code stops
the run), and what overrides use while they run: the kernel (`changePriority`, `rotateReadyQueue`, `transfer`), the EE
clock (`now`, `accountCycles`), `vblankCount`, the hardware registers (`ioRead32`, `ioWrite32`: a port's own access),
the frame end (`frameEnd`: the host takes the frame), `callLibrary` (an override calling the original where it is a
library BozoEngine implements), `scratchpad`, `libraries`, `deterministic`, `realTime`, `setStorePatch`. A
`port::Host` is behind them: `bozo-run`'s log host (`tools/run/log_host.cpp`) or the free host
(`runtime/host/free_host.cpp`, `bozo_host`).

TMB keeps its names: `getRegU32` and friends (`bozo/context.h`), `PS2Runtime` (an alias of BozoEngine's `Runtime`,
`runtime/tmb_port.h`), the generated functions by name (`tmb_functions.h`).

## Overrides that run guest code

[`specs/kernel.md`](specs/kernel.md) section 3b holds the rules, each with the record that settled it. In short: an
override is one out-call; its guest code charges EE time; an event due, a call that blocks, a jump through a register,
an inner call's return and a recursive call's return make guest code leave the override at that point, the stop
deferred to the override's end (`Dispatcher::leave`): the generated code returns (`bozo::checkpoint`, the check after
each call and `bozo::innerReturn` test `ctx->leaving`; a recursive call's return inside an override calls `bozo_leave`),
the override's own code after the guest call runs, and the host stops the thread at the pc. No exception goes through
an override. In a pinned run the log says where the reference's override left (its record's transfer flag, pc and end
cycle) and whether a timed library call inside waited (its traced entry and the OP `block`).

Generated code changed for this (BozoEngine's own ABI, `abi.md` section 3): `bozo::checkpoint` and
`bozo::innerReturn` return true when guest code leaves; `ctx->outcall`, `ctx->leaving`; `bozo_leave`; the
`<prefix>_external_table`; `store 0xADDR` externals (the store sets `ctx->pc` first: TMB's dialog-kick store patch).

## Results

| Run | Frames | Records | ram, scratchpad, calls, threads | Overrides | Time |
|---|---|---|---|---|---|
| boot `--pinned`, TMB's overrides native | 3000 of 3000 | 4,650,558 of 4,650,558 | every frame | 743,917 calls (4,730 nested), 1,090,692 out-calls inside, 2,999 frame ends, 0 divergences | 32 s |
| match1p `--pinned` (`ch1p_frames.txt`), TMB's overrides native | 16000 of 16000 | 74,615,923 of 74,615,923 | every frame | 7,282,624 calls (152,779 nested), 3,823,185 out-calls inside, 15,999 frame ends, 0 divergences | 403 s |

`bozo-run` without a port still matches boot and match1p (pinned); the 9,232 capture samples still match.

The free run (`tmb-headless`, no log, every library native, BozoEngine's own clock, no window): the movies play
(1,607 vblank handler calls, 772 PCM callbacks, 3 videoDecFlush), the shell's loop `uiMain__Fv` ends every frame from
frame 3, its screen (gp - 0x3d2c) is 8 from frame 3 and 0 (`uiTitle_Main__Fv`, the title) from frame 1116 to the end of
an 8000-frame run (about 1,000 frames a second). With match1p's pad script it goes through the menus (screens 1, 3, 5,
7) and from frame 5591 runs the match (`rtMain__Fi` ends every frame, the renderer gets 3D batches) to frame 16000.

Draws (reported, not required yet): the first 1,615 frames' draw lists equal the reference's (logos, movie pictures),
1,916 of match1p's 16,000; from frame 1615 they differ (the shell's 2D, whose font comes from the texture pack this
build did not have: the packs live in TMB_Recomp's `generated_us/`, which this work did not use).

## What M1 needs

- The renderer's GPU side (`native_renderer.cpp`'s present thread, `struct Gpu`: rlgl to plain GL 3.3 through SDL3),
  on top of `bozo/render/renderer.h` (the game side is moved; `published()`, `publishedOverlay()`, `backdrop()` are
  what the present thread takes).
- A real-time host: the free host with the window, `port::Host::realTime()` true (the FramePacer then holds frames to
  the field rate), the vblank on the EE clock as now. `ps2_app`, video settings, input bindings, the F2 panel.
- The packs built by the toolchain (`native_pack/` moves with them) so the draws are textured; then the `draws` field.
- Multitap: `PadState::multitap` is the flag TMB's `mtap*` overrides set; the slots behind it come with the PC pads (M2).
