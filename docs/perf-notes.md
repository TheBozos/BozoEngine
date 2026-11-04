# Frame time: handover (perf/frame, paused 2026-11-05)

Branch `perf/frame` (worktree `D:\BozoEngine_perf`, from main 37ba878). Main 8575559 already holds everything through
c4d767c. TMB: no change was needed (`D:\TMB_perf`, branch `bozo/perf` = `bozo/m0` 03b6a2f; builds in `build/bozo`
for perf/frame and `build/bozo_base` for the unchanged engine at 37ba878, from the detached worktree
`D:\BozoEngine_perfbase`).

## Tools (`D:\BozoScratch\perf\tools`)

- `sampler.exe <pid> <s> <out> [interval_us] [stack depth] [raw]`: a sampling profiler for Windows. It suspends each
  thread that ran, reads RIP (and a dbghelp stack walk when given a depth), and writes per-thread stacks; `raw` adds
  every sample with its time.
- `prof.py`: symbols through nm, with groups, inclusive buckets (`buckets2.py` fits both builds) and leaf callers.
- `callers.py <samples> <tid>`: the first frames in the exe per sample, which shows where a thread blocks.
- `hitch.py`: splits raw samples into frames at the pacer's wait and compares slow frames with the rest. Written, not
  run yet.
- `profrun.sh fork|bozo <name> <start> <dur> ...`: the M5 scene (1P challenge; SCENE=4p, 4pb or stage, the last with
  LEVEL), sampled from `start` for `dur` seconds. It copies the exe beside its samples, so the symbols stay valid.
  EXTRA_ARGS and EXTRA_ENV are passed through to the run.
- `timing.sh <tag> <min> fork|base|name=exe ...`: runs in order under the bench lock.
- `fstat.py --from 125 --to 245 <logs>`: PS2X_FRAME_STATS summaries.
- `checks.sh <tag> <build> pin,free`: the pinned boot and match1p, and free 1P (16,000 frames) and story (12,000
  frames) records.

Don't edit `profrun.sh` while a `timing.sh` runs (it broke the tail of one fork run in t2).

## What the profile showed (game thread, 1P match, % of wall time)

Before (37ba878), BozoEngine against the fork:

| Bucket | BozoEngine | Fork |
|---|---|---|
| Pacer wait | 34% | 52% |
| Mesh submit | 16% | 16% |
| C++ exception throws (StopGuest) | 10.3% | 0 |
| VU0 (programs plus COP2 ops) | 7% | 2.8% |
| Mixer | 4.1% | 0.6% (on its own thread) |
| Particles | 3.6% | 2.0% |
| fesetround | 2.1% | 0 |
| Draw digest | 1.5% | 0 |

Other costs on the BozoEngine side:
- `Kernel::best`: 2.6%.
- `runFrame` with toWords: 2.6%.
- `Renderer::draw` regrowing its vectors: 2.8%, against 0.2% on the fork.

The pinned match1p makes 585 stops a frame (9,355,622 in 16,000 frames).

After c4d767c: pacer wait 66% (so game work about 34% of the frame). Remaining work:
- mesh 11%;
- generated code and the rest 9%;
- VU0 6% (about half of it the COP2 macro ops `bozo::vu0::fmac/madd`, `ps2f::addNearest`, called out of line from
  the -O1 generated code);
- mixer 4.7%;
- particles 2%.

## Fixes and gains (perf/frame)

| Commit | Fix |
|---|---|
| 7f0a6ee | Stops leave guest code by `__builtin_longjmp` (free host only: `Dispatcher::jumpStops`); `keepStopWords` off on the free host |
| 24292a6 | Ready-queue bitmask for `best()` and `sliceContender()` |
| 7021784 | VU0 rounding through MXCSR instead of fesetround |
| 171389f | Published frames recycled through a pool; draw digest only when a record is written |
| b9ad0dd | Hashed out-call lookups; FlushCache by address |
| c4d767c | Present: flare depth tiles read through 3 fenced PBOs, by runs along tile rows, in D24S8's native format |

Before c4d767c the presents fell to 14-45 a second while the window thread blocked in `readDepthTiles` (one
ReadPixels per tile, 200-1,400 tiles a present in night or headlight scenes, and the buffer mapped at the next present).
After it, 60.0 throughout the 1P match.

Headless free 1P, 16,000 frames: 192 s on main, 128 s on perf/frame (the two run side by side).

## Timing (bench lock, 1P match, 125-245 s of each run, t2)

| Run | Frames | Mean ms | Mean of p95 | Worst p95 | Max | Over 16.7 ms |
|---|---|---|---|---|---|---|
| fork 1 | 7,145 | 7.49 | 10.05 | 13.00 | 19.9 | 2 |
| perf/frame 1 | 7,230 | 6.39 | 8.72 | 12.25 | 18.9 | 2 |
| base (37ba878) 1 | 7,184 | 10.86 | 14.26 | 24.25 | 42.0 | 438 |
| fork 2 | 7,137 | 7.36 | 9.87 | 16.00 | 41.9 | 17 |
| perf/frame 2 | 7,227 | 7.14 | 9.46 | 13.75 | 17.2 | 2 |
| base 2 | 7,217 | 7.78 | 10.59 | 17.25 | 22.7 | 26 |

perf/frame comes in at or under the fork on both pairs, with 2 frames over budget each time against the fork's 2 and 17.
That is only 2 pairs, and the box is shared and noisy: get more pairs.

## Checks (after c4d767c)

- Pinned boot and match1p: MATCH. Draws 2999/3000 and 15999/16000 (the log's last frame, as before).
- Free 1P and story records: byte-identical to 37ba878's. Checked after the game-work commits; c4d767c is present-side
  only.
- bozo-replay: 9,232/9,232.
- ctest: 4/5. `instructions` fails on main too: its FMAC O-flag case was not updated for def8230.

## Open, and the next steps

1. **The coordinator's 4P menu drop** (`D:\BozoScratch\verify_4p.txt`: 42 presents a second around game frame
   7000-7300, `--capture-frames 7000`).
   - My reruns of the 4P script on tmb_r4 (c4d767c), with and without `--capture-frames 7000`, held 60 presents a
     second from frame 6000 to 8000, max 18.7 ms between pictures (`prof/m4p-a.log`, `prof/m4p-b.log`).
   - So neither the capture nor the flare path reproduces here. Check whether `D:\BozoScratch\m23\tmb.exe` differs
     (its window size and render scale), and whether that run reached the same screen.
2. **More timing pairs**, then 4P with bots (SCENE=4pb) and the effects-heavy stage (SCENE=stage, LEVEL=20: fires and
   explosions in its sheet), fork against perf/frame.
3. **Hitches.** Run `sampler.exe ... 1000 32 1` on a 1P match, then `hitch.py`. Also look at the window's 1P "pictures
   over 20 ms" (0-10 per 2 s in the coordinator's check).
4. **Game work left:**
   - The mixer on the game thread (4.7%): the inner loop, or a worker thread fed the libsd calls in order.
   - The COP2 macro ops out of line (always_inline fmac/madd, or the generated code at -O2 for those files).
   - Per-batch GL state in present (TexParameteri, uniforms per batch).
5. **PGO:** not a lead. The fork's build doesn't use it (`TMB_PGO=OFF` in `build/game_us`).
