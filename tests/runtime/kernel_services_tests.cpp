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
    if (bozo::ee::kernelCallByName("iInvalidDCache") != bozo::ee::KernelCall::iFlushCache)
        return 8;
    if (d.cop0Read(&ctx, &d.runtime(), 9) != static_cast<uint32_t>(d.cycle()))
        return 9;
    return 0;
}
