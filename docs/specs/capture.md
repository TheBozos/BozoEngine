# Spec: the per-function capture (PS2XCAP1)

Phase 1's whole-function check: every function TMB runs must give the same results with BozoEngine's generated code
as with the reference build's. The reference build records **samples** of guest function calls during the scripted
runs (`reference/tmb/scripts/`); each sample holds everything needed to run that one call again from the same state
without the rest of the game: the guest context and memory at entry, every call the function's subtree makes out of
guest code with what it changed, and the context and memory changes at exit. A replay harness runs the function from
the entry state, answers each out-call from the sample, and compares the exit state bit for bit.

This document is the format. It is enough to read and replay a capture; no other source is needed.
`tools/capture/capture_read.py` lists and validates captures.

## 1. Terms

- **Guest function**: a function of the game's ELF, identified by its start address (the ELF symbol's value, or the
  target of the calls that reach it).
- **Call**: entering a guest function at its start with `ra` (r31) holding where it returns; the call **ends** when
  execution reaches that return address again with `sp` (r29) equal to its value at entry, on the same guest thread.
  A function's own tail jumps (`j other`) and calls are part of its call.
- **Out-call**: anything the call's code does that is not guest code (section 7.3): a call to a library or kernel
  function the runtime implements natively (a **stub**) or to a port function bound to a guest address (an
  **override**), a `SYSCALL`, a VU0 microprogram (`VCALLMS`, `VCALLMSR`), a load from or store to an address that is
  not memory (hardware registers, VU1 memory, the BIOS), or an exception (`BREAK`, a trap, a TLB instruction). Out-calls
  made by guest code that runs *inside* an out-call (a stub that calls back into the game) are part of that out-call.
- **Memory**: main RAM (32 MB), the scratchpad (16 KB) and VU0's data memory (4 KB), addressed together as the
  **flat space** (section 3). Everything else is out-call territory.

## 2. Files

A capture is a folder (one per scripted run) holding:

| File | What |
|---|---|
| `capture.bin` | The samples (sections 4-7) |
| `summary.txt` | Text, one line per guest function called during the run: address, samples, unreplayable calls, calls seen, unreplayable calls by reason (the seven reasons of section 8 in order). A function with no sample and no unreplayable call was only ever called inside a sampled call (or outside the frame window) |
| `selfcheck.txt` | The reference build's own replay of the capture (section 9), when run |

Everything is little-endian. Sizes are in bytes. "Padded to 4" means zero bytes up to the next multiple of 4.

## 3. The flat space

| Flat address | Size | Guest memory |
|---|---|---|
| `0x0000000` - `0x1FFFFFF` | 32 MB | Main RAM: guest physical address `a` (`a & 0x1FFFFFFF` for KSEG0/KSEG1 and the uncached mirrors) |
| `0x2000000` - `0x2003FFF` | 16 KB | The scratchpad (guest `0x70000000` - `0x70003FFF`) |
| `0x2004000` - `0x2004FFF` | 4 KB | VU0 data memory (guest `0x11004000` - `0x11004FFF`) |

Total `0x2005000` bytes. Memory changes are written as **runs**:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | flat address of the first byte |
| 4 | u32 | length (> 0) |
| 8 | u8[length] | the new bytes, padded to 4 |

A list of runs is applied in order by copying each run's bytes to its flat address. Runs never overlap within one
list. (The recorder never makes a run cross a 4 KB boundary, but a reader must not rely on it.)

## 4. Header (128 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | char[8] | magic `PS2XCAP1` |
| 8 | u32 | version (1) |
| 12 | u32 | headerBytes (128: where the chunks start) |
| 16 | u32 | contextBytes (1328, section 5) |
| 20 | u32 | flatBytes (`0x2005000`) |
| 24 | u32 | ramBytes (`0x2000000`) |
| 28 | u32 | scratchpadBytes (`0x4000`) |
| 32 | u32 | vu0DataBytes (`0x1000`) |
| 36 | u32 | samplesPerFunction: the replayable samples sought per function |
| 40 | u32 | frameFrom: the first game frame sampled |
| 44 | u32 | frameTo: the frame sampling stops (exclusive) |
| 48 | u64 | elfHash: XXH64 (seed 0) of the game's ELF file (0 if unknown) |
| 56 | u32 | pageBytes (4096) |
| 60 | u32 | reserved (0) |
| 64 | char[64] | run name, NUL-padded |

Game frames are the reference harness's (`harness.md`: frame 0 runs until the game first hands a picture over).

## 5. Context record (1328 bytes, 332 u32 words)

The guest CPU state, the same fields as `runtime/include/bozo/context.h`. Floats are stored as their bits.

