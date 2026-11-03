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

**The reference's libmpeg allocates from the same heap.** Each `sceMpegCreate` (answered from the log until batch B)
takes 0x600 bytes and never gives them back; the reference keeps the block's address at the work area + 0x44
(`sceMpegCreate`'s second argument), which the record's memory runs show (match1p: 11 creates, the blocks at
`0x752720`, `0x752d20`, ... `0x756320`; the 12th `malloc` after them, record #16897922, returns `0x756920`, 0x4200 past
where a heap without them would). The native heap follows each `sceMpegCreate` answered from the log and checks the
address. A native libmpeg (batch B) must take its 0x600 bytes from this heap in the same order.

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

## 8. Still answered from the log (batch B)

CD and CD streaming (`sceCd*`, `sceFsReset`), SIF and 989SND (`sceSif*` and the IOP heap and modules), libmpeg
(`sceMpeg*`; its `sceMpegCreate` heap blocks are followed by the native heap: section 2.2), VU0 microprograms, hardware
loads and stores (DMA, GS registers), and every port override (TMB's own native renderer, sound and the frame
boundary, the Logitech wheel functions).

## 9. Results (batch A)

`bozo-run <log> --pinned --ref <ref>` with every batch A library native (`--native all`, the default), match1p with
`--pad-script reference/tmb/scripts/ch1p_frames.txt`:

| Run | Calls answered natively | Followed | Divergences | Frames matching | Records | Time |
|---|---|---|---|---|---|---|
| boot | 293,586 (libc 242,372, libm 216, gs 20,982, pad 29,973, mc 43) | 3 | 0 | 3000 of 3000 (ram, scratchpad, calls, threads; eeCycle too) | 4,650,558 of 4,650,558 | 14 s |
| match1p | 11,119,646 (libc 2,060,202, libm 8,780,783, gs 117,534, pad 161,084, mc 43) | 4,843 | 0 | 16000 of 16000 (ram, scratchpad, calls, threads; eeCycle too) | 74,615,923 of 74,615,923 | 165 s |

Open points: L1 (section 3: the Universal CRT's float sine and cosine off Windows), the `save` run's call log
(section 7), L2/R2's pressure places (section 6), PAL's display constants (not in TMB's runs: US only).

Phase 2's runs with everything from the log took 17 s (boot) and 164 s (match1p): the native libraries and their
checks cost nothing measurable. Boot also matches in free mode with every batch A library native (15 s).
