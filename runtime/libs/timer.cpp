// libkernel's timer library and DelayThread (libraries.md section 16).
#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"

#include <algorithm>

namespace bozo::libs
{
    namespace
    {
        constexpr uint64_t kBusHz = 147'456'000ull, kEeHz = 2 * kBusHz;
        constexpr uint32_t kCounterKeys = 0x81000000u, kDelayKeys = 0x82000000u;
        constexpr uint32_t kNotInitialized = 0x80008001u, kBadId = 0x80008002u, kNoCounter = 0x80008005u,
                           kInHandlerContext = 0x80008008u, kBusy = 0x80000010u, kNoHandler = 0x80000016u;
        constexpr uint64_t kMinimumPeriod = 0x3999;

        void ret64(Guest &g, uint64_t v) { g.context()->r[2].ud[0] = v; }

        uint64_t busNow(const TimerState &t, uint64_t cycle)
        {
            return t.systemRunning ? (cycle - t.systemStart) / 2 : t.systemStopped;
        }

        uint64_t countOf(const TimerState &t, const TimerState::Counter &c, uint64_t cycle)
        {
            return c.count + (c.running ? busNow(t, cycle) - c.start : 0);
        }

        int32_t idOf(uint32_t slot, const TimerState::Counter &c)
        {
            return static_cast<int32_t>(((slot + 1) << 10) | c.tag);
        }

        int32_t slotOf(const TimerState &t, int32_t id)
        {
            if (id < 0)
                return -1;
            const uint32_t slot = (static_cast<uint32_t>(id) >> 10) - 1;
            if (slot >= TimerState::kCounters || t.counters[slot].tag == 0 ||
                t.counters[slot].tag != (static_cast<uint32_t>(id) & 0x3FFu))
                return -1;
            return static_cast<int32_t>(slot);
        }

        void arm(Libraries &l, uint32_t slot, uint64_t cycle);

        void fire(Libraries &l, uint32_t slot, uint32_t tag, uint64_t due)
        {
            TimerState &t = l.timers;
            TimerState::Counter &c = t.counters[slot];
            if (c.tag != tag || !c.armed || !c.running)
                return;
            if (t.hostClock && c.hostDue > t.hostClock())
            {
                const auto left = static_cast<uint64_t>(c.hostDue - t.hostClock());
                l.dispatcher()->kernel().scheduleNative(kCounterKeys | slot, due + left * (kEeHz / 1000) / 1000000,
                                                        [&l, slot, tag](uint64_t at) { fire(l, slot, tag, at); });
                return;
            }
            const int32_t id = idOf(slot, c);
            const uint64_t count = countOf(t, c, due);
            ee::QueuedHandler h;
            h.kind = ee::kInvokeAlarm;
            h.pc = c.handler;
            h.a[0] = c.alarm >= 0 ? ((static_cast<uint32_t>(c.alarm) + 1) << 8) | (c.tag & 0xFEu) | 1u
                                  : static_cast<uint32_t>(id);
            h.a[1] = static_cast<uint32_t>(c.compare);
            h.aHigh[1] = static_cast<uint32_t>(c.compare >> 32);
            h.a[2] = static_cast<uint32_t>(count);
            h.aHigh[2] = static_cast<uint32_t>(count >> 32);
            h.a[3] = c.arg;
            h.gp = c.gp;
            h.completion = true;
            t.inHandler = id;
            l.queueHandler(h, [slot, tag, id](Libraries &l2, Guest &g) {
                TimerState &s = l2.timers;
                if (s.inHandler == id)
                    s.inHandler = -1;
                TimerState::Counter &counter = s.counters[slot];
                if (counter.tag != tag)
                    return;
                const uint64_t r = g.context()->r[2].ud[0];
                if (r == ~0ull || (r == 0 && counter.alarm >= 0))
                {
                    if (counter.alarm >= 0)
                        s.alarms[static_cast<size_t>(counter.alarm)] = -1;
                    counter = {};
                    --s.used;
                }
                else if (r == 0)
                    counter.armed = false;
                else
                {
                    counter.compare += std::max(r, kMinimumPeriod);
                    arm(l2, slot, l2.now(g));
                }
            });
        }

        void arm(Libraries &l, uint32_t slot, uint64_t cycle)
        {
            TimerState &t = l.timers;
            TimerState::Counter &c = t.counters[slot];
            ee::Kernel &k = l.dispatcher()->kernel();
            k.cancelDeadline(kCounterKeys | slot);
            if (!c.running || !c.armed || !t.systemRunning)
                return;
            const uint64_t count = countOf(t, c, cycle);
            const uint64_t left = c.compare > count ? c.compare - count : 0;
            const uint32_t tag = c.tag;
            c.hostDue = t.hostClock ? t.hostClock() + static_cast<int64_t>(static_cast<double>(left) * 1e9 / kBusHz) : 0;
            k.scheduleNative(kCounterKeys | slot, cycle + left * 2,
                             [&l, slot, tag](uint64_t at) { fire(l, slot, tag, at); });
        }

