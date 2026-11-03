# Spec: the native libraries (phase 3)

What BozoEngine answers natively of the library calls TMB makes (`tmb-calls.md`), how each answer was settled against
the call logs (`calllog.md`), and what is still answered from the log. Code: `runtime/libs/` (`bozo/libs/`); the
check: `bozo-run --native` (`tools/run/README.md`).

## 1. How a native library is checked

A native library function is known by the game's symbol name (the ELF's symbol table; in a call log, the NAME chunk,
a leading `ps2_` ignored) and bound at that address. It gets a `Guest` (`bozo/libs/guest.h`): the calling context
(arguments in `a0`-`a3`, `t0`-`t3`, then 64-bit stack slots from `sp`; float arguments in `f12`, `f13`) and guest
memory, every write journaled with the bytes it replaced.

`bozo-run --pinned --native <libraries>` runs the whole log as in phase 2, but a call to a native function is made by
the native library instead of being answered from its CALL record, and checked against the record:

- **result registers**: every context word but `pc` is compared with the record's (the record's words where it has
  them, the entry value elsewhere); the record's `pc` must be its `ra` (the call returns);
- **memory**: every run of the record must be in memory after the native call, and every byte the native call changed
  (against its value before the call) must be inside a run;
- **the rest of the record**: the EE cycles it takes (0 for every batch A call), the pending timer-poll cycles (left as
  they were), no traced calls inside it, and its operations on the kernel (`OP` records inside the call), which the
  native call must make itself (they are matched as it makes them; one left over is a divergence).

Any difference is a divergence reported with the record (`native <function> (<library>) differs from the log: ...`);
with `--max-errors` the log's effect then replaces the native one and the run goes on.

**Timed calls (batch B1).** A disc read or a stream read takes EE time and may make its thread wait (`Function::timed`).
Its library decides that with its own model of the drive (free mode: a PS2 drive's pace, section 8). The reference's
decisions came from its emulated IOP, which BozoEngine does not have, so in a pinned run the call takes them from the
log (`Libraries::pin`, set by `bozo-run` before the call): the EE cycles the call took, whether it waited (the log's
`block` inside it) and until which vblank (its token), and, for a stream read, how much of it had arrived (the end of
what the log's call wrote). Everything else (the data, results, the operations it makes) is the library's and is checked
as above. In free mode a timed call's EE time is the library's own; `bozo-run` counts the calls whose time is not the
log's and keeps the log's clock (a free run cannot go on past a different wait: section 8.4).

**Continuations.** A wait a native library makes with a completion (`Libraries::waitVblank` with a continuation) is
finished by the library when its thread resumes (`calllog.md` section 5, origin resume): `bozo-run` runs it on the
thread's context, checks the memory it writes against the log's `MEM` records of that point (every run there, nothing
outside them), its operations as they are made, and the context the thread resumes with (the `CTX` record, strictly).

Native state (the heap, the random generator) also follows what the log still answers: the traced calls made inside a
call answered from the log (an override running game code that calls `rand`: `calllog.md` 4.10) and the libraries
answered from the log that touch it (section 2.2). A disagreement there is a divergence as well.

The exit check stays phase 2's: boot and match1p match `boot.ref` and `match1p.ref` on every frame (ram, scratchpad,
calls, threads) with every record of the log matched.

## 2. The C library (`libc`)

Native: `memcpy`, `memset`, `strlen`, `strchr`, `strcpy`, `strcmp`, `strncat`, `sprintf`, `printf`, `malloc`, `free`,
`srand`, `rand`.

Every one returns at once (0 EE cycles) and leaves `v0` as the reference's library calls do: the result sign-extended
to 64 bits and the upper 64 bits of the register cleared (the record's words 8-11). `free` and `srand` leave `v0`
alone.

### 2.1 Memory and strings

As the C standard, on guest memory. `memcpy` copies with memmove semantics. `strcmp` returns -1, 0 or 1, never the
difference of the bytes (the records show only those three). `strncat` returns its destination.

`sprintf` and `printf` format as the reference: flags, width, precision (`*` too), `h`/`l`/`ll`, and `d i u x X o c
s p f e g %`. Arguments come from the argument registers in order, one 64-bit register each, a double in one register
(the EE's 64-bit GPRs), then 64-bit stack slots. TMB's formats are file names (`\LVL\%s.NGP;1%s`), `MEMORY CARD SLOT %d`
and three debug `printf`s (`%f` included). `printf` writes the text to the host's stdout and returns its length.

### 2.2 The heap

The reference's `malloc` and `free` keep no bookkeeping in guest memory (no record of either writes memory). Its
policy, settled on every `malloc` of boot (1,936) and match1p (7,924):

- the heap starts at SetupHeap's result (syscall 61: the end of the program aligned to 16, `0x751b20` in TMB) and runs
  to the end of RAM;
- a request is rounded up to a multiple of 16 bytes (0 to 16); there is no header;
- the block is the first free block large enough, by address, split at its start;
- `free` gives the block back and merges it with free neighbours.

**The reference's libmpeg allocates from the same heap.** Each `sceMpegCreate` (native since batch B2: section 9)
takes 0x600 bytes and never gives them back; the reference keeps the block's address at the work area + 0x44
(`sceMpegCreate`'s second argument), which the record's memory runs show (match1p: 11 creates, the blocks at
`0x752720`, `0x752d20`, ... `0x756320`; the 12th `malloc` after them, record #16897922, returns `0x756920`, 0x4200 past
where a heap without them would). The native heap follows each `sceMpegCreate` answered from the log and checks the
address. The native libmpeg takes its 0x600 bytes from this heap in the same order, and its callbacks' 32-byte data too
(section 9.3); while libmpeg is answered from a log, the heap follows each `sceMpegCreate` as above.

### 2.3 rand

The reference's `rand` is **not** the game's own (the ELF's `rand` at `0x21d228` is newlib's 32-bit generator,
`next = next * 1103515245 + 12345`, its state at `_impure_ptr + 0x58`). The reference keeps its state outside guest
memory (no record writes memory) and uses a 64-bit linear congruential generator:

    state = state * 6364136223846793005 + 1        (mod 2^64; state 1 until srand)
    rand  = (state >> 32) & 0x7fffffff
    srand(seed): state = seed

Settled on every `rand` of boot (2,994) and match1p (31,454 direct and 4,832 made inside port overrides, traced),
after `srand(0)` (TheGame::Init seeds with minutes * 60 + seconds of `sceCdReadClock`, fixed at 0 in the reference's
deterministic mode): the first results are 0, `0x5851f42d`, `0x40b18ccf`... The native generator follows the traced
`rand` calls of overrides answered from the log, so it stays in step.

Because game logic depends on it, a TMB built on BozoEngine keeps the reference's generator (what players of the
current build get), not the PS2's.

### 2.4 Results

| Run | Calls answered natively | Divergences |
|---|---|---|
| boot | 242,372 (memcpy 223,367, memset 11,000, rand 2,994, malloc 1,936, free 1,933, sprintf 1,107, ...) | 0 |
| match1p | 2,060,202 (memset 1,163,381, memcpy 843,487, rand 31,454, malloc 7,924, free 7,915, strchr 2,713, ...) | 0 |

Plus 32,169 calls the native state followed in match1p (traced `rand`, `sceMpegCreate`).

## 3. The math library (`libm`)

TMB's `sinf` and `cosf` are guest code (the game's newlib), and so is everything that calls them; the reference
replaced only three of their helpers and `sqrt`, which are the out-calls:

| Function | What the reference returns (settled on its records) |
|---|---|
| `sqrt` (float in `f12`, result in `f0`) | the IEEE single square root (correctly rounded): 64,094 calls in match1p |
| `__ieee754_rem_pio2f(x, y)` | `n` = `x * (float)(2/pi)` rounded to a float, then to the nearest integer, ties to even; `y[0] = (float)((double)x - n * (double)(float)(pi/2))`, `y[1] = 0`; returns `n` |
| `__kernel_sinf(x, y, iy)` | the reference C runtime's `sinf((float)(x + y))`; `iy` unused |
| `__kernel_cosf(x, y)` | the reference C runtime's `cosf((float)(x + y))` |

How these were found (match1p's first 400,000 calls of the four: 73,642 reductions with their `n` and `y`, 312,744
kernel results):

- the game's own kernels (fdlibm's float polynomials, read from the ELF: `__kernel_sinf` at `0x216228`,
  `__kernel_cosf` at `0x215780`) in IEEE single arithmetic give other bits on 17% of the calls;
- a correctly rounded sine and cosine (from double) differ on 95 of them (by one unit in the last place);
- the Universal CRT's `sinf` and `cosf` (`ucrtbase.dll`, what the reference build links) give every result;
- the reduction: the game's fdlibm reduction (pi/2 in three parts) gives `y[0] = -4.37e-8` for `x = -pi/2`, the
  reference 0. Of the candidate reductions, only the one above gives every `n` and `y[0]`; `y[1]` is always 0
  (n rounded from a double product fails at `x = 0x407b53d2`, where `x * 2/pi` is a tie in single precision).

Native: on Windows, `libm` calls `ucrtbase.dll`'s `sinf` and `cosf` by name (not the toolchain's libm: mingw-w64 has
its own), so the kernels give the reference's bits: 8,780,783 calls in match1p (3,447,372 `__kernel_sinf`,
3,382,603 `__kernel_cosf`, 1,886,714 reductions, 64,094 `sqrt`), 216 in boot, 0 divergences.

