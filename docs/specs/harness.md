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