        void release(Libraries &l, uint32_t slot)
        {
            TimerState &t = l.timers;
            TimerState::Counter &c = t.counters[slot];
            l.dispatcher()->kernel().cancelDeadline(kCounterKeys | slot);
            if (c.alarm >= 0)
                t.alarms[static_cast<size_t>(c.alarm)] = -1;
            c = {};
            --t.used;
        }

        int32_t allocate(TimerState &t)
        {
            for (uint32_t slot = 0; slot < TimerState::kCounters; ++slot)
                if (t.counters[slot].tag == 0)
                {
                    t.serial = (t.serial + 1) & 0x1FFu;
                    t.counters[slot].tag = (t.serial << 1) | 1u;
                    ++t.used;
                    return static_cast<int32_t>(slot);
                }
            return -1;
        }

        void initTimer(Libraries &l, Guest &g)
        {
            TimerState &t = l.timers;
            if (t.initialized)
            {
                g.retU(kNotInitialized);
                return;
            }
            t.initialized = true;
            t.prescale = g.arg(0) & 3u;
            t.systemRunning = false;
            t.systemStopped = 0;
            g.ret(0);
        }

        void endTimer(Libraries &l, Guest &g)
        {
            TimerState &t = l.timers;
            if (!t.initialized)
                g.retU(kNotInitialized);
            else if (t.used)
                g.retU(kBusy);
            else
            {
                t.initialized = false;
                t.systemRunning = false;
                g.ret(0);
            }
        }

        void preScaleFactor(Libraries &l, Guest &g)
        {
            if (l.timers.initialized)
                g.ret(static_cast<int32_t>(l.timers.prescale));
            else
                g.retU(kNotInitialized);
        }

        void startSystemTime(Libraries &l, Guest &g)
        {
            TimerState &t = l.timers;
            if (t.systemRunning)
            {
                g.ret(1);
                return;
            }
            const uint64_t cycle = l.now(g);
            t.systemStart = cycle - t.systemStopped * 2;
            t.systemRunning = true;
            for (uint32_t slot = 0; slot < TimerState::kCounters; ++slot)
                if (t.counters[slot].tag)
                    arm(l, slot, cycle);
            g.ret(0);
        }

        void stopSystemTime(Libraries &l, Guest &g)
        {
            TimerState &t = l.timers;
            if (!t.systemRunning)
            {
                g.ret(0);
                return;
            }
            t.systemStopped = busNow(t, l.now(g));
            t.systemRunning = false;
            for (uint32_t slot = 0; slot < TimerState::kCounters; ++slot)
                l.dispatcher()->kernel().cancelDeadline(kCounterKeys | slot);
            g.ret(1);
        }

        void systemTime(Libraries &l, Guest &g) { ret64(g, busNow(l.timers, l.now(g))); }

        void allocCounter(Libraries &l, Guest &g)
        {
            const int32_t slot = allocate(l.timers);
            if (slot < 0)
                g.retU(kNoCounter);
            else
                g.ret(idOf(static_cast<uint32_t>(slot), l.timers.counters[static_cast<size_t>(slot)]));
        }

        // The checks every counter call makes: 0 and the slot, or the error returned.
        bool counterArg(Libraries &l, Guest &g, int32_t &slot)
        {
            const int32_t id = g.sarg(0);
            slot = slotOf(l.timers, id);
            if (slot < 0)
            {
                g.retU(kBadId);
                return false;
            }
            if (l.timers.inHandler == id)
            {
                g.retU(kBusy);
                return false;
            }
            return true;
        }

        void freeCounter(Libraries &l, Guest &g)
        {
            int32_t slot;
            if (!counterArg(l, g, slot))
                return;
            release(l, static_cast<uint32_t>(slot));
            g.ret(0);
        }

        void usedUnused(Libraries &l, Guest &g)
        {
            const TimerState &t = l.timers;
            if (!t.initialized)
            {
                g.retU(kNotInitialized);
                return;
            }
            if (g.arg(0))
                g.write32(g.arg(0), t.used);
            if (g.arg(1))
                g.write32(g.arg(1), TimerState::kCounters - t.used);
            g.ret(0);
        }

        void startCounter(Libraries &l, Guest &g)
        {
            int32_t slot;
            if (!counterArg(l, g, slot))
                return;
            TimerState::Counter &c = l.timers.counters[static_cast<size_t>(slot)];
            if (c.running)
            {
                g.ret(1);
                return;
            }
            const uint64_t cycle = l.now(g);
            c.start = busNow(l.timers, cycle);
            c.running = true;
            arm(l, static_cast<uint32_t>(slot), cycle);
            g.ret(0);
        }

