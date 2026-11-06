// Native timer library and DelayThread on synthetic guest state; no game code.
#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/recomp.h"

#include <cstdio>
#include <stdexcept>

using namespace bozo;

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

    constexpr uint64_t kBusHz = 147456000;
    constexpr uint32_t kBadId = 0x80008002u, kBusy = 0x80000010u, kNoHandler = 0x80000016u, kNoCounter = 0x80008005u;
    constexpr uint32_t kNotInitialized = 0x80008001u;

    struct TestHost final : ee::Host
    {
        void libraryCall(ee::Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("library"); }
        void vu0Microprogram(ee::Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("VU"); }
        void ioRead(ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override { throw std::runtime_error("IO"); }
        void ioWrite(ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override { throw std::runtime_error("IO"); }
        void fault(ee::Dispatcher &, R5900Context *, const std::string &why) override { throw std::runtime_error(why); }
    };

    struct Rig
    {
        TestHost host;
        ee::Dispatcher d{host};
        libs::Libraries l;
        R5900Context ctx;
        Rig()
        {
            l.attach(d);
            d.boot(0x100000, 0x1FFF000);
            ctx.r[28].sd[0] = 0x123456;
        }
        libs::TimerState &t() { return l.timers; }
        ee::Kernel &k() { return d.kernel(); }
        void at(uint64_t cycle) { d.setNow(&ctx, cycle); }
        uint64_t call(const char *name, std::initializer_list<uint64_t> args)
        {
            int i = 0;
            for (uint64_t a : args)
                ctx.r[4 + i++].ud[0] = a;
            libs::Guest g(d.runtime(), &ctx);
            l.find(name)->handler(l, g);
            return ctx.r[2].ud[0];
        }
        uint32_t call32(const char *name, std::initializer_list<uint64_t> args)
        {
            return static_cast<uint32_t>(call(name, args));
        }
        // Takes the events due by `cycle`, runs the handler that came due and returns `result` from it.
        ee::Frame &due(uint64_t cycle)
        {
            at(cycle);
            k().processEvents(cycle);
            check(k().handlerQueued(), "a handler came due");
            return k().stackHandler(*k().thread(1));
        }
        void end(ee::Frame &f, uint64_t result)
        {
            f.ctx.r[2].ud[0] = result;
            ee::Thread &thread = *k().thread(1);
            if (auto run = l.takeHandlerEnd(f.sequence))
            {
                libs::Guest g(d.runtime(), &f.ctx);
                run(l, g);
            }
            k().endHandler(thread);
        }
    };

    struct Done
    {
    };

    struct RunHost final : ee::Host
    {
        libs::Libraries l;
        const char *call = nullptr;
        uint64_t resumedAt = 0, idled = 0;
        unsigned handlerCalls = 0;
        void libraryCall(ee::Dispatcher &d, R5900Context *c, uint32_t) override
        {
            const int32_t self = d.kernel().running();
            const uint32_t ra = c->r[31].uw[0];
            libs::Guest g(d.runtime(), c);
            l.find(call)->handler(l, g);
            if (d.kernel().running() != self)
                d.stop(c, ra);
            else
                c->pc = ra;
        }
        void idle(ee::Dispatcher &, uint64_t from, uint64_t to) override { idled += to - from; }
        void handlerCompleted(ee::Dispatcher &d, ee::Thread &t, const ee::Frame &f) override
        {
            if (auto run = l.takeHandlerEnd(f.sequence))
            {
                libs::Guest g(d.runtime(), &t.top().ctx);
                run(l, g);
            }
        }
        void threadEnded(ee::Dispatcher &, ee::Thread &) override { throw Done{}; }
        void vu0Microprogram(ee::Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("VU"); }
        void ioRead(ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override { throw std::runtime_error("IO"); }
        void ioWrite(ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override { throw std::runtime_error("IO"); }
        void fault(ee::Dispatcher &, R5900Context *, const std::string &why) override { throw std::runtime_error(why); }
    };

    RunHost &runHost(Runtime *rt) { return dynamic_cast<RunHost &>(static_cast<ee::Dispatcher *>(rt->services)->host()); }

    void delayingThread(uint8_t *ram, R5900Context *c, Runtime *rt)
    {
        if (c->pc == 0x100000)
        {
            c->r[4].uw[0] = 2000;
            c->r[31].uw[0] = 0x100008;
            bozo_call_indirect(ram, c, rt, 0x300000);
            if (bozo::checkpoint(ram, c, rt, 8, 0x100008))
                return;
        }
        runHost(rt).resumedAt = static_cast<ee::Dispatcher *>(rt->services)->now(c);
        c->pc = 0;
    }

    void delayRuns()
    {
        RunHost host;
        host.call = "DelayThread";
        ee::Dispatcher d(host);
        host.l.attach(d);
        const BozoFunctionEntry f[] = {{0x100000, delayingThread}};
        const BozoResumeEntry resume[] = {{0x100000, 0x100010, delayingThread}};
        d.setFunctions(f, 1, resume, 1);
        d.bindLibrary(0x300000);
        d.boot(0x100000, 0x1FFF000);
        try
        {
            d.run();
            check(false, "the thread ends");
        }
        catch (const Done &)
        {
        }
        check(host.resumedAt >= 2 * 294912, "it went on only after the delay");
        check(host.resumedAt < 2 * 294912 + 1000, "and right after it");
        check(host.idled > 2 * 294000, "the scheduler was idle meanwhile");
    }

    void alarmHandler(uint8_t *, R5900Context *c, Runtime *rt)
    {
        RunHost &host = runHost(rt);
        check(c->r[7].uw[0] == 0xABC, "the handler's arg");
        ++host.handlerCalls;
        c->r[2].ud[0] = host.handlerCalls < 3 ? 0x10000 : 0;
        c->pc = 0;
    }

    void alarmThread(uint8_t *ram, R5900Context *c, Runtime *rt)
    {
        if (c->pc == 0x100000)
        {
            c->r[4].ud[0] = 1000;
            c->r[5].uw[0] = 0x155000;
            c->r[6].uw[0] = 0xABC;
            c->r[31].uw[0] = 0x100008;
            bozo_call_indirect(ram, c, rt, 0x300000);
            if (bozo::checkpoint(ram, c, rt, 8, 0x100008))
                return;
        }
        while (runHost(rt).handlerCalls < 3)
            if (bozo::checkpoint(ram, c, rt, 4096, 0x100008))
                return;
        runHost(rt).resumedAt = static_cast<ee::Dispatcher *>(rt->services)->now(c);
        c->pc = 0;
    }

    void alarmRuns()
    {
        RunHost host;
        host.call = "SetTimerAlarm";
        ee::Dispatcher d(host);
        host.l.attach(d);
        const BozoFunctionEntry f[] = {{0x100000, alarmThread}, {0x155000, alarmHandler}};
        const BozoResumeEntry resume[] = {{0x100000, 0x100010, alarmThread}, {0x155000, 0x155010, alarmHandler}};
        d.setFunctions(f, 2, resume, 2);
        d.bindLibrary(0x300000);
        d.boot(0x100000, 0x1FFF000);
        try
        {
            d.run();
            check(false, "the thread ends");
        }
        catch (const Done &)
        {
        }
        check(host.handlerCalls == 3, "the handler ran until it returned 0");
        check(host.resumedAt >= 2000 + 2 * 2 * 0x10000, "each call after its period");
        check(host.l.timers.used == 0, "and the alarm's counter is free");
    }

    void conversions()
    {
        Rig r;
        check(r.call("TimerUSec2BusClock", {2, 500000}) == 2 * kBusHz + kBusHz / 2, "seconds and microseconds to clocks");
        r.call("TimerBusClock2USec", {3 * kBusHz + kBusHz / 4, 0x100000, 0x100004});
        libs::Guest g(r.d.runtime(), &r.ctx);
        check(g.read32(0x100000) == 3 && g.read32(0x100004) == 250000, "clocks to seconds and microseconds");
        r.call("TimerBusClock2Freq", {kBusHz / 100});
        check(r.ctx.f[0] == 100.0f, "clocks to a frequency");
        r.ctx.f[12] = 1000.0f;
        check(r.call("TimerFreq2BusClock", {}) == kBusHz / 1000, "a frequency to clocks");
    }

    void systemTime()
    {
        Rig r;
        r.at(2000);
        check(r.call("GetTimerSystemTime", {}) == 1000, "bus clocks are half the EE clock");
        check(r.call("iGetTimerSystemTime", {}) == 1000, "the i form");
        check(r.call32("GetTimerPreScaleFactor", {}) == 2, "the factor _InitSys sets");
        check(r.call32("StartTimerSystemTime", {}) == 1, "already running");
        check(r.call32("StopTimerSystemTime", {}) == 1, "was running");
        check(r.call32("StopTimerSystemTime", {}) == 0, "was stopped");
        r.at(9000);
        check(r.call("GetTimerSystemTime", {}) == 1000, "held while stopped");
        check(r.call32("StartTimerSystemTime", {}) == 0, "started");
        r.at(9200);
        check(r.call("GetTimerSystemTime", {}) == 1100, "goes on from where it stopped");

        check(r.call32("InitTimer", {2}) == kNotInitialized, "InitTimer on an initialized library");
        const uint32_t id = r.call32("AllocTimerCounter", {});
        check(r.call32("EndTimer", {}) == kBusy, "EndTimer with a counter in use");
        check(r.call32("FreeTimerCounter", {id}) == 0, "freed");
        check(r.call32("EndTimer", {}) == 0, "EndTimer");
        check(r.call32("EndTimer", {}) == kNotInitialized, "EndTimer twice");
        check(r.call32("GetTimerPreScaleFactor", {}) == kNotInitialized, "the factor of an ended library");
        check(r.call32("GetTimerUsedUnusedCounters", {0, 0}) == kNotInitialized, "the counters of an ended library");
        check(r.call32("InitTimer", {1}) == 0, "InitTimer");
        check(r.call32("GetTimerPreScaleFactor", {}) == 1, "the factor given");
    }

    void counters()
    {
        Rig r;
        const uint32_t id = r.call32("AllocTimerCounter", {});
        check((id & 1) && static_cast<int32_t>(id) > 0, "a counter's id");
        check(r.call32("iAllocTimerCounter", {}) != id, "another counter");
        r.call("GetTimerUsedUnusedCounters", {0x100000, 0x100004});
        libs::Guest g(r.d.runtime(), &r.ctx);
        check(g.read32(0x100000) == 2 && g.read32(0x100004) == 126, "used and unused");

        for (const char *name : {"StartTimerCounter", "StopTimerCounter", "FreeTimerCounter"})
            check(r.call32(name, {id ^ 2}) == kBadId, name);
        check(r.call("GetTimerCount", {id ^ 2}) == ~0ull, "the count of no counter");
        check(r.call("GetTimerBaseTime", {id ^ 2}) == ~0ull, "the base of no counter");
        check(r.call("SetTimerCount", {id ^ 2, 0}) == ~0ull, "setting no counter");
        check(r.call32("SetTimerHandler", {id ^ 2, 0, 0x300000, 0}) == kBadId, "a handler on no counter");

        check(r.call("GetTimerCount", {id}) == 0, "stopped at 0");
        r.at(1000);
        check(r.call32("StopTimerCounter", {id}) == 0, "was stopped");
        check(r.call32("StartTimerCounter", {id}) == 0, "started");
        check(r.call32("StartTimerCounter", {id}) == 1, "already started");
        check(r.call("GetTimerBaseTime", {id}) == 500, "started at this system time");
        r.at(1600);
        check(r.call("GetTimerCount", {id}) == 300, "counts bus clocks");
        check(r.call32("StopTimerCounter", {id}) == 1, "was running");
        check(r.call("GetTimerBaseTime", {id}) == 0, "no base when stopped");
        r.at(5000);
        check(r.call("GetTimerCount", {id}) == 300, "held");
        check(r.call("SetTimerCount", {id, 0x100000000ull}) == 300, "the old count");
        check(r.call("GetTimerCount", {id}) == 0x100000000ull, "a 64-bit count");
        check(r.call32("FreeTimerCounter", {id}) == 0, "freed");
        check(r.call32("FreeTimerCounter", {id}) == kBadId, "freed once");
        for (uint32_t i = 0; i < 127; ++i)
            check(r.call32("AllocTimerCounter", {}) != kNoCounter, "room left");
        check(r.call32("AllocTimerCounter", {}) == kNoCounter, "128 counters");
    }

    void handlers()
    {
        Rig r;
        const uint64_t compare = 0x100000000ull + 50;
        const uint32_t id = r.call32("AllocTimerCounter", {});
        check(r.call32("SetTimerHandler", {id, compare, 0x300000, 0x77}) == 0, "a handler");
        check(!r.k().handlerQueued() && r.k().nextEvent() == r.k().nextVblankStart(), "a stopped counter has no deadline");
        r.call("StartTimerCounter", {id});
        check(r.k().nextEvent() == r.k().nextVblankStart(), "due after the next field");
        r.call("SetTimerCount", {id, 0x100000000ull});
        check(r.k().nextEvent() == 100, "the deadline follows the count");

        ee::Frame &f = r.due(100);
        check(f.kind == ee::kInvokeAlarm && f.pc == 0x300000 && f.gp == 0x123456, "the handler's frame");
        check(f.ctx.r[4].uw[0] == id, "handler(id,");
        check(f.ctx.r[5].ud[0] == compare, "the compare, 64 bits,");
        check(f.ctx.r[6].ud[0] == compare, "the count, 64 bits,");
        check(f.ctx.r[7].uw[0] == 0x77, "arg)");
        check(r.call32("StopTimerCounter", {id}) == kBusy, "a handler's own counter is refused");
        check(r.call32("FreeTimerCounter", {id}) == kBusy, "and cannot be freed");
        check(r.call("SetTimerCount", {id, 0}) == ~0ull, "or set");
        r.end(f, 0x10000);
        check(r.k().nextEvent() == 100 + 0x20000, "the result is the next period");

        ee::Frame &f2 = r.due(100 + 0x20000);
        check(f2.ctx.r[5].ud[0] == compare + 0x10000, "the next compare");
        r.end(f2, 1);
        check(r.k().nextEvent() == 100 + 0x20000 + 0x3999 * 2, "the shortest period");
        ee::Frame &f3 = r.due(100 + 0x20000 + 0x3999 * 2);
        r.end(f3, 0);
        check(r.k().nextEvent() == r.k().nextVblankStart(), "0: no more calls");
        check(r.call("GetTimerCount", {id}) != ~0ull, "the counter stays");

        r.call("SetTimerHandler", {id, r.call("GetTimerCount", {id}) + 10, 0x300000, 0});
        ee::Frame &f4 = r.due(r.k().nextEvent());
        r.end(f4, ~0ull);
        check(r.call32("FreeTimerCounter", {id}) == kBadId, "-1: the counter is freed");

        const uint32_t cleared = r.call32("AllocTimerCounter", {});
        r.call("StartTimerCounter", {cleared});
        r.call("SetTimerHandler", {cleared, 1000000, 0x300000, 0});
        check(r.k().nextEvent() < r.k().nextVblankStart() + 1, "armed");
        r.call("SetTimerHandler", {cleared, 0, 0, 0});
        check(r.k().nextEvent() == r.k().nextVblankStart(), "no handler, no deadline");

        Rig stopped;
        const uint32_t held = stopped.call32("AllocTimerCounter", {});
        stopped.call("SetTimerHandler", {held, 100, 0x300000, 0});
        stopped.call("StartTimerCounter", {held});
        const uint64_t was = stopped.k().nextEvent();
        stopped.call("StopTimerSystemTime", {});
        check(stopped.k().nextEvent() != was, "no deadline while system time is stopped");
        stopped.at(1000);
        stopped.call("StartTimerSystemTime", {});
        check(stopped.k().nextEvent() == 1200, "re-armed with what was left");
    }

    void alarms()
    {
        Rig r;
        check(r.call32("SetTimerAlarm", {100, 0, 0}) == kNoHandler, "an alarm with no handler");
        r.at(1000);
        const uint32_t id = r.call32("SetTimerAlarm", {100, 0x300000, 0x55});
        check(static_cast<int32_t>(id) > 0 && (id & 1), "an alarm's id");
        check(r.k().nextEvent() == 1200, "due in bus clocks");
        ee::Frame &f = r.due(1200);
        check(f.ctx.r[4].uw[0] == id && f.ctx.r[5].ud[0] == 100 && f.ctx.r[6].ud[0] == 100 && f.ctx.r[7].uw[0] == 0x55,
              "handler(alarm id, the time, the count, arg)");
        r.end(f, 0x10000);
        check(r.k().nextEvent() == 1200 + 0x20000, "nonzero repeats");
        ee::Frame &f2 = r.due(1200 + 0x20000);
        r.end(f2, 0);
        check(r.call32("ReleaseTimerAlarm", {id}) == kBadId, "0 ends it");
        r.call("GetTimerUsedUnusedCounters", {0x100000, 0});
        libs::Guest g(r.d.runtime(), &r.ctx);
        check(g.read32(0x100000) == 0, "its counter is free");

        const uint32_t second = r.call32("iSetTimerAlarm", {100, 0x300000, 0});
        check(r.call32("ReleaseTimerAlarm", {second ^ 0x10}) == kBadId, "another alarm's serial");
        check(r.call32("ReleaseTimerAlarm", {0xFFFFFFFFu}) == kBadId, "a negative id");
        check(r.call32("iReleaseTimerAlarm", {second}) == 0, "released");
        check(r.call32("ReleaseTimerAlarm", {second}) == kBadId, "once");
        check(r.k().nextEvent() == r.k().nextVblankStart(), "and it never fires");

        for (uint32_t i = 0; i < libs::TimerState::kAlarms; ++i)
            check(r.call32("SetTimerAlarm", {1000000, 0x300000, 0}) != kNoCounter, "alarms left");
        check(r.call32("SetTimerAlarm", {1000000, 0x300000, 0}) == kNoCounter, "64 alarms");
    }

    void delays()
    {
        Rig r;
        ee::Kernel &k = r.k();
        const int32_t self = k.running();
        check(self == 1, "the main thread runs");
        r.ctx.r[2].ud[0] = 5;
        r.at(1000);
        r.call("DelayThread", {1000});
        check(k.thread(1)->state == ee::ThreadState::Waiting, "the thread waits");
        check(k.nextEvent() == 1000 + 294912, "a millisecond of the EE clock");
        k.processEvents(1000 + 294911);
        check(k.thread(1)->state == ee::ThreadState::Waiting, "still waiting just before");
        k.processEvents(1000 + 294912);
        check(k.thread(1)->state != ee::ThreadState::Waiting, "woken at the deadline");
        check(k.thread(1)->top().ctx.r[2].sd[0] == 0, "returns 0");
        check(!k.handlerQueued(), "no handler ran for it");
    }

    void delayInHandler()
    {
        Rig r;
        ee::Kernel &k = r.k();
        ee::QueuedHandler h;
        h.pc = 0x300000;
        k.queue(h);
        k.stackHandler(*k.thread(1));
        check(r.call32("DelayThread", {1000}) == 0x80008008u, "no delay in a handler");
        check(k.thread(1)->state != ee::ThreadState::Waiting, "and no wait");
    }

    void hostDeadlines()
    {
        Rig r;
        ee::Kernel &k = r.k();
        int64_t host = 5'000'000;
        r.t().hostClock = [&host] { return host; };
        check(r.t().idleHold(0, 1000) == 0, "nothing pending, no hold");
        r.call("DelayThread", {1000});
        const uint64_t due = k.nextEvent();
        check(r.t().idleHold(0, due) == 1'000'000, "held for the delay when idle");
        check(r.t().idleHold(0, due / 2) == 500'000, "up to the next event");
        k.processEvents(due);
        check(k.thread(1)->state == ee::ThreadState::Waiting, "the EE clock alone does not end it");
        check(k.nextEvent() == due + 294912, "due again by what the host has left");
        host += 1'000'000;
        check(r.t().idleHold(due, due + 294912) == 0, "no hold once the host is there");
        k.processEvents(k.nextEvent());
        check(k.thread(1)->state != ee::ThreadState::Waiting, "woken on the host's deadline");

        Rig a;
        int64_t clock = 0;
        a.t().hostClock = [&clock] { return clock; };
        a.call("SetTimerAlarm", {kBusHz / 1000, 0x300000, 0});
        const uint64_t alarm = a.k().nextEvent();
        check(a.t().idleHold(0, alarm) == 1'000'000, "an alarm holds as well");
        a.k().processEvents(alarm);
        check(!a.k().handlerQueued(), "and waits for the host");
        clock = 1'000'000;
        a.k().processEvents(a.k().nextEvent());
        check(a.k().handlerQueued(), "fires on the host's deadline");
    }
}

int main()
{
    try
    {
        conversions();
        systemTime();
        counters();
        handlers();
        alarms();
        delays();
        delayInHandler();
        hostDeadlines();
        delayRuns();
        alarmRuns();
    }
    catch (const std::exception &e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    if (g_failures)
        return 1;
    std::puts("timer library: ok");
    return 0;
}
