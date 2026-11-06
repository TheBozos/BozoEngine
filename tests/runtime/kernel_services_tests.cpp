// Native EE kernel API tests with two synthetic thread descriptors; no game code.
#include "bozo/ee/dispatcher.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
struct TestHost final : bozo::ee::Host
{
    void libraryCall(bozo::ee::Dispatcher &, R5900Context *, uint32_t) override
    {
        throw std::runtime_error("unexpected library");
    }
    void vu0Microprogram(bozo::ee::Dispatcher &, R5900Context *, uint32_t) override
    {
        throw std::runtime_error("unexpected microprogram");
    }
    void ioRead(bozo::ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override
    {
        throw std::runtime_error("unexpected IO");
    }
    void ioWrite(bozo::ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override
    {
        throw std::runtime_error("unexpected IO");
    }
    void fault(bozo::ee::Dispatcher &, R5900Context *, const std::string &why) override
    {
        throw std::runtime_error(why);
    }
};
int main()
{
    TestHost host;
    bozo::ee::Dispatcher d(host);
    d.boot(0x100000, 0x1fff000);
    d.kernel().threadSetup(0x180000, 0x4000, 0x123456);
    const auto other = d.kernel().createThread(0x101000, 0x190000, 0x2000, 32, 0x654321, 7, 9);
    d.bindKernelCall(0x300000, bozo::ee::KernelCall::ReferThreadStatus);
    R5900Context ctx;
    const auto call = [&](uint32_t id, uint32_t out)
    {
        ctx.r[4].uw[0] = id;
        ctx.r[5].uw[0] = out;
        ctx.r[31].uw[0] = 0x100008;
        bozo_call_indirect(d.runtime().memory.ram(), &ctx, &d.runtime(), 0x300000);
        return ctx.r[2].sw[0];
    };
    uint32_t words[12] = {};
    if (call(0, 0x200000) != 1)
        return 1;
    std::memcpy(words, d.runtime().memory.ram() + 0x200000, sizeof(words));
    if (words[0] != 1 || words[4] != 0x123456)
        return 2;
    if (call(other, 0x200000) != other)
        return 3;
    std::memcpy(words, d.runtime().memory.ram() + 0x200000, sizeof(words));
    if (words[0] != 0x10 || words[1] != 0x101000 || words[2] != 0x190000 || words[3] != 0x2000 ||
        words[4] != 0x654321 || words[5] != 32 || words[7] != 7 || words[8] != 9)
        return 4;
    if (call(999, 0x200000) != -1)
        return 5;
    const uint32_t atZero = 0x5a5a5a5a;
    std::memcpy(d.runtime().memory.ram(), &atZero, 4);
    if (call(other, 0) != 0x10)
        return 11;
    std::memcpy(words, d.runtime().memory.ram(), 4);
    if (words[0] != atZero)
        return 12;
    try
    {
        call(0, 0x12000000);
        return 6;
    }
    catch (const std::runtime_error &)
    {
    }
    if (bozo::ee::kernelCallByName("SyncDCache") != bozo::ee::KernelCall::FlushCache)
        return 7;
    {
        using namespace bozo;
        R5900Context perf;
        auto &rt = d.runtime();
        const uint64_t start = d.cycle();
        d.cop0Write(&perf, &rt, kCop0Pcr0, 5);
        d.cop0Write(&perf, &rt, kCop0Pcr1, 50);
        d.setNow(&perf, start + 100);
        if (d.cop0Read(&perf, &rt, kCop0Pcr0) != 5 || d.cop0Read(&perf, &rt, kCop0Pcr1) != 50)
            return 9;
        d.cop0Write(&perf, &rt, kCop0Pccr, kPccrCountEnable | 0x3F8);
        if (d.cop0Read(&perf, &rt, kCop0Pccr) != (kPccrCountEnable | 0x3F8))
            return 10;
        d.setNow(&perf, start + 200);
        if (d.cop0Read(&perf, &rt, kCop0Pcr0) != 105 || d.cop0Read(&perf, &rt, kCop0Pcr1) != 150)
            return 11;
        d.cop0Write(&perf, &rt, kCop0Pcr0, 0);
        d.setNow(&perf, start + 210);
        if (d.cop0Read(&perf, &rt, kCop0Pcr0) != 10 || d.cop0Read(&perf, &rt, kCop0Pcr1) != 160)
            return 12;
        d.cop0Write(&perf, &rt, kCop0Pccr, 0x3F8);
        d.setNow(&perf, start + 300);
        if (d.cop0Read(&perf, &rt, kCop0Pcr0) != 10 || d.cop0Read(&perf, &rt, kCop0Pcr1) != 160)
            return 13;
        const uint32_t before = rt.cop0Status;
        d.cop0Write(&perf, &rt, kCop0ErrorEpc, 0x123450);
        d.cop0Write(&perf, &rt, kCop0Epc, 0x543210);
        rt.cop0Status = kStatusERL | kStatusEXL;
        d.eret(rt.memory.ram(), &perf, &rt);
        if (perf.pc != 0x123450 || rt.cop0Status != kStatusEXL)
            return 14;
        d.eret(rt.memory.ram(), &perf, &rt);
        if (perf.pc != 0x543210 || rt.cop0Status != 0)
            return 15;
        rt.cop0Status = before;
    }
    if (bozo::ee::kernelCallByName("iInvalidDCache") != bozo::ee::KernelCall::iFlushCache)
        return 8;
    if (bozo::ee::kernelCallByName("_InitSys") != bozo::ee::KernelCall::InitSys)
        return 9;
    return 0;
}
