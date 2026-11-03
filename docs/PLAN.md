# BozoEngine: the plan

Our own toolchain, native only, replacing the PS2Recomp fork under TMB_Recomp (and later the team's other ports).
Written 2026-11-02, from the state of TMB_Recomp 0.3.2 and the fork's `native/renderer` branch.

## 1. Where we start

TMB_Recomp 0.3.2 runs fully native by default: no GS emulation, no VU programs, no SPU2, no IOP hardware models,
0 interpreted IOP instructions. A busy 1P match takes about 5 ms of work per 16.7 ms frame. It does that on top of the
PS2Recomp fork (TheBozos/PS2Recomp, `native/renderer`), which is ~147,000 lines:

| Part | Lines | Written by us | ran-j (upstream) | Others |
|---|---|---|---|---|
| Recompiler (`ps2xRecomp`) | 15,325 | 10% | 82% | 8% |
| Analyzer (`ps2xAnalyzer`) | 5,524 | 0% | 97% | 3% |
| Runtime (`ps2xRuntime`) | 78,512 | 38% | 50% | 12% |
| IOP (`ps2xIOP`) | 17,456 | 39% | 58% | 3% |
| Tests (`ps2xTest`) | 28,474 | 10% | 64% | 26% |
| Tools | 1,589 | 92% | 0% | 8% |
| **All** | **146,880** | **29%** | **58%** | **13%** |

("Written by us": `git blame` on the current tree, the team's two GitHub identities. To confirm per file before moving
anything, see section 7.)

About 30,000 of those lines are emulation the native path no longer uses: the GS rasterizers, front end and memory,
the VU1 interpreter, JIT and inline core, the VIF1 interpreter, the GS and VU library stubs that feed them, the IOP
interpreter, the SPU2, the hardware-register side of guest memory. They are **not** part of this plan.

## 2. Goals and non-goals

Goals:
- A recompiler and runtime written by The Bozos, MIT-licensed, with no code copied from PS2Recomp or elsewhere.
- No PS2 hardware model at any level. The game's code is recompiled; everything it asks of the machine is answered
  at the level of the library call it makes.
- TMB_Recomp moves over with **no change a player can see**, proved frame by frame (section 5).
- At least as fast as today (5 ms of work per busy frame on the reference PC), build time no worse.

Non-goals:
- Running games that need hardware emulation. A port on BozoEngine replaces each hardware-driving path natively
  (as TMB already did) before it moves.
- Decompilation. The game's own logic stays recompiled machine code; readable source is the decomp projects' work
  (tmb_decomp asks that no one port its code until it nears completion: we respect that).
- Other platforms in the first version (Windows first; the code is kept portable, Linux after).

## 3. Architecture

```
   the game's ELF (player's disc) ──► recompiler ──► C++ (one function per guest function)
                                                         │ calls, at addresses
                                                         ▼
   ┌──────────────────────── runtime ─────────────────────────────────────────────┐
   │ guest memory: 32 MB RAM + scratchpad, nothing behind any other address        │
   │ EE services: threads, semaphores, event flags, alarms, interrupts handlers,  │
   │   the scheduler (one host thread runs guest code at a time, as today)        │
   │ function table + overrides (a port replaces a guest function by address)    │
   └───────────────────────────────────────────────────────────────────────────────┘
          │ the game's library calls (found by signature, replaced natively)
          ▼
   libs/: pad · memory card (native save files) · CD/DVD files (the ISO) · sound (the
          game's sound driver's API → native mixer) · movies (FFmpeg) · display mode
          ▼
   render/ (native renderer, packs) · audio/ (native mixer) · app/ (window, input, settings)
```

Key choices:
- **Same ABI for recompiled code as today**: `void fn(uint8_t *rdram, R5900Context *ctx, Runtime *rt)`, guest
  registers in a context struct, guest memory as one block. TMB's own code (13,000 lines in `runtime/`) uses that
  surface: `R5900Context` / `getRegU32` (~270 uses), function overrides by address, the native renderer and packs,
  a few scheduler calls. Keeping the shape lets TMB move with mechanical changes only.
- **No IOP.** Today the game's IOP modules (989SND, PADMAN, MCMAN, ...) are recompiled and run on an IOP kernel. In
  BozoEngine the EE side of each library talks to a native driver directly: the pad library reads the PC's input, the
  memory card library reads and writes native save files, the sound library's calls drive the native mixer. No IOP
  recompiler, no IOP kernel, no SIF.
- **No DMA, VIF, GIF, GS, VU.** The game's draw calls are hooked where the port's native renderer takes them (TMB
  does this today for every 3D object, particle, HUD and menu element). A packet the game still builds and kicks is
  completed at once, unread, as `setVif1Dropped` does on the fork; the long-term aim is that the game does not
  build it (its builder hooked away).
- **Determinism switch.** A mode where time comes from a fixed step and the scheduler's order is fixed, so two runs
  with the same inputs give the same state: the basis of the comparison in section 5.

## 4. Phases

Each phase ends with its exit check. Estimates are for focused work, in weeks.

**Phase 0: specs and harness (1-2 weeks).**
- Write the specs the code is written from (section 6): the R5900 instruction semantics we need (from Sony's EE
  manuals and the MIPS documents), the EE kernel calls TMB makes (from traces of the current build: name, arguments,
  results), the library calls TMB makes and what they must do.
- The reference recorder in TMB_Recomp (on the fork): a deterministic mode, and per frame a hash of guest RAM, the
  list of kernel and library calls with their results, the native renderer's draw list hash, the mixer's output
  hash. Recorded for scripted runs: boot to menu, a 1P Challenge match, a 4P match, a story level, a movie.
- Exit: the recorder replays the same run twice with identical hashes.

**Phase 1: recompiler (3-4 weeks).**
- ELF loading, function discovery (entry points, call targets, jump tables), the R5900 translation including the
  128-bit MMI instructions, COP1 with the PS2's float behaviour, COP0 where games touch it, delay slots, the
  checkpoints the scheduler needs. Our own output layout, kept compatible with the ABI above.
- Tests: per-instruction tests written from the spec; whole-function tests against the reference (run a recompiled
  function on both, compare registers and memory).
- Exit: TMB's whole ELF recompiles; every function the reference runs during the scripted runs gives the same
  results under the old runtime with the new generated code.
- **Status (2026-11-03): exit check passed.** Milestone 1: `bozo-recomp` translates TMB's whole `.text` (0 unknown
  words, 0 untranslated instructions), 139 instruction tests. Milestone 2: `bozo-replay` (`specs/replay.md`) replays
  the reference's per-function captures (`specs/capture.md`: boot, match1p, story, 8 passes each) through
  BozoEngine's generated code on BozoEngine's own runtime core, every out-call answered from the log: **9,232 of
  9,232 samples, 1,518 of 1,518 functions, bit for bit** (context and every byte of RAM, scratchpad and VU0 memory;
  6,449 samples at the first run). The fixes settled the FPU and VU0 rounding, the VU0 flags and FCR31
  (`specs/r5900.md` section 12: Q1, Q3, Q6 for VU0, Q7, Q9, Q10, Q11 settled; Q15 partly; the rest open or not
  reached by TMB). The 46 functions the runs call without a replayable sample (thread entry points and calls that
  block or are interrupted) are left to phase 2's whole-run comparison.

