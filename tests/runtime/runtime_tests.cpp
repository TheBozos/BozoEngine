// bozo-runtime-tests: the runtime core (bozo/runtime.h): the guest addresses with memory behind them, and the
// function table with its overrides looked up first.
#include "bozo/runtime.h"

#include <cstdio>
#include <memory>

namespace
{
    int g_failures = 0;
    void eq(const char *what, long long got, long long want)
    {
        if (got == want)
            return;
        ++g_failures;
        std::printf("FAIL %s: 0x%llx, expected 0x%llx\n", what, got, want);
    }

    int g_calls[3] = {};
    void fnA(uint8_t *, R5900Context *, Runtime *) { ++g_calls[0]; }
    void fnB(uint8_t *, R5900Context *, Runtime *) { ++g_calls[1]; }
    uint32_t g_overridePc = 0;
    void over(uint8_t *, R5900Context *ctx, Runtime *)
    {
        ++g_calls[2];
        g_overridePc = ctx->pc;
    }

    struct NoServices final : bozo::RuntimeServices
    {
        uint32_t unknown = 0;
        void unknownTarget(uint8_t *, R5900Context *, Runtime *, uint32_t t) override { unknown = t; }
        void yield(uint8_t *, R5900Context *, Runtime *) override {}
        void syscall(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        void breakpoint(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        void trap(uint8_t *, R5900Context *, Runtime *) override {}
        void unimplemented(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        void eret(uint8_t *, R5900Context *, Runtime *) override {}
        uint32_t cop0Read(R5900Context *, Runtime *, uint32_t) override { return 0; }
        void cop0Write(R5900Context *, Runtime *, uint32_t, uint32_t) override {}
        bool cop0Condition(R5900Context *, Runtime *) override { return false; }
        void vu0CallMicroprogram(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        uint32_t vu0ControlRead(R5900Context *, Runtime *, uint32_t) override { return 0; }
        void vu0ControlWrite(R5900Context *, Runtime *, uint32_t, uint32_t) override {}
        void ioRead(R5900Context *, Runtime *, uint32_t, uint32_t, uint64_t v[2]) override { v[0] = v[1] = 0; }
        void ioWrite(R5900Context *, Runtime *, uint32_t, uint32_t, const uint64_t[2]) override {}
    };
}

int main()
{
    using bozo::GuestMemory;
    // The flat space: RAM through every view, the scratchpad, VU0 data; nothing else.
    eq("RAM", GuestMemory::flatOffset(0x00123450u, 4), 0x123450);
    eq("KSEG0", GuestMemory::flatOffset(0x80123450u, 4), 0x123450);
    eq("KSEG1", GuestMemory::flatOffset(0xA0123450u, 4), 0x123450);
    eq("uncached", GuestMemory::flatOffset(0x20123450u, 4), 0x123450);
    eq("uncached accelerated", GuestMemory::flatOffset(0x30123450u, 4), 0x123450);
    eq("RAM end", GuestMemory::flatOffset(0x01FFFFFCu, 4), 0x1FFFFFC);
    eq("past RAM", GuestMemory::flatOffset(0x02000000u, 4), -1);
    eq("scratchpad", GuestMemory::flatOffset(0x70000010u, 16), 0x2000010);
    eq("scratchpad end", GuestMemory::flatOffset(0x70003FFCu, 8), -1);
    eq("VU0 data", GuestMemory::flatOffset(0x11004FF0u, 16), 0x2004FF0);
    eq("VU1 data (I/O)", GuestMemory::flatOffset(0x1100C000u, 4), -1);
    eq("a hardware register", GuestMemory::flatOffset(0x10009000u, 4), -1);
    eq("the BIOS", GuestMemory::flatOffset(0xBFC00000u, 4), -1);

    // Overrides first, then generated code, then the services.
    static const BozoFunctionEntry table[] = {{0x100000u, fnA}, {0x100100u, fnB}};
    auto rt = std::make_unique<Runtime>();
    NoServices services;
    rt->services = &services;
    rt->functions.setGenerated(table, 2);
    rt->functions.setOverride(0x100100u, over);
    R5900Context ctx;
    bozo_call_indirect(rt->memory.ram(), &ctx, rt.get(), 0x100000u);
    bozo_call_indirect(rt->memory.ram(), &ctx, rt.get(), 0x100100u);
    bozo_call_indirect(rt->memory.ram(), &ctx, rt.get(), 0x100200u);
    eq("generated", g_calls[0], 1);
    eq("overridden generated", g_calls[1], 0);
    eq("override", g_calls[2], 1);
    eq("override sees its address in pc", g_overridePc, 0x100100u);
    eq("unknown target to the services", services.unknown, 0x100200u);

    // ei / di toggle Status.EIE; mfc0 12 reads Status.
    bozo_set_interrupts(rt->memory.ram(), &ctx, rt.get(), true);
    eq("ei", bozo_cop0_read(&ctx, rt.get(), 12), 0x10000);
    bozo_set_interrupts(rt->memory.ram(), &ctx, rt.get(), false);
    eq("di", bozo_cop0_read(&ctx, rt.get(), 12), 0);

    std::printf("runtime tests: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
