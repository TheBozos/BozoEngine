# Phase 4, milestone M3: sound

Written 2026-11-04 on BozoEngine `phase4/m3` (from main a18f26d) and TMB_Recomp `bozo/m3` (from `bozo/m0` a5c9628). M3
of [`phase4-inventory.md`](phase4-inventory.md) section 5: TMB on BozoEngine plays its sound (menus, the movies, the
music, the match's effects) through 989SND's native engine and the moved mixer, to an SDL3 audio stream in real time,
or to a WAV file headless.

## Building and running

As M1 ([`phase4-m1-notes.md`](phase4-m1-notes.md)); then:

```sh
build/bozo_m3/tmb --iso "<TMB.iso>" [--audio-stats] [--audio-latency 32] [--no-sound] [--wav out.wav]
build/bozo_m3/tmb-headless --iso "<TMB.iso>" --frames 3600 --wav boot.wav            # the first 60 s
build/bozo_m3/tmb-headless --iso "<TMB.iso>" --frames 9600 --pad-script <BozoEngine>/reference/tmb/scripts/ch1p_frames.txt \
      --wav match.wav --wav-frames 5400-9600                                           # a scripted match
BOZO_WAV=out.wav build/bozo_m3/tmb-check <log> --ref <ref> --pinned ...                # the pinned run's samples
```

`PS2X_AUDIO_RECORD=<file>` is `tmb --wav <file>`. Debugging aids: `BOZO_SND989_SDLOG=<file>` (every libsd call the
engine makes: IOP clock, export, arguments, result), `BOZO_SND989_PHASE` / `BOZO_SND989_PHASE2` (the tick's phase at
989SND's start / at the first bank load, IOP cycles), `BOZO_SND989_REVDELAY` (the reverb's enable delayed).

## What moved, what is new

- **Moved:** nothing new from the fork. The mixer and libsd had moved before (d8bec0f); `ps2_audio_spu2.cpp` and
  `ps2_audio_stretch.h` were read and not needed: SDL3's stream resamples, so the drift is taken by its frequency ratio
  instead of the WSOLA stretcher, and the WAV writer is a few lines written fresh.
- **Written fresh (BozoEngine):**
  - `bozo/app/audio_device.h` (`bozo_app`): the SDL3 stream at 48 kHz stereo on the default device, pushed once a frame
    by the game's thread, a 32 ms queue target on a 512-frame device buffer, the frequency ratio following the queue
    (at most 1%), drops beyond 160 ms, underruns counted from SDL's request callback.
  - `bozo/audio/wav.h` (`bozo_audio`): the WAV writer (header kept up to date every second).
  - `snd989::Engine`: what 989SND does that only the output hears, read from `989SND.IRX`: StartSoundSystem's libsd
    setup, SetMasterVolume's voices and core 1's master volume, the reverb's attributes, depth and 10-tick enable, the
    movie's sound (0x3b-0x40) on the sound data input with its BVOL; IOP RAM for the movie's PCM (`sceSifSetDma` writes
    it); `advanceTo` (the engine and mixer brought up to an EE cycle between commands); long gaps mixed in steps;
    `Timing::freeRun`.
  - `SndDriver`: the engine made at the first need, the free-mode timing, `iopReset` (`sceSifRebootIop`),
    `advanceAudio`, `iopWrite`; 0x40 always from the log in a pinned run.
  - `FreeHost`: the engine brought up at each frame's end, `onAudio`, the record's `sound` field.
  - `bozo-run`: the sound records (reported: `sound (reported, not checked)`), `BOZO_WAV`.
  - `Mixer::take` / `queuedFrames` (a host that pushes instead of letting the device pull).
- **TMB (`bozo/m3`):** `tmb_window.cpp` opens the device and pushes each frame's samples (`--no-sound`,
  `--audio-latency`, `--audio-stats`, `--wav`, `PS2X_AUDIO_RECORD`); `tmb_headless.cpp` `--wav`, `--wav-frames`,
  `--no-sound`. TMB's audio overrides (pcmCallback, audioDec*) are unchanged: their PCM now reaches 989SND's movie
  buffer through `sceSifSetDma` and plays.

