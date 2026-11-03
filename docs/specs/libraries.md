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
