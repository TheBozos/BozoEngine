# Spec: the replay check (bozo-replay)

Phase 1's exit check (`PLAN.md` section 4): every function the reference runs during the scripted runs gives the same
results under BozoEngine's generated code. `bozo-replay` replays each sample of the PS2XCAP1 captures
(`capture.md`) through the code `bozo-recomp` generates for the game, answering every out-call from the sample's log,
and compares the exit state bit for bit.

Code: `tools/replay/` (the capture reader, the replay services, the driver), `runtime/` (the runtime core it runs on).

## 1. Build and run

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja -DBOZO_TMB_ELF=D:/TMB_Recomp/build/us_probe/SCUS_971.01 -DBOZO_CAPTURES=D:/BozoCaptures/tmb
cmake --build build
build/tools/replay/bozo-replay --out build/replay D:/BozoCaptures/tmb     # every capture folder under the root
build/tools/replay/bozo-replay --show D:/BozoCaptures/tmb/story 657        # one sample in detail
```

| Option | What |
|---|---|
| `--jobs n` | worker processes at once (default: the hardware threads) |
| `--out dir` | one `<capture>.tsv` per capture (a line per sample) and `report.txt` (default `replay-results`) |
| `--function 0xA,0xB` | only these functions' samples (the memory chain still runs through every sample) |
| `--elf path` | the game's executable (default: the one the build was generated from); its XXH64 must equal each capture's `elfHash` |
| `--budget n` | guest cycles (checkpoint counts) after which a sample is stopped as a runaway (default 4e9) |
| `--show <capture> <index>` | the sample's out-call log, the replay's out-calls as they happen, every differing context word (got, want, entry) and the expected bytes of each differing memory range |

The whole TMB set (24 capture folders, 9,232 samples) runs in about 10 seconds on 16 threads. The exit code is 0 when
every sample matches.

**Build cache.** With `BOZO_TMB_ELF` and `BOZO_CAPTURES` set, the build (all under the build folder, never committed):
1. `bozo-capture-tool externals` lists the out-call addresses of every capture into `tmb_gen/externals.txt`;
2. `bozo-recomp <elf> tmb_gen --prefix tmb --files 32 --no-comments --externals tmb_gen/externals.txt` generates the
   code into a fixed list of 32 files, rewriting only the files whose contents change, so Ninja recompiles only those
   (`BOZO_TMB_GEN_FILES`, `BOZO_TMB_GEN_OPT` = `-O1` by default);
3. `bozo-replay` links the generated objects with the runtime core and the replay services.
A change to a runtime header recompiles everything (about 30 s on 16 threads); a change to the replay tool relinks.

## 2. External addresses

The reference answers some guest addresses natively: the library and kernel functions of its stub list (out-call
kind 1) and the port's overrides (kind 2, listed by `OVRD`). The generated code must reach the runtime there instead of
running the guest code, so `bozo-recomp --externals <file>` takes the list (`stub 0xADDR` / `override 0xADDR` lines):

- **stub**: every transfer to the address (jal, jalr, bal, j, a branch, a fall-through, a `jr`) goes through
  `bozo_call_indirect`;
- **override**: calls (jal, jalr, bal) go through `bozo_call_indirect`; a direct jump (j, a branch, a fall-through)
  runs the guest code there, as the reference does (`capture.md` 7.3). An indirect jump reaches the runtime anyway.

`bozo_call_indirect` looks the address up in the override table first. This is also how the real runtime will bind a
port's overrides and the native library calls (`abi.md` section 5): the port's list goes to `--externals` at build
time and the runtime binds a native function to each address.

For TMB the captures give 88 stub and 73 override addresses (1,694 transfers in the generated code).

## 3. The runtime base

What the replay runs on, split into what phase 2 keeps and what only the replay uses.

**Kept (`runtime/`, `bozo/runtime.h`)**: the runtime core.
- `GuestMemory`: one host block laid out as the capture's flat space: RAM (32 MB), the scratchpad (16 KB), VU0 data
  memory (4 KB). `bozo_memory_slow` maps RAM through KSEG0, KSEG1 and the EE's uncached views (0x20000000,
  0x30000000), the scratchpad (0x70000000) and VU0 data (0x11004000); any other address has no memory and goes to the
  services as I/O.
- `FunctionTable`: the generated table and the override table (sorted, looked up first). An override runs with
  `ctx->pc` = the address it was reached at, so one native function can serve several addresses.
- COP0 Status: `ei`/`di` set and clear Status.EIE (bit 16); `mfc0`/`mtc0` 12 read and write it. Other COP0 registers
  go to the services.
- `RuntimeServices`: an interface for everything else the generated code asks (syscall, break, trap, an untranslated
  instruction, an unknown jump target, the scheduler's yield, COP0 registers and CPCOND0, VU0 microprograms and control
  registers, I/O loads and stores). Phase 2 implements it with the kernel and the native libraries.

**Replay only (`tools/replay/`)**: the `Replayer`, a `RuntimeServices` answering everything from the log.
- Every external address is bound to one override that takes the next log record: its kind (1 or 2) and target must
  match, and `a0`-`a3` must equal the record's values; then the record's context words (except `pc`, the reference's
  bookkeeping) and memory runs are applied.
- `syscall` (target: the instruction's address; values v1, a0, a1, a2), `vcallms` (target: the microprogram's byte
  address), I/O loads (the logged value is returned; the size must match) and stores (size and value must match) the
  same way.
- An out-call of another kind or target, or one past the end of the log, stops the sample (**diverged**). Records
  left at the end are reported (**outcalls-left**). A `break`, a trap, an untranslated instruction or a jump to an
  address with neither code nor an override stops it (**exception**).
- `bozo_yield` does nothing until the sample has run `--budget` cycles, then stops it (**runaway**).
- CPCOND0 (`bc0f`/`bc0t`) is not an out-call kind, so the log does not have it. The reference answers it from its DMA
  state; the replay rebuilds that from the sample's own stores: a CHCR store with STR set finishes that channel at once,
  a D_STAT store clears the bits it acknowledges, D_PCR selects the channels CPCOND0 watches (any started channel when
  the sample did not write D_PCR), nothing is finished at entry. This matches all 7 samples of
  `hierCheckVifComplete` (false) and the `hierTraverseAsm` waits after a VIF1 kick (true). The real runtime needs
  its own answer (r5900.md Q15).
- COP0 registers other than Status and VU0 control registers 28 (FBRST), 29 (VPU-STAT) and above read as 0 and
  ignore writes, with a note in the results; no TMB sample reaches them.

## 4. The comparison

For each `SAMP` chunk in file order:
1. Its entry runs are applied to two copies of the flat space: the **chain** (the capture's memory at entry, `capture.md`
   7.2) and the **working** memory the generated code runs on. They are equal between samples.
2. The context is loaded from the 332 entry words (word 328, COP0 Status, into the runtime core). The function at the
   sample's address is called (`no-entry` if the generated table has none).
3. When it returns, `pc` is set to the return address (the call ended there, `capture.md` section 5; the generated code
   keeps `pc` only at function entries and runtime calls) and all 332 words are compared with the exit context.
4. The exit runs are applied to the chain (old bytes saved); the working memory is compared with it 4 KB page by page,
   every differing byte counted and merged into ranges; differing pages are copied back. Then the exit runs are undone
   in both copies, which are equal again.

A worker process replays one capture and writes one line per sample (`S <index> <function> <status bits> <words>
<bytes> <out-calls used>/<logged> <details>`), with a `B <index>` line before it: if the process dies (a host crash
in generated code), the driver marks that sample **crashed** and starts a new worker after it.

## 5. Results (TMB, 24 capture folders)

| Run | Samples | Match (first run) | Match (now) | Functions | All samples match (first run) | (now) |
|---|---|---|---|---|---|---|
| boot | 557 | 526 | 557 | 199 | 188 | 199 |
| boot.2 - .8 | 206 | 196 | 206 | 172 | 162 | 172 |
| match1p | 4,037 | 2,681 | 4,037 | 1,239 | 802 | 1,239 |
| match1p.2 - .8 | 323 | 253 | 323 | 252 | 203 | 252 |
| story | 3,812 | 2,570 | 3,812 | 1,187 | 775 | 1,187 |
| story.2 - .8 | 297 | 223 | 297 | 234 | 193 | 234 |
| **all** | **9,232** | **6,449** | **9,232** | **1,518** | **996** | **1,518** |

("Functions" per row counts each pass's functions; the total counts distinct functions.) Each fix and the evidence for
it is in `r5900.md` section 12.
