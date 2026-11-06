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

**Replies decided by the IOP's time (batch B3).** The 989SND driver behind `sceSifCallRpc` (section 15) answers
most of its replies itself, but whether a one-shot sound still plays, and so which handler a new sound gets, was
decided by the reference's IOP time. A pinned run gives such a call (`Function::logReply`) the log's reply words
(`Pin::logWord`: the record's runs, else the memory as it was); the driver takes from them only those decisions, counts
them, and makes everything else (section 15.5). In free mode it answers from its own models.

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
calls of the SIF: native since batch B3 (section 14).

## 5. The GS library (`gs`)

Ports that explicitly enable `Registry::nativeGraphics` own image uploads and readback at their native asset/render
boundaries. For those ports, `sceGsSetDefLoadImage` and `sceGsSetDefStoreImage` return zero without serializing a
packet or dereferencing its output address. Generic callers retain the SDK output ABI described below. Port hooks
that bind native textures must still run before calling the SDK builder. The `native_image_builders` test covers
both modes, guarded output buffers and inaccessible native outputs. This change does not replace readback consumers.

Native: `sceGsResetGraph`, `sceGsResetPath`, `sceGsSetDefDispEnv`, `sceGsSetDefDrawEnv`, `sceGsSetDefDrawEnv2`,
`sceGsSetDefClear`, `sceGsSetDefLoadImage`, `sceGsSetDefStoreImage`, `sceGsPutDrawEnv`, `sceGsSyncPath`, `sceGsSyncV`,
`sceGsExecLoadImage`, `sceGsExecStoreImage`. (The reference had `sceGsSyncV` and `sceGsSetDefLoadImage` as port
overrides, kind 2 in the logs, the others as library stubs.)

Phase 4 (TMB's overrides native, `kernel.md` section 3b) reached what only ran inside overrides before:
`sceGsExecLoadImage(lp, src)` returns 0 and uploads nothing (boot: 86 calls, all inside `texmResInit` /
`texmLoadResTexture`); `sceGsSetDefLoadImage`'s image tag counts T8H, T4HL and T4HH (0x1b, 0x24, 0x2c) as 32-bit
texels (match1p #16897863). `sceGsExecStoreImage` now requires a native port override; it throws rather than parsing
a packet and fabricating an image. Ports implement readback from their own query/model/texture inputs.

There is no GS register bank or transfer history. The SetDef functions only write SDK output structures for
recompiled callers that still use those bytes; native rendering never consumes those packets. When a port explicitly
enables `Registry::nativeGraphics`, image load/store SetDef calls instead return 0 without reading arguments beyond
the port mode, dereferencing the output or serializing any packet. Native uploads belong to the asset boundary;
readback belongs to the port's high-level caller, and unsupported ExecStoreImage still fails explicitly. Other SDK
templates remain available because callers may inspect their scalar fields. Scalar video
parameters come from the SDK's scratchpad parameters, without a second engine copy. The native renderer gets draws,
materials, textures and presentation settings directly from the port. The historical call-log totals below describe
the earlier implementation and do not qualify the current packet-free boundary.
`native_image_builders` is a standalone game-free test of this opt-in boundary: generic calls journal their SDK
output, native calls do not write valid guarded output buffers, and inaccessible native outputs are never read.

| Function | Native answer | Where the reference differs from the game's libgraph |
|---|---|---|
| `sceGsResetGraph(mode, interlace, video, field)` | mode 0: keeps the display mode; writes `{interlace, video, field, 3}` as bytes at scratchpad `0x100`; sets the kernel's field rate (`OP` 37, 60000/1001 in NTSC); returns 0 | the reference writes its own 4 bytes at scratchpad `0x100` (the game's libgraph keeps them in its GParam at `0x4e1f30`, which the reference leaves alone) and does not call `SetGsCrt` |
| `sceGsResetPath()` | nothing (no result) | |
| `sceGsSetDefDispEnv(disp, psm, w, h, dx, dy)` | PMODE `0x66`, SMODE2 (from the display mode), DISPFB, DISPLAY (magnification `(w + 2559) / w`, the vertical start 50/25 in NTSC), BGCOLOR 0; returns 0 | the horizontal start is 652 in NTSC (the game's libgraph: 636) |
| `sceGsSetDefDrawEnv(env, psm, w, h, ztest, zpsm)`, `...DrawEnv2` | writes eight A+D pairs for context 1 or 2; initializes PRMODECONT/COLCLAMP to 1 and DTHE from the format without reading old packet bytes; returns 8 | reserved bits are initialized |
| `sceGsSetDefClear(clear, ztest, x, y, w, h, r, g, b, a, z)` | TEST, PRIM sprite, RGBAQ (Q 1.0), two XYZ2, TEST; returns 0 | returns 0 (libgraph: 6) |
| `sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h)` | the GIF tag, BITBLTBUF, TRXPOS, TRXREG, TRXDIR 0, the IMAGE tag (its size from the format, as libgraph's table); returns 0 | BITBLTBUF has SBW = 1; returns 0 (libgraph: 6) |
| `sceGsSetDefStoreImage(sp, sbp, sbw, spsm, x, y, w, h)` | VIF codes, the GIF tag, BITBLTBUF, TRXPOS, TRXREG, FINISH, TRXDIR 1; returns 0 | the first VIF code is a NOP (libgraph: MSKPATH3); BITBLTBUF has DBW = 1; returns 0 (libgraph: 7) |
| `sceGsPutDrawEnv(packet)` | returns 0 without reading the packet; the port supplies native materials | |
| `sceGsSyncPath(mode, timeout)` | 0: nothing is ever in flight | |
| `sceGsSyncV(mode)` | 1 when the frames ended by then (this one included) are odd, else 0 | (settled on all 8 calls of boot and 2,290 of match1p; the vblank count's parity matches only half of them) |

`sceGsSyncV` is where the game's frame ends (the FRAME record inside the call). Its result is the library's; the wait
for the vblank and the frame end are the runtime's frame boundary: in a pinned run their time and the FRAME record come
from the log (`Function::frameBoundary`), in the real runtime from the presentation of the frame.

A call the run was cut inside (match1p's last `sceGsSyncV`, record flag `cut`: its result is not recorded) stays
answered from the log.

Native rendering does not obtain any state from libgraph packet submissions. A port must translate its high-level
draw and image operations into native renderer calls. Unhandled readback cannot silently return a black image.

Results: boot 20,982 calls native (`sceGsSyncPath` 17,966, `sceGsPutDrawEnv` 2,994, `sceGsSyncV` 6, the SetDef and
Reset calls 16), match1p 117,534 (`sceGsSyncPath` 95,644, `sceGsPutDrawEnv` 19,291, `sceGsSyncV` 2,289,
`sceGsSetDefStoreImage` 74, `sceGsSetDefDispEnv` 65, ...), 0 divergences.

## 6. Controllers (`pad`)

Native: `scePadInit`, `scePadPortOpen`, `scePadGetState`, `scePadGetReqState`, `scePadInfoMode`, `scePadSetMainMode`,
`scePadInfoAct`, `scePadSetActAlign`, `scePadSetActDirect`, `scePadInfoPressMode`, `scePadEnterPressMode`,
`scePadRead`. (The Logitech wheel's `lgDevInit` and `input*Lg*` are the game's own functions over the USB driver, port
overrides in the reference: they stay with the log.)

The reference presents two DualShock 2 controllers, on ports 1 and 2, slot 0. A game's multitap library is its port's
(TMB's `sceMtap*` overrides open one in port 1 at its first frame, in every reference run: `PadState::multitap`).

Players (`padPlayer`): with the multitap, port 1's slots 0-3 are players 1-4 and port 2 has no player; without it, port 1
slot 0 is player 1 and port 2 slot 0 player 2. Sockets:
- port 1 slot 0 and port 2 slot 0 always hold a controller (port 2's idle when no player is on it: behind the
  multitap, as the reference's boot and match1p read it every frame, idle);
- a multitap slot 1-3 holds one only when its player has an input (a script, or a PC gamepad), else it reports nothing
  (`scePadGetState` 0, `scePadRead` 0), as the reference's runs without PS2X_PAD2..4_SCRIPT;
- slots 1-3 without the multitap: nothing.

Their state comes from a `PadInput` (`bozo/libs/pad.h`), by player: `ScriptInput` reads the reference runs'
frame-keyed scripts (`harness.md` section 1, `reference/tmb/scripts/*_frames.txt`: `frame:BUTTON/frames`, the button
held for the reads made at game frames [frame, frame + frames), player 1 unless given; players 2-4 have a controller
only with a script, as the reference's `PS2X_PAD2_SCRIPT`..`PS2X_PAD4_SCRIPT` in `det4p.env`). The PC's devices
(`bozo/app/device_pad.h`, milestone M2) implement it in a window: keyboard, mouse and gamepads through `input.ini`.
`rumble(player, small, large)` receives the motors the game sets: `scePadSetActDirect`'s six bytes through
`scePadSetActAlign`'s alignment (byte i drives the small motor, on when non-zero, if its alignment is 0; the large one,
0-255, if 1; none if 0xff).

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

`sceCdStatus` derives its public SDK result from native service lifecycle and jobs: stopped before initialization
or after exit-mode initialization, read while a native read/stream is active, emergency on a stored read error,
otherwise paused/ready. Values 0/6/0x20/0x0a follow ps2dev/ps2sdk `libcdvd-common.h`
(https://github.com/ps2dev/ps2sdk/blob/master/common/include/libcdvd-common.h). There is no SCMD/RPC bind or console
register backing this API. The CD suite covers each lifecycle/job state; it does not infer physical media motion.
`sceCdGetReadPos` reports bytes copied by the active native read, and zero outside a read command, following the
SDK's read-command lifetime check. It never returns a sector number. The current synchronous native copy has all
job bytes ready before return; it does not synthesize partial drive-counter progress. The actual ISO read fixture
checks 2048 bytes while pending and zero after completion.

Native callback registration: `sceCdCallback` retains the function and GP per `Libraries` instance, returns the
previous function, and refuses changes during a pending read. `sceCdInitEeCB(priority, stack, bytes)` validates and
retains its callback stack once (1 first, 0 repeated, -1 invalid). The supplied priority is validated; callbacks use
the native handler queue, not an original SDK worker/private semaphores. This does not claim dedicated-worker
priority parity. A successful native `sceCdRead` queues exactly one callback with reason 1, the registered GP and
stack, after the read's existing duration is charged; the copied data is ready before callback execution. Existing
read/sync timing remains unchanged. The synthetic `cd_callbacks` suite checks setup bounds/repetition,
replacement/unregistration/busy refusal, ownership isolation and actual dispatcher read/callback ordering on a
private generated ISO. It makes no full-game or other CD command callback claim.

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

Two more cases (phase 4 M5, the story runs):
- **The last picture taken late.** A movie the game plays a picture a game frame (story's seventh, the Junkyard's
  intro: picture 45 taken at `base + 91`, one vblank after its slot) is not over at `base + 92` (#11644213: 0); the
  game then gets an empty delivery and 1 (#11644909). So while the last picture was taken after `base + 2 x delivered`,
  0. Every other movie of boot, match1p and story took its last picture at `base + 2 x n` or earlier.
- **An aborted playback.** A skipped movie (TMB's `videoDecAbort`, then `videoDecFlush` appending a sequence end to the
  IPU ring BozoEngine's libmpeg never reads) never reaches its stream's end: the port says so
  (`MpegState::aborted`, TMB's videoDecAbort override) and sceMpegIsEnd answers 1 from then on (story #9723777 the
  abort, #9723802 one more picture, #9723824: 1 with 7 of the stream's pictures never delivered).

The demux's read-ahead limit counts a picture from its first header: a sequence or GOP header whole in the stream
counts its coded picture before its picture start code arrives (stage-level0 #11640007, the Midtown Suburbs movie:
0xd000 bytes taken, where picture 42's header is whole and its start code not yet).

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

Native: the remaining register/status compatibility operations (`bozo/libs/io.h`, `libs/io.cpp`). No DMA, VIF, GIF,
IPU or GS executes behind them. `sceDmaReset` clears native completion bookkeeping at its SDK entry point instead
of running a hardware reset loop. The free host faults on unknown device accesses; it no longer silently returns
zero or drops a write. Channel register accesses support byte, halfword and word widths at the register base.

`sceDmaSend`, `sceDmaSendN`, `sceDmaSendI` and `sceDmaSync` also have native SDK bindings. VIF1/GIF sends acknowledge
the graphics work the port already submitted and queue the kernel's completion handlers, without reading or
retaining the packet/chain address or transfer length. Normal, chain and interleave calls keep the SDK's completion
status convention; sync returns immediately for completed native work instead of executing libdma's polling loops.
VIF0 chain submissions select a known native VU0 program image by source identity. IPU-to submissions retain only
the native movie decoder's deferred-completion contract. Invalid handles, unsupported data producers, overlapping
pending transfers and unimplemented blocking waits fail explicitly. `IoState::nativeSubmissions` and
`nativeSynchronizations` are service diagnostics, not device state. A port still touching the registers directly
uses the compatibility operations listed below.

With a port registered for native graphics, these SDK submissions and sync calls bypass the retired channel and
status banks entirely. The SDK scheduler receives completion notifications directly. Masked callbacks retain native
job-completion state (`nativeCompleted`) so enabling their handler still delivers them; no CHCR/D_STAT/CPCOND0
values are manufactured. Native reset clears SDK jobs/configuration without resetting a device model.
`sceDmaPutEnv` / `sceDmaGetEnv` validate and round-trip the opaque 20-byte SDK configuration, including the original
field error codes, without programming a controller. `sceGsPutDispEnv` returns without reading its obsolete display
structure or issuing display-register writes; the native renderer/platform layer already owns presentation.

`FreeOptions::profileIo` collects remaining device accesses by guest PC and address in `FreeStats::ioSites`.
It is off by default. Ports can use it to find callers that still need native adaptation; the existing native
graphics guard rejects any attempt to access retired graphics devices before the compatibility path runs.

| Address | Register | Native answer |
|---|---|---|
| 0x10000000 | T0_COUNT | the count written last plus the periods since (T0_MODE 0x82: BUSCLK/256, a count every 512 EE cycles); a load that reads the same count as the previous one (a poll) leaves the period, 512 cycles, pending (charged at the next checkpoint: `calllog.md` section 2) |
| 0x10000010 | T0_MODE | kept (0x82) |
| (any store) | a port's store patch | a store the port's patch names (`bozo::port::setStorePatch`, by its instruction: the externals' `store` lines) is made as an out-call without its effect (match1p #62516600: TMB's dialog kick, logged, no completion after it) |
| 0x12001000 | GS CSR (loads) | FIELD (bit 13) the vblank count's parity, nothing else (TMB's vblankHandler reads it inside its override: boot #729700 on). A port's own access (`bozo::port::ioRead32` / `ioWrite32`) is not the game's: a store of its does not complete channel 4's transfer (match1p #14450438); a T0 read of its is a poll as the game's, a T0 write keeps the last read, its (#34736003) and the game's alike (stage-level0 #13062314: the game read 0, wrote 0 and read 0: a poll), and the EE time the port charges (`bozo::port::accountCycles`, TMB's frame end) takes the poll's pending cycles as a checkpoint would (#17538393, #17541951) |
| 0x10008000-30 | VIF0 CHCR, MADR, QWC, TADR | a start (CHCR STR) completes at once: D_STAT bit 0 and the DMAC interrupt (the kernel runs or keeps it); a chain start notes the microcode image (section 10) |
| 0x10009000-30 | VIF1 | as VIF0 (D_STAT bit 1): TMB's 3D packets, 2.5M kicks in match1p |
| 0x1000a000-30 | GIF | immediate native completion (bit 2); no chain or packet is read, regardless of PRE/PRIM |
| 0x1000b400-30 | IPU_TO (channel 4) | the transfer completes (bit 4, the interrupt) at the game's first store after libmpeg's decoder took it, i.e. after the decoder asked for more data (it queued the game's nodata callback: the native libmpeg says so, section 9.4); a CHCR store without STR stops the channel (TMB's movie end: the last transfer never completes) |
| any channel | CHCR load | the value written, STR cleared once complete |
| any channel | MADR, QWC, TADR loads | 0 (the reference's answer: TMB reads channel 4's MADR, 0 in every record, also before any transfer) |
| 0x1000e000 | D_CTRL | kept (TMB writes 3; reads 0 before) |
| 0x1000e010 | D_STAT | bits 0-9 set by completions, written 1 clear them; bits 16-25 written 1 toggle |
| 0x1000e020 | D_PCR | kept; CPCOND0 (BC0F/BC0T) is every channel it names completed (any channel before the first write: phase 2's rule), the completions counting from the last FlushCache on (phase 4: TMB's hierCheckVifComplete, right after hier__Fi's FlushCache, reads it false although VIF1's last transfer completed: match1p #17540836, every one of its calls) |
| 0x1000e030-60 | D_SQWC, D_RBSR, D_RBOR, D_STADR | kept |
| 0x1000f520 | D_ENABLER | 0: no transfer is ever suspended (none runs) |
| 0x1000f590 | D_ENABLEW | kept |
| 0x12000000-f0 | obsolete GS display-register stores | acknowledged and discarded; native presentation owns the display, no register bank is retained |

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

The 2D image's VU0 programs (never called), and every port override (TMB's own native renderer, sound and the frame
boundary, the Logitech wheel functions, the movie's PCM callback, and the overrides that call 989SND: section 15.7).
Since batch B3 the SIF layer is native (section 14) and so is the 989SND driver behind it (section 15), except the
replies the reference's IOP time decided, which a pinned run takes from the log (section 15.5).

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
| boot, batch B3 | 1,537,679 (batch B2's, sif 7,212) and 3,591 989SND replies (1 pinned) | 937 | 0 | 3000 of 3000 | 4,650,558 of 4,650,558 | 21 s |
| match1p, batch B3 | 53,026,840 (batch B2's, sif 245,108) and 119,225 989SND replies (3,294 pinned) | 14,691 | 0 | 16000 of 16000 | 74,615,923 of 74,615,923 | 287 s |

Frames match on ram, scratchpad, calls and threads (eeCycle too). Phase 2's runs with everything from the log took 17 s
(boot) and 164 s (match1p). match1p's extra time is the check, not the libraries: each of its 42 million VU0 and I/O
calls has its 332 context words compared before and after, and its writes journaled.

The "followed" column counts the calls answered from the log the native state followed (since B3 also the port
overrides whose 989SND calls the driver follows: section 15.7). Open points: L1 (section 3: the Universal CRT's float
sine and cosine off Windows), the `save` run's call log
(section 7), L2/R2's pressure places (section 6), PAL's display constants (not in TMB's runs: US only), libmpeg's open points
(section 9.8), the CD's free-mode timing against a real drive (section 8.2: a model, not the reference's IOP), and the two points
the I/O model follows from the log (section 11), and 989SND's sound lifetimes, handler slots and voice allocation
(section 15.10).

## 14. The SIF layer (`sif`)

Native: `sceSifInitRpc`, `sceSifBindRpc`, `sceSifCallRpc`, `sceSifCheckStatRpc`, `sceSifGetSreg`, `sceSifSetSreg`,
`sceSifInitIopHeap`, `sceSifAllocIopHeap`, `sceSifFreeIopHeap`, `sceSifLoadModule`, `sceSifRebootIop`, `sceSifSyncIop`,
and `sceSifSetDma`, `sceSifDmaStat` (`bozo/libs/sif.h`, `libs/sif.cpp`). There is no IOP: an RPC goes to a native
server (TMB's only one: 989SND, section 15), a module load loads nothing, the IOP heap is a model of the memory the
reference's IOP allocator handed out. Every call of both logs returns at once (0 EE cycles, no record inside it): the
reference's IOP answered every RPC inside the call, so nothing is ever in flight.

### 14.1 What each call answers (settled on the records)

| Function | Native answer |
|---|---|
| `sceSifInitRpc(mode)` | 0, nothing written (boot #25, #29, #150; 14 calls in match1p, the movie code's too) |
| `sceSifBindRpc(client, service, mode)` | the client header's `rpc_id` (`client + 4`) the next one (1, 2...) and its semaphore (`+ 8`) -1; once the service has a server, `client + 0x24` = its server data (0x1f10000) and the server data's first word the service id; 0. The reference's 989SND registered its server between the game's first and second attempts (boot #151: `rpc_id` 1, no server; #152: `rpc_id` 2, bound), so a service binds at the second attempt |
| `sceSifCallRpc(client, fno, mode, send, ssize, recv, rsize, end, param)` | `client + 0x0c` = mode, `+ 0x10` = fno; in the server data `+ 0x0c` ssize, `+ 0x1c` client, `+ 0x24` fno, `+ 0x28` recv, `+ 0x2c` rsize (boot #156, #159, #210987); the server's reply in `recv` (section 15); 0. The server is the one the client was bound to (kept host-side): the server data's first word is EE RAM the game may reuse (the story's second level, the Suburbs, loads over 0x1f10000; reading the service from it left StopAllSounds unanswered and `snd_GotReturns` waiting for good) |
| `sceSifCheckStatRpc(client)` | 0 (never busy) |
| `sceSifGetSreg(reg)`, `sceSifSetSreg(reg, value)` | the 32 software registers; Set returns the old value (boot #153, #154; match1p #17536116: GetSreg(8) 0) |
| `sceSifInitIopHeap()` | 0 |
| `sceSifAllocIopHeap(bytes)`, `sceSifFreeIopHeap(address)` | a block of the IOP heap (14.2); 0 (match1p: 11 of each, every block at 0x8b6c0) |
| `sceSifLoadModule(path, argc, argv)` | the module id: 1 for the first since the IOP's reboot, then 2... (boot #119-#127: TMB's nine modules, ids 1-9) |
| `sceSifRebootIop(image)`, `sceSifSyncIop()` | 1 (`v0` is already 1 in the records: #27, #28) |
| `sceSifSetDma(transfers, n)`, `sceSifDmaStat(id)` | the transfer's id (1, 2, ...), and the DMAC interrupt of channel 5 (SIF0) raised as it returns (boot #2157635, an OP inside the override's record at SetDma's return address); -1 (finished). TMB calls them only inside a port override (the movie's PCM, traced: boot #2157634 id 1, then 2...), so they are never checked against a record of their own; the native ids follow the traced ones |

The reference keeps the IOP side of the bound server (its `SifRpcServerData`) in EE RAM at 0x1f10000, so the game's
memory holds those words and the native call writes them exactly. A call with an end function (none in TMB) would need
its RPC callback handler queued: not implemented (open).

### 14.2 The IOP heap

`IopHeap` (`sif.h`): IOP memory from 0x7fe60 (the reference's first free byte after its modules: TMB's first bank lands
there, boot #210987) to 2 MB, blocks rounded up to 16 bytes, first fit by address, a freed block merged with its free
neighbours. `sceSifAllocIopHeap` and 989SND's allocations (banks, the movie's buffer, the VAG streams' buffers) share
it. Every placement of both logs (21 banks, 11 movie buffers, 11 `sceSifAllocIopHeap` blocks) is the native heap's.

### 14.3 Results

Native calls: boot 7,212 (`sceSifCallRpc` and `sceSifCheckStatRpc` 3,592 each, and the setup calls), match1p 245,108
(`sceSifCallRpc` and `sceSifCheckStatRpc` 122,519 each, `sceSifGetSreg` 18, `sceSifInitRpc` 14, ...); 0 divergences.

## 15. The 989SND sound driver (`snd`)

TMB's IOP-side sound driver is 989SND (989 Studios' sound system), RPC service 0x123456, client at 0x4fe970. The EE side
is the game's own code (the ELF's `snd_*` functions, 0x1baca0-0x1bcec4); the native driver (`bozo/libs/snd.h`,
`libs/snd.cpp`) is the IOP side behind `sceSifCallRpc`: it decodes each request, keeps the driver's state, writes the
reply, and tells a `SoundSink` what should be playing.

### 15.1 The client and its two kinds of call

From the EE client's code:

- **Batched commands** (`snd_SendIOPCommandNoWait`, 0x1bc188): a command is 32 bytes, seven argument words then the
  command id, appended to the current one of two buffers (`gSndCommandBuffer1/2`, 0x4871c0 and 0x488200: a count word,
  then up to 128 commands). Only the arguments the function has are copied, so the other words of a slot are what an
  older command left there (match1p #4083097: SoundIsStillPlaying's words 1-2 are an earlier command's volume and
  pan). `snd_SendCurrentBatch` (0x1bc448) sends the current buffer as **function 0x4a**: `ssize` = 32n + 4, the reply
  to that side's return buffer (0x48a240, 0x48a480), `rsize` = 4n + 8; then it switches buffers and zeroes the new
  one's count. TMB's batches hold one command each (every one of both logs).
- **Direct calls** (`snd_SendIOPCommandAndWait`, 0x1bbfc0): function = the command id, the arguments as the request
  (`ssize` = 4 x their count), the reply to 0x487040, 12 bytes.

`snd_PrepareReturnBuffer` clears the reply's first word and the one after its last result; `snd_GotReturns` waits until
both are -1. So a reply is **-1, one word per command, -1**.

### 15.2 The reply: results and the driver's last result

A command that returns something (15.3) puts its result in its slot. One that does not leaves **the driver's last
result**: that of the last command that had one, direct call or batch alike (boot #228345: SetMasterVolume's slot holds
0x80cc4, the last bank load's address; #1394803: ResetMovieSound's holds 0x856b0, the movie buffer; match1p #6588780:
0x81000002, the shell music's handle; #18034990: PlaySound_A's holds the handle the SetSoundParams before it returned).
Settled on every reply of both logs (match1p: 20,828 slots of commands with no result, and 464 more right after a call
answered from the log, which the driver follows: 15.7).

### 15.3 Commands

The command ids are the client's (each `snd_*` function passes its own). What TMB's runs use:

| Id | Client function | Arguments | Result | Native |
|---|---|---|---|---|
| 0x00 | (boot's first call, #156) | | none | yes |
| 0x03 | `snd_BankLoadByLoc` | sector, offset | the bank's IOP address (15.4) | yes |
| 0x06 | `snd_UnloadBank` | bank | none | yes: its blocks freed |
| 0x08 | `snd_ResolveBankXREFS` | | none | yes |
| 0x09 | `snd_SetMasterVolume` | group, volume | none | yes |
| 0x0a | `snd_GetMasterVolume` | group | the group's volume, 0x400 until set (match1p #17541046-#17541055: 0x370, 0x400, 0x334, 0x30b) | yes |
| 0x0b | `snd_SetPlaybackMode` | mode | none | yes |
| 0x0e, 0x0f | `snd_SetReverbType`, `snd_SetReverbDepth` | core, type / core, left, right | none | yes |
| 0x11 | `snd_PlaySoundVolPanPMPB` | bank, sound, volume, pan, pitch modifier, pitch bend | the sound's handle, 0 if it does not play | an empty sound (type 0): 0; else the handle is pinned (15.5) |
| 0x12 | `snd_PlaySoundVolPanPMPB_A` | the same | none (the play takes a handler all the same) | yes |
| 0x15 | `snd_StopSound` | handle | none | yes |
| 0x18 | `snd_StopAllSounds` | | none | yes |
| 0x19 | `snd_SoundIsStillPlaying` | handle | the handle while it plays, else 0 | 15.5 |
| 0x20 | `snd_SetSoundPitchModifier` | handle, modifier | none | yes |
| 0x21 | `snd_SetSoundParams` | handle, mask, volume, pan, pitch modifier, pitch bend | the handle while it plays, else 0 | 15.5 |
| 0x2a | `snd_InitVAGStreamingEx` | streams, buffer bytes | none (IOP memory: streams x bytes + 0x6200) | yes |
| 0x2c | `snd_PlayVAGStreamByLoc` | left sector, right sector, flags, ... | the stream's handle | pinned (15.5) |
| 0x2d | `snd_PauseVAGStream` | handle | none | yes |
| 0x35 | `snd_CloseVAGStreaming` | | none | yes: the streams stopped, their IOP block freed (stage-level20 #26677069: the next bank lands at 0x7fe60) |
| 0x3b | `snd_InitMovieSound` | (inside the movie override only) | the movie's buffer: 0x6010 bytes of IOP memory | followed (15.7) |
| 0x3c, 0x3d, 0x3e | `snd_CloseMovieSound` (frees the buffer), `snd_ResetMovieSound`, `snd_StartMovieSound` | | none | yes |
| 0x40 | `snd_GetTransStatus` | (inside the movie override only) | the movie sound's position | followed |

The results of the other commands that return one (0x05, 0x0c, 0x1a, 0x1c, 0x1d, 0x26, 0x28, 0x30-0x33, 0x36,
0x41-0x44, 0x47, 0x48) are not modelled (TMB never sends them outside an override): a pinned run would take them from
the log, a free run answers 0.

### 15.4 Banks

`snd_BankLoadByLoc` loads a bank file from the disc by sector (the IOP reads it; the native driver reads the disc image,
section 8). The file: u32 1, u32 the chunk count, then each chunk's offset and size. Chunk 0 is the `SBv2` header,
chunk 1 the samples (SPU ADPCM, to SPU memory), chunk 2 (when present) MIDI data with its own header (u32 2, 1, 0x10,
then the inner size). In the header: the bank id at 0x0c; the sound, program, tone and sample counts (int16) at 0x14;
the offsets of the sound, program and tone tables at 0x1c. A sound is 28 bytes (type at 0, program at 13, note at 14);
a program 8 (tone count at 0, tone table offset at 4); a tone 24 (priority, volume, centre note and fine at 0-3, pan at
4, key range at 6-7, ADSR at 10-13, flags at 14, the sample's offset at 16). A sample ends at its first ADPCM block
flagged 1; flagged 3 it loops.

**Where a bank lands in IOP memory** (settled on all 21 bank loads of match1p, boot's 3 among them): its header takes a
block of the header chunk's size; a bank with MIDI data takes the header's size + 4 and starts 4 bytes into it, and its
MIDI data (the inner size) the next block. So boot's banks are at 0x7fe60 (header 0x42c), 0x80290 (0xa24) and 0x80cc4
(0x5f8 + 4, its MIDI data's 0x43e4 at 0x812c0); the movie buffer then lands at 0x856b0 (boot #228390) and
`sceSifAllocIopHeap(0x28010)` at 0x8b6c0 (#228406). At match1p's level start (#17536095-#17536220) the three shell
banks are unloaded, `snd_InitVAGStreamingEx(6, 0xfa00)` takes 0x7fe60-0xe3c60, and the eighteen level banks follow
from 0xe3c64.

### 15.5 Handles, lifetimes and the reference's IOP time

A handle is `pool << 24 | slot << 16 | generation`: pool 0x80 for programs and sound effects, 0x81 for MIDI (type 4),
0x84 for VAG streams; the slot a handler's index; the generation counts that slot's uses (a slot's first is 2: match1p
#4082688, the shell music 0x81000002; #17541061, the first stream 0x84000002). The native model gives the lowest slot
whose sound is not playing.

Whether a sound still plays (0x19, 0x21) is decided:

- natively when it is certain: 0 for a handle the driver never gave (0, -1), one stopped (0x15, 0x18, 0x2f, 0x34) or
  seen ended before; the handle for a looping program (type 3: every sample of its tones loops), a MIDI sequence (type
  4), and a stream whose data (its VAGp header's size, 28 samples a 16-byte block, at its rate) lasts more than a second
  past now (TMB's streams last 462 s);
- by the reference's IOP time otherwise: a one-shot program or sound effect (types 1 and 2) lives as long as its voices
  play, which the reference's SPU and 989SND's tick decided. A pinned run takes that decision from the log's reply
  (`Pin::logWord`, set for `Function::logReply`: section 1), and once a sound is seen ended every later answer about it
  is native. The handle a play returns (0x11, 0x2c) names a free slot, and which slots are free depends on those same
  ends: it is pinned too (the native model's handle is compared with it and counted).

Pinned decisions in match1p: 776 play handles, 1,284 SoundIsStillPlaying and 1,232 SetSoundParams results, 2 stream
handles (3,294 of 122,519 replies; boot: 1, the shell music's handle, which the model gives). `bozo-run
--snd-log-replies off` makes the driver answer these from its models as well (15.11).

**The lifetime model** (the native estimate, used in free mode and compared in pinned runs): a one-shot sound (types 1
and 2, no tone looping) lives as long as its longest tone in the note's key range (tone bytes 6-7), that tone's sample
(ADPCM blocks to the end flag, 28 samples each) at 48 kHz shifted by the distance from the tone's centre note (byte 2)
to the sound's note and by the play's pitch bend (argument 5, over the tone's bend range: bytes 8 and 9, semitones
down and up), **plus 1.166 s** (989SND's 256-tick end counter: 15.12). The constant is settled on the only natural ends match1p shows: two handles of sound 3
of bank 0xe8310 (no bend; tones of 941 blocks at centre notes 78 and 79, note 59: 1.743 s of sample), started at EE
cycles 39,259,868,387 and 46,869,503,443, alive 2.9002 s later and ended 2.9169 s later: the constant lies in
(1.1575, 1.1742]. It is not the tones' ADSR release (0x5fcd: linear rate 13, 0.341 s on the SPU2's envelope; 0x5fce
in other banks would double it, which the shell's plays exclude), nor a pitch factor (sound 0x1d of bank 0xe5284 is
still alive at 0.934 s where a factor fitted on 0xe8310's would end it). With it, the estimate gives every liveness
answer of match1p's pinned one-shots but 6 (it was wrong on 570 with the sample alone).

**The 6 it misses** are ends earlier than the model's: handles of the same sound (0xe8310/3) that ended 0.017 to 1.6 s
after they started, each right after other sounds started (ended by match1p frames 6841, 9976, 10740, 11426, 12250
and 13083; the first dead answers at EE cycles 35,696,201,907, 51,106,909,307, 54,862,243,331, 58,234,405,851,
62,284,900,179 and 66,379,689,835): 989SND giving their voices to newer sounds (voice stealing by priority: the tones'
byte 0, 0x58 here against 0x63-0x64 for most others).

**Handler slots.** Settled: one pool per kind, the generation counts a slot's uses and starts at 2, and after
StopAllSounds the next plays take slots 0, 1, 2... in order (match1p #17536092, then the level's first plays: 0x8000000d,
0x80010007, 0x80020004, 0x80030003, 0x80040003); a stopped handle's slot is the next one given when no lower slot is
free (#18051947 stops 0x800a0002, the next play gets 0x800a0003). Asynchronous plays (0x12) take slots too: the
generations of the slots the game sees skip 619 uses between two of its handles, against 763 asynchronous plays. Which
slot a play gets then depends on which of those sounds (never queried, their handles never seen) have ended: the
lowest-free model with the lifetime model gives 372 of the 778 pinned handles (302 with the sample alone). Two things
are not settled (15.11): the shell's generations at the level start need one of its 21 plays not to take a slot (the
model gives slot 1 generation 6 where the reference gives 7: #17541429, the first divergence of `--snd-log-replies
off`), and the asynchronous plays' lifetimes in the first seconds of the level (frames 5612-5722) do not fit the
lifetime model for any pitch scale or constant, so either the allocation rule or their lifetimes differ from it.

Never time-dependent, and native: the empty sound (match1p: sound 7 of bank 0xe8694, 1,161 plays, every one 0).

### 15.6 Output: `SoundSink`

What should be playing goes to `Libraries::snd.sink` (null: dropped), like libmpeg's `MovieSoundSink`, for phase 4's
mixer: `bankLoaded` (the parsed bank: sounds, programs and tones with their sample offsets, loop flags and ADSR, the
sample data, the MIDI data), `bankUnloaded`, `soundStarted` (the handle, 0 for an asynchronous play; bank, sound, volume,
pan, pitch modifier, pitch bend, EE cycle), `soundParams` (SetSoundParams' mask and words), `soundPitchModifier`,
`soundStopped`, `masterVolume`, `reverb`, `streamStarted` (the two VAGp files' sectors, flags, volume), `streamPaused`,
`streamStopped`. `bozo-run` counts them: match1p 21 banks, 1,539 sounds, 85,285 parameter changes, 768 stops, 5
streams. No audio device in this batch.

### 15.7 Calls answered from the log

Port overrides call 989SND from game code inside them: the movie's sound (`audioDecCreate` 0x3b, `audioDecReset` 0x3d,
`audioDecSendToIOP` 0x40), the menus (`uiCarSel`, `uiLvlSel`, `uiAiSel`) and `TheGame::Update` (batches: plays, stops,
the music stream's 0x2c, 0x19 and 0x21). Their RPC calls are traced (client, function, mode, send buffer) and their
memory is the log's, so the driver follows them after the call: for each traced call, the request left in its send
buffer and the reply in its receive buffer (which goes with which send buffer it learned from the calls it saw); every
result taken from the reply; the state updated (handles given, sounds ended or stopped, the movie buffer's allocation
checked against the native heap, the last result). A call whose receive buffer a later call of the same override
reused lost its request (10 in match1p, each `TheGame::Update`'s first of three batches); nothing such a call did was
needed later. `sceSifCallRpc` answered from the log (`--native` without `sif`) is followed the same way.

### 15.8 Timing, and what free mode needs

In both logs every reply lands inside its `sceSifCallRpc` (no IOP change outside a call: `calllog.md` section 7), so the
RPC never waits. What the reference's IOP time decided, which free mode needs natively:

- **sound lifetimes**: the lifetime model (15.5) gives every natural end; voice stealing is not modelled (6 answers);
- **handler slots**, which follow from every sound's end, the asynchronous plays' included (the model gives 48% of
  match1p's play handles);
- **voice allocation**: a non-empty sound that could not play (0x11 returning 0: match1p #21696667, #32014550,
  #51184025, #51299223): 989SND's voice priorities and stealing, not modelled;
- **the movie sound's position** (0x40) and the streams' ends.

A free run answers these from the models (a different reply then ends a log-checked run, as the CD's waits do: section
8.4). Boot runs in free mode with every library but the CD native (`--native libc,libm,gs,pad,mc,vu0,io,mpeg,sif,snd`):
3,592 replies native, none pinned, every frame matching.

### 15.9 Results

| Run | Replies native | Pinned | Followed | Divergences |
|---|---|---|---|---|
| boot | 3,591 (SetMasterVolume 2,191, SoundIsStillPlaying 1,385, bank loads 3, ...) | 1 (the shell music's handle) | 937 commands | 0 |
| match1p | 119,225 (SetSoundParams 83,641, SetSoundPitchModifier 17,615, SoundIsStillPlaying 13,106, SetMasterVolume 2,195, plays 1,913, stops 687, bank loads 21, ...) | 3,294 | 2,481 commands (10 calls lost) | 0 |

### 15.10 Open

- Voice allocation and stealing, the handler slots that follow from them, and the four failed plays (15.5, 15.11):
  match1p's 3,294 pinned replies.
- The results of the commands TMB does not send (15.3); an RPC end function (14.1).
- 0x2a's IOP memory (streams x bytes + 0x6200) and the movie buffer's 0x6010 rest on TMB's single sizes.

### 15.11 Without the log's replies (`--snd-log-replies off`)

Every reply is the driver's own: boot matches every frame and record (3,592 replies, the shell music's handle from the
handle model). match1p does not: its first divergence is the level's second play (#17541429, slot 1 generation 6 for
the reference's 7, 15.5), and since a wrong handle leaves the driver's state apart from the game's, 82,841 replies
differ (the log's effect replacing each, every frame still matches). What would close it, from the records:

- the voice model: 989SND's voice count and its stealing rule (by tone priority, then age), the voices the MIDI music
  and the VAG streams hold, so that the 6 early ends and the 4 failed plays come out;
- the asynchronous plays' lifetimes and the allocation rule tested on them (15.5), which the generations of the game's
  handles constrain (619 hidden uses);
- the shell's 21 plays against its slot generations (one play without a slot).

The driver's own rules are in its module on the disc (15.12); what stays outside the clean room is the fork's mixer
under it (15.13).

### 15.12 989SND's own code (`989SND.IRX`)

The driver ships on the disc as `/MOD/989SND.IRX` ("989snd (c)2000, 2001 Sony Computer Entertainment America", built with
sce library rev 2.1.3), an IOP relocatable (MIPS R3000) that imports `libsd` (`/MOD/LIBSD.IRX`), `cdvdman`, `sysmem`,
`intrman`, `sifcmd`, `sifman`, `thbase`, `thsemap`, `timrman`. `tools/irx/irx_disasm.py` lists it (sections, import and
export tables, code, data references); addresses below are offsets into the module's `.text`/`.data`. What it settles:

| Rule | Where (989SND.IRX) | Records it explains |
|---|---|---|
| The RPC server 0x123456 is registered with function 0xf20 and buffer 0x15980 (a second server, 0x123457, "Loading RPC", with 0x1030) | 0xfa0, 0x10f0 | the bind (14.1) |
| 0xf20 calls the command table at 0x1490c by function number; the reply is a static buffer (0x15760): -1, then each command's result written through a pointer (0x14908) that advances per command, then -1. A command that writes no result leaves what the buffer held: the "driver's last result" | 0xf20; batch 0xe6c | 15.2: every reply of both logs |
| PlaySound (0x11, 0x12) goes to 0x5730: sound type 1-3 to 0x7460 (programs), 4 to 0x87a0 (MIDI), 5 to 0x9b10; type 0 (or no data) answers 0; an `SBlk` bank to 0x11fe0 | 0x5730 | the empty sound (15.5) |
| A type 2 sound stops the instances of the same sound still playing before it plays (the handler list at 0x14ac0) | 0x6fd0 | the asynchronous plays' lifetimes (15.5) |
| A sound handler is the lowest free of 48 (56 bytes each at 0x197f0; free when its sound word is 0); its handle word is slot << 16 \| (the slot's previous low half + 1): the generation counts the slot's allocations (the first is 2); MIDI handlers are a second pool (0x1a3d0, prefix 0x01000000) | 0x64d8, 0x65ac | 15.5's handle format and slots |
| The tick: an IOP hardware timer at 1/240 s (USec2SysClock(1 s) / 240) whose handler wakes the tick thread, which runs 0x54b8; a command locks it (0x1630) and the ticks missed meanwhile run at the unlock (0x16a0) | 0x4a04, 0x4c78, 0x4cd4, 0x54b8 | |
| KeyOn and KeyOff are sent once a tick, after the voice loop (0x1ed0: KOFF, 0x1db0: KON through `sceSdProcBatch`): a sound's voices start at the tick after its command | 0x56bc | |
| **A voice's end** (0x54b8): for each active voice the tick reads ENVX (`sceSdGetParam(0x500 \| voice << 1 \| core)`). Not 0: nothing. 0 and keyed off: the voice is freed (0x1fc0, its owner told with reason 2). 0 and still keyed on: the voice's byte counter (0x183c0) is decremented and, when it reaches 0, the sound ends (0x214c: its owner told with reason 3, KeyOff queued). The counter's reset ("4 when the last ENVX was not 0") never fires, because the last ENVX (0x18360) is only stored on the zero path: the counter runs from 0 to 0 through 255, so **a natural end comes 256 silent ticks after the mixer reports the voice silent** | 0x54b8, 0x214c, 0x1fc0 | the +1.166 s of 15.5 |
| A voice's pitch at KeyOn: `sceSdNote2Pitch(centre note, centre fine, note, fine)` from the tone, the note's pitch including the pitch modifier and bend (`989snd.md` Part A 3.1); a non-negative centre note is converted to 48 kHz (x 44100 / 48000), a negative one is negated and used as is; ADSR1/ADSR2 the tone's bytes 10-13; the start address the tone's sample | 0x1a00-0x1c88, 0x27d0 | |
| `sceSdNote2Pitch` (LIBSD.IRX 0x33a0): fine = note's fine + centre fine (the semitones it carries moved to the note), the semitone from a 12-entry table (LIBSD 0x4680), the fine from a 128-entry one (0x4698), their product >> 16, then shifted right by the octave with rounding | LIBSD 0x33a0 | |
| InitVAGStreamingEx creates four streaming threads (stacks of 0x800) and allocates the streams' buffers and state (0x61d00 for TMB's 6 x 0xfa00); with the reference's thread stacks taken from the IOP heap (0x840 each) that is the 0x63e00 of 15.4 | 0x7fc, 0xb4ac-0xb590 | the first level bank at 0xe3c64 |
| Bank transfers to sound memory take a free DMA channel (0xe590), their end handler (0x4714) frees it; while streams run, disc reads go through the streaming thread (0xd55c) | | |

**The +1.166 s, explained.** Sound 3 of bank 0xe8310 (941 blocks): 989SND gives its two voices pitches 1288 and 1216
(of 4096), so the longer one plays 1.849 s; the mixer reports it silent at the next tick after its end; the 256th silent
tick ends the sound: start + (KeyOn at the next tick) + 1.849 s + 255 ticks = 2.912-2.920 s after the play, against the
log's 2.9002-2.9169 s (both natural ends). The constant in the native estimate (15.5) is this counter, not a release.

### 15.13 What is still open: the mixer under libsd

989SND decides from ENVX, the envelope the mixer reports. In the reference the mixer is the fork's native libsd
(TMB_Recomp's `docs/RECOMP_NOTES.md`: "989SND's calls to libsd act on the fork's native mixer"; its voice search is
native code there too), outside the clean room. What the records need from it, and do not settle:

- whether ENVX is already non-zero at the first tick after a KeyOn (a silent first tick starts a voice's counter);
- what ENVX does at a sample's end without repeat (silent at once, or a release first) and how long a release after a
  KeyOff lasts (the voice stays taken until ENVX is 0: it decides which voices are free, and so voice stealing);
- the mixer's granularity, and where the IOP timer's ticks fall against the EE's calls (the IOP runs ahead of EE time
  after a vblank: `calllog.md` section 7).

A replay of 989SND.IRX's own code against the logs' command streams (an analysis aid kept out of the repository: the
module's code on a MIPS interpreter, with libsd and the IOP answered by a model of the mixer) gives every reply of boot
(but GetTransStatus 0x40, inside the movie override) and every reply of match1p's shell; from the level start it
depends on the mixer model: the best of the variants tried (silent at the sample's end, the SPU2 envelope's release, a
silent first tick) gives 15 of the 26 play handles to frame 5770; the first difference is the shell's slot generations
(#17541429: one asynchronous play's slot). With the mixer's behaviour known, 989SND's rules above give every reply; the
native driver then takes them (handler and voice pools, the tick, the counter, stealing in 989SND's order). The
mixer's behaviour is now 15.14 (the mixer itself moved here), the IOP clock under it `iop-timing.md`.

### 15.13a The replay with the moved mixer and the reference's IOP time (2026-11-04)

The analysis replay of 989SND.IRX (kept out of the repository: it runs IOP code) now runs over `bozo_audio`'s own Mixer
and Libsd (clocked) and an IOP clock per `iop-timing.md`: P = (EE cycle - 4,927,984) / 8 + G, ticks due every 153,600
IOP cycles and served at max(due, the IOP clock), the clock advancing by the replayed driver's instruction count inside
each RPC call and tick (so a tick due during a call fires after it, its lateness carried forward). With G = 0 and one
pinned point, the tick phase at the level start (`snd_InitVAGStreamingEx`, match1p #17536113: any due tick 0.5 ms or
more after it), it gives every 989SND reply of boot but the movie positions, and every reply of match1p through frame
8662, and 791 of the 932 movie positions (0x40) with no per-movie pin. With the tick phase at the level start not pinned
(the first due tick at the call), one asynchronous sound of bank 0xe5284 ends a tick before the reference's and the
first play handle differs at frame 6597: the phase is the one point the records must give.

That difference (frame 8663, the stream handle 0x84020002 ended in the reference) is cleared by running 989SND's
streaming threads (15.13b).

### 15.13b The replay with 989SND's threads (2026-11-04, second session)

The replay now runs 989SND's IOP threads on a scheduler per `iop-timing.md` section 3: the four streaming threads
(0xe3ec priority 60, 0xe7d8 61, 0xd3a0 62, 0xdcdc 63) run between the RPC calls and the ticks, highest priority first,
in slices of at most 256 instructions up to the next payment's target P, with the services (due ticks, queued
interrupt calls, CD completions) between slices; semaphores, SleepThread/WakeupThread and cdvdman's asynchronous
`sceCdRead` with its callback (`sceCdCallback`) are modelled; the tick and the RPC server stay inline (they never block
in TMB's runs, but for the bank loads' disc reads while streaming is initialised, which complete at once). 989SND's
behaviour, read from its code for the native driver, is `989snd.md` (Part A programs, voices, the tick; Part B MIDI;
Part C streams).

**Results** (match1p's 125,000 commands, about 6 minutes; boot unchanged: every reply but 51 movie positions):

- Every reply through frame 10826: the three short streams (0x84020002-4, `DEA_AI_M`) end at the reference's frame
  (1007 ticks after their KeyOn: `989snd.md` Part C section 6), and so do the program sounds whose voices the streams
  took (`allocVoice(group, 127)` steals a program voice). Without the threads the first difference is frame 8999
  (a play handle), so the streams' voices matter to the programs' handler slots.
- First difference left: frame 10827 (trace line 82837), PlaySound of bank 0xe6500 sound 3: the reference gives slot
  0x11 (0x80110009), the replay slot 0x16 (0x8016000c). Slot 0x11 holds 0x80110008 (bank 0xe5284 sound 0xf, played at
  frame 10696, never queried): two tones (centre notes 76 and 77, one sample, ADSR 0x80ff/0x5fcf), on voices 35 and
  40 in the replay, which take the first free voices ascending. Voice 35's end counter was 253 at the play (two silent
  ticks under the MIDI sound 0x81010004 before a steal at tick 43536), so its sound ends 2.37 s after the play in the
  replay; the reference had it ended by 2.18 s. The per-voice end counter is never reset (Part A 4.2), so a voice's
  counter carries all its earlier silent ticks: whatever differs is upstream of tick 44440 and invisible in the replies
  so far. 323 replies differ after it (0x11 171, 0x21 152), every 0x19 matches.
- What it does not depend on (each run to frame 10830): the instruction cost (each IOP instruction counted 0.5, 1.0 or
  1.1 cycles: the same first difference; 0 cycles diverges at frame 6868, so a native driver needs a cost model, but a
  rough one), the level-start phase (1.5 or 3 ms), G (0 or 550), the CD callback inside `sceCdRead` or at the next
  service point. The CD's latency does matter: 20,000 IOP cycles (0.5 ms) per read moves the first short stream's end
  one frame late (frame 10304), 200,000 that of 8663; the reference's reads complete within a fraction of that.
- Movie positions (0x40, followed: they never need a native answer): with the global G = 550 every position of the
  first movie fits (with G = 0, 141 of them, from frame 646 on, are one sample ahead); no single extra
  G fits the second movie in the replay (the positions drift by a sample within it), which the reference's records do
  fit with one offset (`iop-timing.md` section 8): the replay's clock gains time during the second movie that the
  reference's did not.

**Handover (state at 2026-11-04 end of the second session).**

- *The oracle* (scratch, never committed): `D:\BozoScratch\irx\`. The first session's files (`iop.py`, `drv.py`,
  `run.py`, `t*.py`) are unchanged; the threaded replay is in copies:
  - `iop2.py`: the interpreter with a thread level (pause after n instructions, block in an import, resume at
    (pc, npc)).
  - `drv2.py`: `drv.py` on `iop2`, with an instruction-cost factor.
  - `sched.py`: `SDriver`, the threads, semaphores, sleep/wakeup, cdvdman (read, sync, callback, break), the event
    queue, `advance(P)` (ticks, events, slices) and `tick()`.
  - `run2.py`: the replay. Options as `run.py`'s plus `cost=` (cycles per instruction), `cd_delay=` (IOP cycles per
    read; -1 completes inside `sceCdRead`), `G2=`/`g2_line=` (an extra G from a trace line), `run_threads=0`.
    Run: `cd D:\BozoScratch\irx; python run2.py m1.snd phase2=0.0015 verbose=30`.
  - `t10.py <line> <slot hex>`: the handler slot and all 48 voices (state, owner, priority, age, end counter) before
    a trace line. `t11.py <from line> <to line> <voice>`: one voice's owner/state/counter changes per tick.
    `t12.py <bank hex> <sound hex>`: a sound's record and tones.
  - `cov/runcov.py`: the replay with instruction coverage (match1p runs 9,940 distinct instructions of 989SND, boot
    4,457).
- *Pinned points:* unchanged (G = 0, level-start phase 1.5 ms, IOP reset at EE cycle 4,927,984); CD reads complete
  at once.
- *Next:*
  1. Frame 10827: find what differs before tick 44440. Candidates not yet tested: the order in which a missed tick
     runs inside a command (Part A 1.5, 6.3: a tick due inside PlaySound's lock runs at its unlock, inside the
     command, in the reference only if the tick thread can preempt the RPC thread, which `iop-timing.md` section 4
     excludes), the MIDI sound 0x81010004's notes (Part B: its voices' first silent ticks), the stream threads' slice
     boundaries against the ticks, and `sceSdNote2Pitch` (the replay uses LIBSD.IRX's tables; the reference may use
     another implementation).
  2. The native C++ driver per `989snd.md` (programs, voices, the tick and its counter, MIDI, streams) on
     `bozo::audio::Mixer`/`Libsd`, with a cost model for the IOP clock (the replay's instruction counts per call and
     tick as the target), checked against the oracle command by command, behind a per-command log-reply switch.
  3. The IOP clock in `bozo-run` with the pinned points above; then `--snd-log-replies off` as the default.
  (Done: 15.13c.)

### 15.13c The native 989SND model (`bozo/libs/snd989.h`, 2026-11-04)

989SND's behaviour (`989snd.md`) now runs natively on `bozo_audio`'s Mixer and Libsd: `snd989::Engine`, fed by
`SndDriver` with every command in order (direct calls, batches, the commands it follows from calls answered from the
log) at the EE cycle it was made. It answers the replies the reference's IOP time decided (PlaySound's handle 0x11,
SoundIsStillPlaying 0x19, SetSoundParams 0x21, PlayVAGStreamByLoc's handle 0x2c); `SndDriver` keeps the SIF side,
the IOP heap, bank parsing and the replies that never depended on time. What it implements:

- **Voices and handlers** (Part A): the 48 voices and their active list, `allocVoice` (first free ascending, else the
  lowest priority, then oldest, then lowest index; a stream voice is priority 127), KeyOn setup and the pending
  KeyOn/KeyOff masks flushed once a tick, the per-voice end counter (256 silent ticks, never reset), the program,
  MIDI and stream handler pools with 989SND's handle generations, the program callback freeing a handler with its
  last voice, type 2's stop of earlier instances, StopSound/StopAllSounds, pitch (`sceSdNote2Pitch` from
  2^(n/12) and 2^(f/1536), floored, and the 44.1 kHz conversion), volume, pan and auto-pan.
- **MIDI** (Part B): MID and MMID blocks, the sequencer with 989SND's time base (4,170 us a tick), notes to voices,
  note-off, sustain, pitch bend, bank select, loops and the stop at the end of track.
- **Streams** (Part C): the buffers in sound memory from 989SND's own allocator (Part A's free-list tree, 64-byte
  units; the reverb work areas below a 128 KB boundary), the disc pipeline as the threads leave it with the
  reference's instant reads (header, chunks, `markBlocks`, the transfers and re-reads in their order), KeyOn setup when
  the threads get there, the stream tick (NAX halves, refills, the end test on ENVX in the end phase), pause and
  volume. Bank samples go to sound memory at their allocated address and the tones' sample offsets become addresses.
- **The IOP clock** (`iop-timing.md`): P = (EE cycle - 4,927,984) / 8 + G with G = 0; the first tick due 153,600 IOP
  cycles after the first command; at `snd_InitVAGStreamingEx` (0x2a) the next tick due 55,296 cycles (1.5 ms) after
  it; ticks served at max(due, the clock), the next due 153,600 after the service, so lateness carries; a command runs
  at max(P, the clock) and moves the clock by its cost; the stream threads' KeyOn setups come 1,280 (two-chunk mono)
  or 2,470 (long stereo) IOP cycles after the 0x2c command. Every libsd call goes through `Libsd::call`, so the
  clocked mixer is brought up to the engine's time first (calling the per-entry exports directly leaves the mixer
  behind: that alone diverged at frame 6868).
- **The cost model**: each step of the tick and of the commands (a voice read, a callback, a list step, a MIDI event,
  a note-on's tone scan and voice, the flushes...) advances the clock by 989SND's instruction count for it, fitted on
  the analysis replay's libsd call times over the shell (22,536 ticks: 95% of 241,428 call times exact, the rest
  within a few cycles); a command adds, on top of its steps, a constant per command id (the replay's mean instruction count for it).
- **Lost calls** (15.7): the 10 batches of match1p whose request a later call overwrote are taken as StopSound of the
  first handle a play gave in the 5 M EE cycles before (pinned from the log: each is `TheGame::Update` stopping the
  sound it replays every frame, bank 0xe3c64 sound 0x48, whose next play takes the freed slot; without it the first
  difference is frame 13568).

`--snd-log-replies` takes `off` (the default now: the model answers), `on`, or the command ids still taken from the
log (`11,19,21,2c`); a pinned run reports the model against the log per command either way (`989snd model against
the log (agree/differ)`).

| Run | Replies native | From the log | Model against the log | Result | Time |
|---|---|---|---|---|---|
| boot, `--snd-log-replies off` | 3,592 | 0 | 0x11 1/0, 0x19 1,385/0 | MATCH | 20 s |
| match1p, `--snd-log-replies off` | 122,519 (0x11 1,937, 0x19 14,390, 0x21 84,873, 0x2c 2, ...) | 0 | 0x11 776/0, 0x19 14,390/0, 0x21 84,873/0, 0x2c 2/0 | MATCH | 4 min 23 s |

(Run times on this machine, where the same match1p run of 42a1d7c takes 4 min 26 s.) The analysis replay's first
difference at frame 10827 (15.13b) does not occur in the model: the replay itself, not the reference, differs there.

Open: the movie positions (0x40) stay followed from the log; free mode uses the same model with these pinned points
(G, the level-start phase) and needs them from elsewhere; the cost model's last few-cycle errors (5% of the call
times) have not mattered in either log. (Free mode: 15.13d.)

### 15.13d The engine's output and free mode (phase 4, milestone M3, 2026-11-04)

What 989SND does that only the output hears (no reply depends on it) was left out of 15.13c; the engine now does it, read
from `989SND.IRX`, so that its mixer plays what the reference's did. A libsd call log of the engine
(`BOZO_SND989_SDLOG=<file>`: the IOP clock, the export, its arguments and result) equals the analysis replay's (15.13b,
989SND.IRX's own code on the same mixer) call for call over boot, every call within a sample of the replay's time.

- **StartSoundSystem** (command 0x00, 0x4828): sceSdInit; the mixes (MMIX core 0 0xf00: the voices only, the sound
  data input off; core 1 0xf0c: the voices and core 0's output); core 0's master volume 0x3fff; the effect volumes 0;
  core 1's external input volume (AVOL) 0x7fff; the effect off; the volume groups 0, 1, 2 and 7 at 1024.
- **SetMasterVolume** (0x09, 0x7cf0): the volume clamped to 0..1024; group 7 is core 1's master volume (MVOL =
  (v << 14) - v, / 1024 truncated); another group's playing voices (state 1 or 4) get their VOLL / VOLR again through
  `groupScale` (0x819c), one batch a voice.
- **SetReverbType** (0x0e, 0x83b0) per core: the effect off, EVOL 0, the old area freed and the new one allocated
  (15.13c's model), EEA, sceSdSetEffectAttr (mode, depth 0, delay 0, feedback 0), then (type not 0) the area cleared
  through a DMA channel (the first free: core 0 gets 0, core 1 1) and a countdown of 10 ticks; at its end (the tick's
  last step, 0x86ac, after the KeyOn flush) EVOL takes the depth set meanwhile and the effect comes on.
  **SetReverbDepth** (0x0f, 0x85b0): the depth kept; EVOL set now unless that countdown runs.
- **The movie's sound** (0x3b-0x40, 0x104c0-0x109b0). InitMovieSound(bytes, volume 0..1024, pan, group): the buffer
  (the driver's IOP heap block, 15.3), a DMA channel (the first free: 0), the volume as (v x 127) >> 10, 1 KB of the
  buffer played once through sceSdBlockTrans. StartMovieSound(address, bytes, start): the buffer as the channel's
  core's sound data input, looping (mode 0x13), BVOL from `voiceVolPan(127, volume, pan, 127, 0, 127, 0)` then
  `groupScale` << 1, capped at 0x7fff (TMB: 0x7efa), and MMIX | 0xc0 (the input to the output, not the reverb).
  Reset (0x3d) and Close (0x3c): BVOL 0, MMIX & 0xff3f, the input stopped (Close also frees the channel). Pause (0x3f):
  BVOL 0, the input stopped, its position & 0xffffff. GetTransStatus (0x40): sceSdBlockTransStatus. The game's PCM
  reaches the buffer through `sceSifSetDma` (libsd sees IOP RAM: the engine keeps 2 MB of it, the uploads' staging
  area above), the mixer brought up to the transfer's EE cycle first. A pinned run still takes 0x40's reply from the
  log (the movie code paces itself on it); the engine's agrees on 926 of boot's 932 (the 6 others one sample apart).
  libmpeg's `MovieSoundSink` keeps counting the demuxed PCM (`bozo-run`'s summary) but is not a second source for the
  mixer: the game's own path (pcmCallback, audioDecSendToIOP, 989SND's input) is what the reference played, paced by
  the game.

**The sound records** (`harness.md` section 6). The engine's clock and mixer are brought up to each frame's end on the
EE clock (`Engine::advanceTo`: ticks and stream KeyOns up to then, the mixer to the IOP clock; nothing a later command
sees changes), the samples made since the last frame are the record's `sound` and `soundSamples`. Long gaps between the
engine's libsd calls are mixed in steps (the reference's mixer is brought up at every IOP payment and never skips;
15.14's one-call limit would drop samples and the reverb's every-other-sample phase with them). In a pinned run the
frame's samples end 192 EE cycles (24 IOP cycles) before the frame's end: the reference's last payment, fitted on the
records' sample counts (every frame of boot and match1p's shell but the movie start's pair, frames 608-609). The first
frame has none (the reference's output starts with frame 1).

| Run (`--pinned`, every library native) | Sound records equal to the reference's | Sample counts | Result |
|---|---|---|---|
| boot | 1,659 of 3,000: every silent frame (632), both movies (every frame but two where a frame boundary falls a sample apart), 47 of the title music's | 2,998 of 3,000 | MATCH |
| match1p (`ch1p_frames.txt`) | 3,926 of 16,000 (2,900 silent; the movies; the same title frames) | 14,673 of 16,000 (from the level start, frame 5591, the IOP clock gained unpaid time: the grid moved by a fraction of a sample) | MATCH |

What differs is the music and effects from the title on (boot frame 1614: the title music starts with a MIDI sound and
SetReverbDepth). The libsd calls are 989SND's (the call log above), so what is left is time: which 768-cycle sample a
KeyOn, a volume or the reverb's enable falls on, set by the phase of the 240 Hz tick against the EE (S0 and the
unpaid IOP time G of `iop-timing.md`, which the replies settle only to within a tick) and by the parity of the reverb's
every-other-sample step since its enable. The tick's phase is now a pinned point of its own (`Timing::firstTickPhase`,
27,776 IOP cycles: the first tick due 153,600 + 27,776 after 989SND's start), found by a search over the phase in
768-cycle then 8-cycle steps against boot's title frames (none of 86 frames matches with the phase 0; 27,752-27,792 give
47 of them, 28,416 only 8): it changes no reply (boot and match1p MATCH with it). What still differs is single calls a
sample apart: the cost model's few-cycle errors (15.13c) now decide which side of a 768-cycle boundary a KeyOff or a
volume falls (boot frames 1625-1642 differ, 1643-1675 equal again, from 1690 on differ), and the level's G (from match1p
frame 5591 the records' sample counts follow another grid; neither the jump nor its effect on the ticks is modelled).
Closing it needs 989SND's instruction counts per call, not a mean per step: the analysis replay (989SND's own code)
gives them, and is the tool to fit a finer cost model with these records as the check.

**Free mode** (`FreeHost`, `SndDriver::freeTiming`). The four points a pinned run takes from the reference's logs:

| Pinned (`Timing{}`) | Free mode (`Timing::freeRun`) | Why |
|---|---|---|
| G = 0 against the IOP reset at EE cycle 4,927,984 (boot #27) | the reset at the EE cycle the game calls `sceSifRebootIop` (`SndDriver::iopReset`), G = 0 | no IOP runs ahead of the EE: P = (EE - reset) / 8 is the IOP clock |
| the level start: the next tick 55,296 IOP cycles (1.5 ms) after 0x2a | the tick keeps its grid (`streamInitRephase` off) | the reference's phase came from its IOP's inline work (the bank loads' disc waits); nothing natively is inline |
| a stream's KeyOn 1,280 (mono) / 2,470 (stereo) IOP cycles after its 0x2c | at the command's end (0, 0) | the streaming threads' run time; the disc reads complete at once |
| a lost call (a request overwritten before a call answered from the log ended) taken as StopSound of the last play | not needed | free mode answers every call natively, so no request is lost |

The free host brings the engine up at each frame's end and hands the frame's samples to `FreeHost::onAudio` (the
window's device, `tmb-headless --wav`); in real time the EE clock follows the host's (the FramePacer), so the samples
come at the field's rate, and `bozo::app::AudioDevice` absorbs the drift (`bozo/app/audio_device.h`).

### 15.13e Free mode in real time: the IOP on the host's clock (2026-11-04)

Between frames the EE clock does not follow the host's, and the sound, played as it is made, heard it. At a match
start (`tmb --audio-stats`, ch1p into Junkyard) the load's disc reads are charged a PS2 drive's time: the EE gained
about 2.8 s in 0.24 s of the PC's. The engine made those 2.8 s at once (the device kept the last 0.34 s and dropped the
rest), then TMB's frame pacer held the EE back to pay that debt (EE/PC 0.48 for the next 5 s), so the sound was made at
half speed: 270 underruns, about 4 s of silence in bursts. A stall on the PC (no frame ending, a frame charged at most
0.1 s) left the output empty the same way.

`SndDriver::realTimeClock` (the free host in real time with sound) holds the IOP's clock to the host's:
`snd989::Engine::lead` is added to P = (EE - reset) / 8 + G. Before the engine takes a command (`SndDriver::call`), a
write (`iopWrite`) or is brought up (`advanceAudio`), `syncClock` compares P with a line on the host's steady clock
anchored at the first call (36.864 MHz of host time per IOP second):

- more than 4 ms behind the line: the lead brings P up to 4 ms behind it;
- more than 25 ms ahead: P is held at 25 ms ahead (never below the engine's clock: P only moves forward); the band
  covers a frame's guest code running ahead of the host's clock within it, so commands keep their EE order and spacing;
- more than 32 ms behind (the output queue's target: the device has already played silence, nothing was there to bring
  it up): the line moves to P + 32 ms, so the sound goes on from where it stopped instead of skipping.

A stall's sound: when no frame has ended for 25 ms, a watcher thread of the free host raises a flag (it touches nothing
else); the watchdog's next look on the game's thread, between two out-calls (never inside a library's handler, so
never inside a 989SND command or a SIF call), brings the engine up to the line and hands the samples to `onAudio`. The
next frame end takes what the engine made since (its clock only moves forward), so nothing is made twice.
`BOZO_SND_CLOCK_LOG=1`: each move of the lead over 50 ms on stderr.

Pinned, checked and headless runs (not real time) never set it: their P is the EE's as before, their records unchanged.
Same run as above, after: 9 underruns (54 ms) over 6,700 frames against 625 (5.9 s), 3 of them in the match start's
window, none dropped there; frames dropped 1,899 (all at boot) against 12,906.

### 15.14 The mixer under libsd (`bozo/audio/`)

The reference's mixer is the team's own code on the fork, so it moved here (PLAN.md section 7) instead of being
reconstructed: `runtime/audio/` (`bozo_audio`: `bozo/audio/mixer.h`, the mixer; `bozo/audio/libsd.h`, libsd's exports on
it, through a `LibsdHost` giving the IOP clock, its RAM and interrupt calls). `bozo-audio-tests` checks the rules below
and a fixed scenario (`tests/audio/mixer_scenario.h`) whose output samples and ENVX, ENDX and NAX answers are the fork
mixer's, digest for digest, free-running and clocked. It is not wired into `bozo-run` yet. What 989SND sees of it, which
15.13 left open; the IOP clock it runs on is `iop-timing.md`.

**Clock and granularity.**

- One output sample per **768 IOP cycles** (48 kHz at 36.864 MHz; 6,144 EE cycles). Sample n is the one made when the
  IOP clock reaches 768 n: the grid is the IOP clock's own, from its last reset.
- In the reference's deterministic mode the mixer is brought up to sample floor(t / 768) at the start of every libsd
  call, before the call acts or answers (t: the IOP clock at the call), and at the end of each IOP run
  (`iop-timing.md` section 3). A read at clock t sees the state after sample floor(t / 768); a setting or KeyOn made at
  t acts from sample floor(t / 768) + 1. Nothing else moves the mixer, so what a driver reads depends only on IOP time.
- One bring-up mixes at most 16,384 samples (0.34 s); a longer gap advances the sample count without mixing the rest
  (voices do not move for them). The first bring-up after the clock starts, or after it goes backwards (an IOP reset),
  only sets the sample count.
- Per sample, voice by voice: both volume sweeps step (every voice, playing or not); an off voice stops there; else the
  output is interpolated at the voice's position, the envelope steps once, the output is scaled by the new level, the
  position advances by the pitch (capped at 0x3FFF; 0x1000 is one source sample per output sample), and each 28-sample
  block crossed is left (its end flags acted on, below) and the next decoded. The reverb steps every other sample.

**The envelope step** (rate r, 0-127; shift s = r >> 2; step 7 - (r & 3), or -(8 - (r & 3)) decreasing): every
2^max(0, s - 11) samples the level moves by step << max(0, 11 - s), clamped to 0..0x7FFF (a counter gains
0x8000 >> (s - 11) a sample and the level moves when it reaches 0x8000, the counter then back to 0; s >= 27: never).
Exponential and decreasing: the move is (step x level) >> 15, rounded down (so at least -1). Exponential and increasing,
from level 0x6000: s < 10 a quarter of the move, s = 10 half the move at half the counter's speed, s >= 11 a quarter
of the counter's speed. The phases: **attack** (ADSR1 bits 8-14, exponential bit 15) until the level reaches 0x7FFF;
**decay** (rate (ADSR1 bits 4-7) x 4, exponential, decreasing) until the level is at most min(((ADSR1 & 0xF) + 1) x
0x800, 0x7FFF); **sustain** (ADSR2 bits 6-12, decreasing bit 14, exponential bit 15) until KeyOff; **release** (rate
(ADSR2 & 0x1F) x 4, decreasing, exponential bit 5) until the level is 0, then the voice is off. Each phase starts with
the counter at 0. ENVX is the level.

**Where psx-spx is ambiguous** (its "SPU ADSR" gives the step, the wait of 2^max(0, s - 11) samples and the
exponential decrease, and for exponential increase above 0x6000 only "cycles x 4"; it gives no upper limit to the
shift). The reference mixer's behaviour, settled black-box (every rate, both directions, both modes, 200,000 samples
each, its output compared with the documented formula's): the wait is a counter gaining 0x8000 >> (s - 11) a sample
(0x8000 below s = 11), the level moving when it reaches 0x8000; **s >= 27 never moves** (psx-spx's formula would move
every 2^(s - 11) samples: rates 108-127 differ from the 65,536th sample on); **exponential and increasing, at a level of
0x6000 or more**, the slowing by 4 depends on the shift: s < 10 (the wait already one sample) a quarter of the move;
s = 10 half the move at half the counter's gain; s >= 11 a quarter of the counter's gain (psx-spx's "cycles x 4" read
literally differs on every one of the 64 exponential increasing rates). `envelopeStep` (`runtime/audio/mixer.cpp`) is
written from psx-spx and these rules; `bozo-audio-tests`' digests are the moved mixer's.

**ENVX at the first tick after KeyOn.** KeyOn puts the voice at its start address (SSA) with level 0 in attack,
decodes its first block and clears its ENDX bit; ENVX read at the KeyOn's own clock is 0. After N samples of attack the
level is non-zero from the first sample when s <= 11 (rate < 0x30: N x (7 - (r & 3)) << (11 - s) until it saturates),
and stays 0 until sample 2^(s - 11) when s > 11. 989SND sends KON at the end of a tick and reads ENVX at the next one,
N = 200 samples later (153,600 / 768; 199-201 depending on where in their ticks the two calls fall: `iop-timing.md`
section 6). So ENVX is **already non-zero at the first tick** for every attack rate below 0x4C; from 0x4C (s = 19, 256
samples before the first step) it is still 0 there, and 989SND counts that tick as silent (15.12). The test
`testAttack`: rate 0x28 gives 14 after one sample and 2,800 after 200; rate 0x50 gives 0 at 200 and 511 samples, 7 at
512.

**A sample's end without repeat.** When the position leaves a block whose flags have bit 0 (end) set, the voice's ENDX
bit is set and the position goes to the loop address; without bit 1 (repeat) the voice is off **at once**: level 0, so
ENVX reads 0 from that sample on, with no release (the sample output in that step still had the old level). With
repeat it plays on from the loop address, envelope unchanged. The loop address is LSAX when the driver set it after the
KeyOn, else the last block with flag bit 2 (loop start) decoded since the KeyOn, else what it was (an LSAX set before the
KeyOn holds until a loop-start block replaces it). NAX shows the loop address after an end; an off voice's position no
longer moves. ENDX bits stay set until that voice's KeyOn, or a write to ENDX (any value) clears the core's 24. The same
happens during a release: the voice is off at the end. (`testEnd`: two blocks at pitch 0x1000, ENDX and ENVX 0 at the
56th sample, not the 55th.)

**The release after KeyOff.** KeyOff of a voice that is not off starts its release from its current level (a KeyOff of
an off voice does nothing; the voice keeps playing its samples meanwhile). Linear (ADSR2 bit 5 clear): the level falls
by 8 << max(0, 11 - s) every 2^max(0, s - 11) samples, s = ADSR2 & 0x1F, so from level L it is off after
ceil(L / (8 << max(0, 11 - s))) x 2^max(0, s - 11) samples. TMB's 0x5fcd (s = 13) from 0x7FFF: 4,096 steps of 8 every
4 samples, **16,384 samples (0.3413 s, 81.92 ticks)**; 0x5fce (s = 14): 32,768. Exponential: each step is the linear
step x level >> 15, at least 1, so it slows as it falls. A release never sets ENDX; while the level is not 0, ENVX is
not 0 and 989SND keeps the voice (15.12). (`testRelease`: 7 after 16,383 samples, 0 after 16,384.)

**Volumes.** A voice, master or effect volume with bit 15 clear is fixed (level = value << 1); with bit 15 a sweep
(bit 14 exponential, 13 decreasing, 12 negative phase, rate bits 0-6) stepping each sample by the envelope step above.
VOLXL/VOLXR (0x600, 0x700) answer the setting & 0x7FFF, not the swept level. The sound data input's volume is BVOL;
AVOL is kept and answered as set, without effect.

**libsd's other answers.** sceSdVoiceTrans copies at once and returns the size (mode 2: 0); the channel's transfer
handler (or callback) runs as an interrupt handler 2,000 IOP cycles after the IOP takes the queued call;
sceSdVoiceTransStatus answers 1 (done). sceSdBlockTrans plays the driver's buffer as the input (256 left samples then
256 right per 1 KB) and signals the channel's transfer end at each half; sceSdBlockTransStatus answers the buffer
address + the byte played next, | 1 << 24 in the second half (989SND's 0x40, GetTransStatus: `iop-timing.md` section
8). Parameters, switches, addresses and core attributes the mixer does not use are answered as set (0 before).
