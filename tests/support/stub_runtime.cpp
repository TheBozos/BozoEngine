// The tests' implementation of runtime_api.h.
#include "stub_runtime.h"

#include "bozo/ops_mem.h"

#include <algorithm>
#include <cstdio>

namespace
{
    std::string hexEvent(const char *what, uint32_t v)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s 0x%x", what, v);
        return buf;
    }
}

void bozo_call_indirect(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target)
{
    rt->indirect.push_back(target);
    const BozoFunctionEntry *end = rt->table + rt->tableSize;
    const BozoFunctionEntry *it = std::lower_bound(rt->table, end, target,
                                                   [](const BozoFunctionEntry &e, uint32_t a) { return e.address < a; });
    if (it != end && it->address == target)
    {
        it->function(rdram, ctx, rt);
        return;
    }
    rt->events.push_back(hexEvent("unknown-target", target));
}

void bozo_resume_failed(uint8_t *, R5900Context *, Runtime *rt, uint32_t pc) { rt->events.push_back(hexEvent("resume-failed", pc)); }

bool bozo_jump(uint8_t *, R5900Context *, Runtime *, uint32_t) { return false; }
void bozo_tail_call(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target) { bozo_call_indirect(rdram, ctx, rt, target); }

void bozo_leave(uint8_t *, R5900Context *ctx, Runtime *, uint32_t pc) { ctx->pc = pc; }
void bozo_yield(uint8_t *, R5900Context *ctx, Runtime *rt)
{
    ++rt->yields;
    ctx->cycleLimit = ctx->cycles + 1000000;
}

void bozo_syscall(uint8_t *, R5900Context *ctx, Runtime *rt, uint32_t)
{
    rt->events.push_back(hexEvent("syscall", ctx->r[3].uw[0])); // the EE kernel's call number is in v1
}
void bozo_break(uint8_t *, R5900Context *, Runtime *rt, uint32_t code) { rt->events.push_back(hexEvent("break", code)); }
void bozo_trap(uint8_t *, R5900Context *ctx, Runtime *rt) { rt->events.push_back(hexEvent("trap", ctx->pc)); }
void bozo_unimplemented(uint8_t *, R5900Context *, Runtime *rt, uint32_t word)
{
    rt->events.push_back(hexEvent("unimplemented", word));
}

uint32_t bozo_cop0_read(R5900Context *, Runtime *rt, uint32_t reg) { return rt->cop0[reg & 31u]; }
void bozo_cop0_write(R5900Context *, Runtime *rt, uint32_t reg, uint32_t value) { rt->cop0[reg & 31u] = value; }
void bozo_set_interrupts(uint8_t *, R5900Context *, Runtime *rt, bool enabled) { rt->interruptsEnabled = enabled; }
bool bozo_cop0_condition(R5900Context *, Runtime *rt) { return rt->cop0Condition; }
void bozo_eret(uint8_t *, R5900Context *, Runtime *rt) { rt->events.push_back("eret"); }

uint8_t *bozo_vu0_memory(Runtime *rt) { return rt->vu0Memory; }
void bozo_vu0_callms(uint8_t *, R5900Context *, Runtime *rt, uint32_t address)
{
    rt->events.push_back(hexEvent("callms", address));
}
uint32_t bozo_vu0_control_read(R5900Context *, Runtime *rt, uint32_t reg) { return reg == 29 ? rt->vpuStat : 0u; }
void bozo_vu0_control_write(R5900Context *, Runtime *rt, uint32_t reg, uint32_t value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ctc2 %u 0x%x", reg, value);
    rt->events.push_back(buf);
}

uint8_t *bozo_memory_slow(uint8_t *rdram, Runtime *rt, uint32_t address, uint32_t size, bool)
{
    if (address >= 0x70000000u && address + size <= 0x70004000u)
        return rt->scratchpad + (address - 0x70000000u);
    const uint32_t phys = bozo::physicalAddress(address);
    if (phys + size <= bozo::kRamSize)
        return rdram + phys;
    return nullptr;
}
uint64_t bozo_io_read(R5900Context *, Runtime *rt, uint32_t address, uint32_t)
{
    rt->events.push_back(hexEvent("io-read", address));
    return 0;
}
void bozo_io_write(R5900Context *, Runtime *rt, uint32_t address, uint32_t, uint64_t)
{
    rt->events.push_back(hexEvent("io-write", address));
}
void bozo_io_read128(R5900Context *, Runtime *rt, uint32_t address, uint64_t out[2])
{
    rt->events.push_back(hexEvent("io-read", address));
    out[0] = out[1] = 0;
}
void bozo_io_write128(R5900Context *, Runtime *rt, uint32_t address, const uint64_t *)
{
    rt->events.push_back(hexEvent("io-write", address));
}
