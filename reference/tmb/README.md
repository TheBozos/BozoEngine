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

## Per-function captures (phase 1)

Samples of single guest function calls, for checking BozoEngine's generated code function by function
(`docs/specs/capture.md`, format `PS2XCAP1`; `tools/capture/capture_read.py` lists and checks them). Recorded
2026-11-03 on TMB_Recomp `harness/reference` (c83a924) with the fork's `harness/capture` (the code generated with
`capture_hooks`), from the same scripts and deterministic runs as the records above (each run's reference record
stayed identical to the one here). They hold game memory, so they are kept outside the repository, in
`D:\BozoCaptures\tmb\<run>[.<pass>]\`.

| Run | Script | Passes | Samples | Functions sampled / called | Unreplayable calls | Size |
|---|---|---|---|---|---|---|
| `boot` | `ref_boot.env` | 8 | 763 | 371 / 401 | 66 | 34.5 MB |
| `match1p` | `det1p.env` | 8 | 4,360 | 1,491 / 1,541 | 191 | 125.8 MB |
| `story` | `ref_story.env` | 8 | 4,109 | 1,421 / 1,469 | 154 | 111.3 MB |

Up to 4 replayable samples per function, spread over the run. Pass 1 samples every function; pass n only those the
earlier passes called but never tried (they were only ever called inside a sampled call). Together: 1,518 of the
1,564 guest functions these runs call have samples; the other 46 only block or are interrupted (thread entry points,
the main loops, disc loads, movie decoding). The reference build's own replay of every sample (`selfcheck.txt` in each
folder) matches bit for bit on all 9,232.

## Call logs (phase 2)

The whole run as BozoEngine's phase 2 needs it (`docs/specs/calllog.md`, format `PS2XCLG1`;
`tools/capture/calllog_read.py` lists and checks them): every out-call in order with its point and effects, and every
change made outside guest code (the dispatcher's, the libraries' continuations', the IOP's) with the point it was made,
the thread switches, handlers and kernel operations. Recorded 2026-11-03 on TMB_Recomp `harness/reference` (c83a924)
with the fork's `harness/calllog` (recorded with 6dc3de9, replayed with 4e5ef52; code generated with
`capture_hooks`), deterministic mode, fresh user
folder. They hold game memory, so they are kept outside the repository, in `D:\BozoCaptures\tmb\calllog\`.

| Run | Script | Frames | Records | Calls | Size | SHA-256 |
|---|---|---|---|---|---|---|
| `boot` | `ref_boot.env` | 3000 | 4,650,558 | 2,432,936 | 194 MB | `b7745379...b6ab1f3aea` |
| `match1p` | `det1p.env` | 16000 | 74,615,923 | 63,675,806 | 3.46 GB | `3c0baeb8...73698f11` |
| `story` | `ref_story.env` | 11000 | 40,608,019 | 33,414,935 | 1.84 GB | `e046822b...2853c5dd` |
| `save` | `ref_save.env` | 20000 | 65,104,895 | 53,889,583 | 3.04 GB | `736ee269...d8904792` |
| `match4p` | `det4p.env` | 17000 | 87,437,930 | 76,421,081 | 4.97 GB | `3f62e0d9...1381d1db` |
| `stage-level0` | `ref_0.env` | 11000 | 54,205,000 | 46,438,334 | 2.57 GB | `bc781ed5...d7a8ab46` |
| `stage-level2` | `ref_2.env` | 11000 | 49,742,332 | 42,279,624 | 2.41 GB | `eaaf8aef...0c553947` |
| `stage-level5` | `ref_5.env` | 11000 | 50,850,869 | 42,995,577 | 2.27 GB | `be9abc0b...4268d1ea` |
| `stage-level6` | `ref_6.env` | 11000 | 32,995,788 | 26,238,711 | 1.39 GB | `7f68b15a...ee1637aa` |
| `stage-level14` | `ref_14.env` | 11000 | 34,064,002 | 27,019,295 | 1.47 GB | `93ec6ee9...1cbb80a0` |
| `stage-level3` | `ref_3.env` | 11000 | 43,897,973 | 36,385,003 | 1.83 GB | `f5e6fed2...82dec463` |
| `stage-level16` | `ref_16.env` | 11000 | 43,976,847 | 37,316,873 | 1.86 GB | `0942199e...3e12a3a6` |
| `stage-level4` | `ref_4.env` | 11000 | 52,010,924 | 44,193,622 | 2.12 GB | `0eade185...c3c3a059` |
| `stage-level13` | `ref_13.env` | 11000 | 45,523,473 | 38,773,954 | 1.98 GB | `369069ea...034c143b` |
| `stage-level20` | `ref_20.env` | 11000 | 32,671,886 | 22,962,916 | 1.21 GB | `04f53b45...7179a008` |

The thirteen rows after match1p were recorded 2026-11-04 (phase 4 M5) on the same build (fork `harness/calllog`
4e5ef52, TMB_Recomp c83a924), deterministic mode, fresh user folder, four at a time (between 28 and 86 minutes each);
each one's reference record is identical to the stored `.ref` on every field, and each recording also wrote the fork's
sound (`PS2X_AUDIO_RECORD`). `boot` recorded again the same way gives the same file as `boot.clg` but for the run name.
How BozoEngine fares on them: `docs/phase4-m5-report.md`.

Checked on the reference build (`calllog.md` section 9): a second recording is the same file (but for the run name);
the strict recording is the same; the substitution check matches all 4,650,558 records with every window's effects
taken from the log, its reference record identical to `boot.ref` on every field; the full replay (every call but the
EE kernel's answered from the log, the IOP stopped, only guest code, the kernel and the scheduler running) makes the
same 4,650,556 records as the log (the last two: the frame-3000 call the run ends inside) and its reference record
equals `boot.ref` on `ram`, `scratchpad`, `calls`, `threads` and `eeCycle` for all 3000 frames (`draws` and `sound`
are not fed in a replay). match1p (recorded once, 79 min at about 3.3 frames a second: each of its ~4,000 out-calls a
frame compares the written pages): its reference record equals `match1p.ref` on every field, and its full replay
makes the same 74,615,921 records with the reference record equal to `match1p.ref` on the same fields for all 16000
frames. In both runs the IOP never changes EE state outside a call.
