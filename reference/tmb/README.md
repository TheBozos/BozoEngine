# TMB reference records

Recorded 2026-11-02 on TMB_Recomp branch `harness/reference` (c83a924) with the PS2Recomp fork's `native/renderer`
(df2dfe2), deterministic mode (`PS2X_DETERMINISTIC=1`), each from a fresh user folder. Format:
TMB_Recomp's `docs/REFERENCE_HARNESS.md` (`PS2XREF1`, 80 bytes per game frame: hashes of RAM, scratchpad, calls, draw
list, threads, sound). Every run was recorded twice and both records are identical on every field; one of each is
kept here. They contain hashes and counts only, no game data.

| Record | Script (`scripts/`) | Frames | What |
|---|---|---|---|
| `boot.ref` | `ref_boot.env` (no input) | 3000 | Boot, logos, intro movie, title |
| `match1p.ref` | `det1p.env` (`ch1p_frames.txt`) | 16000 | Challenge, one player with bots |
| `match4p.ref` | `det4p.env` (`p1`-`p4_frames.txt`) | 17000 | Four-player split screen (no bots) |
| `story.ref` | `ref_story.env` (`story_frames.txt`, invincible) | 11000 | Story from the start: stage 1, the Junkyard |
| `stage-level0.ref` | `ref_0.env` | 11000 | Story started at stage 2, Midtown Suburbs |
| `stage-level2.ref` | `ref_2.env` | 11000 | Stage 2, Midtown Expressway 104 |
| `stage-level5.ref` | `ref_5.env` | 11000 | Stage 3, Downtown Business District |
| `stage-level6.ref` | `ref_6.env` | 11000 | Stage 3, Black Vista Highway |
| `stage-level14.ref` | `ref_14.env` | 11000 | Stage 4, Minion's Stadium |
| `stage-level3.ref` | `ref_3.env` | 11000 | Stage 5, Prison Passage |
| `stage-level16.ref` | `ref_16.env` | 11000 | Blackfield Asylum |
| `stage-level4.ref` | `ref_4.env` | 11000 | Stage 7, Abandoned Skyscrapers |
| `stage-level13.ref` | `ref_13.env` | 11000 | Rooftops, second part |
| `stage-level20.ref` | `ref_20.env` | 11000 | Stage 8, Warhawk's Rooftop |
| `save.ref` | `ref_save.env` (`story_frames.txt`, invincible, enemies destroyed every 750 updates) | 20000 | Story: the Junkyard won, the save to the memory card (icon.sys, LISTICON.ICO, the save, globalFile written then read back), Midtown Suburbs loaded |

`run.sh` is the runner used (paths of the recording machine).

## The Midtown Suburbs freeze (found and fixed in the harness)

The first recordings froze at frame 4855 of `stage-level0` (both runs): the game drew an object whose header had been
overwritten and looped on its garbage texture count. Traced with a memory watch: the bytes were written by the IOP's
sound driver answering the menus' sound teardown (`unloadShellSoundBanks`, `sceSifCallRpc` to 989SND's service
0x123456), into memory the menus' `SHELL1.NGP` had used and `SUBURB.NGP` had just been loaded over. The cause was
the deterministic mode itself: a disc read took no EE time, so the IOP (advanced only by EE cycles) barely ran during
a level load and answered late. A disc read now takes a PS2 drive's time in deterministic mode (fork df2dfe2:
114,400 EE cycles a sector, DVD at 4x), the IOP keeps pace, and every record above was re-recorded with it. Normal
play was never affected (the IOP runs on real time there).

Lesson for BozoEngine: the runtime's time model must give the IOP side (or its native replacement) the time a load
takes, or replies the game expects before a load land after it.