The rules, with what settled each, are in [`specs/libraries.md`](specs/libraries.md) 15.13d.

## Results

**Pinned, every library native, TMB's overrides native** (`tmb-check`):

| Run | Result | Sound records equal to the reference's | Sample counts | Time |
|---|---|---|---|---|
| boot | MATCH (3,000 frames, every record) | 1,659 / 3,000 | 2,998 / 3,000 | 35 s |
| match1p | MATCH (16,000 frames, every record) | 3,926 / 16,000 | 14,673 / 16,000 | 8 min |

989SND replies: all native but GetTransStatus (0x40, from the log; the engine agrees on 926 of 932 in boot, 916 of 932
in match1p). The sound records equal: every silent frame, both movies to the sample (the movie's PCM through
`sceSifSetDma` and 989SND's input), and 47 of the title music's first 86 frames with the tick phase pinned
(`Timing::firstTickPhase` = 27,776, found by searching the phase against boot's records). The rest is single libsd calls
landing a sample apart (the cost model's few-cycle errors) and, from match1p's level start, the IOP's unpaid time G
moving the sample grid (libraries.md 15.13d).

**Free mode, headless** (`tmb-headless`, no log; WAVs in `D:\BozoScratch\m3\`):
- `boot_free.wav`: the first 3,600 frames, 60.4 s (logos silent to 10.7 s, the intro movie, the title music from about
  17 s). Its movie samples are bit-identical to the pinned run's (`boot_pinned.wav`), so to the reference's.
- `match1p_free.wav`: the `ch1p_frames.txt` run, frames 5400-9600, 70.3 s (the end of the menus, then the match from
  frame 5591: music, engines, weapons; peaks reach full scale, as the mixer clamps).

**Real time** (`tmb --audio-stats --quit-after 1500`, 26 s of window, the default device at 48 kHz with a 480-frame
buffer): 60 frames a second as without sound; output latency about 50-65 ms (queue 40-55 ms, target 32, plus the
device's 10 ms); no underrun while the game makes its frames. Underruns (about 0.9 s of silence over the run) come only
at the game's own stalls, which the run without sound shows too (frames of 400-800 ms: the movie's start, the title's
load): no frame ends, so no sound is made; the burst after each is dropped (the queue is capped at 64 ms) so the
latency comes back at once.

## Shared files touched

- `runtime/host/free_host.cpp`, `bozo/host/free_host.h`: `FreeOptions::sound`, `onAudio`, the frame's sound (a
  function called from `endFrame`), the free-mode timing in `setUp`.
- `runtime/CMakeLists.txt`: `audio/wav.cpp` in `bozo_audio`, `app/audio_device.cpp` in `bozo_app`.
- `bozo/tmb_window.cpp`: the device and WAV around `game.run` (no change to `window.h` / `window.cpp`).
- `runtime/libs/sif.cpp` (`sceSifSetDma`, `sceSifRebootIop`), `tools/run/log_host.cpp` and `run_main.cpp` (the sound
  records).

## Open

- The sound records: a finer cost model (989SND's instruction counts per call, from the analysis replay) for the music
  frames; the level's G (the unpaid IOP time at the level start, `iop-timing.md` section 3) for match1p from frame 5591.
- The game's frame stalls (400-800 ms at the movie's start and the title) silence the output: the mixer is brought up
  only at frame ends. Bringing the engine up from the device's side (or at the dispatcher's idle points) on the EE clock
  would fill them, if the stalls stay.
- The queue's ratio sits near its +1% limit after each stall's burst: the clamp holds the latency, but a slightly
  larger gain (or the WSOLA stretcher, `ps2_audio_stretch.h`) could keep it nearer 32 ms.
- `--audio-latency`, the device buffer and the cap are options of `AudioOptions`; not yet in a settings file (M4's F2
  panel).
