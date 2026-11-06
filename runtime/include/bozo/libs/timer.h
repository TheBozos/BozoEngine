// libkernel's timer library and DelayThread as the native runtime answers them: counters on the EE clock, handlers on
// the kernel's deadlines, no timer interrupt. Spec: docs/specs/libraries.md section 16.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>

namespace bozo::libs
{
    struct TimerState
    {
        static constexpr uint32_t kCounters = 128, kAlarms = 64;
        struct Counter
        {
            uint32_t tag = 0; // 0: free; else the low 10 bits of its id
            bool running = false, armed = false;
            uint64_t start = 0, count = 0, compare = 0; // bus clocks
            uint32_t handler = 0, arg = 0, gp = 0;
            int32_t alarm = -1;
            int64_t hostDue = 0;
        };
        std::array<Counter, kCounters> counters{};
        std::array<int32_t, kAlarms> alarms = [] {
            std::array<int32_t, kAlarms> a{};
            a.fill(-1);
            return a;
        }();
        uint32_t used = 0, serial = 0;
        bool initialized = true;
        uint32_t prescale = 2;
        bool systemRunning = true;
        uint64_t systemStart = 0, systemStopped = 0;
        int32_t inHandler = -1;
        std::map<int32_t, int64_t> delays; // a thread in DelayThread -> its host deadline

        // Set in real time: the host's steady clock in ns. A delay or an alarm then also waits for its host deadline.
        std::function<int64_t()> hostClock;
        // How long (ns) the host holds while every thread waits and the EE clock would jump from `from` to `to`.
        int64_t idleHold(uint64_t from, uint64_t to) const;
    };
}
