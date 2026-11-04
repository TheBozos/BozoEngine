# Phase 4, milestone M2: pads and players

Written 2026-11-04 on BozoEngine `phase4/m2` (from main a18f26d) and TMB_Recomp `bozo/m2` (from `bozo/m0` a5c9628).
M2 of [`phase4-inventory.md`](phase4-inventory.md) section 5: a match playable with pads, 1 to 4 players, in the `tmb`
window.

## Building and running

As M1 ([`phase4-m1-notes.md`](phase4-m1-notes.md)), with `BOZO_ENGINE_DIR` this worktree:

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build/bozo_m2 -G Ninja -DTMB_BOZO=ON -DBOZO_ENGINE_DIR=<BozoEngine> -DTMB_ELF=<SCUS_971.01> \
      -DTMB_ISO="<TMB.iso>" -DBOZO_FFMPEG_ROOT=D:/tmb_kit/kit/ffmpeg -DFETCHCONTENT_SOURCE_DIR_SDL3=<SDL3 3.4.20 sources>
cmake --build build/bozo_m2
build/bozo_m2/tmb --iso "<TMB.iso>"
```

- Pads: the keyboard (the fork's default keys) and up to four gamepads through `input.ini`. Player 1 is the keyboard
  plus the first gamepad; players 2-4 get the second to fourth gamepads. Players 3 and 4 need the game's multitap
  modes, which TMB always opens.
- Scripts instead of the devices: `--pad-script [n=]file`, repeated. The same option works in `tmb-headless` and in
  `bozo-run` / `tmb-check`.
- The user folder: `%APPDATA%\TMB_Recomp`, the fork's TMB folder. `PS2X_USER_DIR` overrides it. It holds:
  - `input.ini`;
  - the memory cards `mc0` / `mc1` (`--card-dir` overrides);
  - `logs/TMB_Recomp.log`;
  - `crashes/`.
- Environment:
  - `PS2X_INPUT_CONFIG`: another `input.ini`;
  - `PS2X_GAMEPAD_DB`: an SDL mapping file (by default `gamecontrollerdb.txt` beside the executable or in the user
    folder);
  - `PS2X_RUMBLE=0`: no rumble;
  - `PS2X_INPUT_BACKGROUND=1`: the devices reach the game while the window is in the background;
  - `PS2X_INPUT_LOG=1`: each change of a player's buttons;
  - `PS2X_LOG_FILE`, `PS2X_NO_DIALOGS`.

## What moved, what is new

Moved (team code, per-file check in the inventory):
- **`ps2_app` → `bozo/app/app.h`, `runtime/app/app.cpp`:**
  - kept: the user folder, the executable folder, `findGameData`, `migrateFrom`, the log tee with rotation, the
    crash handler and reports, the message boxes;
  - changed: the identity is set at run time (`setIdentity`) instead of the fork's `PS2X_APP_*` compile definitions,
    so one `bozo_app` serves every port. TMB sets the fork's values: title, `TMB_Recomp`, `tmb_recomp`,
    `SCUS_971.01`, `game`.
- **`ps2_input_bindings` → `bozo/app/input_bindings.h`, `runtime/app/input_bindings.cpp`:**
  - kept: input.ini's format and names, the defaults, the load, save and evaluate logic, mouse movement and the wheel;
  - raylib codes became SDL3 codes: keys are scancodes (GLFW's key tokens were positional too, so the meaning holds),
    pad buttons are `SDL_GamepadButton` (the face and D-pad buttons by position; `LEFT_TRIGGER_2` /
    `RIGHT_TRIGGER_2` read the trigger axes), axes keep their numbers, and mouse buttons are SDL's;
  - fork files: every name table also holds raylib's number, so `key:283` or `pad:10` in a fork file still parses;
    a key the fork had no name for is written back as its raylib number, so the fork can still read our files;
    the fork ignores the new `[player3]` / `[player4]` sections, and their defaults are player 2's bindings, as on
    the fork;
  - checked with a fork-format file through load, save and reload (numbers, names, mouse, wheel, an empty line that
    unbinds, `gamepad = 1`).

Written fresh:
- **libpad multitap** (`bozo/libs/pad.h`, `runtime/libs/pad.cpp`, `docs/specs/libraries.md` section 6):
  - `PadInput` is now keyed by player, and `padPlayer(port, slot, multitap)` maps sockets to players. With the
    multitap, port 1's slots are players 1-4 and port 2's pad is there but idle. That is what the reference does:
    TMB opens the multitap at frame 0, and boot and match1p read port 2 every frame.
  - The pads in ports 1 and 2 are always connected. A multitap slot is connected only while its player has an input.
  - `rumble(player, small, large)` comes from `scePadSetActDirect` through the alignment.
  - `PadState::socketReads` counts reads per socket (`tmb-headless` prints them).
  - `ScriptInput` takes players 1-4.
- **`DevicePad` rewritten** (`bozo/app/device_pad.h`, `runtime/app/device_pad.cpp`):
  - four gamepad places, hot-plug, and SDL's mapping database plus `gamecontrollerdb.txt`;
  - the bindings are evaluated on the window's thread, and the game thread reads a snapshot;
  - pressure is how far each binding is pressed (an analog trigger's travel, full for digital buttons);
  - rumble: the large motor drives SDL's low-frequency rumble, the small motor the high-frequency one. It is resent
    each second while active and stopped when the window loses focus or the player's pad changes;
  - mouse capture uses SDL's relative mode.
- **Free host:** `FreeOptions::playerScripts` holds the scripts for players 2-4.
- **`bozo-run`:** `--pad-script` now takes players 1-4.
- **TMB:**
  - `tmb_window.cpp`: the app identity, logging, the crash handler, the cards in the user folder, `--elf` falls back
    to `findGameData`, `--pad-script n=file`;
  - `tmb_headless.cpp`: `--pad-script n=file`, and the per-socket read counts in the report.

Not moved yet:
- the F2 settings window. Its Controls tab needs ImGui; `input::bindingFromEvent` is the capture it will use;
- `ps2_video_settings` (`video.ini`).

## Results

| Check | Result |
|---|---|
| `tmb-check boot.clg --pinned` (`--native all`, TMB's overrides native) | **MATCH**: 3000 frames, pad 29,973 calls native, 0 divergences |
| `tmb-check match1p.clg --pinned --pad-script ch1p_frames.txt` | **MATCH**: 16000 frames, pad 161,084 calls native, 0 divergences; draws 15999/16000 (the last frame, as in M1) |
| `tmb-headless --frames 16000 --pad-script ch1p_frames.txt` | same report as the M1 build; its PS2XREF1 record identical to the M1 build's on every field and every frame; match from frame 5591 |
| `tmb-headless --frames 17000`, scripts p1-p4, `TMB_NO_AI=1` | all four multitap slots read every frame (16,974 each), port 2 idle; the shell's multiplayer screens (22, 30, 31) then a 4-player match from frame 4700 to the end |
| control: the same with player 1's script alone | the game stays on screen 4 (waiting for players) from frame 3422; slots 1-3 never read |
| `tmb` real time, `ch1p_frames.txt` | a 1P match in progress (`D:\BozoScratch\m2\png1p\frame_006800.png`), about 60 game frames a second |
| `tmb` real time, scripts p1-p4 | "PLAYER 3 CHOOSE A VEHICLE" (`png4p\frame_003600.png`); the 4-way split screen (`png4p\frame_006001.png`) |

No 4P call log exists (`D:\BozoCaptures\tmb\calllog` has boot and match1p), so 4P is not pinned. A free run cannot be
compared with `match4p.ref`, because its time model is not the reference's. Recording a short 4P call log needs the
fork's `harness/calllog` build.

Memory cards: libmc keeps the fork's layout, plain files under `<dir>/mc0/BASCUS-97101/`, so a fork save is read as
it is. A save could not be written end to end here: the story script's free run (`story_frames.txt`, as `save.ref`)
stops with a dispatcher FAULT at frame 5458. The message is "an out-call at 0x215268 after a stop deferred inside a
native out-call, deferred pc 0x2014f0". The M1 build stops at the same place, so this is not M2's; it is for the
host / dispatcher owners.

## What a real player still needs to test

- rumble on real pads (DualShock 4 / DualSense through SDL's HIDAPI drivers, Xbox through XInput);
- hot-plug during a match;
- four real gamepads, and their order;
- whether the analog-trigger pressure feels right;
- mouse capture;
- an existing fork `input.ini`, and a fork save read from `%APPDATA%\TMB_Recomp\mc0`.