        void stopCounter(Libraries &l, Guest &g)
        {
            int32_t slot;
            if (!counterArg(l, g, slot))
                return;
            TimerState::Counter &c = l.timers.counters[static_cast<size_t>(slot)];
            if (!c.running)
            {
                g.ret(0);
                return;
            }
            c.count = countOf(l.timers, c, l.now(g));
            c.running = false;
            l.dispatcher()->kernel().cancelDeadline(kCounterKeys | static_cast<uint32_t>(slot));
            g.ret(1);
        }

        void setCount(Libraries &l, Guest &g)
        {
            const int32_t id = g.sarg(0), slot = slotOf(l.timers, id);
            if (slot < 0 || l.timers.inHandler == id)
            {
                ret64(g, ~0ull);
                return;
            }
            TimerState::Counter &c = l.timers.counters[static_cast<size_t>(slot)];
            const uint64_t cycle = l.now(g);
            const uint64_t old = countOf(l.timers, c, cycle);
            c.count = g.arg64(1);
            c.start = busNow(l.timers, cycle);
            arm(l, static_cast<uint32_t>(slot), cycle);
            ret64(g, old);
        }

        void baseTime(Libraries &l, Guest &g)
        {
            const int32_t slot = slotOf(l.timers, g.sarg(0));
            if (slot < 0)
            {
                ret64(g, ~0ull);
                return;
            }
            const TimerState::Counter &c = l.timers.counters[static_cast<size_t>(slot)];
            ret64(g, c.running ? c.start - c.count : 0);
        }

        void getCount(Libraries &l, Guest &g)
        {
            const int32_t slot = slotOf(l.timers, g.sarg(0));
            ret64(g, slot < 0 ? ~0ull : countOf(l.timers, l.timers.counters[static_cast<size_t>(slot)], l.now(g)));
        }

        void setHandler(Libraries &l, Guest &g)
        {
            int32_t slot;
            if (!counterArg(l, g, slot))
                return;
            TimerState::Counter &c = l.timers.counters[static_cast<size_t>(slot)];
            c.handler = g.arg(2);
            c.armed = c.handler != 0;
            if (c.armed)
            {
                c.compare = g.arg64(1);
                c.arg = g.arg(3);
                c.gp = g.context()->r[28].uw[0];
            }
            arm(l, static_cast<uint32_t>(slot), l.now(g));
            g.ret(0);
        }

        void busClock2USec(Libraries &, Guest &g)
        {
            const uint64_t clocks = g.arg64(0);
            if (g.arg(1))
                g.write32(g.arg(1), static_cast<uint32_t>(clocks / kBusHz));
            if (g.arg(2))
                g.write32(g.arg(2), static_cast<uint32_t>((clocks % kBusHz) * 1000000 / kBusHz));
        }

        void uSec2BusClock(Libraries &, Guest &g)
        {
            ret64(g, g.arg(0) * kBusHz + g.arg(1) * kBusHz / 1000000);
        }

        void busClock2Freq(Libraries &, Guest &g)
        {
            g.context()->f[0] = static_cast<float>(kBusHz) / static_cast<float>(g.arg64(0));
        }

        void freq2BusClock(Libraries &, Guest &g)
        {
            ret64(g, static_cast<uint64_t>(static_cast<float>(kBusHz) / g.context()->f[12]));
        }

        void setAlarm(Libraries &l, Guest &g)
        {
            TimerState &t = l.timers;
            const uint32_t handler = g.arg(1);
            if (!handler)
            {
                g.retU(kNoHandler);
                return;
            }
            const auto free = std::find(t.alarms.begin(), t.alarms.end(), -1);
            const int32_t slot = free == t.alarms.end() ? -1 : allocate(t);
            if (slot < 0)
            {
                g.retU(kNoCounter);
                return;
            }
            const auto alarm = static_cast<uint32_t>(free - t.alarms.begin());
            *free = slot;
            TimerState::Counter &c = t.counters[static_cast<size_t>(slot)];
            c.alarm = static_cast<int32_t>(alarm);
            c.handler = handler;
            c.arg = g.arg(2);
            c.gp = g.context()->r[28].uw[0];
            c.compare = g.arg64(0);
            c.armed = true;
            const uint64_t cycle = l.now(g);
            c.start = busNow(t, cycle);
            c.running = true;
            arm(l, static_cast<uint32_t>(slot), cycle);
            g.ret(static_cast<int32_t>(((alarm + 1) << 8) | (c.tag & 0xFEu) | 1u));
        }

