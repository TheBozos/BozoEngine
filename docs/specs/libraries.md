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