| Words | Field |
|---|---|
| 0 - 127 | `r0` - `r31`, 128 bits each: word `4n` is bits 0-31 of `rn`, `4n+3` bits 96-127 |
| 128 - 129 | `hi` (low 64 bits of HI), low word first |
| 130 - 131 | `lo` |
| 132 - 133 | `hi1` (bits 64-127 of HI, used by `MULT1`/`DIV1` and the MMI) |
| 134 - 135 | `lo1` |
| 136 | `pc` |
| 137 | `sa` (the funnel shift amount) |
| 138 - 169 | `f0` - `f31` (COP1) |
| 170 | `fcr31` |
| 171 | the FPU accumulator (`ADDA.S`, `MADD.S`...) |
| 172 - 299 | VU0 `vf0` - `vf31`, 4 words each (x, y, z, w) |
| 300 - 303 | VU0 `ACC` (x, y, z, w) |
| 304 - 319 | VU0 `vi0` - `vi15` (16 bits, zero-extended) |
| 320 | VU0 `Q` |
| 321 | VU0 `I` |
| 322 | VU0 `P` |
| 323 | VU0 `R` |
| 324 | VU0 status flags (control register 16) |
| 325 | VU0 MAC flags (control register 17) |
| 326 | VU0 clipping flags (control register 18) |
| 327 | VU0 `CMSAR0` (control register 27) |
| 328 | COP0 `Status` (the game's `EI`/`DI` and `MFC0` see it) |
| 329 - 331 | reserved (0) |

`pc` at a sample's entry is the function's start; at its exit, the return address.

## 6. Chunks

After the header, chunks until the end of the file:

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | tag |
| 4 | u32 | flags: bit 0 = the payload is zlib-compressed (RFC 1950, as Python's `zlib.decompress`) |
| 8 | u32 | storedBytes: the bytes that follow |
| 12 | u32 | rawBytes: the payload's size once decompressed (= storedBytes when bit 0 is clear) |
| 16 | u8[storedBytes] | payload |

| Tag | Payload |
|---|---|
| `SAMP` | A replayable sample (section 7) |
| `SKIP` | A call that was tried and could not be sampled (section 8) |
| `OVRD` | u32 count, then count u32 guest addresses: the addresses bound to port overrides (out-calls of kind 2). Written before the first sample, again if the list changes; the latest one applies |
| `END ` | u32 samples, u32 skips, u32 functions with at least one sample, u32 the game frame the run ended at, u32 guest functions called during the run (sampled or not). Last chunk of a complete capture; a capture without it was cut short (every chunk before is still valid). Readers take the fields present (later versions may add more) |

Unknown tags must be skipped (their size is in the chunk header).

## 7. `SAMP`: a sample

### 7.1 Layout

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | function: the guest address called |
| 4 | u32 | sampleIndex: 0, 1, 2... in file order |
| 8 | u32 | frame: the game frame at entry |
| 12 | i32 | thread: the guest thread id |
| 16 | u64 | callIndex: the function's calls seen so far, this one included (1 = its first call) |
| 24 | u32 | slot: which of the function's samples this is (0 .. samplesPerFunction-1) |
| 28 | u32 | outcallCount |
| 32 | u32 | entryRunCount |
| 36 | u32 | entryRunBytes |
| 40 | u32 | outcallBytes |
| 44 | u32 | exitRunCount |
| 48 | u32 | exitRunBytes |
| 52 | u32 | returnAddress: `ra` at entry |
| 56 | u32 | stackPointer: `sp` at entry |
| 60 | u32 | reserved (0) |
| 64 | context | entry context (1328 bytes) |
| 1392 | runs | entry memory: entryRunCount runs, entryRunBytes bytes |
| ... | out-calls | outcallCount out-call records, outcallBytes bytes, in the order they happened |
| ... | context | exit context (1328 bytes) |
| ... | runs | exit memory: exitRunCount runs, exitRunBytes bytes |

### 7.2 Memory at entry

The memory at a sample's entry is a chain: start from the flat space all zeros, and apply the **entry runs of every
`SAMP` chunk in file order, up to and including this sample's**. (`SKIP` chunks do not change it.) The first sample's
runs hold the whole initial state; each later sample's runs hold what changed since the previous sample's entry.
To replay sample n alone, a reader applies the entry runs of samples 0..n (`capture_read.py state n out.bin` writes
the result).

### 7.3 Out-call records

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | kind (below) |
| 1 | u8 | size: for kinds 5 and 6, the access size (1, 2, 4, 8 or 16); otherwise 0 |
| 2 | u16 | reserved (0) |
| 4 | u32 | target (below) |
| 8 | u32[4] | value (below) |
| 24 | u32 | ctxCount: context words the out-call changed |
| 28 | u32 | runCount: memory runs it wrote |
| 32 | u32 | reserved (0) |
| 36 | u32[11] | changed-word mask: bit i of word i/32 set when context word i changed (bits past word 331 are 0) |
| 80 | u32[ctxCount] | the new values of the changed words, in word order |
| ... | runs | runCount runs: the memory the out-call changed (bytes it wrote with the same value are not listed) |

| Kind | Name | target | value |
|---|---|---|---|
| 1 | stub | the guest address called (a library or kernel function implemented natively) | `a0`-`a3` (low words) at the call |
| 2 | override | the guest address called (a port function bound there; listed by `OVRD`) | `a0`-`a3` at the call |
| 3 | syscall | the address of the `SYSCALL` instruction | `v1` (the call number), `a0`, `a1`, `a2` |
| 4 | vu0 | the microprogram's start address in VU0 micro memory, in bytes (`VCALLMS`'s 15-bit immediate times 8, or `CMSAR0` for `VCALLMSR`) | 0 |
| 5 | io-load | the guest (virtual) address loaded | the value read, little-endian in `size` bytes |
| 6 | io-store | the guest address stored to | the value stored |
| 7 | exception | the address of the instruction (`BREAK`, trap, TLB, an overflow exception) | 0 |

A sample with a kind 7 out-call is never written as `SAMP` (it is a `SKIP`, reason `exception`).

**When a call is an out-call.** Kind 1 targets are the ELF's library and kernel functions the reference runtime
implements natively (TMB: the 362 addresses of its stub list, the calls of `tmb-calls.md`); any transfer to one (a call
`JAL`/`JALR`, a direct jump `J`, an indirect jump `JR`) is an out-call. Kind 2 targets (the `OVRD` list) are out-calls
when reached by a call (`JAL`/`JALR`) or an indirect jump (`JR`); the reference build runs the guest code at the
address for a direct jump (`J`, a tail jump), so its effects are then part of the sample's own execution. The set of
out-call addresses a replay needs is the union of the kind 1 and 2 targets in the capture and the `OVRD` list. An
out-call happens where the transfer does (after a branch's delay slot).

**Context changes.** The mask and words are what the out-call changed, against the context at its start. For
kinds 1-3, word 136 (`pc`) is the runtime's own bookkeeping (the return address, or unchanged): a replay may ignore it
and continue after the call as the call instruction would (at `ra` for a call, at the caller's return for a jump).
Every other word is the out-call's result (`v0`/`v1` for a function, VU0 registers for a microprogram, `a0`-`a3` or
temporaries a native function happened to change).

### 7.4 Memory at exit

The memory when the call returns is the memory at entry with the exit runs applied. Every byte of the flat space not
covered by an exit run is unchanged from entry.

## 8. `SKIP`: calls not sampled

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | function |
| 4 | u32 | frame |
| 8 | u32 | reasons (bits below) |
| 12 | i32 | thread |
| 16 | u64 | callIndex |
| 24 | u32 | out-calls recorded before it was abandoned |
| 28 | u32 | reserved |

| Bit | Reason |
|---|---|
| 0 | threadSwitch: the call blocked (a `WaitSema` on a zero count, a vblank wait...) and the guest scheduler switched threads, or the thread changed |
| 1 | interrupted: the scheduler took control inside the call (an interrupt, an alarm, a timer or vblank event, the end of a time slice), so other code may have run in the middle |
| 2 | unwound: the call went where the reference build cannot follow it (a jump to an address with no code, the run stopping) |
| 3 | exception: a `BREAK`, a trap, an exception or a TLB instruction |
| 4 | tooBig: more than 96 MB of memory changes |
| 5 | cop0Timer: it read or wrote COP0 Count or a performance counter |
| 6 | abandoned: the run ended inside the call |

Sampling tries a function again at a later frame after a `SKIP` (at most 12 times by default); functions that never
return (thread entry points, the main loop) or always block only have `SKIP`s.

## 9. Replaying a sample

1. Build the flat memory at entry (7.2) and load it: RAM, scratchpad, VU0 data memory.
2. Load the entry context. Every other piece of runtime state is irrelevant: whatever the function asks of the
   runtime is an out-call answered from the sample.
3. Run the function at `function` until the call ends (`pc` = returnAddress, `sp` = stackPointer). No scheduler
   checkpoint may preempt it (none did in the reference: a sample interrupted there is a `SKIP`).
4. At each out-call, in order: the next record must have the same kind and target (and for kinds 1-2 the same
   `a0`-`a3`, for kind 6 the same size and value); then apply its changed context words and its memory runs, and for
   kind 5 give the load the recorded value. The call is not performed.
5. At the end: every out-call record must have been used; the context must equal the exit context (all 332 words);
   the flat memory must equal the entry memory with the exit runs applied, every byte.

The reference build replays its own captures this way through its own generated code (its self-check,
`selfcheck.txt`); a capture is only published when that replay matches on every sample.

## 10. How samples are chosen

For each function, up to samplesPerFunction replayable samples, spread over [frameFrom, frameTo): sample slot i is
taken at the function's first call that can be sampled at or after frame `frameFrom + i * (frameTo - frameFrom) /
samplesPerFunction`. Only one call is sampled at a time: calls made inside a sampled call are part of it and are not
sampled themselves. Functions the scripted run never calls have no entry.

A function called only inside sampled calls is never tried in a first capture. A scripted run is therefore captured in
**passes**, each a separate capture of the same deterministic run: pass 1 (`<run>`) samples every function, pass n
(`<run>.n`) only the functions the earlier passes called but never tried, until none are left. Each pass stands
alone (its own memory chain); together they cover every function the run calls that can be sampled.
