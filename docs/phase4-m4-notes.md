# Phase 4, milestone M4: movies, menus and the F2 panel

Written 2026-11-04 on BozoEngine `phase4/m4` (from main f5f45dc) and TMB_Recomp `bozo/m4` (from `bozo/m0` 2b020b5).
M4 of [`phase4-inventory.md`](phase4-inventory.md) section 5. In the `tmb` window: the movies, the game's menus and
pause, the settings window (F2), the frame rate counter (F3) and the captures (F8, F9), as today's TMB has them.

## Building and running

As M1 ([`phase4-m1-notes.md`](phase4-m1-notes.md)). Dear ImGui is a new pinned dependency, fetched like SDL3:

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build/bozo_m4 -G Ninja -DTMB_BOZO=ON -DBOZO_ENGINE_DIR=<BozoEngine> -DTMB_ELF=<SCUS_971.01> \
      -DTMB_ISO="<TMB.iso>" -DBOZO_FFMPEG_ROOT=D:/tmb_kit/kit/ffmpeg -DFETCHCONTENT_SOURCE_DIR_SDL3=<SDL3 3.4.20 sources> \
      [-DFETCHCONTENT_SOURCE_DIR_IMGUI=<imgui b1bcb12a sources, e.g. the kit's deps/imgui-src>]
cmake --build build/bozo_m4
build/bozo_m4/tmb --iso "<TMB.iso>"
```

- **ImGui:** commit b1bcb12a (tag v1.92.7-docking, the version the fork's TMB used and the kit's `deps/imgui-src` holds),
  `https://github.com/ocornut/imgui/archive/b1bcb12a624af7509894c8e77dd47416997777fa.tar.gz`, SHA256
  `123926eb...0f2c27`. MIT licence: the kit (M6) keeps `deps/imgui-src` and its licence; rlImGui drops.
- **Targets:**
  - `bozo_ui`: ImGui's core plus the tab list (`bozo/app/settings_tabs.h`). No SDL3, so TMB's runtime can draw its
    Cheats tab and still link into `tmb-headless` / `tmb-check`. It defines `BOZO_SETTINGS_UI`.
  - `bozo_app`: adds ImGui's official `imgui_impl_sdl3` and `imgui_impl_opengl3` backends.

## What a player gets (as today's TMB)

| Key / variable | What it does |
|---|---|
| F2 | Settings window: Controls, Video, and the port's tabs (TMB: Cheats). `PS2X_SETTINGS_OPEN=<tab>` opens it on that tab at launch |
| F3 | Frame rate counter at the top right: game frames a second (green at 59+, amber at 50+, red below), ms a frame, presents a second. `PS2X_SHOW_FPS=1` starts with it |
| F8 | Capture sequence: `PS2X_CAPTURE_SEQUENCE` consecutive presents (default 1) as `seq<k>_<i>.png` with `seq<k>.txt` (each one's time and game frame), in `PS2X_CAPTURE_SEQUENCE_DIR` or the capture folder. `PS2X_CAPTURE_SEQUENCE_AT=<s>,...` does the same at those seconds |
| F9 | Capture burst on / off: ten a second, `burst_<ms>.png`. `PS2X_CAPTURE_BURST_AT=<s>,...` toggles it at those seconds (new: for unattended checks) |
| F11, Alt+Enter | Between the window and the last full-screen mode (borderless by default) |
| `PS2X_CAPTURE_DIR` + `PS2X_CAPTURE_INTERVAL=<s>` | The game's picture every s seconds (`interval_<ms>.png`) |
| `PS2X_WINDOW_SIZE`, `PS2X_WINDOW_MODE`, `PS2X_RENDER_SCALE`, `PS2X_FRAME_RATE`, `PS2X_VIDEO_CONFIG`, `PS2X_VIDEO_SCRIPT` | As the fork: over `video.ini`; another file; scripted mode changes |

- **Where captures go:** `--capture-dir`, else `PS2X_CAPTURE_WINDOW_DIR`, else `<user folder>/captures` (M1 wrote to
  the current folder).
- **What a capture holds:**
  - F8 and F9 capture the window as shown, panel included.
  - `--capture-frames` and the interval captures take the game's picture before the panel is drawn, so pixel
    comparisons (M5) are not affected.
- **PNG writing:** on a thread of its own; the window's thread only reads the pixels back.
- **imgui.ini:** in the user folder, as on the fork.

### The settings window

- **Controls** (moved from the fork's lines 2230-2519, ours):
  - every PS2 button and stick direction, for players 1-4 (the fork had 1-2), with the gamepad choice;
  - Add: the next key, gamepad button, axis past 60 %, mouse button, wheel turn or large mouse move
    (`input::bindingFromEvent`). Mouse input is taken only over the game picture, not over the panel;
  - clicking a binding removes it;
  - Mouse: sensitivity, invert Y, capture;
  - Save writes `input.ini`. Reset to defaults.
- **Video** (moved):
  - Mode: windowed / fullscreen (exclusive) / borderless.
  - Resolution: common 4:3 and 16:9 sizes up to the monitor's, plus the monitor's own.
  - Render scale: 50-200 %.
  - Apply changes them at once. Apply and save writes `video.ini`. Revert.
  - The picture stays 4:3 with bars, as today.
  - **Frame rate:** shown, disabled (see "Not done").
- **Cheats:** TMB's own tab from `runtime/tmb_cheats.cpp`, unchanged: unlock everything, Story: start at, per-player
  infinite health / ammo / energy, No AI, Weak enemies. It now registers through `bozo::app::ui::registerTab` under
  `BOZO_SETTINGS_UI`.
- **No Audio tab:** today's TMB has none (its F2 is Controls, Video, Cheats). The audio latency is still the
  `--audio-latency` option.
- **No debugger or emulation views:** no GS, VIF, IOP or CPU tabs, no F1.

**Input while the panel is open:**
- The mouse is the panel's: no game mouse bindings, no cursor capture.
- The keyboard reaches the game, except while a binding is being captured or an ImGui text field is typed in.
- Gamepads always reach the game.

### video.ini

The fork's format, line for line (`size`, `mode`, `render_scale`, `gs_scale`, `frame_rate`), in the user folder:
- A fork file was read: 1024x768, render scale 1.5, `gs_scale = 2`, `frame_rate = monitor`.
- The file was left untouched by a run that did not save.
- `gs_scale` and `frame_rate` are kept and written back as read, so the fork still reads our file the same.
- `--width/--height/--fullscreen/--render-scale` override the file. `--fullscreen` now means borderless.

## What moved, what is new

- **Moved (team code, inventory 2.1):**
  - `ps2_video_settings` became `bozo/app/video_settings.h` / `runtime/app/video_settings.cpp`. raylib's window and
    monitor calls became SDL3's:
    - exclusive fullscreen: `SDL_SetWindowFullscreenMode` with `SDL_GetClosestFullscreenDisplayMode`;
    - borderless: SDL's desktop full screen;
    - the monitor's size and refresh: `SDL_GetDesktopDisplayMode`.
  - `ps2_debug_ui` became `bozo/app/settings_tabs.h` / `.cpp`.
  - The Settings window's Video and Controls tabs, lines 2230-2519 of the fork's `ps2_debug_panel.cpp`, went into
    `runtime/app/panel.cpp`.
- **Written fresh:**
  - the panel host (`bozo/app/panel.h`: the ImGui context, backends, keys, tab bar);
  - the F3 counter;
  - the capture code in `window.cpp` (sequences, bursts, interval, the writer thread);
  - `DevicePad::setHeld`.
- **TMB:**
  - `tmb_window.cpp`: `video::loadAtStartup` and the command line over it;
  - `cmake/bozo.cmake`: links `bozo_ui` into `tmb_runtime` when `TMB_WINDOW` is on;
  - `tmb_cheats.cpp`: `BOZO_SETTINGS_UI`;
  - `tmb_overrides.cpp`: `TMB_TRACE_CLOCK=1` logs the EE clock against the PC's every 300 frames.
- **Not read (outside the allowed list):** the fork's F3 overlay and capture code in `ps2_runtime.cpp`. They were
  rewritten from the README's description. So:
  - the counter's exact look;
  - the sequence's `[interp-trace]` lines;
  - the fork's F8 behaviour without `PS2X_CAPTURE_SEQUENCE`

  may differ from today's TMB.
- **The profiler:** not moved; not needed for M4.

## Movies

Nothing needed changing: libmpeg's picture hook goes to TMB's `moviePicture`, then to `loadImage`'s override, then to
the backdrop / display picture (M0/M1). Checked in the window:

- **Boot movie** (the Incognito logo, frames ~664-1116): shown full frame, 4:3.
- **Movies menu** (main menu, MOVIES, Billy Ray Stillwell's Prologue, "play all unlocked"):
  - it plays at the picture's size, centred in the 640 x 448 frame, inside the 4:3 window;
  - its first ~5 s are a flat dark grey (RGB 15, exactly the movie's rectangle), then the Blackfield Asylum shots;
  - START at frame 3400 skips it: back on the Movies screen by 3450.
- **Story movie:**
  - in the free story run (`story_frames.txt`, `TMB_PLAYER_GOD=1`), choosing Junkyard Dog starts the prologue
    (frame ~3975), with the same grey opening;
  - the script's CROSS at 4253 skips it, and the level's stage card follows (4350);
  - the story's later movies need levels won, which the god-mode script never does.
- **Skip:** it works where the game allows it.
  - Headless with START / CROSS every 40 frames from 680: the title came at frame 936 instead of 1116.
  - The logo's first part ignores the pad (presses before ~900 did nothing), as the game does.

**Sync (a real-time run with the Prologue, `movies2`):**
- Pictures and sound both come from the EE clock, so they are in step there. The pinned M3 records and M0 draws match
  the reference for both movies.
- In real time, the sound reaches the speaker after the queue plus the device's 10 ms. During the movie (frames
  2412-3376) the queue was 47-65 ms, with no drop and no underrun.
- The picture is presented within one present, 0-17 ms.
- **The sound trails the picture by about 40-75 ms.** That is inside the usual lip-sync tolerance (sound up to ~125 ms
  late), but noticeable on a careful look.
- Each movie's start stalls the game 400-560 ms, as in M3. The sound is dropped there (3,282-8,067 frames), but
  pictures and sound restart together.

**Clock drift (M1 / M3, not changed here).** `TMB_TRACE_CLOCK=1` shows the EE clock running about **1.0 % faster than
the PC's** between stalls (frames 1500 to 1800: EE 5.052 s for PC 5.000 s; 2400 to 3000: 10.096 s for 10.000 s).
- **Why:** `nativeFrameEnd` charges `max(real, accounted)` each frame. A frame whose guest code waits past a vblank
  (59.94 Hz fields against 60.0 presents) keeps its extra field, and nothing pays it back.
- **Effect:** the sound is made 1 % faster than it plays. The device's ratio sits at its +1 % limit, the queue climbs
  to 64 ms, and then 33-68 ms chunks are dropped every few seconds (menus, title). They are audible as small skips,
  and the A/V offset saw-tooths between ~40 and ~75 ms.
- **Choice for the lead:**
  - pace frames by the EE clock (hold a two-field frame for two periods);
  - or carry the over-accounted time in real-time mode (only when paced: headless and pinned runs unchanged);
  - or widen the device ratio.

  Not done here, because it changes the game's timing in real time.

## Menus, options, pause

- **Title / main menu, Movies screen, Options:** rendered through the shell's native 2D.
  - Options shows difficulty, controls, audio stereo/mono, FX / music volume, brightness, load / save settings.
  - Screenshots: `options_menu.png` and the sheets.
- **Pause (open question settled):** START in the 1P Challenge match (ch1p script to frame 6900, START at 7000)
  shows "PLAYER 1 PAUSED: RESUME / QUIT / SET VIBRATION OFF / ADJUST REAR VIEW".
  - **The scenery stays visible:** the frozen 3D (truck, arena, sparks) in a band across the middle, black bars above
    and below.
  - The HUD is hidden.
  - `pause_screen.png`.
- **Before the pause** (frame 6950): the game's rear-view mirror is in the top half (the script's TRIANGLE). That is
  the game's own feature, not a split-screen bug.

## Results

| Check | Result |
|---|---|
| `tmb-check boot.clg --pinned` | **MATCH** (3000 frames, 0 divergences); again on the final build |
| `tmb-check match1p.clg --pinned --pad-script ch1p_frames.txt` | **MATCH** (16000 frames, 0 divergences; draws 15999/16000, the log's last frame as in M1) |
| Free 1P (`tmb-headless --frames 16000`, ch1p) and story (`--frames 12000`, story, `TMB_PLAYER_GOD=1`) | **Unchanged**: the PS2XREF1 records (`--out`) are byte-identical to the pre-M4 build's (TMB `bozo/m0` 2b020b5 on BozoEngine main f5f45dc) |
| F2 tabs, F3 | `f2_controls.png`, `f2_video.png`, `f2_cheats.png`, `f3_overlay.png` (60.0 FPS, 16.7 ms) |
| Video live | `PS2X_VIDEO_SCRIPT=6:1280x960:windowed:0.5`: applied from 1024x768 at 1.5 to 1280x960 at 0.5 while running; captures 1024x768 before, 1280x960 after |
| F8 / F9 | `PS2X_CAPTURE_SEQUENCE=3` at 4 s and 11 s: `seq1_000-002`, `seq2_000-002` with their `.txt`; the burst from 9.0 to 9.45 s gave 5 PNGs |
| Movie mid-play | `movie_prologue_midplay_f2950.png`, `f3250` |
| Intro | `movie_intro_logo_f1100.png` |

Screenshots and logs are in `D:\BozoScratch\m4\`. The sheets are `movies1_sheet` (to the Movies screen),
`movies2_sheet` (the Prologue and its skip), `story1_sheet` (the story to the level), `options_sheet` and
`pause_sheet`.

## For the user to test

- **Rebinding by hand** (Controls, Add, then a key / pad button / trigger / stick / mouse button / wheel / mouse move):
  - Save, then restart;
  - players 2-4;
  - the gamepad index field.
- **Fullscreen switching:**
  - F11 / Alt+Enter;
  - Video Mode Fullscreen (exclusive, with a resolution) and Borderless, then back to Windowed;
  - on a second monitor.
- **Video options applied live** with Apply (resolution, render scale), and Apply and save, then restart.
- **Cheats by clicking:** each checkbox in a match, and Story: start at a level, then a new story.
- **The panel's size** on a high-DPI monitor (scaled by the window's display scale).
- **The movie's sound against its picture by eye**, and the small skips the clock drift causes (see above).

## Not done

- **Frame rates above 60** (the fork's "Match the monitor" interpolation): BozoEngine's present has no interpolation
  yet. The fork renderer's interpolation is in `native_renderer.cpp`, which this task could not read. `frame_rate` is
  kept in `video.ini` and the Video tab shows it disabled. `Renderer::setFrameRate` is never called with a nonzero
  rate, because it would change the mesh submitter's tracked draws.
- **The GS scale:** no GS.
- **Crash reports:** already in M2 (`app.cpp`); nothing new.
- **The EE / PC clock drift** above.
