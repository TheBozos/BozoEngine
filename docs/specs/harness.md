# Spec: the reference harness

How BozoEngine proves a port runs the same as on the runtime it replaces. Phase 0 of the plan builds the recording
side in TMB_Recomp (on its current runtime); phase 1 onwards builds the same recorder into BozoEngine, and a tool
compares the two.

## 1. Deterministic mode

Two runs with the same inputs must give the same guest state, frame by frame. In deterministic mode:

- **Time is the game frame.** Every clock the game can observe advances by a fixed amount per game frame (the frame
  the game ends when it hands its picture over), never by host time:
  - the step the game's frame loop is given: exactly one field (1/60 s for NTSC);
  - EE cycles charged for time the game did not account: a fixed amount per frame;
  - vblank interrupts, alarms and timers: counted in game frames / EE cycles;
  - the IOP clock (its threads, timers, vblank waits): a fixed number of IOP cycles per game frame;
  - real-time clock reads (date, time of day): a fixed date.
- **Sound is mixed on the guest's schedule.** The mixer produces a fixed number of samples per game frame (48000 / 60
  = 800 per channel at 60 Hz) on the guest thread; anything the game or its sound driver can observe (voice ended
  flags, transfer completions, stream positions) changes only there. The audio device plays from a queue and never
  changes guest-visible state.
- **Input is the script's, keyed on game frames.** `frame:BUTTON/frames` entries; the host's devices are not read.
- **No host thread writes guest-visible state** except at the guest thread's own frame boundary.
- **The host's files are fixed.** A test user folder (empty memory cards or a given card image, default settings).
- The window still shows the frames and may pace them to real time; pacing must not change what is computed.

## 2. What is recorded

One record per game frame, in a binary file `<run>.ref` (little-endian), after a header naming the run, the build
and the game's ELF hash:

| Field | What |
|---|---|
| frame | game frame number (0 = the first frame the game ends) |
| ram | 64-bit hash (xxHash3 or FNV-1a 64; the header says which) of the 32 MB guest RAM at the frame end |
| scratchpad | hash of the 16 KB scratchpad |
| calls | hash of the frame's kernel and library calls, each `(id, arguments, result)` in call order; the full list is kept for the first N frames and on demand |
| draws | hash of the native renderer's draw list for the frame: per batch the material, matrix, vertex data, texture identity (content hash, never a host pointer) |
| sound | hash of the 800 x 2 samples mixed for the frame |
| threads | the guest thread that ran last and the ready queue's order |

Ranges of RAM that hold host-side values only (none should exist; the recorder checks) can be masked by the header.

## 3. Comparing

`compare a.ref b.ref` reports the first frame where any field differs, which field, and for `ram` the first differing
32-byte block (address and both values, from full dumps the recorder writes around the first mismatch on request).
For `calls` it prints both frames' call lists side by side from the first difference.

## 4. Reference runs

Scripted runs, each from a cold boot with a fixed user folder:

| Run | What it covers | Length |
|---|---|---|
| `boot` | boot, movies, title, menus to the level select | ~60 s |
| `match1p` | a Challenge match (Junkyard) with bots | ~3 min |
| `match4p` | 4-player split screen | ~3 min |
| `story` | a story start and its first level | ~4 min |
| `stage-N` | each story stage started directly (Story: start at) | ~2 min each |
| `movie` | a full movie with sound | ~1 min |
| `save` | a save to the memory card and a load | ~2 min |

## 5. Acceptance

A component of BozoEngine is accepted when every reference run matches field for field on its whole length, and
the timed runs (`PS2X_FRAME_STATS`-style work per frame) are no slower.

## 6. The record file (PS2XREF1)

The format both the reference build and BozoEngine write (`runtime/harness/reference.cpp`, `bozo/reference.h`;
compared with `tools/capture/ref_compare.py`). Little-endian.

Header (240 bytes, then `maskCount` pairs of u32 {address, bytes}, the RAM ranges hashed as zeros):

| Offset | Type | Field |
|---|---|---|
| 0 | char[8] | magic `PS2XREF1` |
| 8 | u32 | version (1) |
| 12 | u32 | headerBytes (240 + 8 x maskCount: where the records start) |
| 16 | u32 | recordBytes (80) |
| 20 | u32 | hash kind (1: XXH64 seed 0; the sound FNV-1a 64) |
| 24 | u32 | ramBytes (0x2000000) |
| 28 | u32 | scratchpadBytes (0x4000) |
| 32 | u64 | XXH64 of the game's ELF file |
| 40 | u32 | flags (bit 0: deterministic mode) |
| 44 | u32 | maskCount |
| 48 | char[64] | run name |
| 112 | char[64] | build |
| 176 | char[64] | the ELF's file name |

Record (80 bytes), one per game frame:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | frame |
| 4 | i32 | the guest thread running at the frame end |
| 8 | u64 | `ram`: XXH64 of the 32 MB of RAM at the frame end |
| 16 | u64 | `scratchpad`: XXH64 of the 16 KB scratchpad |
| 24 | u64 | `calls`: XXH64 of the frame's calls, 7 u32 each {id, a0, a1, a2, a3, v0, flags} in call order |
| 32 | u64 | `draws`: the renderer's draw lists (0 until BozoEngine has its renderer) |
| 40 | u64 | `sound`: FNV-1a 64 of the frame's mixed samples (0 when none) |
| 48 | u64 | `threads`: XXH64 of i32 {the running thread, then the ready threads in run order} |
| 56 | u64 | `eeCycle`: the EE cycle count at the frame end |
| 64 | u32 | callCount |
| 68 | u32 | drawBatches |
| 72 | u32 | drawVertices |
| 76 | u32 | soundSamples |

`calls`: the calls guest code makes by `jal`/`jalr` to a named library or kernel stub, and raw syscalls (`kernel.md`
section 5 for what is left out). `id` is the FNV-1a 32 hash of the stub's name with the top bit clear, or
`0x80000000 | number` for a syscall; `v0` the result's low word; `flags` 1 for a call that blocked, 2 for the call
the frame ended inside (both with v0 0). The run order of `threads` is the kernel's (`kernel.md` section 2): the
ready queues from priority 0, each in order.