        void releaseAlarm(Libraries &l, Guest &g)
        {
            TimerState &t = l.timers;
            const int32_t id = g.sarg(0);
            const uint32_t alarm = (static_cast<uint32_t>(id) >> 8) - 1;
            if (id < 0 || alarm >= TimerState::kAlarms || t.alarms[alarm] < 0 ||
                ((t.counters[static_cast<size_t>(t.alarms[alarm])].tag & 0xFEu) | 1u) != (static_cast<uint32_t>(id) & 0xFFu))
            {
                g.retU(kBadId);
                return;
            }
            release(l, static_cast<uint32_t>(t.alarms[alarm]));
            g.ret(0);
        }

        void delayDue(Libraries &l, int32_t thread, uint64_t due)
        {
            TimerState &t = l.timers;
            ee::Kernel &k = l.dispatcher()->kernel();
            const auto d = t.delays.find(thread);
            if (d == t.delays.end())
                return;
            if (t.hostClock && d->second > t.hostClock())
            {
                const auto left = static_cast<uint64_t>(d->second - t.hostClock());
                k.scheduleNative(kDelayKeys | static_cast<uint32_t>(thread), due + left * (kEeHz / 1000) / 1000000,
                                 [&l, thread](uint64_t at) { delayDue(l, thread, at); });
                return;
            }
            t.delays.erase(d);
            const ee::Thread *waiting = k.thread(thread);
            if (waiting && waiting->waitReason == ee::kWaitDelay)
                k.wake(thread, 0, false, ee::kWaitDelay);
        }

        void delayThread(Libraries &l, Guest &g)
        {
            ee::Kernel &k = l.dispatcher()->kernel();
            const int32_t thread = k.running();
            const ee::Thread *t = k.thread(thread);
            if (!t || t->depth() != 0)
            {
                g.retU(kInHandlerContext);
                return;
            }
            const uint64_t microseconds = g.arg(0);
            l.timers.delays[thread] =
                l.timers.hostClock ? l.timers.hostClock() + static_cast<int64_t>(microseconds * 1000) : 0;
            k.block(thread, ee::kWaitDelay, 0, 0, 0, false, 0, 0);
            k.scheduleNative(kDelayKeys | static_cast<uint32_t>(thread), l.now(g) + microseconds * kEeHz / 1000000,
                             [&l, thread](uint64_t at) { delayDue(l, thread, at); });
            g.ret(0);
        }

        void initAlarm(Libraries &, Guest &g) { g.ret(0); }
    }

    int64_t TimerState::idleHold(uint64_t from, uint64_t to) const
    {
        if (!hostClock || to <= from)
            return 0;
        int64_t due = 0;
        for (const auto &[thread, at] : delays)
            due = std::max(due, at);
        for (const Counter &c : counters)
            if (c.tag && c.running && c.armed)
                due = std::max(due, c.hostDue);
        const int64_t left = due - hostClock();
        if (left <= 0)
            return 0;
        return std::min<int64_t>(left, static_cast<int64_t>(static_cast<double>(to - from) * 1e9 / kEeHz));
    }

    void addTimer(std::vector<Function> &out)
    {
        for (const auto &[name, handler] : std::initializer_list<std::pair<const char *, Handler>>{
                 {"InitAlarm", initAlarm},
                 {"InitTimer", initTimer},
                 {"EndTimer", endTimer},
                 {"GetTimerPreScaleFactor", preScaleFactor},
                 {"StartTimerSystemTime", startSystemTime},
                 {"StopTimerSystemTime", stopSystemTime},
                 {"GetTimerSystemTime", systemTime},
                 {"iGetTimerSystemTime", systemTime},
                 {"AllocTimerCounter", allocCounter},
                 {"iAllocTimerCounter", allocCounter},
                 {"FreeTimerCounter", freeCounter},
                 {"iFreeTimerCounter", freeCounter},
                 {"GetTimerUsedUnusedCounters", usedUnused},
                 {"iGetTimerUsedUnusedCounters", usedUnused},
                 {"StartTimerCounter", startCounter},
                 {"iStartTimerCounter", startCounter},
                 {"StopTimerCounter", stopCounter},
                 {"iStopTimerCounter", stopCounter},
                 {"SetTimerCount", setCount},
                 {"GetTimerBaseTime", baseTime},
                 {"iGetTimerBaseTime", baseTime},
                 {"GetTimerCount", getCount},
                 {"iGetTimerCount", getCount},
                 {"SetTimerHandler", setHandler},
                 {"iSetTimerHandler", setHandler},
                 {"TimerBusClock2USec", busClock2USec},
                 {"TimerUSec2BusClock", uSec2BusClock},
                 {"TimerBusClock2Freq", busClock2Freq},
                 {"TimerFreq2BusClock", freq2BusClock},
                 {"SetTimerAlarm", setAlarm},
                 {"iSetTimerAlarm", setAlarm},
                 {"ReleaseTimerAlarm", releaseAlarm},
                 {"iReleaseTimerAlarm", releaseAlarm},
                 {"DelayThread", delayThread},
             })
            out.push_back({name, kTimer, handler});
    }
}
