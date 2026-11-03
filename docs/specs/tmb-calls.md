# Spec: what Twisted Metal: Black asks of the runtime

Every kernel, library and helper call TMB makes, from the reference recordings (`reference/tmb/`: boot, a 1P and a
4P match, a story start and each story stage started directly; the full call lists of 300 to 3000 frames per run).
This is the list BozoEngine's runtime must answer, and how. 100 distinct calls; counts are over the recorded frames.

Calls are identified at the game's call site (the recompiled code's dispatcher, for the addresses of the library
functions linked into the ELF, and the `syscall` instruction). A call the game makes by a tail jump compiled to a
direct C++ call is not recorded (a known gap of the recorder).

## 1. C library and math (answered natively, must be bit-exact)

| Call | Count | Must do |
|---|---|---|
| `memcpy`, `memset` | 670k, 1.24M | As the C standard, on guest memory |
| `malloc`, `free` | 5.8k each | The game's heap, in guest memory, set up by `syscall 60/61` (below); addresses returned are part of guest state, so the allocator's policy must match the reference run's addresses |
| `strlen`, `strchr`, `strcpy`, `strcmp`, `strncat`, `sprintf` | 6.6k ... 18 | As the C standard (`sprintf` formats as the PS2's newlib did: integers, `%s`, `%x`) |
| `rand`, `srand` | 39k, 3 | The PS2 libc's generator exactly (same sequence for the same seed): game logic depends on it |
| `sqrt` | 6.6k | IEEE single, exactly |
| `sinf`, `cosf` (`__kernel_sinf`, `__kernel_cosf`, `__ieee754_rem_pio2f`) | 2.1M each | The same results bit for bit as the reference (the PS2 libm's float algorithm), not the host's libm |

## 2. EE kernel

| Call | Count | Must do |
|---|---|---|
| `syscall 60` (`SetupThread`), `syscall 61` (`SetupHeap`) | 3 | Boot: the main thread's stack (returns its top) and the heap's start |
| `InitThread`, `CreateThread`, `StartThread`, `TerminateThread`, `DeleteThread` | 3-10 | Guest threads with priorities; the scheduler runs the highest-priority ready thread, preemption only at the guest's own waits and at interrupts |
| `CreateSema`, `DeleteSema`, `WaitSema`, `SignalSema` | 5, 3, 122k, 122k | Counting semaphores; a wait on a zero count blocks the thread |
| `AddIntcHandler`, `RemoveIntcHandler`, `EnableIntc`, `DisableIntc` | 3-5 | The game's interrupt handlers: in TMB the vblank (frame) and timer handlers; called from the scheduler at their frame points, never from host threads |
| `AddDmacHandler`, `RemoveDmacHandler`, `EnableDmac`, `DisableDmac` | 3-9.6k | DMA-completion handlers. With no DMA in BozoEngine: the handlers the game relies on are called when the native renderer has taken the matching draw (same order as the reference) |
| `FlushCache` | 1.8M | Nothing (no caches); still recorded |

## 3. IOP communication (SIF): replaced by native drivers

Native since phase 3's batch B3: `libraries.md` sections 14 (SIF) and 15 (989SND, its commands and replies).

| Call | Count | Must do |
|---|---|---|
| `sceSifInitRpc`, `sceSifBindRpc` | 14, 6 | One server is bound: **service 0x123456, the 989SND sound driver's command channel** (client at 0x4fe970) |
| `sceSifCallRpc`, `sceSifCheckStatRpc` | 91k each | 989SND function numbers: **0x4a** (89.8k calls: the driver's batched command buffer, every frame), 0x40 (934), 0x03, 0x3b-0x3e, 0x08, 0x00 (a few each). BozoEngine answers these with a native sound driver behind the native mixer, with the same results and the same effect on what the game later reads back |
| `sceSifSetDma`, `sceSifDmaStat` | 1k each | Data pushed to the IOP (sound banks, streams): goes to the native sound driver directly |
| `sceSifLoadModule` (9 modules), `sceSifRebootIop`, `sceSifSyncIop`, `sceSifSetSreg`, `sceSifInitIopHeap`, `sceSifAllocIopHeap`, `sceSifFreeIopHeap` | 3-27 | No IOP: succeed with the results the reference saw (module ids 1-9, heap addresses), loading nothing |

## 4. Controllers

| Call | Count | Must do |
|---|---|---|
| `scePadInit`, `scePadPortOpen` | 3, 24 | Ports and multitap slots (up to 4 players) |
| `scePadRead`, `scePadGetState`, `scePadGetReqState` | 20k, 77k, 63 | The pad's buttons, sticks and pressure from the PC's input (latched per frame), states "stable" once connected |
| `scePadInfoMode`, `scePadSetMainMode`, `scePadInfoAct`, `scePadSetActAlign`, `scePadSetActDirect`, `scePadInfoPressMode`, `scePadEnterPressMode` | 9-1.2k | DualShock 2 mode, pressure mode, vibration (to the PC controller's rumble) |
| `lgDevInit`, `inputLgUpdate`, `inputSetLgLargeActuatorEffect`, `inputSetLgSpringOffset`, `inputSetLgTurningFriction` | 3-9.6k | The Logitech USB wheel's force feedback (the game's own functions over the USB driver): no wheel on the PC side today; answered as "no wheel" |

## 5. Memory card

| Call | Count | Must do |
|---|---|---|
| `sceMcInit`, `sceMcGetInfo`, `sceMcGetDir`, `sceMcSync` | 3-69 | Two cards (slot 1, 2), formatted, free space, the save folder's files; saves stored as native files |
| `sceMcOpen`, `sceMcRead`, `sceMcWrite`, `sceMcClose` (and the card's `sceMcMkdir`) | (the `save` run) | The folder `BASCUS-97101`: `icon.sys` and `LISTICON.ICO` created and written (flags 0x203), the save file `BASCUS-97101` written then read (0x3, 0x1), `globalFile` (options, unlocks, progress) read at boot (missing on a fresh card: -4) and written after a level (0x202) |

## 6. Disc

| Call | Count | Must do |
|---|---|---|
| `sceCdInit`, `sceCdMmode`, `sceCdDiskReady`, `sceFsReset` | 3-12 | A DVD present and ready |
| `sceCdRead`, `sceCdSync`, `sceCdGetError` | 105, 138, 12 | Sectors of the disc image into guest memory |
| `sceCdStInit`, `sceCdStStart`, `sceCdStRead`, `sceCdStStop` | 3-371 | Streaming (movies, music) from the disc image |
| `sceCdReadClock` | 3 | The date and time: **TheGame::Init seeds `rand` with minutes * 60 + seconds** (fixed in the deterministic mode) |

## 7. Display and uploads (GS library)

| Call | Count | Must do |
|---|---|---|
| `sceGsResetGraph`, `sceGsSetDefDispEnv`, `sceGsSetDefDrawEnv`, `sceGsSetDefDrawEnv2`, `sceGsSetDefClear` | 6-11 | The display mode and draw environment: values the native renderer takes; no GS |
| `sceGsSetDefLoadImage`, `sceGsExecLoadImage` | 522, 258 | Texture and CLUT uploads: the native texture cache takes the data |
| `sceGsSyncPath`, `sceGsSyncV` | 79k, 8 | Wait for drawing / the vblank: return as the reference did (drawing done; the frame boundary) |

## 8. Movies (libmpeg)

| Call | Count | Must do |
|---|---|---|
| `sceMpegInit`, `sceMpegCreate`, `sceMpegDelete`, `sceMpegReset`, `sceMpegAddCallback`, `sceMpegAddStrCallback` | 3-25 | The decoder and the game's stream callbacks |
| `sceMpegDemuxPssRing`, `sceMpegGetPicture`, `sceMpegIsEnd` | 2.1k, 1.1k, 1.1k | Demux the PSS stream, a picture per call (FFmpeg), the end: pictures to the native renderer, the sound to the mixer |

## Not seen yet

The options screens and every story ending: to record (longer story runs).
