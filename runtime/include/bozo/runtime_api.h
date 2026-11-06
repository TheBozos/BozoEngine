// What recompiled code asks of the runtime. Every function here is implemented by BozoEngine's runtime (phase 2) or,
// for the tests, by a stub (tests/support/stub_runtime.cpp). The generated code calls nothing else outside its own
// functions and the inline helpers of bozo/recomp.h.
#pragma once

#include "bozo/context.h"

// A statically selected instruction replaced by the port. False: execute its original operation (e.g. a checker).
// For a BC0 instruction, true means the native operation is complete, without querying a DMA condition register.
bool bozo_native_instruction(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc);

// ---- Functions -----------------------------------------------------------------------------------------------------
// The function table the recompiler emits: every guest function (and every extra entry point) by address, sorted.
struct BozoFunctionEntry
{
    uint32_t address;
    RecompiledFunction function;
};

// Every guest function's range [start, end) and the entry that runs it from any of its labels at ctx->pc (the
// dispatcher's way back into a thread that stopped: abi.md section 3).
struct BozoResumeEntry
{
    uint32_t start;
    uint32_t end;
    RecompiledFunction resume;
};

// The externals the code was generated with (bozo-recomp --externals): the addresses the runtime answers, sorted.
struct BozoExternalEntry
{
    uint32_t address;
    uint32_t kind; // 1 stub (every transfer goes to the runtime), 2 override (calls do; a direct jump runs the guest code)
};

// A resume entry was asked for a pc that is not one of its function's labels.
void bozo_resume_failed(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc);

// A call or jump through a register (jalr, jr to a non-return address, a computed tail call): the runtime finds
// `target` in its table (overrides first) and calls it. Unknown targets are the runtime's to report.
void bozo_call_indirect(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target);

// A jump (j, a branch, a fall-through: no return address written) to an external address: as bozo_call_indirect,
// the runtime knowing it was not a call (the reference's record of calls only has calls made by jal and jalr).
void bozo_tail_call(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target);

// A jump through a register (a jr that is not a return), after its checkpoint: true when the runtime took it over
// (the dispatcher runs on from `target` itself, as the reference does: the jumping function's frame is gone); false
// to let the generated code make the jump (a switch to its own label, or a tail call).
bool bozo_jump(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target);

// ---- Scheduling ----------------------------------------------------------------------------------------------------
// Called by the generated checkpoint when ctx->cycles reaches ctx->cycleLimit (backward branches, calls and jumps
// leaving a function), with ctx->pc set to where the guest code goes on. The runtime may stop the thread here (switch
// threads, run a handler: it then does not return) or raise the limit and return.
void bozo_yield(uint8_t *rdram, R5900Context *ctx, Runtime *rt);
// Guest code leaves for the runtime at `pc` (an inner call's return inside a port's native override: kernel.md section
// 3b); the generated code returns right after.
void bozo_leave(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc);

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
