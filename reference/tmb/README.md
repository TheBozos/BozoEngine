# TMB reference records

Recorded 2026-11-02 on TMB_Recomp branch `harness/reference` (63cbddc) with the PS2Recomp fork's `native/renderer`
(711b88b), deterministic mode (`PS2X_DETERMINISTIC=1`), each from a fresh user folder. Format:
TMB_Recomp's `docs/REFERENCE_HARNESS.md` (`PS2XREF1`, 80 bytes per game frame: hashes of RAM, scratchpad, calls, draw
list, threads, sound). Every run was recorded twice and both records are identical on every field; one of each is
kept here. They contain hashes and counts only, no game data.

| Record | Script (`scripts/`) | Frames | What |
|---|---|---|---|
| `boot.ref` | `ref_boot.env` (no input) | 3000 | Boot, logos, intro movie, title |
| `match1p.ref` | `det1p.env` (`ch1p_frames.txt`) | 16000 | Challenge, Junkyard, one player with bots |
| `match4p.ref` | `det4p.env` (`p1`-`p4_frames.txt`) | 17000 | Four-player split screen (no bots) |
| `story.ref` | `ref_story.env` (`story_frames.txt`, invincible) | 11000 | Story from the start: stage 1, the Junkyard |
| `stage-level2.ref` | `ref_2.env` | 11000 | Story started at stage 2, Midtown Expressway 104 |
| `stage-level5.ref` | `ref_5.env` | 11000 | Stage 3, Downtown Business District |
| `stage-level6.ref` | `ref_6.env` | 11000 | Stage 3, Black Vista Highway |
| `stage-level14.ref` | `ref_14.env` | 11000 | Stage 4, Minion's Stadium |
| `stage-level3.ref` | `ref_3.env` | 11000 | Stage 5, Prison Passage |
| `stage-level16.ref` | `ref_16.env` | 11000 | Blackfield Asylum |
| `stage-level4.ref` | `ref_4.env` | 11000 | Stage 7, Abandoned Skyscrapers |
| `stage-level13.ref` | `ref_13.env` | 11000 | Rooftops, second part |
| `stage-level20.ref` | `ref_20.env` | 11000 | Stage 8, Warhawk's Rooftop |
| `stage-level0.ref` | `ref_0.env` | **4855** | Stage 2, Midtown Suburbs: **the game freezes at frame 4855** in both runs |

## The Midtown Suburbs freeze

At frame 4855 the game thread loops in `pktAddVu1ObjAsm` (0x1eabc4: the copy of an object's texture register
blocks, the count from the object's +0x5a) with an object pointer that does not point at an object; its stack
pointer and gp read 0 afterwards (overwritten). A normal-mode run of the same stage played ~15000 frames without it,
with different input timing. Not known yet whether the game itself can do this or a runtime bug corrupts memory
before; the deterministic mode reproduces it every time (`PS2X_REFERENCE_DUMP` around frame 4850).

`run.sh` is the runner used (paths of the recording machine).
