// Synthetic native-library fixtures: no retail code, firmware or interpreter.
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace
{
    void allocate(Env &e)
    {
        const uint32_t caller = e.cpu.gpr[31];
        e.cpu.gpr[4] = 0;
        e.cpu.gpr[5] = 64;
        e.cpu.gpr[31] = 0x1004;
        if (!e.call(0x1080, 0x1004))
            throw std::runtime_error("fixture native allocation blocked");
        e.w32(0x1010, e.cpu.gpr[2]);
        e.cpu.pc = caller;
    }
    void clockConversion(Env &e)
    {
        const uint32_t caller = e.cpu.gpr[31];
        e.w32(0x2020, 92160000);
        e.w32(0x2024, 0);
        e.cpu.gpr[4] = 0x2020;
        e.cpu.gpr[5] = 0x2028;
        e.cpu.gpr[6] = 0x202c;
        e.cpu.gpr[7] = 0x800000; // outside library RAM: the API has three pointer arguments, no fourth output
        e.cpu.gpr[31] = 0x2004;
        if (!e.call(0x2080, 0x2004) || e.r32(0x2028) != 2 || e.r32(0x202c) != 500000)
            throw std::runtime_error("native SDK clock conversion ABI");
        e.cpu.pc = caller;
    }
    void threadStatus(Env &e)
    {
        const uint32_t caller = e.cpu.gpr[31];
        const uint32_t descriptor[] = {0x02000000, 123, 0x3008, 0x1000, 32};
        for (unsigned i = 0; i < 5; ++i)
            e.w32(0x3040 + i * 4, descriptor[i]);
        e.cpu.gpr[4] = 0x3040;
        e.cpu.gpr[31] = 0x3004;
        if (!e.call(0x30c0, 0x3004))
            throw std::runtime_error("native CreateThread blocked");
        const uint32_t id = e.cpu.gpr[2];
        e.w32(0x3144, 0xfeed1234);
        e.cpu.gpr[4] = id;
        e.cpu.gpr[5] = 0x3100;
        e.cpu.gpr[31] = 0x3004;
        if (!e.call(0x30c8, 0x3004) || e.r32(0x3100) != 0x02000000 || e.r32(0x3104) != 123 || e.r32(0x3108) != 0x10 ||
            e.r32(0x310c) != 0x3008 || e.r32(0x3114) != 0x1000 || e.r32(0x3118) != 0x4000 || e.r32(0x311c) != 32 ||
            e.r32(0x3140) != 0 || e.r32(0x3144) != 0xfeed1234)
            throw std::runtime_error("native SDK thread status ABI");
        const uint32_t stackTop = e.r32(0x3110) + e.r32(0x3114);
        e.w32(0x3180, stackTop);
        e.w32(stackTop - 8, 0xabcddcba);
        e.cpu.pc = caller;
    }
    void stackProbe(Env &e)
    {
        e.w32(e.cpu.gpr[29] - 8, 0x55555555);
        if (e.r32(e.r32(0x3180) - 8) != 0xabcddcba)
            throw std::runtime_error("callback overwrote suspended task stack");
        e.cpu.pc = e.cpu.gpr[31];
    }
    int timerCallbacks = 0;
    uint32_t sdkCall(Env &e, uint32_t function, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0)
    {
        e.cpu.gpr[4] = a0;
        e.cpu.gpr[5] = a1;
        e.cpu.gpr[6] = a2;
        e.cpu.gpr[7] = a3;
        e.cpu.gpr[31] = 0x4004;
        if (!e.call(function, 0x4004))
            throw std::runtime_error("fixture SDK call blocked");
        return e.cpu.gpr[2];
    }
    void installTimers(Env &e)
    {
        const uint32_t caller = e.cpu.gpr[31];
        for (unsigned i = 0; i < 2; ++i)
        {
            const uint32_t id = sdkCall(e, 0x4100, 0, 0, 1);
            e.w32(0x4080 + i * 4, id);
            sdkCall(e, 0x4110, 16 + id, 0, 0x4040, id);
            sdkCall(e, 0x4118, 16 + id);
            sdkCall(e, 0x4120, id, 100);
            sdkCall(e, 0x4128, id, 1);
        }
        e.cpu.pc = caller;
    }
    void cancelTimers(Env &e)
    {
        const uint32_t caller = e.cpu.gpr[31];
        ++timerCallbacks;
        sdkCall(e, 0x4108, e.r32(0x4080)); // cancel this callback's own timer
        sdkCall(e, 0x4108, e.r32(0x4084)); // cancel another callback due at the same time
        e.cpu.pc = caller;
    }
    const Entry timerEntries[] = {{0x4000, installTimers}, {0x4040, cancelTimers}};
    const Registration timerRegistration("timer.irx", 0xabcd, 0x4000, 0x200, timerEntries, 2);
    const Entry threadEntries[] = {{0x3000, threadStatus}};
    const Registration threadRegistration("thread.irx", 0x6789, 0x3000, 0x200, threadEntries, 1);
    const Entry probeEntries[] = {{0x3400, stackProbe}};
    const Registration probeRegistration("probe.irx", 0x9876, 0x3400, 0x100, probeEntries, 1);
    const Entry clockEntries[] = {{0x2000, clockConversion}};
    const Registration clockRegistration("clock.irx", 0x5678, 0x2000, 0x100, clockEntries, 1);
    const Entry entries[] = {{0x1000, allocate}};
    const Registration registration("fixture.irx", 0x1234, 0x1000, 0x100, entries, 1);
    Image fixture()
    {
        return {"fixture.irx",           0x1234, 0x1000, 0x1000, 0x2000, std::vector<uint8_t>(0x100),
                {{0x1080, "sysmem", 4}}, {},     false};
    }
    int failures = 0;
    void check(bool okay, const char *what)
    {
        if (!okay)
        {
            ++failures;
            std::fprintf(stderr, "FAIL %s\n", what);
        }
    }
    template <class F> void rejects(F fn, const char *what)
    {
        try
        {
            fn();
            check(false, what);
        }
        catch (const std::runtime_error &)
        {
        }
    }
} // namespace
int main()
{
    bozo::libs::Libraries a, b;
    CompiledLibraries first(a, 0x10000), second(b, 0x20000);
    first.addImage(fixture());
    second.addImage(fixture());
    rejects([&] { first.addImage(fixture()); }, "duplicate image cannot overwrite running library data");
    rejects([&] { CompiledLibraries invalid(a, 0x180000); }, "invalid native heap boundary");
    check(first.startImage("fixture.irx") == 1, "first module starts");
    check(second.startImage("fixture.irx") == 1, "second port independently starts");
    check(first.nativeCalls() == 1 && second.nativeCalls() == 1, "only linked native functions execute");
    check(first.allocateBuffer(64) == 0x10040, "first port native allocator");
    check(second.allocateBuffer(64) == 0x20040, "second port independent native allocator");
    check(first.startImage("fixture.irx") == 1 && first.nativeCalls() == 1, "module start is idempotent");
    rejects([&] { first.startImage("missing.irx"); }, "missing modules fail explicitly");
    const uint8_t value = 1;
    rejects([&] { first.writeBuffer(0x1f801000, &value, 1); }, "device accesses have no backing model");
    rejects([&] { first.writeBuffer(0x1fffff, &value, 2); }, "buffer writes are bounded");
    first.reset();
    check(first.startImage("fixture.irx") == 1, "native reset reloads initial data");
    check(first.allocateBuffer(64) == 0x10040, "reset shares allocator state across native and RPC allocations");
    {
        bozo::libs::Libraries l;
        CompiledLibraries host(l, 0x10000);
        auto image = fixture();
        image.entry = 0x1008;
        host.addImage(std::move(image));
        rejects([&] { host.startImage("fixture.irx"); }, "missing generated targets cannot interpret image bytes");
    }
    {
        bozo::libs::Libraries l;
        CompiledLibraries host(l, 0x10000);
        auto image = fixture();
        image.imports[0].library = "unsupported";
        host.addImage(std::move(image));
        rejects([&] { host.startImage("fixture.irx"); }, "unsupported imports cannot fall back");
    }
    {
        bozo::libs::Libraries l;
        CompiledLibraries host(l, 0x10000);
        Image image{"clock.irx", 0x5678, 0x2000, 0x2000, 0x3000, std::vector<uint8_t>(0x100), {{0x2080, "thbase", 40}},
                    {},          false};
        host.addImage(std::move(image));
        check(host.startImage("clock.irx") == 1, "SDK clock pointer arguments");
    }
    {
        bozo::libs::Libraries l;
        CompiledLibraries host(l, 0x10000);
        Image image{"thread.irx",
                    0x6789,
                    0x3000,
                    0x3000,
                    0x4000,
                    std::vector<uint8_t>(0x200),
                    {{0x30c0, "thbase", 4}, {0x30c8, "thbase", 22}},
                    {},
                    false};
        host.addImage(std::move(image));
        check(host.startImage("thread.irx") == 1, "SDK thread status layout and bounds");
        Image probe{"probe.irx", 0x9876, 0x3400, 0x3400, 0x4000, std::vector<uint8_t>(0x100), {}, {}, false};
        host.addImage(std::move(probe));
        check(host.startImage("probe.irx") == 2, "callback stack isolated from native tasks");
    }
    {
        bozo::libs::Libraries l;
        CompiledLibraries host(l, 0x10000);
        Image image{"timer.irx",
                    0xabcd,
                    0x4000,
                    0x4000,
                    0x5000,
                    std::vector<uint8_t>(0x200),
                    {{0x4100, "timrman", 4},
                     {0x4108, "timrman", 6},
                     {0x4110, "intrman", 4},
                     {0x4118, "intrman", 6},
                     {0x4120, "timrman", 11},
                     {0x4128, "timrman", 7}},
                    {},
                    false};
        host.addImage(std::move(image));
        check(host.startImage("timer.irx") == 1, "native timers start");
        host.advanceTo(0);
        host.advanceTo(8000);
        host.advanceTo(16000);
        check(timerCallbacks == 1, "callback can cancel itself and another due timer without stale dispatch");
    }
    return failures ? 1 : 0;
}
