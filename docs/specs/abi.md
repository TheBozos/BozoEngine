# Spec: the interface between recompiled code, the runtime and a port

What BozoEngine's recompiler emits and what a port's own code (TMB_Recomp's `runtime/`, ~13,000 lines) may rely on.
The names below are the ones TMB's code uses today, so it moves with mechanical changes; how they are implemented is
BozoEngine's own design.

## 1. Recompiled functions

- One C++ function per guest function: `void <name>_0x<address>(uint8_t *rdram, R5900Context *ctx, Runtime *rt)`.
  `<name>` is the ELF symbol (C++ mangled names kept as they are, characters not valid in C++ identifiers replaced),
  or `func` when there is none; `<address>` is lowercase hex without leading zeros. A header declares them all, so a
  port can call one directly (TMB calls a dozen: `vehicleTakeHit__7VehicleP8_fvectorii_0x1ad208(rdram, &ctx, rt)`).
- Arguments and results follow the guest's calling convention, in `ctx`: a0-a3 (r4-r7), then the stack; results in
  v0/v1 (r2/r3), floats in f0. A port calls a recompiled function by filling a copy of a context and calling it.
- `rdram` is the 32 MB guest RAM; guest address `a` (with the KSEG bits masked, `a & 0x1FFFFFF`) is `rdram[a]`.
- A function table maps every guest function's address to its C++ function, for indirect calls and jumps.

## 2. The context

`R5900Context` holds:
- `r[32]`: the 32 general registers, 128 bits each (the R5900's GPRs are 128-bit; the MMI instructions use all of
  them; ordinary instructions the low 64);
- `hi`, `lo`, and the second pair `hi1`, `lo1` (MMI);
- `f[32]`: the FPU registers (single precision), `fcr31` (condition bit), the accumulator for the FPU's multiply-add;
- VU0 macro mode state as the game uses it: `vu` (32 x 4 floats), `vi[16]`, the accumulator, Q, P, flags;
- `pc`: the guest address being executed (set at function entry and at points the runtime needs it);
- `sa` (the shift amount register of the R5900 funnel shifts).

Helpers a port uses: `getRegU32(ctx, n)`, `getRegS32(ctx, n)`, `setRegU32(ctx, n, v)`, `setReturnU32(ctx, v)` /
`setReturnS32` / `setReturnU64`, `SET_GPR_U32(ctx, n, v)`, `GPR_U32(ctx, n)`.

## 3. Control flow

- A call to a known function is a direct C++ call. A call or jump through a register looks the target up in the
  function table.
- Delay slots are executed as the hardware does (the instruction after a branch runs before the branch takes
  effect; the "likely" branches skip it when not taken).
- A jump into the middle of another function (tail calls, shared epilogues) must work: either the target becomes a
  function of its own, or the generated function can be entered at that label.
- The runtime must be able to stop guest code at points it chooses (the scheduler switching guest threads, a frame
  boundary): the generated code charges EE cycles and checks at backward branches, calls and jumps through a register
  (the reference's time model, `kernel.md` section 3), with the point it goes on from in `ctx->pc`. A stopped thread
  is run on later through each function's resume entry (`<prefix>_resume_table`: any label, any call's return
  address). After a call, a `ctx->pc` other than the return address returns to the runtime.

## 4. Memory accesses

- Ordinary loads and stores read and write `rdram` directly (little-endian, the R5900's), with the R5900's aligned
  and unaligned forms (`lwl/lwr`, `ldl/ldr`, `sdl/sdr`, `lq/sq` 16-byte aligned).
- The scratchpad (16 KB at 0x70000000) is a second block. Accesses to other addresses (hardware registers, which
  BozoEngine does not model) go to a runtime function that answers them as the native libraries need; in TMB they
  are rare.

## 5. Overrides and library calls

- A port replaces a guest function by address (`PS2_REGISTER_GAME_OVERRIDE` in TMB, binding a C++ function of the
  same signature to an address); calls to that address then reach the override, which may call the original.
- Sony library functions linked into the game (pad, memory card, CD, SIF, GS, MPEG, libc, libm: see
  `tmb-calls.md`) are found by symbol name and bound to the runtime's native implementations the same way.

## 6. Floating point

The R5900's FPU is not IEEE 754: no infinities or NaNs (results saturate to the largest magnitude), denormals flushed
to zero, rounding toward zero. Generated code must give the same bits as the console for every operation the game
performs, because game state depends on it (the reference records compare RAM bit for bit).
