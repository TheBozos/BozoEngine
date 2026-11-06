// Native kernel services reached by syscall number, on synthetic guest state; no game code.
#include "bozo/ee/dispatcher.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace bozo::ee;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        if (!ok)
        {
            std::printf("FAILED: %s\n", what);
            ++g_failures;
        }
    }

    struct TestHost final : Host
    {
        std::string library;
        bool answersLibrary = true;
        uint32_t beginTarget = 0, beginKind = 0;
        void libraryCall(Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("unexpected library"); }
        void vu0Microprogram(Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("unexpected microprogram"); }
        void ioRead(Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override { throw std::runtime_error("unexpected IO"); }
        void ioWrite(Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override { throw std::runtime_error("unexpected IO"); }
        void kernelCallBegin(Dispatcher &, R5900Context *, uint32_t target, uint32_t kind) override
        {
            beginTarget = target;
            beginKind = kind;
        }
        bool librarySyscall(Dispatcher &, R5900Context *, const char *name) override
        {
            library = name;
            return answersLibrary;
        }
        void fault(Dispatcher &, R5900Context *, const std::string &why) override { throw std::runtime_error(why); }
    };

    struct Rig
    {
        TestHost host;
        Dispatcher d{host};
        R5900Context ctx;
        Rig() { d.boot(0x100000, 0x1FFF000); }
        int32_t sys(int32_t number, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0)
        {
            ctx.pc = 0x100000;
            ctx.r[3].sd[0] = number;
            ctx.r[4].sd[0] = static_cast<int32_t>(a0);
            ctx.r[5].sd[0] = static_cast<int32_t>(a1);
            ctx.r[6].sd[0] = static_cast<int32_t>(a2);
            ctx.r[31].sd[0] = 0x100100;
            d.syscall(d.runtime().memory.ram(), &ctx, &d.runtime(), 0);
            return ctx.r[2].sw[0];
        }
        void poke(uint32_t address, uint32_t value) { std::memcpy(d.runtime().memory.ram() + address, &value, 4); }
        uint32_t peek(uint32_t address)
        {
            uint32_t v = 0;
            std::memcpy(&v, d.runtime().memory.ram() + address, 4);
            return v;
        }
        uint64_t peek64(uint32_t address)
        {
            uint64_t v = 0;
            std::memcpy(&v, d.runtime().memory.ram() + address, 8);
            return v;
        }
        std::string faultOf(int32_t number)
        {
            try
            {
                sys(number);
            }
            catch (const std::runtime_error &e)
            {
                return e.what();
            }
            return {};
        }
    };

    void table()
    {
        check(kernelCallBySyscall(64) == KernelCall::CreateSema, "64 is CreateSema");
        check(kernelCallBySyscall(-67) == KernelCall::iSignalSema, "-67 is iSignalSema");
        check(kernelCallBySyscall(100) == KernelCall::FlushCache, "100 is FlushCache");
        check(kernelCallBySyscall(-104) == KernelCall::iFlushCache, "-104 is iFlushCache");
        check(kernelCallBySyscall(252) == KernelCall::SetAlarm, "252 is SetAlarm");
        check(kernelCallBySyscall(-254) == KernelCall::iSetAlarm, "-254 is iSetAlarm");
        check(kernelCallBySyscall(115) == KernelCall::SetVSyncFlag, "115 is SetVSyncFlag");
        check(kernelCallBySyscall(9999) == KernelCall::None, "9999 is nothing");
        check(std::string(librarySyscall(119)) == "sceSifSetDma", "119 is sceSifSetDma");
        check(std::string(librarySyscall(-118)) == "sceSifDmaStat", "-118 is sceSifDmaStat");
        check(!librarySyscall(64), "64 is no library");
        for (int32_t number : {1, 2, 16, 17, 18, 19, 20, 21, 22, 23, -26, -27, -28, -29, 32, 33, 34, 35, 36, 37, 41, 43, 47, -47, 48, -49,
                               50, 51, -52, 55, -56, 57, 62, 64, 65, 66, -67, 68, 69, 74, 75, 100, 111, 112, 113, 115, 127})
            check(kernelCallBySyscall(number) != KernelCall::None, "a number libkernel's wrappers use has a kernel call");
    }

    void unknownNumbers()
    {
        Rig r;
        check(r.faultOf(9999).find("syscall 9999") != std::string::npos, "an unknown number faults with the number");
        check(r.faultOf(-3).find("syscall -3") != std::string::npos, "a negative unknown number faults with the number");
        check(r.faultOf(13).find("syscall 13") != std::string::npos, "SetVTLBRefillHandler has no native service");
        check(r.faultOf(45).find("syscall 45") != std::string::npos, "a call not implemented names the number");
    }

    void threadsAndSemaphores()
    {
        Rig r;
        check(r.sys(60, 0x123456, 0x180000, 0x4000) == 0x184000, "SetupThread still answers");
        check(r.sys(47) == 1 && r.sys(-47) == 1, "GetThreadId by both numbers");
        check(r.host.beginKind == 3 && r.host.beginTarget == 0x100004, "the bracket is a syscall's");

        r.poke(0x200004, 5);
        r.poke(0x200008, 1);
        const int32_t sema = r.sys(64, 0x200000);
        check(sema > 0, "CreateSema");
        check(r.sys(69, static_cast<uint32_t>(sema)) == sema, "PollSema takes the unit");
        check(r.sys(69, static_cast<uint32_t>(sema)) < 0, "PollSema finds none");
        check(r.sys(66, static_cast<uint32_t>(sema)) == sema, "SignalSema");
        check(r.sys(-70, static_cast<uint32_t>(sema)) == sema, "iPollSema");
        check(r.sys(65, static_cast<uint32_t>(sema)) == sema, "DeleteSema");
    }

    void blockingResumesAfterTheWrapper()
    {
        for (bool wrapper : {true, false})
        {
            Rig r;
            r.poke(0x200004, 1);
            r.poke(0x200008, 0);
            const int32_t sema = r.sys(64, 0x200000);
            if (wrapper)
                r.poke(0x100004, 0x03E00008u);
            bool stopped = false;
            try
            {
                r.sys(68, static_cast<uint32_t>(sema));
            }
            catch (const StopGuest &)
            {
                stopped = true;
            }
            check(stopped, "WaitSema blocks through the syscall");
            check(r.ctx.pc == (wrapper ? 0x100100u : 0x100004u), "the thread goes on after the syscall");
            check(r.d.kernel().running() == 0, "no thread runs");
        }
    }

    void suspendAndResume()
    {
        Rig r;
        r.poke(0x200004, 0x101000);
        r.poke(0x200008, 0x190000);
        r.poke(0x20000C, 0x2000);
        r.poke(0x200014, 32);
        const int32_t id = r.sys(32, 0x200000);
        check(r.sys(34, static_cast<uint32_t>(id)) == id, "StartThread");
        check(r.d.kernel().thread(id)->state == ThreadState::Ready, "a started thread is ready");
        check(r.sys(57, static_cast<uint32_t>(id)) == -415, "ResumeThread of a thread not suspended");
        check(r.sys(55, static_cast<uint32_t>(id)) == id, "SuspendThread");
        check(r.d.kernel().thread(id)->state == ThreadState::Suspended, "suspended");
        check(r.d.kernel().firstReady() == 0, "it left the ready queue");
        check(r.sys(57, static_cast<uint32_t>(id)) == id, "ResumeThread");
        check(r.d.kernel().thread(id)->state == ThreadState::Ready, "ready again");
        check(r.sys(-56, static_cast<uint32_t>(id)) == id, "iSuspendThread");
        check(r.sys(-58, static_cast<uint32_t>(id)) == id, "iResumeThread");
        check(r.sys(55, 0) == -406 && r.sys(55, 1) == -406, "a thread does not suspend itself");
        check(r.sys(55, 999) == -407, "an unknown thread");
    }

    void imrHeapOsd()
    {
        Rig r;
        check(r.sys(112) == 0x7F00, "the IMR masks everything at boot");
        check(r.sys(113, 0xFF00) == 0x7F00, "GsPutIMR returns the old mask");
        check(r.sys(112) == 0xFF00 && r.sys(-112) == 0xFF00, "GsGetIMR reads it back");
        check(r.sys(-113, 0x7F00) == 0xFF00, "iGsPutIMR");
        check(r.sys(2, 1, 2, 1) == 0, "SetGsCrt answers");
        check(r.sys(127) == 0x2000000, "GetMemorySize");

        r.sys(60, 0x123456, 0x180000, 0x4000);
        check(r.sys(61, 0x500001, 0xFFFFFFFFu) == 0x500010, "SetupHeap aligns its start");
        check(r.sys(62) == 0x180000, "a heap of negative size ends at the main stack");
        r.sys(61, 0x500000, 0x1000);
        check(r.sys(62) == 0x501000, "a sized heap ends at its size");

        r.sys(75, 0x210000);
        check(r.peek(0x210000) == 0x00014010u, "GetOsdConfigParam: English, 4:3, version 2");
        r.poke(0x210000, 0x12345678u);
        r.sys(74, 0x210000);
        check(r.d.osdConfig == 0x12345678u, "SetOsdConfigParam");
        r.poke(0x210010, 0xAABBCCDDu);
        r.sys(110, 0x210010, 4, 8);
        r.sys(111, 0x210020, 4, 8);
        check(r.peek(0x210020) == 0xAABBCCDDu, "the OSD's second block round-trips");
        bool faulted = false;
        try
        {
            r.sys(111, 0x210020, 0x200, 0);
        }
        catch (const std::runtime_error &)
        {
            faulted = true;
        }
        check(faulted, "an access past the second block faults");
    }

    void alarms()
    {
        Rig r;
        Kernel &k = r.d.kernel();
        const uint64_t line = k.hblank();
        check(line == k.field() * 2 / 525, "a line of the NTSC field");
        check(r.sys(252, 3, 0x200000, 0x55) == 0, "the first alarm");
        check(k.nextEvent() == 3 * line, "due in horizontal blanks");
        check(r.sys(252, 2, 0x200100, 0x66) == 1, "the second alarm");
        check(k.nextEvent() == 2 * line, "the earlier one comes first");
        check(r.sys(253, 0) == 0, "ReleaseAlarm while pending");
        check(r.sys(253, 0) == -1, "ReleaseAlarm of nothing");
        check(r.sys(253, 64) == -1 && r.sys(253, 0xFFFFFFFFu) == -1, "ReleaseAlarm of no id");
        check(r.sys(-254, 2, 0x200200, 0x77) == 0, "iSetAlarm takes the free id");
        check(r.sys(-255, 0) == 0, "iReleaseAlarm");

        check(!k.handlerQueued(), "nothing queued before it is due");
        k.processEvents(2 * line);
        check(k.handlerQueued(), "the alarm's handler is queued when due");
        Frame &f = k.stackHandler(*k.thread(1));
        check(f.kind == kInvokeAlarm && f.pc == 0x200100, "an alarm handler");
        check(f.a[0] == 1 && f.a[1] == 2 && f.a[2] == 0x66, "handler(id, the count at due, arg)");
        check(r.sys(253, 1) == -1, "a fired alarm is no longer pending");

        for (int32_t i = 0; i < Kernel::kAlarms; ++i)
            check(r.sys(252, 100, 0x200000, 0) == i, "every id in turn");
        check(r.sys(252, 100, 0x200000, 0) == -1, "no alarm left");

        Rig pal;
        pal.d.kernel().fieldRate(50, 1);
        check(pal.d.kernel().hblank() == pal.d.kernel().field() * 2 / 625, "a line of the PAL field");
    }

    void frameBoundary()
    {
        Rig r;
        Kernel &k = r.d.kernel();
        r.poke(0x220000, 0);
        r.d.frameCompleted();
        check(r.peek(0x220000) == 0, "no flag registered, nothing written");
        check(r.sys(115, 0x220000, 0x220010) == 0, "SetVSyncFlag");
        check(r.peek(0x220000) == 0, "the call itself writes nothing");
        r.d.frameCompleted();
        check(r.peek(0x220000) == 1, "the frame boundary sets the flag");
        check(r.peek64(0x220010) == 0, "the CSR word: the field of an even frame");
        r.poke(0x220000, 0);
        r.d.frameCompleted();
        check(r.peek(0x220000) == 1, "again at the next one");
        check(r.peek64(0x220010) == (1u << 13), "the CSR word: the field of an odd frame");

        check(!k.handlerQueued(), "no callback set, nothing queued");
        check(r.d.setSyncVCallback(0x200300, 0x777) == 0, "no callback before");
        r.d.frameCompleted();
        check(k.handlerQueued(), "the frame boundary queues the callback");
        Frame &f = k.stackHandler(*k.thread(1));
        check(f.kind == kInvokeGsCallback && f.pc == 0x200300 && f.gp == 0x777 && f.a[0] == 2, "the callback's frame");
        k.endHandler(*k.thread(1));
        check(r.d.setSyncVCallback(0, 0) == 0x200300, "the old callback is returned");
        r.d.frameCompleted();
        check(!k.handlerQueued(), "a removed callback is not queued");
        k.processEvents(k.field() * 3);
        check(!k.handlerQueued(), "the field clock does not run it");
    }

    void librarySyscalls()
    {
        Rig r;
        r.sys(119);
        check(r.host.library == "sceSifSetDma", "119 goes to the host's library");
        r.sys(-118);
        check(r.host.library == "sceSifDmaStat", "-118 goes to the host's library");
        r.host.answersLibrary = false;
        check(r.faultOf(119).find("syscall 119") != std::string::npos, "no library answer faults with the number");
    }
}

int main()
{
    table();
    unknownNumbers();
    threadsAndSemaphores();
    blockingResumesAfterTheWrapper();
    suspendAndResume();
    imrHeapOsd();
    alarms();
    frameBoundary();
    librarySyscalls();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