**Open point L1.** Elsewhere (no Universal CRT) the kernels fall back to a correctly rounded value, which differs
from the reference on about 6 calls in 10,000. A portable bit-exact answer needs the Universal CRT's float sine and
cosine reproduced; their inputs here are always within [-pi/4, pi/4] (the reduction's), so an exhaustive table of
where they differ from the correctly rounded value (about 1e9 inputs each to scan, once, on Windows) would do it.

## 4. Kernel-side calls still in the log

None. Every NAME class 1 (kernel) function of the boot and match1p logs is native since phase 2 (`kernel.md`; `bozo-run`
prints a note for any kernel function it would leave to the log, and prints none): `FlushCache` (30,121 calls in boot, 2,675,616 in match1p) and
the other trivial ones (`EnableCache`, `DisableCache`, `GetMemorySize`, `GsGetIMR`/`GsPutIMR`, `SetGsCrt`...) included.
The SIF-side helpers that look kernel-like (`sceSifGetSreg`, `sceSifSetSreg`, `sceSifInitIopHeap`...) are library
calls of the SIF and stay with batch B.

## 5. The GS library (`gs`)

Native: `sceGsResetGraph`, `sceGsResetPath`, `sceGsSetDefDispEnv`, `sceGsSetDefDrawEnv`, `sceGsSetDefDrawEnv2`,
`sceGsSetDefClear`, `sceGsSetDefLoadImage`, `sceGsSetDefStoreImage`, `sceGsPutDrawEnv`, `sceGsSyncPath`, `sceGsSyncV`.
(`sceGsExecLoadImage` and `sceGsExecStoreImage` are not called in TMB's runs; the reference had `sceGsSyncV` and
`sceGsSetDefLoadImage` as port overrides, kind 2 in the logs, the others as library stubs.)

There is no GS. The SetDef functions build, in guest memory, the packets the game sends later (the sends themselves are
DMA and GIF stores, answered from the log until the native renderer takes the draws); the native library writes them
exactly as the reference did. Their layout follows the game's own libgraph (its code in the ELF, `0x2033b0`-`0x204e80`)
where the reference agrees with it; where it does not, the reference wins (the records), noted below. What a renderer
needs is kept in `GsState` (`bozo/libs/gs.h`).

| Function | Native answer | Where the reference differs from the game's libgraph |
|---|---|---|
| `sceGsResetGraph(mode, interlace, video, field)` | mode 0: keeps the display mode; writes `{interlace, video, field, 3}` as bytes at scratchpad `0x100`; sets the kernel's field rate (`OP` 37, 60000/1001 in NTSC); returns 0 | the reference writes its own 4 bytes at scratchpad `0x100` (the game's libgraph keeps them in its GParam at `0x4e1f30`, which the reference leaves alone) and does not call `SetGsCrt` |
| `sceGsResetPath()` | nothing (no result) | |
| `sceGsSetDefDispEnv(disp, psm, w, h, dx, dy)` | PMODE `0x66`, SMODE2 (from the display mode), DISPFB, DISPLAY (magnification `(w + 2559) / w`, the vertical start 50/25 in NTSC), BGCOLOR 0; returns 0 | the horizontal start is 652 in NTSC (the game's libgraph: 636) |
| `sceGsSetDefDrawEnv(env, psm, w, h, ztest, zpsm)`, `...DrawEnv2` | the eight A+D pairs (FRAME, ZBUF with the Z buffer after two frame buffers, XYOFFSET at 2048 - w/2, SCISSOR, PRMODECONT and COLCLAMP or'ed with 1, DTHE for 16-bit formats, TEST) for context 1 or 2; returns 8 | |
| `sceGsSetDefClear(clear, ztest, x, y, w, h, r, g, b, a, z)` | TEST, PRIM sprite, RGBAQ (Q 1.0), two XYZ2, TEST; returns 0 | returns 0 (libgraph: 6) |
| `sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h)` | the GIF tag, BITBLTBUF, TRXPOS, TRXREG, TRXDIR 0, the IMAGE tag (its size from the format, as libgraph's table); returns 0 | BITBLTBUF has SBW = 1; returns 0 (libgraph: 6) |
| `sceGsSetDefStoreImage(sp, sbp, sbw, spsm, x, y, w, h)` | VIF codes, the GIF tag, BITBLTBUF, TRXPOS, TRXREG, FINISH, TRXDIR 1; returns 0 | the first VIF code is a NOP (libgraph: MSKPATH3); BITBLTBUF has DBW = 1; returns 0 (libgraph: 7) |
| `sceGsPutDrawEnv(packet)` | the A+D pairs kept in `GsState::registers`; returns 0 | |
| `sceGsSyncPath(mode, timeout)` | 0: nothing is ever in flight | |
| `sceGsSyncV(mode)` | 1 when the frames ended by then (this one included) are odd, else 0 | (settled on all 8 calls of boot and 2,290 of match1p; the vblank count's parity matches only half of them) |

`sceGsSyncV` is where the game's frame ends (the FRAME record inside the call). Its result is the library's; the wait
for the vblank and the frame end are the runtime's frame boundary: in a pinned run their time and the FRAME record come
from the log (`Function::frameBoundary`), in the real runtime from the presentation of the frame.

A call the run was cut inside (match1p's last `sceGsSyncV`, record flag `cut`: its result is not recorded) stays
answered from the log.

**What the native renderer will need from here** (phase 3's renderer, or TMB's native renderer moved over in phase 4):
the display mode (`interlace`, `videoMode`, `fieldMode`), the display environment (DISPFB: the frame buffer's width and
format; DISPLAY: position, magnification and size of the picture), the draw environment registers the game sets
(FRAME, ZBUF, XYOFFSET, SCISSOR, TEST, DTHE, COLCLAMP of both contexts), and the image transfers set up (GS buffer base,
width, format, rectangle, direction; the last 64 kept). The pixel data of uploads never goes through here: TMB's native
renderer takes textures from the game's own assets; the packets the game kicks (`sceGsExecLoadImage` and the DMA chains)
are still answered from the log as stores, until the renderer takes them (no-packets plan).

Results: boot 20,982 calls native (`sceGsSyncPath` 17,966, `sceGsPutDrawEnv` 2,994, `sceGsSyncV` 6, the SetDef and
Reset calls 16), match1p 117,534 (`sceGsSyncPath` 95,644, `sceGsPutDrawEnv` 19,291, `sceGsSyncV` 2,289,
`sceGsSetDefStoreImage` 74, `sceGsSetDefDispEnv` 65, ...), 0 divergences.

## 6. Controllers (`pad`)

Native: `scePadInit`, `scePadPortOpen`, `scePadGetState`, `scePadGetReqState`, `scePadInfoMode`, `scePadSetMainMode`,
`scePadInfoAct`, `scePadSetActAlign`, `scePadSetActDirect`, `scePadInfoPressMode`, `scePadEnterPressMode`,
`scePadRead`. (The Logitech wheel's `lgDevInit` and `input*Lg*` are the game's own functions over the USB driver, port
overrides in the reference: they stay with the log.)

The reference presents two DualShock 2 controllers, on ports 1 and 2, slot 0; no multitap (slots 1-3 report nothing).
Their state comes from a `PadInput` (`bozo/libs/pad.h`): `ScriptInput` reads the reference runs' frame-keyed scripts
(`harness.md` section 1, `reference/tmb/scripts/*_frames.txt`: `frame:BUTTON/frames`, the button held for the reads
made at game frames [frame, frame + frames), port 1 unless given). A PC input source (keyboard, gamepads) implements the
same interface later; `rumble()` receives the motor levels the game sets.

| Function | Native answer (settled on the records) |
|---|---|
| `scePadInit`, `scePadPortOpen` | 1 (every slot opens) |
| `scePadGetState(port, slot)` | 6 (stable); 5 (executing a command) in the frame `scePadSetMainMode` or `scePadEnterPressMode` was called and the next; 0 for slots 1-3 |
| `scePadGetReqState` | 0 (the request completed) |
| `scePadInfoMode(port, slot, term, offs)` | terms 1 and 2 (current, extended ID): 4 before analog mode is set, 7 once `scePadSetMainMode(.., 1, ..)` completed |
| `scePadInfoAct(port, slot, -1, 0)` | 2 actuators |
| `scePadSetMainMode`, `scePadSetActAlign`, `scePadSetActDirect`, `scePadInfoPressMode`, `scePadEnterPressMode` | 1 (the modes, the alignment and the motor levels kept) |
| `scePadRead(port, slot, data)` | 1, and the data: status 0, the mode ID (`0x79` in pressure mode, `0x73` analog, `0x41` digital), the buttons active low, then the right and left sticks, then twelve pressures |

The script's button maps as the reference's input did: the D-pad also pushes the left stick to its end (left `lx = 0`,
right `lx = 0xff`, up `ly = 0`, down `ly = 0xff`); a pressed button reports pressure `0xff`. The reference's pressure
order is right, left, up, down, triangle, circle, cross, square, **L1, L2, R1, R2** (match1p: L1 at byte 16, R1 at byte
18; L2 and R2 are never pressed in the reference runs, so their places are the two left). In pressure mode a read writes
20 bytes.

Check: `bozo-run match1p.clg ... --pad-script reference/tmb/scripts/ch1p_frames.txt` (`det1p.env`'s
`PS2X_PAD_SCRIPT`); boot runs without a script (`ref_boot.env` has none: both pads idle).

Results: boot 29,973 calls native (`scePadGetState` 23,960, `scePadRead` 5,968, ...), match1p 161,084
(`scePadGetState` 127,848, `scePadRead` 31,940, `scePadSetActDirect` 1,251, ...), 0 divergences.

## 7. Memory cards (`mc`)

Native: `sceMcInit`, `sceMcGetInfo`, `sceMcGetDir`, `sceMcSync`; and, not checked yet (below), `sceMcMkdir`,
`sceMcOpen`, `sceMcClose`, `sceMcRead`, `sceMcWrite`.

Two PS2 memory cards (ports 1 and 2, slot 0), formatted (`McCard`, `bozo/libs/mc.h`), each backed by a host folder
(`<dir>/mc0`, `<dir>/mc1`: a card's folders and files as host folders and files, loaded at the first card call and
written as they change) or by memory alone (`bozo-run`'s default: a fresh, empty card every run, as the reference's
test user folder; `--card-dir <dir>` for host files). Every operation finishes inside its call; `sceMcSync` hands its
result over.

| Function | Native answer (settled on the records) |
|---|---|
| `sceMcInit` | 0; the cards count as new again (next) |
| `sceMcGetInfo(port, slot, type, free, format)` | 0; writes type 2 (PS2 card), the free clusters (`0x2000` on an empty 8 MB card), format 1 at once (each pointer when not null); its result is -1 at the first call on a port after `sceMcInit` (and before it), 0 after |
| `sceMcGetDir(port, slot, name, mode, maxent, table)` | 0; writes the matching entries at once (64-byte `sceMcTblGetDir`: created and modified dates, size, attributes, name); a listing with a wildcard starts with `.` and `..` (attributes `0x80a3`, the date 2001-06-18 12:00:00: the reference's deterministic clock, as `sceCdReadClock`); its result is the count, -4 when nothing matches |
| `sceMcSync(mode, cmd, result)` | 1; writes the last operation's result (not `cmd`) |

What TMB does at boot, on the fresh cards: `GetInfo` on both ports (-1, then 0), `GetDir("*")` on the root (`.` and
`..`: no `BASCUS-97101` folder yet), `GetDir("BASCUS-97101/globalFile")` on both ports (-4). Boot and match1p: 43
calls native each, 0 divergences.

**The `save` run.** `reference/tmb/records/save.ref` has the per-frame record of a save and a load, but there is no
call log of it, so the save path (`sceMcMkdir` of `BASCUS-97101`; `sceMcOpen` of `icon.sys` and `LISTICON.ICO` with
flags `0x203`, of the save file `BASCUS-97101` with `0x3` then `0x1`, of `globalFile` with `0x202`; `sceMcWrite`,
`sceMcRead`, `sceMcClose`) is implemented from libmc's documented behaviour and unit-tested (`tests/libs`), not checked
against the reference. To check it: record `save.clg` with the reference build (`ref_save.env` with the call log on),
then `bozo-run save.clg --pinned --ref save.ref`. It will settle what the records cannot show yet: the results of
Open/Read/Write/Close (descriptors, byte counts), the free clusters after files are written (the reference's cluster
accounting), the attributes and dates of created folders and files, whether `sceMcSync`'s `cmd` is ever written, and
the card's error codes on the paths TMB tries.

## 8. The CD and file access (`cd`)

Native: `sceCdInit`, `sceCdMmode`, `sceCdDiskReady`, `sceFsReset`, `sceCdReadClock`, `sceCdRead`, `sceCdSync`,
`sceCdGetError`, `sceCdStInit`, `sceCdStStart`, `sceCdStRead`, `sceCdStStop` (`bozo/libs/cd.h`, `libs/cd.cpp`).

Sectors come from the game's disc image (`DiscImage`: an ISO 9660 image of 2048-byte sectors, checked by its primary
volume descriptor at sector 16; sectors past its end read as zeros). `bozo-run --iso <image>` (or `BOZO_ISO`) names
it; without one the CD library stays answered from the log. TMB_Recomp's runtime finds the user's image the same way
(`TMB_ISO`, else the `.iso` beside the game's folder). The extracted game folder cannot serve TMB: the game reads raw
sectors (the primary volume descriptor and the directories at boot, with its own ISO 9660 code) and streams movies by
LBN, so the image is required. Every sector a call wrote in the logs equals the image's.

### 8.1 What each call answers (settled on the records)

| Function | Native answer |
|---|---|
| `sceCdInit(mode)`, `sceCdMmode(media)` | 1 |
| `sceCdDiskReady(mode)` | 2 (ready) |
| `sceFsReset()` | 0 |
| `sceCdReadClock(clock)` | 1; the 8 bytes stat 0, second, minute, hour, 0, day, month, year in BCD: the reference's deterministic clock, 2001-06-18 12:00:00 (`CdState::hostClock` for the host's time in the real runtime) |
| `sceCdRead(lbn, sectors, buffer, mode)` | 1; the sectors are in the buffer when it returns (the reference's too: its runs hold them); it takes `sectors` x 114,400 EE cycles (every read of the logs) |
| `sceCdSync(0)` | waits for the read to end: when it waits, a vblank wait (reason 4, its result 0, no completion) until the vblank the drive is done by; then 0 |
| `sceCdSync(1)` | 1 while a read is in progress, else 0 |
| `sceCdGetError()` | the last read's error: 0 |
| `sceCdStInit(sectors, banks, buffer)` | 1; the ring's size (TMB: 0x50 sectors in 5 banks) |
| `sceCdStStart(lbn, mode)`, `sceCdStStop()` | 1; the stream starts at `lbn` / stops |
| `sceCdStRead(sectors, buffer, mode, error)` | the next sectors of the stream. Those that have arrived are copied at once; mode 1 (TMB's) waits for the rest: a vblank wait (its result the vblank's parity, -1) with a completion that copies them and sets v0 = sectors as the thread resumes. `*error` = 0; returns the sectors copied |

### 8.2 The drive's time and waits

In the reference these points came from the IOP it emulated (its CD driver answering RPCs as EE time was accounted):
`sceCdSync(0)` sometimes returned at once, sometimes took 273,058 cycles (the IOP's reply), sometimes waited for one
or two vblanks; a stream read waited for the vblank by which its sectors had arrived. Pinned runs take those points
from the log (section 1, timed calls). The stream ring's state the game observes is what a read copies at once: TMB's
streams always show the same shape (the first two reads of a stream wait with nothing there, the third copies 16
sectors and waits for 16, the rest find all 32 there).

Free mode (the native model, `CdState`): a read finishes within its call after its sectors' time (so sceCdSync never
waits); the stream's drive reads a sector every 114,400 cycles from `sceCdStStart` into the ring and pauses while the
ring is full; a read waits until the vblank by which its sectors have arrived. This gives the IOP side (and later the
native sound driver) a load's time, as the Midtown Suburbs lesson asks (`reference/tmb/README.md`).

### 8.3 What overrides read

Some reads are made by game code inside port overrides (`T` lines of `bozo-calllog-tool calls`: 19 `sceCdRead` in
match1p); those calls are answered whole from the log. None of them leaves state the native library depends on (the
drive is idle after every read in this model), so nothing is followed.

### 8.4 Free mode

`bozo-run` without `--pinned` and with the native CD departs from boot's log at the first `sceCdSync` (record #36,
where the reference waited for a vblank and the native drive has finished): a free run of the native CD is the real
runtime's, which no log can follow. Boot still matches in free mode with every other library native (15 s).

Results: boot 378 calls native (`sceCdStRead` 281, `sceCdSync` 40, `sceCdRead` 33, ...) and 9 stream continuations;
match1p 587 (`sceCdStRead` 338, `sceCdSync` 87, `sceCdRead` 65, `sceCdGetError` 52, ...) and 33 continuations; 0
divergences.

## 9. libmpeg (`mpeg`)

Native: `sceMpegInit`, `sceMpegCreate`, `sceMpegDelete`, `sceMpegReset`, `sceMpegAddCallback`, `sceMpegAddStrCallback`,
`sceMpegDemuxPssRing`, `sceMpegGetPicture`, `sceMpegIsEnd`, and the reference's stub of TMB's own `videoCallback`
(`videoCallback__FP7sceMpegP16sceMpegCbDataStrPv`, 0x1ec2c8) (`bozo/libs/mpeg.h`, `libs/mpeg.cpp`,
`libs/mpeg_ffmpeg.cpp`). The pictures are decoded by FFmpeg (9.1 below); a build without FFmpeg leaves libmpeg answered
from the log (`bozo-run` says so). Every rule below was settled on every libmpeg call of boot (3 movies) and match1p
(11 movies, the same 3 first); the records named are those that show each rule first or best.

### 9.1 FFmpeg

`libavcodec` (the `mpeg2video` decoder only, one thread) and `libavutil`; no `libswscale` (the conversion to the game's
pictures is ours, 9.4) and no `libavformat` or parser (the PSS demux and the cutting of the video stream into coded
pictures are ours, 9.3). The build looks in `BOZO_FFMPEG_ROOT` (a folder with `include/` and `lib/`: the TMB build
kit's static FFmpeg 9.0.2, LGPL, `--disable-everything` plus `mpeg2video`), else asks `pkg-config` for `libavcodec` and
`libavutil` (MSYS2's 9.0.2 package); without either, `mpeg_ffmpeg.cpp` builds without `BOZO_HAVE_FFMPEG` and
`addMpeg` registers nothing. LGPL: linked as the kit links it (its object files are shipped for relinking).

The decoder gets each coded picture with the headers before it: the video stream is cut at the first sequence or GOP
header after a picture's slices, else at the next picture header; a picture goes to the decoder once the next one's
cut is in the stream, the last at the end with a flush. FFmpeg holds one picture back (`has_b_frames`), so display
picture k comes out once picture k + 1 was sent. Its pictures equal the reference's on every byte of every picture of
both logs.

### 9.2 Create, callbacks, reset, delete

| Function | Native answer (records) |
|---|---|
| `sceMpegInit()` | 0 |
| `sceMpegCreate(mp, work, size)` | returns `work + 0x118`; 0x600 bytes from the game's heap (section 2.2), kept at `work + 0x44`; writes the sceMpeg (`mp + 0` .. `+ 7` zero: width and height, filled at the first picture; `+ 0x10` and `+ 0x28`, 16 bytes each, all ones; `+ 0x40` = work), the work area (`+ 0x1c` 0x1209f8, `+ 0x24` 0x120a08, `+ 0x80` all ones, `+ 0x94` 12 bytes all ones, `+ 0xb0` byte 1, `+ 0xd8` and `+ 0xe4` zero, `+ 0xf0` 8 bytes all ones, `+ 0x108`, `+ 0x110`, `+ 0x114` = work + 0x118, `+ 0x10c` = size - 0x118), libmpeg's static words in the ELF (0x1717bc = mp; 0x171800: nine pointers 0x171a50 ... 0x171bf0) (boot #228374 on fresh memory, #1394903 the second movie) |
| `sceMpegAddCallback(mp, type, function, data)` | the count of callbacks set since the create (1, 2, 3, 4, 5 for TMB's types 0, 1, 2, 3, 5) |
| `sceMpegAddStrCallback(mp, type, channel, function, data)` | 0 (types 0 video, 2 PCM; `data` is the fifth argument, in `t0`) |
| `sceMpegReset(mp)` | the picture count `mp + 8` and the static word 0x171904 cleared; `v0` left (#1393605) |
| `sceMpegDelete(mp)` | 0, nothing written |

A callback's handler gets the gp the game had when it registered it.

### 9.3 The demux: `sceMpegDemuxPssRing(mp, data, bytes, ring, ringSize)`

- The ring's size is the fifth argument (`t0`: 0x50000 in TMB); `data` is the ring address of the stream byte the call
  starts at (stream offsets map to the ring modulo its size).
- It takes the stream a sector (0x800 bytes) at a time while fewer than **8** starts are ahead of the game: the picture
  starts, the sequence end (`00 00 01 b7`) and the program end (`00 00 01 b9`) whole in the video packets demuxed so far,
  less the pictures delivered. Returns the bytes taken (boot #729735: 0x9800, nineteen sectors, the ninth packet's
  picture start making 8; #1009679: 0x2000; 0 when already 8 ahead). Nothing after the program end is read, but the
  sectors after it are still taken while the limit allows (boot #1391772 stops on the program end's sector, the eighth
  start; match1p #14756730 takes all 0x10000 of a short movie).
- A pack (`ba`), system header (`bb`), padding (`be`) are skipped; a PES packet is handled once its last byte is in. For
  a packet of a registered stream (`e0` video: type 0; `bd` private stream 1: type 2, PCM), its callback data (32
  bytes) is taken **from the game's heap** (section 2.2: first fit) and holds the type, 0, the payload's ring address,
  its length, its PTS and DTS (64-bit, -1 when the packet has none) (#2157551: video and PCM entries). The callback is
  queued as a handler of kind 4 (RPC callback) with `a0` = mp, `a1` = its data, `a2` = the callback's data, a
  completion; that completion gives the 32 bytes back to the heap (match1p #7651932: a movie whose data starts 0x20
  further on because a nodata callback's 32 bytes were still held).
- When the call ends: a thread waiting in `sceMpegGetPicture` for data whose picture is now decodable is woken (a `wake`
  inside the call, result 0, completion: match1p #14450352), then the callbacks are queued in packet order.
- The reference's stub of TMB's `videoCallback` returns 1 and copies nothing (#729748): the decoder takes the video
  from the demux. The PCM callback (0x1ec3e8) is a port override, still answered from the log.

### 9.4 `sceMpegGetPicture(mp, rgb32, mbcount)`

Picture k (k pictures delivered before it) is **decodable** when the stream holds its data and the next picture's whole
(two more starts after its own: the decoder holds one back), or once the stream has ended (the sequence or program end
seen). Then:

1. **Not decodable**: the game's nodata callback (`sceMpegAddCallback` type 1) is queued as a library handler (kind 7,
   `a0` mp, `a1` its 32-byte data {1, 0...} from the game's heap, completion) and the thread waits (reason 6 "libmpeg's
   picture", object 1, token `mp`, completion) (boot #228435-#228437). The handler's end gives the data back and wakes
   the thread (`wake` origin handler-end, result 0, #228462) unless a demux woke it already; as the thread resumes the
   call is decided again (origin resume: the next nodata loop, #228467, or the picture). The IPU's transfer (section 11)
   completes once this callback was queued.
2. **Not due**: picture k is due at the vblank count `base + 2k`, where `base` is the count when picture 0 was delivered,
   less one (two fields a picture: TMB's movies are 29.97 pictures a second). While the count is not past it, the
   thread waits for that vblank (reason 4, token `base + 2k`, the parity as result, completion) and the picture is
   delivered as it resumes (boot #729828 token 0x50, #1316108 0x52, #1316768 0x54; match1p #6578726 0xd70).
3. **Delivered**: the picture is written to `rgb32`, `mp + 0` and `+ 4` get its width and height, `mp + 8` its index k,
   the work area `+ 0xd8` the buffer and `+ 0xe4` mbcount; `v0` = 0 (boot #1317337).

**Past the last picture** (stream ended, every picture delivered): an empty delivery, at once (not paced): only `mp + 8`
and the work area are written (boot #1393483, match1p #6587612, #12948419).

**The picture as the game receives it**: 640 x 368 RGBA32, alpha 0x80, in 16 x 16 macroblocks in **column-major**
order (macroblock (x, y) at index x * 23 + y), pixels row by row inside one. Each pixel from its Y and its 2 x 2
block's Cb and Cr (nearest), in 13-bit fixed point with each product shifted on its own (arithmetic shift, so floor),
then clamped to 0-255:

    l = (9539 * (Y - 16)) >> 13
    R = l + ((13074 * (Cr - 128)) >> 13)
    G = l + ((-3210 * (Cb - 128)) >> 13) + ((-6660 * (Cr - 128)) >> 13)
    B = l + ((16524 * (Cb - 128)) >> 13)

Settled on 84,868 distinct (Y, Cb, Cr) of five pictures of boot's third movie and match1p's in-game movies (Cb 51-138,
Cr 119-219): R exact for 13073-13075, G for -6661 to -6657 with -3210, B for 16523-16525; the middle values are kept.
Every picture of both logs then equals the reference's on every byte.

### 9.5 `sceMpegIsEnd(mp)`

1 once the stream has ended, every picture was delivered and the last one's display time is over: the vblank count past
`base + 2 x pictures`; else 0 (match1p #6587505: 0 at the count `base + 2 x 37`, the game then gets an empty delivery;
#6587721: 1). That is why some movies end with one empty delivery and others not (boot's first movie: one; its second
and third: none).

### 9.6 The movie's sound

The PCM stream's packets go to a `MovieSoundSink` (`Libraries::movieSound`; none: dropped), for the future mixer. TMB's
movies carry Sony's PCM in private stream 1: a 4-byte sub-stream header (`ff a0 00 00`), then in the first packet an
`SShd` header (size 0x18: format 1, rate 48000, 2 channels, interleave 0x200) and an `SSbd` data header (its size); the
samples are 16-bit **little-endian**, 512 bytes of the left channel then 512 of the right. The sink gets the format
once, then interleaved frames with the packet's PTS. `bozo-run` counts them: boot 783,872 frames (16.3 s, the two
movies with sound).

### 9.7 What the renderer takes later

Each picture as decoded (`YuvPicture`, before the conversion) or as the game receives it, with the sceMpeg's width and
height, shown as the backdrop while TMB's vblank handler would upload it (TMB's native renderer does that today).

### 9.8 Open

- A library handler's first context: the reference's copies the calling thread's registers (its `CTX` records: `v0`,
  `t0`, `s0`, `s1`... of the caller), which `bozo-run` takes from the log as it did before (lenient, `calllog.md` 4.4);
  the native runtime gives a handler a fresh context. TMB's nodata callback does not read them.
- The pace assumes two fields a picture; a movie at another rate (none in TMB) needs it from the sequence header.

## 10. VU0 microprograms (`vu0`)

Native: every VCALLMS TMB makes, the hierarchy image's eight programs (`libs/vu0.cpp`):

| Program | What it computes | Calls (boot / match1p) |
|---|---|---|
| 0x278 `hierFovTest2Asm` | an object's bounding sphere in the two view frames at VU vi10 + 50 and + 53, pushed out and in by its radius; vi2, vi5 the sign tests the microcode reads of them | 230,002 / 11,883,364 |
| 0x3a8 | vf3 = vf1 + vf4, vf6.x = (vf3 . vf3) x vf8.y | 0 / 2,514,914 |
| 0x410 `hierMulEoForObjAsm` | vf6 = the rotation at VU vi10 + 59 applied to vf4; vf7-9 = VU vi10 + 56..58 | 18,016 / 3,993,967 |
| 0x450 `hierCsUpdateAsm` | the camera: its rotation to VU 59-61, its transpose applied to VU 10 + vf19, VU 50-58 the three frames at VU 11-21 by it | 126,565 / 1,471,274 |
| 0x5a8 `hier` | the root's frames: VU 50-58 from VU 11-21, VU 59-61 the identity | 0 / 13,722 |
| 0x670 `vu0HierRotateAsm` | a child's frames twelve quadwords on from its parent's, by the rotation vf1-3 | 0 / 1,082,417 |
| 0x808 `hierTraverseAsm` | the lights reaching an object (at most four of VU 9.x spheres), as the light matrix and colours | 0 / 2,813,762 |
| 0xab0 `hdHatSphereTestAsm` | a sphere against a segment: vi3 0x80 when it is hit | 0 / 274,504 |

Each was written from its microcode in the ELF (the hierarchy image: file offset 0x135070, two MPGs of 256 pairs at 0
and 135 at 0x800, from the static VIF0 chain at 0x234060) and checked on every call of the logs: every VU register
word (vf, vi, ACC, Q, I, the clip flags) and every VU0 data memory byte each call changes, 0 divergences.

**Arithmetic (what makes them bit-exact).** Single precision rounded toward zero (products, quotients, roots); sums
on VU0's adder: the operand with the smaller exponent loses its bits below the larger one's last mantissa bit first (no
guard bits), then the sum is rounded toward zero; MULA/MADDA/MADD chains are those products and sums in the microcode's
order (nothing fused); a field moved with ADDbc vf0 turns -0 into +0. The status and MAC flags are not changed (the
reference leaves them). `vu0.cpp` is compiled with `-frounding-math -ffp-contract=off` and runs each program under
`fesetround(FE_TOWARDZERO)`. vi registers are kept as their 16 bits.

**Which image VU0 holds.** The game sends VU0 its microcode through VIF0 from two static chains in the ELF: the
hierarchy one (0x234060, by `main` and `hier__Fi` each frame) and the 2D one (0x2337d0, `drawInit`: debug draws only,
never in TMB's runs). The hierarchy chain's kicks happen inside calls the logs answer whole (no VIF0 store of theirs is
an out-call), so the image is the hierarchy one until a kick of the 2D chain is seen (`Vu0State::kick`, from the I/O
model's VIF0 channel); the 2D image's programs (0x278, 0x3b0, 0x438, 0x548, 0x5e0) are not native: a call to one would
be answered from the log and counted ("calls with no native answer").

## 11. Hardware registers (`io`)

Native: every hardware load and store TMB's logs hold (`bozo/libs/io.h`, `libs/io.cpp`), answered by a model of only
what the game observes. No DMA, VIF, GIF, IPU or GS runs behind them.

| Address | Register | Native answer |
|---|---|---|
| 0x10000000 | T0_COUNT | the count written last plus the periods since (T0_MODE 0x82: BUSCLK/256, a count every 512 EE cycles); a load that reads the same count as the previous one (a poll) leaves the period, 512 cycles, pending (charged at the next checkpoint: `calllog.md` section 2) |
| 0x10000010 | T0_MODE | kept (0x82) |
| 0x10008000-30 | VIF0 CHCR, MADR, QWC, TADR | a start (CHCR STR) completes at once: D_STAT bit 0 and the DMAC interrupt (the kernel runs or keeps it); a chain start notes the microcode image (section 10) |
| 0x10009000-30 | VIF1 | as VIF0 (D_STAT bit 1): TMB's 3D packets, 2.5M kicks in match1p |
| 0x1000a000-30 | GIF | as VIF0 (bit 2), except a chain whose first packet draws (its GIF tag sets PRIM: the sprite TMB sends from 0x4fe180 before `sceGsSyncV` on its loading screens, 2,269 kicks in match1p): the reference drops it, no completion and no interrupt |
| 0x1000b400-30 | IPU_TO (channel 4) | the transfer completes (bit 4, the interrupt) at the game's first store after libmpeg's decoder took it, i.e. after the decoder asked for more data (it queued the game's nodata callback: the native libmpeg says so, section 9.4); a CHCR store without STR stops the channel (TMB's movie end: the last transfer never completes) |
| any channel | CHCR load | the value written, STR cleared once complete |
| any channel | MADR, QWC, TADR loads | 0 (the reference's answer: TMB reads channel 4's MADR, 0 in every record, also before any transfer) |
| 0x1000e000 | D_CTRL | kept (TMB writes 3; reads 0 before) |
| 0x1000e010 | D_STAT | bits 0-9 set by completions, written 1 clear them; bits 16-25 written 1 toggle |
| 0x1000e020 | D_PCR | kept; CPCOND0 (BC0F/BC0T) is every channel it names completed (any channel before the first write: phase 2's rule) |
| 0x1000e030-60 | D_SQWC, D_RBSR, D_RBOR, D_STADR | kept |
| 0x1000f520 | D_ENABLER | 0: no transfer is ever suspended (none runs) |
| 0x1000f590 | D_ENABLEW | kept |
| 0x12000000-f0 | the GS's privileged registers (PMODE, SMODE2, DISPFB1/2, DISPLAY1/2, BGCOLOR...) | 64-bit stores kept in `IoState::gsPrivileged` for the renderer (TMB's movie layer uses both circuits: PMODE 0xff03) |

The kernel's question at `EnableDmac` (is the channel's D_STAT bit still set, `calllog.md` section 6) is now answered by
the model's D_STAT, no longer from the log.

**Followed from the log.** Two of the model's points live inside calls the logs still answer whole, and `bozo-run`
follows them (counted as followed calls):
- TMB's frame boundary (the port override that ends the frame) restarts timer 0 inside it: after a call with a frame
  inside, the next load of T0_COUNT re-bases the count on the log's value (and whether the override's last read was
  that same value, from the log's pending poll cycles). The native frame boundary of the real runtime restarts it
  itself.
- IPU_TO's completion follows libmpeg's nodata callbacks: from the native libmpeg, or from the log's queue operations
  when libmpeg is answered from the log (a build without FFmpeg).

Results: boot 855,098 accesses native (`D_ENABLEW`/`D_ENABLER` 472,180, IPU_TO 236,108, VIF1 62,997, ...), match1p
17,605,351 (VIF1 7,544,895, `D_STAT`/`D_PCR` 5,020,295, `D_ENABLEW`/`D_ENABLER` 2,631,172, T0_COUNT 927,111 loads, ...);
0 divergences.

## 12. Still answered from the log

SIF and 989SND (`sceSif*`, the RPC to service 0x123456, the IOP heap and modules), the 2D image's VU0 programs (never
called), and every port override (TMB's own native renderer, sound and the frame boundary, the Logitech wheel functions,
the movie's PCM callback).

## 13. Results

`bozo-run <log> --pinned --ref <ref> --iso <image>` with every library native (`--native all`, the default), match1p
with `--pad-script reference/tmb/scripts/ch1p_frames.txt`:

| Run | Calls answered natively | Followed | Divergences | Frames matching | Records | Time |
|---|---|---|---|---|---|---|
| boot, batch A | 293,586 (libc 242,372, libm 216, gs 20,982, pad 29,973, mc 43) | 3 | 0 | 3000 of 3000 | 4,650,558 of 4,650,558 | 14 s |
| match1p, batch A | 11,119,646 (libc 2,060,202, libm 8,780,783, gs 117,534, pad 161,084, mc 43) | 4,843 | 0 | 16000 of 16000 | 74,615,923 of 74,615,923 | 165 s |
| boot, batch B1 | 1,523,645 (batch A's, cd 378, vu0 374,583, io 855,098) and 9 CD continuations | 3 | 0 | 3000 of 3000 | 4,650,558 of 4,650,558 | 17 s |
| match1p, batch B1 | 52,773,508 (batch A's, cd 587, vu0 24,047,924, io 17,605,351) and 33 CD continuations | 12,979 | 0 | 16000 of 16000 | 74,615,923 of 74,615,923 | 267 s |
| boot, batch B2 | 1,530,467 (batch B1's, mpeg 6,822: `videoCallback` 3,685, `sceMpegDemuxPssRing` 1,532, `sceMpegIsEnd` 788, `sceMpegGetPicture` 785, ...) and every libmpeg handler end and resume (the 59k nodata callbacks') | 0 | 0 | 3000 of 3000 | 4,650,558 of 4,650,558 | 20 s |
| match1p, batch B2 | 52,781,732 (batch B1's, mpeg 8,224) | 12,968 | 0 | 16000 of 16000 | 74,615,923 of 74,615,923 | 286 s |

Frames match on ram, scratchpad, calls and threads (eeCycle too). Phase 2's runs with everything from the log took 17 s
(boot) and 164 s (match1p). match1p's extra time is the check, not the libraries: each of its 42 million VU0 and I/O
calls has its 332 context words compared before and after, and its writes journaled.

Open points: L1 (section 3: the Universal CRT's float sine and cosine off Windows), the `save` run's call log
(section 7), L2/R2's pressure places (section 6), PAL's display constants (not in TMB's runs: US only), libmpeg's open points
(section 9.8), the CD's free-mode timing against a real drive (section 8.2: a model, not the reference's IOP), and the two points
the I/O model follows from the log (section 11).
