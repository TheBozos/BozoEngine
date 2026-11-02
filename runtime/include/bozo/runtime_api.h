// What recompiled code asks of the runtime. Every function here is implemented by BozoEngine's runtime (phase 2) or,
// for the tests, by a stub (tests/support/stub_runtime.cpp). The generated code calls nothing else outside its own
// functions and the inline helpers of bozo/recomp.h.
#pragma once

#include "bozo/context.h"

// ---- Functions -----------------------------------------------------------------------------------------------------
// The function table the recompiler emits: every guest function (and every extra entry point) by address, sorted.
struct BozoFunctionEntry
{
    uint32_t address;
    RecompiledFunction function;
};

// A call or jump through a register (jalr, jr to a non-return address, a computed tail call): the runtime finds
// `target` in its table (overrides first) and calls it. Unknown targets are the runtime's to report.
void bozo_call_indirect(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target);

// ---- Scheduling ----------------------------------------------------------------------------------------------------
// Called by the generated checkpoint when ctx->cycles reaches ctx->cycleLimit (function entries, backward branches).
// The runtime may switch guest threads here, end a frame, or raise the limit and return.
void bozo_yield(uint8_t *rdram, R5900Context *ctx, Runtime *rt);

// ---- Exceptions and system instructions ----------------------------------------------------------------------------
void bozo_syscall(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t code); // `syscall` (number in v1)
void bozo_break(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t code);   // `break`
void bozo_trap(uint8_t *rdram, R5900Context *ctx, Runtime *rt);                   // a trap whose condition held (teq...)
// An instruction the recompiler decoded but does not translate (none in TMB's code; see r5900.md).
void bozo_unimplemented(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t word);

// ---- COP0 ----------------------------------------------------------------------------------------------------------
uint32_t bozo_cop0_read(R5900Context *ctx, Runtime *rt, uint32_t reg);
void bozo_cop0_write(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t value);
void bozo_set_interrupts(uint8_t *rdram, R5900Context *ctx, Runtime *rt, bool enabled); // ei / di
bool bozo_cop0_condition(R5900Context *ctx, Runtime *rt);                               // bc0f / bc0t's CPCOND0
void bozo_eret(uint8_t *rdram, R5900Context *ctx, Runtime *rt);

// ---- COP2 / VU0 ----------------------------------------------------------------------------------------------------
// VU0's 4 KB data memory (VLQI/VSQI... in macro mode, and what microprograms use).
uint8_t *bozo_vu0_memory(Runtime *rt);
// VCALLMS / VCALLMSR: run the VU0 microprogram at `address` (bytes in VU0 micro memory). No VU emulation: the runtime
// answers with the port's native version of the program.
void bozo_vu0_callms(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t address);
// Control registers the context does not hold (28 FBRST, 29 VPU-STAT, 31 CMSAR1, ...).
uint32_t bozo_vu0_control_read(R5900Context *ctx, Runtime *rt, uint32_t reg);
void bozo_vu0_control_write(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t value);

// ---- Memory outside main RAM ---------------------------------------------------------------------------------------
// The generated code reads and writes guest RAM (addresses below 32 MB) inline. Any other address comes here:
// the runtime returns a host pointer for memory it backs (RAM mirrors, the scratchpad, VU0 memory) with at least
// `size` bytes valid, or nullptr for an address with no memory behind it, which then goes to bozo_io_read/write.
uint8_t *bozo_memory_slow(uint8_t *rdram, Runtime *rt, uint32_t address, uint32_t size, bool write);
uint64_t bozo_io_read(R5900Context *ctx, Runtime *rt, uint32_t address, uint32_t size);   // size 1, 2, 4, 8
void bozo_io_write(R5900Context *ctx, Runtime *rt, uint32_t address, uint32_t size, uint64_t value);
void bozo_io_read128(R5900Context *ctx, Runtime *rt, uint32_t address, uint64_t out[2]);
void bozo_io_write128(R5900Context *ctx, Runtime *rt, uint32_t address, const uint64_t value[2]);