**Phase 2: runtime core (3-4 weeks).**
- Guest memory (RAM, scratchpad), the function table and overrides, the EE kernel services TMB uses (threads,
  semaphores, event flags, alarms, vblank and timer handlers as the game installs them), the scheduler with native
  frame pacing (as `FramePacer` does today), file access to the ISO.
- Exit: TMB boots to its title screen on BozoEngine with the recorded kernel call trace identical to the reference.
- **Reference side (2026-11-03): ready for boot.** The call log (`specs/calllog.md`, `PS2XCLG1`): the whole boot run's
  out-calls with their effects, and every change made outside guest code with its exact point; what BozoEngine
  implements natively (the EE kernel, section 6 there) and what it answers from the log. The reference build replays
  its own log with only guest code, its kernel and its scheduler running: every record and the PS2XREF1 fields `ram`,
  `scratchpad`, `calls`, `threads`, `eeCycle` match `boot.ref` for all 3000 frames.
- **Status (2026-11-03): boot passes; match1p to frame 5772.** BozoEngine's own EE kernel, dispatcher and cycle model
  (`runtime/ee/`, `specs/kernel.md`) run TMB's generated code with every library answered from the call log
  (`bozo-run`, `tools/run/README.md`), every record checked in lockstep. **boot: all 4,650,558 records, and the
  PS2XREF1 record equal to `boot.ref` on `ram`, `scratchpad`, `calls`, `threads`, `eeCycle` for all 3000 frames**
  (13.5 s). match1p: 18,297,744 records and frames 0-5772 equal on every field; frame 5773 stops on the EE time of
  a recursion stopped in the middle, whose rule is open (`specs/kernel.md` section 9, K1). `draws` and `sound` wait
  for the native renderer and mixer. The recompiler now emits the reference's time model and resume entries; phase 1's replay still
  matches 9,232 of 9,232 samples.

