// BozoEngine's runtime core: runtime_api.h on top of guest memory, the function table and the services interface.
// See bozo/runtime.h.
#include "bozo/runtime.h"

#include "bozo/ops_mem.h"
#include "bozo/port.h"

#include <algorithm>
#include <atomic>
#include <cstring>

bool bozo_native_instruction(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc)
{
    return rt->port && rt->port->nativeInstruction(rdram, ctx, rt, pc);
}

namespace bozo
{
    GuestMemory::GuestMemory() : flat_(new uint8_t[kFlatBytes]) { std::memset(flat_.get(), 0, kFlatBytes); }

    int64_t GuestMemory::flatOffset(uint32_t address, uint32_t size)
    {
        if (address >= kScratchpadBase && address - kScratchpadBase + uint64_t(size) <= kScratchpadBytes)
            return kFlatScratchpadOffset + (address - kScratchpadBase);
        if (address >= kVu0DataBase && address - kVu0DataBase + uint64_t(size) <= kVu0DataBytes)
            return kFlatVu0Offset + (address - kVu0DataBase);
        const uint32_t phys = physicalAddress(address);
        if (uint64_t(phys) + size <= kRamSize)
            return kFlatRamOffset + phys;
        return -1;
    }

    const BozoResumeEntry *FunctionTable::resumeEntry(uint32_t pc) const
    {
        const BozoResumeEntry *end = resume_ + resumeCount_;
        const BozoResumeEntry *it =
            std::upper_bound(resume_, end, pc, [](uint32_t a, const BozoResumeEntry &e) { return a < e.start; });
        if (it == resume_)
            return nullptr;
        --it;
        return pc < it->end ? it : nullptr;
    }

    RecompiledFunction FunctionTable::generated(uint32_t address) const
    {
        const BozoFunctionEntry *end = table_ + count_;
        const BozoFunctionEntry *it =
            std::lower_bound(table_, end, address, [](const BozoFunctionEntry &e, uint32_t a) { return e.address < a; });
        return it != end && it->address == address ? it->function : nullptr;
    }

    void FunctionTable::setOverride(uint32_t address, RecompiledFunction fn)
    {
        auto it = std::lower_bound(overrides_.begin(), overrides_.end(), address,
                                   [](const BozoFunctionEntry &e, uint32_t a) { return e.address < a; });
        if (it != overrides_.end() && it->address == address)
            it->function = fn;
        else
            overrides_.insert(it, BozoFunctionEntry{address, fn});
    }

    RecompiledFunction FunctionTable::override(uint32_t address) const
    {
        auto it = std::lower_bound(overrides_.begin(), overrides_.end(), address,
                                   [](const BozoFunctionEntry &e, uint32_t a) { return e.address < a; });
        return it != overrides_.end() && it->address == address ? it->function : nullptr;
    }
}

using bozo::GuestMemory;

void bozo_call_indirect(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target)
{
    if (RecompiledFunction o = rt->functions.override(target))
    {
        ctx->pc = target;
        o(rdram, ctx, rt);
        // It returned to its caller (what a tail call's caller returns to), unless a stop was deferred inside a native
        // out-call: the context's pc then says where the thread goes on, and the callers return up to the out-call.
        if (!rt->stopDeferred)
            ctx->pc = ctx->r[31].uw[0];
        return;
    }
    if (RecompiledFunction f = rt->functions.generated(target))
    {
        f(rdram, ctx, rt);
        return;
    }
    rt->services->unknownTarget(rdram, ctx, rt, target);
}

void bozo_tail_call(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target)
{
    rt->tailCall = true;
    bozo_call_indirect(rdram, ctx, rt, target);
    rt->tailCall = false;
}

void bozo_yield(uint8_t *rdram, R5900Context *ctx, Runtime *rt) { rt->services->yield(rdram, ctx, rt); }
void bozo_leave(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc) { rt->services->leaveAt(rdram, ctx, rt, pc); }
bool bozo_jump(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target)
{
    return rt->services->jump(rdram, ctx, rt, target);
}
void bozo_resume_failed(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc)
{
    rt->services->resumeFailed(rdram, ctx, rt, pc);
}

void bozo_syscall(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t code) { rt->services->syscall(rdram, ctx, rt, code); }
void bozo_break(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t code) { rt->services->breakpoint(rdram, ctx, rt, code); }
void bozo_trap(uint8_t *rdram, R5900Context *ctx, Runtime *rt) { rt->services->trap(rdram, ctx, rt); }
void bozo_unimplemented(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t word)
{
    rt->services->unimplemented(rdram, ctx, rt, word);
}

uint32_t bozo_cop0_read(R5900Context *ctx, Runtime *rt, uint32_t reg)
{
    if (reg == bozo::kCop0Status)
        return rt->cop0Status;
    return rt->services->cop0Read(ctx, rt, reg);
}
void bozo_cop0_write(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t value)
{
    if (reg == bozo::kCop0Status)
        rt->cop0Status = value;
    else
        rt->services->cop0Write(ctx, rt, reg, value);
}
void bozo_set_interrupts(uint8_t *rdram, R5900Context *ctx, Runtime *rt, bool enabled)
{
    rt->cop0Status = enabled ? (rt->cop0Status | bozo::kStatusEIE) : (rt->cop0Status & ~bozo::kStatusEIE);
    rt->services->interruptsChanged(rdram, ctx, rt, enabled);
}
bool bozo_cop0_condition(R5900Context *ctx, Runtime *rt) { return rt->services->cop0Condition(ctx, rt); }
void bozo_eret(uint8_t *rdram, R5900Context *ctx, Runtime *rt) { rt->services->eret(rdram, ctx, rt); }

uint8_t *bozo_vu0_memory(Runtime *rt) { return rt->memory.vu0Data(); }
void bozo_vu0_callms(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t address)
{
    rt->services->vu0CallMicroprogram(rdram, ctx, rt, address);
}
uint32_t bozo_vu0_control_read(R5900Context *ctx, Runtime *rt, uint32_t reg) { return rt->services->vu0ControlRead(ctx, rt, reg); }
void bozo_vu0_control_write(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t value)
{
    rt->services->vu0ControlWrite(ctx, rt, reg, value);
}

uint8_t *bozo_memory_slow(uint8_t *, Runtime *rt, uint32_t address, uint32_t size, bool)
{
    return rt->memory.hostPointer(address, size);
}
uint64_t bozo_io_read(R5900Context *ctx, Runtime *rt, uint32_t address, uint32_t size)
{
    uint64_t v[2] = {0, 0};
    rt->services->ioRead(ctx, rt, address, size, v);
    return v[0];
}
namespace bozo::port
{
    namespace
    {
        std::atomic<StorePatch> g_storePatch{nullptr};
    }
    void setStorePatch(StorePatch patch) { g_storePatch.store(patch, std::memory_order_release); }
    StorePatch storePatch() { return g_storePatch.load(std::memory_order_acquire); }
}

void bozo_io_write(R5900Context *ctx, Runtime *rt, uint32_t address, uint32_t size, uint64_t value)
{
    const uint64_t v[2] = {value, 0};
    rt->services->ioWrite(ctx, rt, address, size, v);
}
void bozo_io_read128(R5900Context *ctx, Runtime *rt, uint32_t address, uint64_t out[2])
{
    rt->services->ioRead(ctx, rt, address, 16, out);
}
void bozo_io_write128(R5900Context *ctx, Runtime *rt, uint32_t address, const uint64_t value[2])
{
    rt->services->ioWrite(ctx, rt, address, 16, value);
}
