# Phase 4, milestone M5: is TMB on BozoEngine unchanged from today's build?

Written 2026-11-04 on BozoEngine `phase4/m5` (from main 2ed83b7) and TMB_Recomp `bozo/m5` (from `bozo/m0` 816a03d).
M5 of [`phase4-inventory.md`](phase4-inventory.md) section 5. The reference is today's build: TMB_Recomp main with
the PS2Recomp fork. Its deterministic recorder is the harness worktree: TMB c83a924, which is main a7869c0 plus the
harness, and the fork's `harness/calllog`. Main after a7869c0 only adds a SIF alias fix that TMB never reaches.

## Summary

- **Call logs: 13 new recordings.** Every reference run now has one (story, save, match4p, the ten stage runs). Each
  recording reproduces its stored `.ref` on every field. They are in `D:\BozoCaptures\tmb\calllog\`, listed with
  sizes and hashes in [`reference/tmb/README.md`](../reference/tmb/README.md).
- **Pinned checks** (`tmb-check --pinned --native all`, with the pad scripts and cheats of each run's env), using
  the default 989SND models:
  - 9 of 15 runs match on every frame for ram, scratchpad, calls, threads and draws: boot, match1p and stage
    levels 0, 4, 5, 13, 14, 16 and 20.
  - 4 more match on every frame once the 989SND replies that the reference's IOP timing decided are taken from the
    log (`--snd-log-replies on`): story, match4p (4P split screen), stage levels 2 and 3. Their default runs stop
    where 989SND's lifetime and handle models disagree with the reference (an open item since M3).
  - 2 stop before the end. `save` stops at frame 5458 in the kill-enemies test cheat. `stage-level6` stops at frame
    7105, where an interrupt lands one checkpoint off.
  - Draws equal the reference's on every frame of every run checked, except the last frame of three logs. There the
    log ends before TMB's frame end publishes the frame (boot, match1p, stage-level20).
  - Sound is reported, about 2,290 frames equal in every story-mode run: the silent frames and the movies equal the
    reference's; the music and effects are not bit-equal.
- **Pixels.** I compared 371 frames, 640x448 at render scale 1, the fork's capture against BozoEngine's pinned
  capture of the same game frame. Every picture is **pixel-identical** (max difference 0) except the level-load
  screens: during a load, several pictures share one frame count (section 3).
  - Two real differences were found on the way and fixed: the letterbox's 1-pixel offset and the heat haze.
  - Sheets are in `D:\BozoScratch\m5\sheets\<run>.png`.
- **Sound** (WAVs for A/B in `D:\BozoScratch\m5\wav\`):
  - Levels match to 0.2 dB, and the spectra correlate at 0.9995 or better per 10 s.
  - The intro movie is bit-identical.
  - Music and effects land a few samples apart, so per-sample equality is low (12% in a match).
- **Frame time.** In a busy 1P match, BozoEngine's game work per frame is about 8-9 ms, against the fork's 6-8 ms on
  the same machine. Frames over 16.7 ms: 46-353 against 5-30. Not at the inventory's "at or under the fork's" bar
  (section 5). 4P with no bots: 0.85 ms against 0.47 ms, both far under budget.
- **Fixes:** 17, in 6 commits on `phase4/m5` and 2 on `bozo/m5` (section 6). **Open:** section 7.

## 1. The recordings

Recorded on the harness worktree build in deterministic mode with a fresh `PS2X_USER_DIR`, four at a time.

**Method check.** Recording `boot` again this way (`PS2X_CALL_LOG`, plus `PS2X_AUDIO_RECORD`, render scale 1)
gives the same file as `boot.clg` but for the run name. Its record is identical to `boot.ref`.

| Run | Frames | Records | Calls | Size | Time | Record vs stored `.ref` |
|---|---|---|---|---|---|---|
| story | 11000 | 40,608,019 | 33,414,935 | 1.84 GB | 39 min | identical, every field |
| save | 20000 | 65,104,895 | 53,889,583 | 3.04 GB | 62 min | identical |
| match4p | 17000 | 87,437,930 | 76,421,081 | 4.97 GB | 86 min | identical |
| stage-level0 | 11000 | 54,205,000 | 46,438,334 | 2.57 GB | 52 min | identical |
| stage-level2 | 11000 | 49,742,332 | 42,279,624 | 2.41 GB | 46 min | identical |
| stage-level5 | 11000 | 50,850,869 | 42,995,577 | 2.27 GB | 49 min | identical |
| stage-level6 | 11000 | 32,995,788 | 26,238,711 | 1.39 GB | 32 min | identical |
| stage-level14 | 11000 | 34,064,002 | 27,019,295 | 1.47 GB | 33 min | identical |
| stage-level3 | 11000 | 43,897,973 | 36,385,003 | 1.83 GB | 43 min | identical |
| stage-level16 | 11000 | 43,976,847 | 37,316,873 | 1.86 GB | 43 min | identical |
| stage-level4 | 11000 | 52,010,924 | 44,193,622 | 2.12 GB | 50 min | identical |
| stage-level13 | 11000 | 45,523,473 | 38,773,954 | 1.98 GB | 43 min | identical |
| stage-level20 | 11000 | 32,671,886 | 22,962,916 | 1.21 GB | 28 min | identical |

Each recording also wrote the fork's sound. Those WAVs are wall-clock device output, so they are not used for sound
(section 4). The deterministic real-time runs below give that sound instead.

## 2. Pinned checks

The final build is BozoEngine def8230 + 79fb362 and TMB 03b6a2f.

| Run | Default (989SND models) | ram / scratchpad / calls / threads | draws | sound frames equal (silent) | With the 989SND IOP-time replies from the log |
|---|---|---|---|---|---|
| boot | **MATCH** | 3000/3000 | 2999/3000 (last) | 1659 (632) | |
| match1p | **MATCH** | 16000/16000 | 15999/16000 (last) | 3926 (2900) | |
| match4p | stops at frame 5613: a play's handle (0x11) | 5613/5613 | 5613/5613 | 1660 (633) | **MATCH**: 17000/17000, draws 17000/17000 |
| story | stops at frame 5067: SoundIsStillPlaying (0x19) | 5067/5067 | 5067/5067 | 2289 (1013) | **MATCH**: 11000/11000, draws 11000/11000 |
| save | stops at frame 5067 (as story) | 5067/5067 | 5067/5067 | 2289 | stops at frame 5458: the kill-enemies cheat (7.3) |
| stage-level0 | **MATCH** | 11000/11000 | 11000/11000 | 2289 (1013) | |
| stage-level2 | stops at frame 6782: a play's handle | 6782/6782 | 6782/6782 | 2289 | **MATCH**: 11000/11000 |
| stage-level5 | **MATCH** | 11000/11000 | 11000/11000 | 2289 | |
| stage-level6 | stops at frame 7105: an irq one checkpoint off (7.4) | 7105/7105 | 7105/7105 | 2290 | the same |
| stage-level14 | **MATCH** | 11000/11000 | 11000/11000 | 2289 | |
| stage-level3 | stops at frame 5201: a play's handle | 5201/5201 | 5201/5201 | 2289 | **MATCH**: 11000/11000 |
| stage-level16 | **MATCH** | 11000/11000 | 11000/11000 | 2289 | |
| stage-level4 | **MATCH** | 11000/11000 | 11000/11000 | 2289 | |
| stage-level13 | **MATCH** | 11000/11000 | 11000/11000 | 2288 | |
| stage-level20 | **MATCH** | 11000/11000 | 10999/11000 (last) | 2402 (1126) | |

- **eeCycle** matches on every frame checked, as `--pinned` follows the log's clock.
- **Draws** are now a checked field in a port's pinned run (`run_main.cpp`), on every frame but the log's last.
  - That last frame's FRAME record comes inside a library call, before TMB's frame end publishes (M1).
  - Logs whose last frame ends elsewhere match there too: story, match4p and the stage runs.
- **Sound** frames equal: about the same count for every story-mode run (2288-2290; 2402 in stage-level20).
  - Those are the silent frames, the logos, the intro movie to the sample, and the shell music's first notes.
  - From the shell music on, 989SND's single libsd calls land a sample or a few apart (the cost model and the unpaid
    IOP time G, M3's open items). So the per-frame hashes differ from there.

## 3. Pixels

**How the pictures are made.**

- **Fork.**
  - The harness build in deterministic mode at real time, from a fresh user folder.
  - Window 640x448, `PS2X_RENDER_SCALE=1`, game frame rate (no interpolation).
  - `PS2X_CAPTURE_SEQUENCE=1` every 8 s (`rec/env/pic-<run>.env`).
  - Each capture's `seq<k>.txt` names the frame it shows. Every such run's record was again identical to the
    stored one.
- **BozoEngine.** The same pinned run as section 2, shown in a window: `tmb-check --window --capture-frames`, new
  (`bozo/tmb_check.cpp`, `run::setFrameHook`, `WindowOptions::onCaptured`).
  - The run is held at each chosen frame's end until the window has read that frame's picture back. So the picture
    is exactly that frame's.
  - Window 640x448, render scale 1, 4:3 letterboxed.
  - The runs that stop on a 989SND model reply are shown with the log's replies.
- **Pairing.** The fork's label is its own published-frame count. It equals BozoEngine's frames-ended count, but
  runs 1-2 frames behind in busy stretches (match1p: 27 of 37 at +0, 3 at +1, 7 at +2). So each fork picture is
  compared with BozoEngine's frames label-1 to label+3, and the closest is kept. Every pair then matched exactly
  (`D:\BozoScratch\m5\sheets\pairs\<run>.offsets.txt`).
- **Measures.** `tools/pixcmp.py`: the largest channel difference, the share of pixels that differ at all, by more
  than 2 and by more than 8, and |difference| x8 images.

| Run | Pictures compared | Pixel-identical | Different (max, share of pixels) | What they show |
|---|---|---|---|---|
| boot | 7 | 7 | | logos, credits, intro movie, title |
| match1p | 37 | 37 | | menus, Junkyard 1P: explosions, rain, rear view |
| match4p | 39 | 39 | | menus, 4P split screen (four HUDs, radars) |
| story | 26 | 25 | frame 4707: 151, 6.7% (loading screen) | menus, story movies, Junkyard |
| save | 14 (34 after frame 5458 not reached) | 14 | | as story to the first kill |
| stage-level0 | 26 | 25 | frame 4707: 151, 5.2% (loading screen) | Midtown Suburbs |
| stage-level2 | 26 | 25 | frame 4707: 151, 5.6% (loading screen) | Midtown Expressway 104 |
| stage-level5 | 26 | 26 | | Downtown Business District |
| stage-level6 | 17 (9 after frame 7105 not reached) | 17 | | Black Vista Highway (with its road, TMB 0.3.2's mesh-pack fix) |
| stage-level14 | 25 | 25 | | Minion's Stadium |
| stage-level3 | 26 | 26 | | Prison Passage |
| stage-level16 | 25 | 24 | frame 4705: 151, 0.5% (loading screen) | Blackfield Asylum |
| stage-level4 | 26 | 26 | | Abandoned Skyscrapers |
| stage-level13 | 25 | 24 | frame 4705: 151, 0.5% (loading screen) | Rooftops, second part |
| stage-level20 | 26 | 26 | | Warhawk's Rooftop, then the title |
| **all** | **371** | **366** | **5, all loading screens** | |

- Of the 371 pairs, 352 matched at the fork's own label, 18 at +1 or +2 and 1 at -1.
- Two stage-level20 title pictures after its level matched only at +7 and +8. A wider search confirmed both
  identical (`bz\l20x`; the sheet shows them at the narrow search).
- Each run's sheet: `D:\BozoScratch\m5\sheets\<run>.png`, fork | BozoEngine | |difference| x8 with the numbers
  under each row. The diff images are in `sheets\diff\`.

**The loading screens** (frame 4705-4709 of the story-mode runs) are not a difference of the pictures:

- During a level load the game keeps redrawing the loading screen (bar, text) without ending a game frame.
- The fork's window captured one of those intermediate pictures. BozoEngine's capture is the picture at the frame's
  end.
- Both carry the same frame count, so this capture method cannot pair them. The draws of every frame end match
  (section 2).

**Two differences found and fixed** (before them, no gameplay picture was identical):

1. **The letterbox.** The fork places its 4:3 picture at fractional coordinates. In 640x448 it covers columns 21-618
   (x 21.33, width 597.33). BozoEngine's window rounded to x 22, width 597: every picture was one pixel off, with
   edge-like differences up to 140 on the title screen. `window.cpp` now passes the exact rectangle; boot then
   became 7/7 identical.
2. **The heat haze.** BozoEngine's renderer sampled the target for `Material::sampleTarget` batches, written fresh in
   M1. The fork's renderer never sampled it: those batches are drawn as ordinary untextured batches (its
   `native_renderer.cpp` only hashes the flag). The haze around explosions therefore differed (match1p frame 6728).
   - It is now off unless `BOZO_HEAT_HAZE=1`, so the picture is the fork's.
   - Turning it on is a visible improvement the user may want. Decide at review.

The other large differences first seen in match1p (frames 9104-13374, up to 255) were the pairing's 1-2 frame offset
above, not picture differences.

## 4. Sound

**Fork.** The deterministic real-time runs' `PS2X_AUDIO_RECORD`: what its audio device played. That is the mixer's
samples, with silence wherever the device's queue ran dry. `PS2X_NATIVE_AUDIO_INPUT_DUMP` is not in this build.

**BozoEngine.** The pinned run's mixer samples (`BOZO_WAV`).

**Tools.**

- `tools/wavcmp.py` removes exact-zero runs of 64+ sample frames from both. It aligns each 50 ms chunk by
  normalised cross-correlation within 2400 sample frames of the previous chunk's shift. It reports bit-identical
  chunks, identical sample frames, SNR and spectral SNR.
- `tools/wavenv.py` compares each 10 s block's level, its 10 ms loudness envelope and its 24-band log spectrum.

| Run | Compared | Bit-identical chunks | SNR / spectral SNR | Levels | Envelope corr (mean, min) | Spectrum corr (mean, min) |
|---|---|---|---|---|---|---|
| boot | 38.6 s | 43.7% (logos and movie 99.5%; the title music 0%) | 29.3 / 38.6 dB | equal | 0.997, 0.993 | 1.000, 1.000 |
| match1p | 220 s | 7.6% | 1.5 / 6.4 dB | within 0.2 dB | 0.828, 0.591 | 0.9995, 0.999 |
| stage-level0 | 167 s | 12.5% | -0.7 / 4.0 dB | within 0.2 dB | 0.837, 0.458 | 0.9999, 0.999 |
| stage-level5 | 167 s | 12.5% | -1.4 / 2.8 dB | within 0.1 dB | 0.848, 0.565 | 0.9997, 0.999 |

- **Same sounds, same levels, same moments to within tens of milliseconds.**
  - The intro movie is bit-identical, from 989SND's input.
  - Locally aligned, the title music matches at 35-48 dB SNR with lags of 46-60 samples.
  - In matches the effects start a few samples to a frame apart. That is the same cause as the pinned `sound`
    hashes (section 2), and is audible only as timing jitter, if at all.
- **For A/B by ear:** `D:\BozoScratch\m5\wav\<run>_fork_det.wav` against `<run>_bozo_pinned.wav`, for boot,
  match1p, stage-level0 and stage-level5. Every other run has its `_bozo_pinned.wav`. The fork's has the device's
  silences at stalls.

## 5. Frame time

Both builds ran in real time at the same settings: 1280x960 window, render scale 2, game frame rate, sound on,
`PS2X_FRAME_STATS=1` (the FramePacer's game work per frame, every 2 s). The shared bench lock was held throughout.

- **Fork:** TMB main's `build/game_us`, normal mode, timed scripts (`*_sec.txt`).
- **BozoEngine:** `tmb` with the same scripts keyed on frames. The 4P run has no bots (`TMB_NO_AI=1`).

The runs are not frame-identical: each build plays its own free-running match. So the comparison is statistical,
over the match stretch of each run.

| Scene | Build | Frames | Mean | Mean of 2-s p95 | Worst 2-s p95 | Max | Over 16.7 ms |
|---|---|---|---|---|---|---|---|
| 1P Challenge (bots) | fork, run 1 | 7,381 | 6.09 ms | 8.10 | 12.50 | 27.1 | 5 |
| | fork, run 2 | 10,139 | 7.74 ms | 10.23 | 17.50 | 31.4 | 30 |
| | BozoEngine (generated code -O1, as built) | 10,118 | 7.75 ms | 10.57 | 17.00 | 25.0 | 46 |
| | BozoEngine (generated code -O2, the fork's level) | 10,204 | 8.80 ms | 14.24 | 42.75 | 59.4 | 353 |
| 4P split screen (no bots) | fork | 9,865 | 0.47 ms | 0.78 | 1.25 | 33.9 | 7 |
| | BozoEngine -O1 | 9,581 | 0.88 ms | 1.37 | 1.75 | 8.6 | 0 |
| | BozoEngine -O2 | 9,574 | 0.83 ms | 1.26 | 2.25 | 4.2 | 0 |
| Boot and menus | fork | 1,162 | 1.11 ms | 2.00 | | 93.7 | 1 |
| | BozoEngine | 1,126 | 1.51 ms | 3.10 | | 7.7 | 0 |

**Reading it.**

- The two fork runs differ by 27% on the same script, so the noise is large: free-running gameplay, and other
  ports' jobs on the box outside the lock.
- Even so, BozoEngine is consistently above the fork in a busy 1P match: 8-9 ms against 6-8 ms. It also has more
  frames over budget.
- Building the generated code at -O2 instead of -O1 did not help. The -O2 run was worse, which is within the noise.
- 4P and the menus are far under budget on both.
- The inventory's bar ("a busy 1P frame at or under the fork's ~5 ms") is **not met**. 60 fps holds on average, but
  more frames miss it. Section 7.1 has what to look at.

## 6. Fixes

**BozoEngine `phase4/m5`:**

| Commit | Fix | Found by |
|---|---|---|
| 0555189 | The window's letterbox at its exact fractional place and size (the fork's: columns 21-618 in 640x448, not 22-618) | pixels: every picture one pixel off |
| 0555189 | `Material::sampleTarget` (heat haze) off unless `BOZO_HEAT_HAZE=1`, as the fork's renderer draws those batches | pixels: match1p explosions |
| 0555189 | `run::setFrameHook`, `WindowOptions::onCaptured`; a capture's frame count read before its present | the pinned window captures |
| 0555189 | `draws` checked in a port's pinned run (every frame but the log's last) | M5's "draws must match" |
| 918a91a | libmpeg: an aborted playback (TMB's skip) ends, sceMpegIsEnd 1 | story #9723824, a skipped stage movie |
| 918a91a | libmpeg: a last picture taken late is not over before an empty delivery | story #11644213 |
| 4d36248 | libmpeg's demux counts a picture read ahead from its first header | stage-level0 #11640007 |
| 4d36248 | sceSifCallRpc writes the server data's packet address, reply mode and id (0) | stage-level0 #13052622: a level load had overwritten them |
| 4d36248 | A T0 write by the game keeps the last read (a read-write-read of 0 is a poll) | stage-level0 #13062314 |
| a8bef92 | TerminateThread / DeleteThread of an unknown thread: -407 (KE_UNKNOWN_THID), not -406 | match4p #11334755 (the movie player torn down twice) |
| a8bef92 | RemoveDmacHandler / RemoveIntcHandler: 0 also for a handler not there | match4p #11334761 |
| a8bef92 | DeleteSema makes its operation for an unknown id too (-408) | match4p #11334775 |
| a8bef92 | sceSifFreeIopHeap: -1 for a block not allocated | match4p #11334788 |
| a8bef92 | sceMcGetInfo of a multitap slot: no card, type, free and format 0, result -4 | match4p #228107 (frame 1) |
| a8bef92 | printf's `%p` as the reference's host printed it: 16 hex digits | match4p #16615141; every stage's "toc starts at" |
| 79fb362 | 989SND `snd_CloseVAGStreaming` (0x35) frees the streams' IOP block | stage-level20 #26677069: the next bank's address |
| def8230 | VU0 O flag: an overflow, or a result at the largest magnitude 0x7fffffff, not every exponent-255 result | stage-level3 #15807299, match4p #76722184 (BuildRadarStuff) |

- The 9,232 capture samples still match after every change (`bozo-replay`).
- boot and match1p were rechecked after each change.
- Specs are updated: `libraries.md` 9.5 (movie ends, demux limit), 11 (T0) and 15.3 (0x35); `r5900.md` (VU0 O).

**TMB `bozo/m5`:**

- 9a6d4a3: `tmb-check --window`.
- 03b6a2f: TMB's videoDecAbort override tells BozoEngine's libmpeg that the playback was aborted, as the fork's
  `abortMpegPlayback` did.

## 7. Left, by severity

1. **Frame time (high for M7).** A busy 1P match costs BozoEngine about 15-30% more game work than the fork. It also
   has 2-10x more frames over 16.7 ms.
   - Not explained by the generated code's optimisation level.
   - To look at next: a profile of `tmb` in a 1P match (dispatcher, checkpoint and kernel overhead per out-call;
     match1p makes about 4,000 out-calls a frame). Then PGO, which the fork's release build uses (`TMB_PGO`).
2. **989SND's lifetime and handle models (medium).** These replies depend on the reference's IOP time
   (`libraries.md` 15.5, M3's open item):
   - The pinned runs of story, save, match4p and stage levels 2 and 3 stop on them.
   - Per run, the model disagrees with the log on 1-3,319 play handles (0x11) and on 0-504 liveness answers
     (0x19, 0x21). With the log's replies, every one of these runs matches to the end but save (item 3).
   - In free play this decides when the game believes a sound has ended, and which handle a play gets. That matters
     for which sounds a player hears, not for the game's logic: no ram difference follows.
   - `--snd-log-replies on` can itself lead a run astray where the models were right: stage-level13's ram differs
     from frame 5496 with it, and the default matches. The window captures therefore use the log's replies only
     where the default stops.
3. **The kill-enemies test cheat (low: test only).** `save` (`TMB_KILL_ENEMIES=15`) stops at frame 5458, at the
   first kill: TMB's theGameUpdate override calls `vehicleTakeHit` on a copy of its context.
   - The guest code then takes another path: traced call 695 is a memset where the reference calls
     `__kernel_sinf`, and a 156-byte struct at 0x4062e0 differs.
   - It is the only guest call on a copied context in TMB (`kernel.md` 3b's rule for them). Players never run it.
   - The save's own steps (Junkyard won, the card writes, Suburbs loaded) are past frame 5458, so they are unchecked.
4. **An irq one checkpoint off (low).** stage-level6, frame 7105. The reference takes an interrupt 130 checkpoints
   after the last out-call at bspVecTestIn's inner return 0x2014f0; BozoEngine has passed 131, at 0x201418. This is
   the same inner-return spot `kernel.md` 3b names. Pinned only: a free run takes events by its own clock.
5. **Sound sample timing (low).** Music and effects land a few samples apart (sections 2 and 4). Same sounds and
   levels. These are M3's open items: the cost model and G.
6. **Loading-screen pictures (none).** Several pictures share one frame count during a load (section 3).
   Comparing them would need captures keyed on the published overlays instead of frame ends.
7. **The heat haze (decision).** It is off to match the fork. Turn it on (`BOZO_HEAT_HAZE=1`, or make it the default
   and accept the difference) if the user prefers the PS2's look.

## 8. Reproducing

- **Scripts:** `D:\BozoScratch\m5\`.
  - `rec\rec.sh` and `rec\queue.sh`: the recordings.
  - `rec\picqueue.sh`: the fork's pictures and sound.
  - `tools\pin.sh <run> [window]`, `tools\sheet.sh`, `tools\win.sh`: the pinned checks, captures and sheets.
  - `tools\pixcmp.py`, `tools\wavcmp.py`, `tools\wavenv.py`, `tools\bestframe.py`.
  - `tim\timing.sh` and `tim\timing2.sh`: the timing runs, under the lock.
- **Outputs:**
  - `pin\<run>.txt`: the pinned summaries.
  - `sheets\<run>.png` and `.txt`: the pixel sheets and numbers.
  - `wav\`: the sound.
  - `tim\` and `rec\out\tim-*`: the frame stats logs.
- **Build:** as M4 ([`phase4-m4-notes.md`](phase4-m4-notes.md)), with `BOZO_ENGINE_DIR` this branch. For example:

  ```sh
  build/bozo/tmb-check D:/BozoCaptures/tmb/calllog/stage-level0.clg --ref reference/tmb/records/stage-level0.ref \
      --pinned --native all --iso "<TMB.iso>" --pad-script reference/tmb/scripts/story_frames.txt
  ```

  Run it with `TMB_PLAYER_GOD=1 TMB_STORY_LEVEL=0` set, as `ref_0.env` sets them.
