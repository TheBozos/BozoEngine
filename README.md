# BozoEngine

The Bozos' own toolchain for native PC ports of PlayStation 2 games: a static recompiler for the game's EE code, and a
runtime that answers the game's calls with native PC code. **No PS2 hardware at any level**: no GS, no VU, no VIF/GIF,
no DMA controller, no IOP, no SPU2. What the game draws goes to a native renderer; what it plays goes to a native
mixer; what it reads, saves and polls comes from native implementations of the libraries it calls.

Status: **planning**. Nothing here runs yet. See [docs/PLAN.md](docs/PLAN.md).

## Why

Our ports (Twisted Metal: Black first) run today on a fork of [PS2Recomp](https://github.com/ran-j/PS2Recomp). That
fork carries a full PS2 emulation stack (GS rasterizers, VU interpreters and JIT, VIF/GIF, the IOP interpreter, SPU2)
which the native ports have spent months routing around, and most of its core is someone else's code under GPL-3.0.
BozoEngine is the native path only, written by us, under our licence:

- **Own it.** Code written by The Bozos, MIT-licensed. Nothing copied from PS2Recomp or any other project (see
  "Clean room" in the plan).
- **Native by construction.** The emulation layers are not rewritten; they do not exist. The runtime implements Sony's
  libraries at the call the game makes (read the pad, open a file, play a movie), not the chips below them.
- **Proved against what works.** Each step is checked against the current TMB_Recomp build: same game state, same
  picture, same sound, frame by frame.

## Layout (planned)

| Folder | What |
|---|---|
| `recompiler/` | ELF loader, function discovery, R5900 (EE) to C++ translation |
| `runtime/` | Guest memory, the EE kernel's services (threads, semaphores, alarms), the scheduler |
| `libs/` | Native implementations of the Sony libraries games call (pad, memory card, CD/DVD files, sound, movies, display) |
| `render/`, `audio/` | The native renderer and mixer (moved from our PS2Recomp fork, where we wrote them) |
| `app/` | Window, input, settings, launcher hooks |
| `tests/` | Our own tests |
| `docs/` | The plan, the specs the code is written from |

## Licence

MIT, see [LICENSE](LICENSE). Game data is never part of this repository: every port builds from the player's own disc.

By The Bozos (boubou666 and Arzaroth).