**Phase 3: native libraries (4-6 weeks).**
- Pad (and multitap, 4 players), memory card (native save files, plus an importer for today's `mc0`/`mc1` card
  images so nobody loses a save), CD/DVD streaming, the sound library calls onto the native mixer (989SND's API: the
  largest piece, banks, sequences, voices, effects), movies (the PSS stream through FFmpeg, picture and sound), the
  display mode and vblank, the font and libc helpers the game links.
- Exit: every scripted run matches the reference hashes (RAM, calls, draw list, sound) for its whole length.

**Phase 4: move TMB (1-2 weeks).**
- TMB_Recomp builds against BozoEngine: its `runtime/` adapted (mechanical), the build kit and launcher pointed at
  the new tree, the release workflow updated.
- Both builds side by side for a release cycle; players get the new one when the comparison is clean on every
  stage and arena and a few people have played it.
- Exit: a TMB release built on BozoEngine, the fork retired for TMB.

**Phase 5: the other ports (each when ready).**
- CoN, VP2S, SC3, JC2, DC and the rest move when their native path no longer needs the fork's emulation, each with its
  own reference recording. BozoEngine takes their game-generic pieces (library implementations, renderer features)
  as they come.

Total for TMB: roughly 12-18 weeks.

## 5. How we prove nothing changed

The current TMB build is the reference. Both runtimes run the same scripted inputs in the deterministic mode, and we
compare, frame by frame:
1. a hash of guest RAM at the end of each game frame (game state: positions, health, AI, timers);
2. the kernel and library calls with arguments and results;
3. the native renderer's draw list (batches, vertices, textures, matrices);
4. the mixer's output samples.

A mismatch names the first frame and the first differing call or memory range, which is where the bug is. On top of
that, screenshots of every stage and arena compared pixel by pixel, the timed FPS runs (`PS2X_FRAME_STATS`), and
people playing before a release.

## 6. Clean room

The point is that the code is ours. Rules:
- No file, function or line copied from PS2Recomp (or any other project) into this repository.
- Code is written from **specs** in `docs/specs/`: hardware and library documentation, the game's observed
  behaviour (traces, recordings), and our own descriptions of what each piece must do. The specs may be written by
  someone who knows PS2Recomp; the specs must describe behaviour, not its code.
- Code we wrote on the fork (the native renderer, the mixer, the packs, the IOP-free pieces, tools) moves over after
  the per-file check in section 7.
- Third-party libraries (raylib or SDL3, Dear ImGui, FFmpeg, miniaudio) are used as dependencies under their own
  licences, never copied in.

## 7. Moving our own fork code

For each file we want to bring: `git blame` on the fork's current tree; a file moves only if every line is the team's
(boubou666, thomas.epitech, and Arzaroth's identities). Files with lines by anyone else are rewritten from a spec
instead. To confirm: whose is `gitea@barrut.me` (5,000+ runtime lines)?

## 8. Risks

- **Recompiler correctness.** Most of the bugs of the last months were in exact MIPS behaviour and timing. Mitigation:
  per-instruction tests, then whole-function comparison against the reference before anything else is built on it.
- **Scheduling and timing.** The game's threads and vblank handlers depend on order. Mitigation: the kernel call
  trace comparison from the first boot; the deterministic mode.
- **The sound driver.** 989SND's API is large. Mitigation: start from the calls TMB makes (recorded), our native
  mixer already plays them today.
- **Scope creep from the other ports.** Each moves on its own schedule; BozoEngine does not take emulation back.

## 9. Open questions

- Public or private until it runs (created private).
- raylib (as today) or SDL3 for window and input: the plan works with either; SDL3 is a separate decision.
- The authorship check of section 7 for Arzaroth's commits.
