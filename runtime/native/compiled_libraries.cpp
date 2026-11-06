// SPDX-License-Identifier: MIT
// Native SDK services for ahead-of-time compiled IRX sound libraries. See docs/native-irx.md.
#include "bozo/native/compiled_libraries.h"
#include "bozo/ee/dispatcher.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <stdexcept>
namespace bozo::native::irx
{
    namespace
    {
        struct Registered
        {
            std::string name;
            uint64_t hash;
            uint32_t base, size;
            const Entry *entries;
            size_t count;
        };
        std::vector<Registered> &registrations()
        {
            static std::vector<Registered> entries;
            return entries;
        }
        constexpr uint32_t kReturn = 0xfffffff0u;
    } // namespace
    Registration::Registration(const char *name, uint64_t hash, uint32_t base, uint32_t size, const Entry *entries,
                               size_t count)
    {
        registrations().push_back({name, hash, base, size, entries, count});
    }
    struct CompiledLibraries::Impl : audio::LibsdHost
    {
        libs::Libraries &libraries;
        std::vector<uint8_t> memory = std::vector<uint8_t>(0x200000);
        uint32_t heapStart;
        libs::Heap heap;
        audio::Mixer mixer;
        audio::Libsd libsd;
        uint64_t cycle = 0, eeOrigin = 0, calls = 0;
        bool origin = false;
        std::vector<Image> images;
        std::map<uint32_t, Import> imports;
        std::map<std::pair<std::string, uint32_t>, uint32_t> exports;
        std::map<uint32_t, std::function<void(Env &)>> functions;
        std::map<std::string, int32_t> started;
        struct Task
        {
            NativeCpu cpu{};
            bool waiting = false, done = false, started = false;
            uint32_t attr = 0, option = 0, entry = 0, stack = 0, stackBytes = 0, initialPriority = 0, priority = 0;
            int32_t wakeups = 0;
            int32_t waitingSema = 0;
            bool sleeping = false;
            uint64_t due = 0;
        };
        std::map<int32_t, Task> tasks;
        int32_t nextTask = 1, current = 0, callbackTask = 0;
        int32_t logicalTask() const { return current ? current : callbackTask; }
        // Native callback exclusion scopes, never a processor status/register bank.
        struct CallbackExclusion { int32_t owner; uint32_t token; };
        std::vector<CallbackExclusion> exclusions;
        uint64_t deferredTarget = 0;
        uint32_t transferSerial = 0;
        uint32_t nextStack = 0x1fe000, invokeDepth = 0;
        struct Interrupt
        {
            uint32_t function = 0, argument = 0, gp = 0;
            bool enabled = false;
        };
        std::map<uint32_t, Interrupt> interrupts;
        struct Timer
        {
            uint32_t compare = 0, mode = 0, cause = 0, rate = 36864000, prescale = 1;
            uint64_t due = 0;
            uint64_t period() const
            {
                return std::max<uint64_t>(1, (uint64_t(compare) * prescale * 36864000 + rate - 1) / rate);
            }
        };
        std::map<uint32_t, Timer> timers;
        uint32_t nextTimer = 1;
        struct Event
        {
            uint64_t due;
            uint32_t function, a0, a1, gp;
        };
        std::deque<Event> events;
        struct Sema
        {
            int32_t count = 0;
            int32_t initial = 0, max = 0;
            uint32_t attr = 0, option = 0;
            std::deque<int32_t> waiting;
        };
        std::map<int32_t, Sema> semas;
        int32_t nextSema = 1;
        explicit Impl(libs::Libraries &l, uint32_t start) : libraries(l), heapStart(start), libsd(mixer, *this)
        {
            if (start >= 0x180000)
                throw std::runtime_error("native heap starts outside its arena");
            heap.reset(start, 0x180000);
            mixer.setClocked(true);
            mixer.advanceTo(0);
            for (const auto &r : registrations())
                for (size_t i = 0; i < r.count; ++i)
                {
                    if (!functions.emplace(r.entries[i].pc, r.entries[i].function).second)
                        throw std::runtime_error("overlapping native IRX function registrations");
                }
        }
        uint64_t iopCycle() override { return cycle; }
        uint8_t *iopRam() override { return memory.data(); }
        uint32_t iopRamBytes() override { return static_cast<uint32_t>(memory.size()); }
        void interruptCall(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t gp, uint32_t delay) override
        {
            events.push_back({cycle + delay, fn, a0, a1, gp});
        }
        uint32_t physical(uint32_t a, uint32_t n) const
        {
            const auto p = a & 0x1fffffff;
            if (uint64_t(p) + n > memory.size())
                throw std::runtime_error(std::format("native library buffer outside RAM: 0x{:x}, {} bytes", a, n));
            return p;
        }
        uint32_t read(uint32_t a) const
        {
            uint32_t v;
            std::memcpy(&v, memory.data() + physical(a, 4), 4);
            return v;
        }
        void write(uint32_t a, uint32_t v) { std::memcpy(memory.data() + physical(a, 4), &v, 4); }
        void copy(uint32_t dest, const uint8_t *data, uint32_t n)
        {
            std::memcpy(memory.data() + physical(dest, n), data, n);
        }
        std::string string(uint32_t a, uint32_t limit = 4096) const
        {
            std::string s;
            while (s.size() < limit)
            {
                const auto c = memory[physical(a++, 1)];
                if (!c)
                    return s;
                s += char(c);
            }
            throw std::runtime_error("unterminated native library string");
        }
        uint32_t gpAt(uint32_t address) const
        {
            for (const auto &image : images)
                if (address >= image.base && address - image.base < image.bytes.size())
                    return image.gp;
            throw std::runtime_error(std::format("native library target 0x{:x} has no registered image", address));
        }
        struct Environment final : Env
        {
            Impl &owner;
            Environment(Impl &o, NativeCpu &c) : Env(c), owner(o)
            {
                ram = o.memory.data();
                ramSize = static_cast<uint32_t>(o.memory.size());
                budget = 1000000;
            }
            uint8_t read8(uint32_t a) override { return owner.memory[owner.physical(a, 1)]; }
            uint16_t read16(uint32_t a) override
            {
                uint16_t v;
                std::memcpy(&v, ram + owner.physical(a, 2), 2);
                return v;
            }
            uint32_t read32(uint32_t a) override
            {
                try
                {
                    return owner.read(a);
                }
                catch (const std::runtime_error &error)
                {
                    throw std::runtime_error(std::format("{}; native pc=0x{:x} ra=0x{:x} sp=0x{:x}", error.what(),
                                                         cpu.pc, cpu.gpr[31], cpu.gpr[29]));
                }
            }
            void write8(uint32_t a, uint8_t v) override { ram[owner.physical(a, 1)] = v; }
            void write16(uint32_t a, uint16_t v) override { std::memcpy(ram + owner.physical(a, 2), &v, 2); }
            void write32(uint32_t a, uint32_t v) override { owner.write(a, v); }
            bool call(uint32_t target, uint32_t ret) override
            {
                cpu.pc = target;
                while (cpu.pc != ret)
                {
                    const auto imported = owner.imports.find(cpu.pc);
                    if (imported != owner.imports.end())
                    {
                        try
                        {
                            owner.service(*this, imported->second);
                        }
                        catch (const std::runtime_error &error)
                        {
                            throw std::runtime_error(
                                std::format("{}; import {}:{} at 0x{:x}, sp=0x{:x}, a0=0x{:x} a1=0x{:x}", error.what(),
                                            imported->second.library, imported->second.ordinal, cpu.pc, cpu.gpr[29],
                                            cpu.gpr[4], cpu.gpr[5]));
                        }
                        cpu.pc = cpu.gpr[31];
                        if (owner.current && owner.tasks.at(owner.current).waiting)
                            return false;
                    }
                    else
                    {
                        const auto fn = owner.functions.find(cpu.pc);
                        if (fn == owner.functions.end())
                            throw std::runtime_error(std::format("no compiled IRX target at 0x{:x}", cpu.pc));
                        ++owner.calls;
                        fn->second(*this);
                    }
                    cpu.gpr[0] = 0;
                    if (expired() || (owner.current &&
                                      (owner.tasks.at(owner.current).waiting || owner.tasks.at(owner.current).done)))
                    {
                        if (!owner.exclusions.empty())
                            throw std::runtime_error("native routine yielded with callbacks excluded");
                        return false;
                    }
                }
                return true;
            }
        };
        NativeCpu context(uint32_t function, uint32_t gp, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0,
                          uint32_t stackBytes = 0x4000)
        {
            if (stackBytes > 0x7e000)
                throw std::runtime_error("native library task stack is too large");
            stackBytes = (std::max(stackBytes, 0x100u) + 15u) & ~15u;
            NativeCpu cpu{};
            cpu.pc = function;
            cpu.gpr[28] = gp;
            cpu.gpr[29] = nextStack;
            nextStack -= stackBytes;
            if (nextStack < 0x190000)
                throw std::runtime_error("native library task stacks exhausted");
            cpu.gpr[31] = kReturn;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            return cpu;
        }
        uint32_t invoke(uint32_t function, uint32_t gp, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0,
                        int32_t ownerTask = 0)
        {
            if (ownerTask && (!tasks.contains(ownerTask) || tasks.at(ownerTask).done))
                throw std::runtime_error("native RPC owner task is not available");
            const auto exclusionsBefore = exclusions.size();
            // Root calls and callbacks use a separate, bounded stack arena. Creating a persistent task
            // during a root call must never let the next callback overwrite that task's suspended stack.
            if (invokeDepth >= 4)
                throw std::runtime_error("native callback nesting limit exceeded");
            NativeCpu cpu{};
            cpu.pc = function;
            cpu.gpr[28] = gp;
            cpu.gpr[31] = kReturn;
            cpu.gpr[29] = 0x190000 - invokeDepth++ * 0x4000;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            struct Scope
            {
                Impl &host;
                int32_t saved, savedCallback;
                ~Scope()
                {
                    host.current = saved;
                    host.callbackTask = savedCallback;
                    --host.invokeDepth;
                }
            } scope{*this, current, callbackTask};
            Environment env(*this, cpu);
            current = 0;
            callbackTask = ownerTask;
            const bool done = env.call(function, kReturn);
            cycle += env.executed;
            if (exclusions.size() != exclusionsBefore)
                throw std::runtime_error("native routine returned with unbalanced callback exclusion");
            if (!done)
                throw std::runtime_error(std::format("native library call 0x{:x} exceeded its work budget", function));
            return cpu.gpr[2];
        }
        void runReady()
        {
            if (!exclusions.empty()) return;
            for (unsigned pass = 0; pass < 32; ++pass)
            {
                bool ran = false;
                for (auto &[id, task] : tasks)
                {
                    if (task.done)
                        continue;
                    if (task.waiting && task.due && task.due <= cycle)
                    {
                        task.waiting = false;
                        task.due = 0;
                    }
                    if (task.waiting)
                        continue;
                    current = id;
                    Environment env(*this, task.cpu);
                    if (env.call(task.cpu.pc, kReturn))
                        task.done = true;
                    cycle += env.executed;
                    current = 0;
                    ran = true;
                }
                if (!ran)
                    return;
            }
            throw std::runtime_error("native library tasks made no blocking progress");
        }
        void service(Environment &env, const Import &import);
        void syncEe(uint64_t eeCycle)
        {
            if (!origin)
            {
                eeOrigin = eeCycle;
                origin = true;
            }
            advance(std::max(cycle, eeCycle >= eeOrigin ? (eeCycle - eeOrigin) / 8 : 0));
        }
        void advance(uint64_t target)
        {
            if (!exclusions.empty()) { deferredTarget = std::max(deferredTarget, target); return; }
            target = std::max(target, deferredTarget);
            deferredTarget = 0;
            runReady();
            unsigned work = 0;
            while (true)
            {
                uint64_t next = std::max(target, cycle);
                bool due = false;
                for (const auto &[id, t] : timers)
                    if (t.mode && t.compare && t.due <= next)
                    {
                        next = t.due;
                        due = true;
                    }
                for (const auto &event : events)
                    if (event.due <= next)
                    {
                        next = event.due;
                        due = true;
                    }
                cycle = next;
                mixer.advanceTo(cycle);
                if (!due)
                    break;
                if (++work > 100000)
                    throw std::runtime_error(std::format(
                        "native library event queue exceeded work limit at {}, target {}, timers {}, events {}", cycle,
                        target, timers.size(), events.size()));
                // Native callbacks may cancel themselves or another due timer. Never keep an
                // iterator/reference into the timer registry across a callback.
                std::vector<uint32_t> readyTimers;
                for (const auto &[id, t] : timers)
                    if (t.mode && t.compare && t.due <= cycle)
                        readyTimers.push_back(id);
                for (const uint32_t id : readyTimers)
                {
                    const auto timer = timers.find(id);
                    if (timer == timers.end() || !timer->second.mode || !timer->second.compare ||
                        timer->second.due > cycle)
                        continue;
                    timer->second.due = cycle + timer->second.period();
                    const auto irq = interrupts.find(timer->second.cause);
                    if (irq != interrupts.end() && irq->second.enabled && irq->second.function)
                    {
                        const auto handler = irq->second;
                        invoke(handler.function, handler.gp, handler.argument);
                    }
                }
                while (true)
                {
                    const auto it = std::find_if(events.begin(), events.end(),
                                                 [&](const Event &event) { return event.due <= cycle; });
                    if (it == events.end())
                        break;
                    const Event event = *it;
                    events.erase(it);
                    if (event.function)
                        invoke(event.function, event.gp, event.a0, event.a1);
                }
                runReady();
            }
        }
    };
    void CompiledLibraries::Impl::service(Environment &env, const Import &import)
    {
        auto &c = env.cpu;
        const auto &lib = import.library;
        const auto n = import.ordinal;
        const uint32_t a0 = c.gpr[4], a1 = c.gpr[5], a2 = c.gpr[6], a3 = c.gpr[7];
        auto ret = [&](uint32_t v = 0) { c.gpr[2] = v; };
        if (const auto found = exports.find({lib, n}); found != exports.end() && lib != "libsd")
        {
            const auto target = found->second;
            if (!env.call(target, c.gpr[31]))
                return;
            return;
        }
        if (lib == "libsd")
        {
            const uint32_t args[5] = {a0, a1, a2, a3, read(c.gpr[29] + 16)};
            if (n == 17 && std::getenv("BOZO_NATIVE_IRX_TRACE"))
                std::fprintf(stderr, "[libsd] voiceTrans(%x,%x,%x,%x,%x) sp=%x\n", a0, a1, a2, a3, args[4], c.gpr[29]);
            uint32_t result = 0;
            if (!libsd.call(n, args, c.gpr[28], result))
            {
                // SDK note/pitch conversions are pure arithmetic in the statically compiled library.
                const auto found = exports.find({lib, n});
                if ((n == 13 || n == 14) && found != exports.end())
                {
                    env.call(found->second, c.gpr[31]);
                    return;
                }
                throw std::runtime_error(std::format("no native libsd export {}", n));
            }
            ret(result);
            return;
        }
        if (lib == "loadcore")
        {
            // Native linking and cache coherence are supplied by the host executable.
            if (n == 5 || n == 6 || n == 12)
            {
                ret();
                return;
            }
        }
        if (lib == "stdio" && n == 4)
        {
            if (libraries.printOutput)
                std::fprintf(stdout, "[native-irx] %s", string(a0).c_str());
            ret();
            return;
        }
        if (lib == "sysclib")
        {
            if (n == 8)
            {
                const unsigned char ch = static_cast<unsigned char>(a0);
                ret((std::isupper(ch) ? 1 : 0) | (std::islower(ch) ? 2 : 0) | (std::isdigit(ch) ? 4 : 0) |
                    (std::isspace(ch) ? 8 : 0) | (std::ispunct(ch) ? 16 : 0) | (std::iscntrl(ch) ? 32 : 0) |
                    (std::isxdigit(ch) ? 64 : 0) | (ch == ' ' ? 128 : 0));
                return;
            }
            if (n == 12)
            {
                std::memmove(memory.data() + physical(a0, a2), memory.data() + physical(a1, a2), a2);
                ret(a0);
                return;
            }
            if (n == 14)
            {
                std::memset(memory.data() + physical(a0, a2), static_cast<unsigned char>(a1), a2);
                ret(a0);
                return;
            }
            if (n == 27)
            {
                ret(static_cast<uint32_t>(string(a0, 0x100000).size()));
                return;
            }
            if (n == 29)
            {
                ret(static_cast<uint32_t>(std::strncmp(reinterpret_cast<char *>(memory.data() + physical(a0, a2)),
                                                       reinterpret_cast<char *>(memory.data() + physical(a1, a2)),
                                                       a2)));
                return;
            }
            if (n == 36)
            {
                const auto value = string(a0);
                char *end = nullptr;
                const auto parsed = std::strtol(value.c_str(), &end, int(a2));
                if (a1)
                    write(a1, a0 + uint32_t(end - value.c_str()));
                ret(uint32_t(parsed));
                return;
            }
        }
        if (lib == "sysmem")
        {
            if (n == 4)
            {
                ret(heap.allocate(a1));
                return;
            }
            if (n == 5)
            {
                ret(heap.release(a0) ? 0 : 0xffffffffu);
                return;
            }
        }
        if (lib == "thsemap")
        {
            if(n==4)
            {
                const auto attr=read(a0),option=read(a0+4);
                const auto initial=static_cast<int32_t>(read(a0+8)),max=static_cast<int32_t>(read(a0+12));
                if(attr!=0)throw std::runtime_error("native library semaphore supports FIFO attributes only");
                if(initial<0 || max<=0 || initial>max)
                    throw std::runtime_error("invalid native library semaphore counts");
                const int32_t id=nextSema++;
                Sema s;s.attr=attr;s.option=option;s.initial=s.count=initial;s.max=max;
                semas.emplace(id,std::move(s));ret(static_cast<uint32_t>(id));return;
            }
            const auto found=semas.find(static_cast<int32_t>(a0));
            if(found==semas.end()){ret(static_cast<uint32_t>(-408));return;}
            auto &s=found->second;
            const auto wake=[&](int32_t id,int32_t result) {
                auto &t=tasks.at(id);t.waiting=false;t.waitingSema=0;
                t.cpu.gpr[2]=static_cast<uint32_t>(result);
            };
            if(n==5)
            {
                for(const auto id:s.waiting)wake(id,-425);
                semas.erase(found);ret();return;
            }
            if(n==6 || n==7)
            {
                if(!s.waiting.empty()) {const auto id=s.waiting.front();s.waiting.pop_front();wake(id,0);}
                else if(s.count==s.max){ret(static_cast<uint32_t>(-420));return;}
                else ++s.count;
                ret();return;
            }
            if(n==8 || n==9)
            {
                if(s.count>0){--s.count;ret();return;}
                if(n==9){ret(static_cast<uint32_t>(-419));return;}
                if(!current){ret(static_cast<uint32_t>(-100));return;}
                if(!exclusions.empty())throw std::runtime_error("native semaphore wait with callbacks excluded");
                auto &t=tasks.at(current);t.waiting=true;t.waitingSema=static_cast<int32_t>(a0);t.sleeping=false;
                s.waiting.push_back(current);ret();return;
            }
        }
        if (lib == "thbase")
        {
            if (n == 4)
            {
                const auto fn = read(a0 + 8);
                const int32_t id = nextTask++;
                Task task;
                task.attr = read(a0);
                task.option = read(a0 + 4);
                task.entry = fn;
                const uint32_t requested = read(a0 + 12);
                if (requested > 0x6e000)
                    throw std::runtime_error("native task stack request exceeds arena");
                task.stackBytes = (std::max(requested, 0x100u) + 15u) & ~15u;
                task.initialPriority = task.priority = read(a0 + 16);
                task.cpu = context(fn, c.gpr[28], 0, 0, 0, task.stackBytes);
                task.stack = task.cpu.gpr[29] - task.stackBytes;
                task.waiting = true;
                tasks.emplace(id, std::move(task));
                ret(uint32_t(id));
                return;
            }
            if (n == 6)
            {
                auto &t = tasks.at(int32_t(a0));
                t.cpu.gpr[4] = a1;
                t.waiting = false;
                t.started = true;
                ret();
                return;
            }
            if (n == 14 || n == 15)
            {
                const auto it = tasks.find(a0 ? int32_t(a0) : logicalTask());
                if (it == tasks.end())
                {
                    ret(0xffffffffu);
                    return;
                }
                it->second.priority = a1;
                ret();
                return;
            } // host callbacks remain cooperative; no preemption inside a native call
            if (n == 20)
            {
                ret(uint32_t(logicalTask()));
                return;
            }
            if (n == 42)
            {
                const auto it = tasks.find(logicalTask());
                ret(it == tasks.end() ? uint32_t(-100) : it->second.priority);
                return;
            }
            if (n == 22 || n == 23)
            {
                const auto it = tasks.find(a0 ? int32_t(a0) : logicalTask());
                if (it == tasks.end())
                {
                    ret(0xffffffffu);
                    return;
                }
                // IOP iop_thread_info: attributes, options, status, entry, stack, stack size, GP, priorities and waits.
                const auto &t = it->second;
                const uint32_t status = !t.started || t.done            ? 0x10u
                                        : int32_t(it->first) == logicalTask() ? 1u
                                        : t.waiting                     ? 4u
                                                                        : 2u;
                const uint32_t words[17] = {t.attr,
                                            t.option,
                                            status,
                                            t.entry,
                                            t.stack,
                                            t.stackBytes,
                                            t.cpu.gpr[28],
                                            t.initialPriority,
                                            t.priority,
                                            0,
                                            0,
                                            uint32_t(t.wakeups),
                                            0,
                                            0,
                                            0,
                                            0,
                                            0};
                copy(a1, reinterpret_cast<const uint8_t *>(words), sizeof(words));
                ret();
                return;
            }
            if (n == 24)
            {
                if (!current)
                    throw std::runtime_error("native library root call attempted SleepThread");
                auto &t = tasks.at(current);
                if (t.wakeups > 0)
                    --t.wakeups;
                else
                {
                    if (!exclusions.empty()) throw std::runtime_error("native sleep with callbacks excluded");
                    t.waiting = true;
                    t.sleeping = true;
                }
                ret();
                return;
            }
            if (n == 25 || n == 26)
            {
                auto &t = tasks.at(int32_t(a0));
                if (t.waiting && t.sleeping)
                {
                    t.waiting = false;
                    t.sleeping = false;
                }
                else
                    ++t.wakeups;
                ret();
                return;
            }
            if (n == 33)
            {
                if (!current)
                    throw std::runtime_error("native root DelayThread");
                if (!exclusions.empty()) throw std::runtime_error("native delay with callbacks excluded");
                auto &t = tasks.at(current);
                t.waiting = true;
                t.due = cycle + uint64_t(a0) * 36864 / 1000;
                ret();
                return;
            }
            if (n == 34)
            {
                write(a0, uint32_t(cycle));
                write(a0 + 4, uint32_t(cycle >> 32));
                ret();
                return;
            }
            if (n == 40)
            {
                const uint64_t time = uint64_t(read(a0 + 4)) << 32 | read(a0);
                const uint64_t usec = (time / 36864) * 1000 + (time % 36864) * 1000 / 36864;
                if (a1)
                    write(a1, uint32_t(usec / 1000000));
                if (a2)
                    write(a2, uint32_t(usec % 1000000));
                ret();
                return;
            }
        }
        if (lib == "intrman")
        {
            if (n == 17)
            {
                const uint32_t token = exclusions.empty() ? 1u : 0u;
                if (!exclusions.empty() && exclusions.back().owner != logicalTask())
                    throw std::runtime_error("native callback exclusion owner mismatch");
                write(a0, token);
                exclusions.push_back({logicalTask(),token});
                ret(token ? 0u : uint32_t(-102));
                return;
            }
            if (n == 18)
            {
                if (exclusions.empty() || exclusions.back().owner != logicalTask() || exclusions.back().token != a0)
                    throw std::runtime_error("unbalanced native callback exclusion release");
                exclusions.pop_back();ret();return;
            }
            if (n == 4)
            {
                interrupts[a0] = {a2, a3, c.gpr[28], false};
                ret();
                return;
            }
            if (n == 5)
            {
                interrupts.erase(a0);
                ret();
                return;
            }
            if (n == 6)
            {
                interrupts[a0].enabled = true;
                ret();
                return;
            }
            if (n == 7)
            {
                interrupts[a0].enabled = false;
                if (a1)
                    write(a1, 1);
                ret();
                return;
            }
            if (n == 9)
            {
                if (!exclusions.empty())
                    throw std::runtime_error("native CpuEnableIntr cannot replace a scoped callback release");
                ret();
                return;
            } // native callbacks are only delivered at safe points
        }
        if (lib == "timrman")
        {
            static unsigned trace = 0;
            if (std::getenv("BOZO_NATIVE_IRX_TRACE") && trace++ < 100)
                std::fprintf(stderr, "[native-irx:timer] %u(%x,%x,%x,%x) cycle=%llu\n", n, a0, a1, a2, a3,
                             (unsigned long long)cycle);
            if (n == 4)
            {
                const auto id = nextTimer++;
                auto &t = timers[id];
                t.cause = 16 + id;
                t.rate = a0 == 4 ? 15734 : a0 == 2 ? 13500000 : 36864000;
                t.prescale = std::max(1u, a2);
                ret(id);
                return;
            }
            auto it = timers.find(a0);
            if (it == timers.end())
                throw std::runtime_error("unknown native periodic callback handle");
            auto &timer = it->second;
            if (n == 6)
            {
                timers.erase(it);
                ret();
                return;
            }
            if (n == 7)
            {
                timer.mode = a1;
                timer.due = cycle + timer.period();
                ret();
                return;
            }
            if (n == 8)
            {
                ret(timer.mode | 0x800);
                return;
            }
            if (n == 11)
            {
                timer.compare = a1;
                timer.due = cycle + timer.period();
                ret();
                return;
            }
            if (n == 16)
            {
                ret(timer.cause);
                return;
            }
        }
        if (lib == "sifman" && (n == 7 || n == 32))
        {
            auto *dispatcher = libraries.dispatcher();
            if (!dispatcher) throw std::runtime_error("native bulk copy requires attached EE data");
            if (!a1 || a1 > 32 || uint64_t(a0) + uint64_t(a1) * 16 > memory.size())
                throw std::runtime_error("invalid native bulk-copy descriptor batch");
            if (transferSerial == 0x7fffffffu) throw std::runtime_error("native bulk-copy IDs exhausted");
            if (n == 32 && a2)
            {
                const bool admitted = std::any_of(images.begin(), images.end(), [&](const auto &image) {
                    return a2 >= image.base && uint64_t(a2) < uint64_t(image.base) + image.bytes.size();
                });
                if (!admitted || !functions.contains(a2) || imports.contains(a2))
                    throw std::runtime_error("native bulk completion requires an admitted linked function");
            }
            struct Copy { uint8_t *destination; std::vector<uint8_t> bytes; };
            std::vector<Copy> batch;
            auto &ee = dispatcher->runtime().memory;
            for (uint32_t i = 0; i < a1; ++i)
            {
                const uint32_t descriptor = a0 + i * 16;
                const auto source = read(descriptor), destination = read(descriptor + 4);
                const auto bytes = read(descriptor + 8), attributes = read(descriptor + 12);
                if (attributes) throw std::runtime_error("unported native bulk-copy attributes");
                const auto offset = physical(source, bytes);
                auto *to = ee.hostPointer(destination, bytes);
                if (!to || to < ee.ram() || uint64_t(to - ee.ram()) + bytes > 0x2000000u)
                    throw std::runtime_error("native bulk-copy destination is outside EE RAM");
                batch.push_back({to, std::vector<uint8_t>(memory.begin() + offset, memory.begin() + offset + bytes)});
            }
            // Validate and snapshot the whole batch before writing any output.
            for (const auto &copy : batch)
                if (!copy.bytes.empty()) std::memcpy(copy.destination, copy.bytes.data(), copy.bytes.size());
            const uint32_t id = ++transferSerial;
            if (n == 32 && a2) events.push_back({cycle, a2, a3, 0, c.gpr[28]});
            ret(id);return;
        }
        if (lib == "sifman" && n == 8)
        {
            // Native copies are complete when returned. Unknown IDs also have
            // no pending operation; this is the SDK's terminal/error result.
            ret(0xffffffffu);return;
        }
        if (lib == "sifman" && (n == 5 || n == 29))
        {
            ret(1);
            return;
        }
        if (lib == "sifcmd")
        {
            if (n == 14)
            {
                ret();
                return;
            }
            if (n == 17)
            {
                const uint32_t function = a2, buffer = a3, gp = c.gpr[28];
                const int32_t ownerTask = logicalTask();
                libraries.sif.servers[a1] = [this, function, buffer, gp, ownerTask](libs::Libraries &, libs::Guest &guest,
                                                                         const libs::NativeRpcRequest &request)
                {
                    syncEe(libraries.now(guest));
                    copy(buffer, guest.in(request.send, request.sendBytes), request.sendBytes);
                    const uint32_t reply = invoke(function, gp, request.command, buffer, request.sendBytes, ownerTask);
                    if (request.receiveBytes)
                    {
                        try
                        {
                            guest.write(request.receive, memory.data() + physical(reply, request.receiveBytes),
                                        request.receiveBytes);
                        }
                        catch (const std::runtime_error &error)
                        {
                            throw std::runtime_error(std::format("{}; RPC command {} handler 0x{:x} returned 0x{:x}",
                                                                 error.what(), request.command, function, reply));
                        }
                    }
                    runReady();
                };
                write(a0 + 0x20, a1);
                write(a0 + 0x28, a2);
                write(a0 + 0x2c, a3);
                ret(a0);
                return;
            }
            if (n == 19)
            {
                ret(a0);
                return;
            }
            if (n == 22)
            {
                if (!current)
                    throw std::runtime_error("RpcLoop outside a native task");
                if (!exclusions.empty()) throw std::runtime_error("native RPC park with callbacks excluded");
                tasks.at(current).waiting = true;
                ret();
                return;
            }
        }
        if (lib == "ioman")
        {
            auto &cd = libraries.cd;
            if (!cd.disc.isOpen())
            {
                std::string error;
                if (!cd.disc.open(cd.imagePath, error))
                    throw std::runtime_error(error);
            }
            if (n == 4)
            {
                uint32_t sector = 0, bytes = 0;
                if ((a1 & 3) != 1 || !cd.disc.findFile(string(a0), sector, bytes))
                {
                    ret(0xffffffffu);
                    return;
                }
                const int32_t id = cd.nextFile++;
                cd.files.emplace(id, libs::CdState::File{sector, bytes, 0});
                ret(uint32_t(id));
                return;
            }
            const auto it = cd.files.find(int32_t(a0));
            if (it == cd.files.end())
            {
                ret(0xffffffffu);
                return;
            }
            auto &file = it->second;
            if (n == 5)
            {
                cd.files.erase(it);
                ret();
                return;
            }
            if (n == 6)
            {
                const uint32_t bytes = std::min(a2, file.position < file.bytes ? file.bytes - file.position : 0);
                const uint32_t offset = file.position % 2048, count = (offset + bytes + 2047) / 2048;
                std::vector<uint8_t> data(size_t(count) * 2048);
                if (count && !cd.disc.read(file.sector + file.position / 2048, count, data.data()))
                    throw std::runtime_error("native library disc read failed");
                if (bytes)
                    copy(a1, data.data() + offset, bytes);
                file.position += bytes;
                ret(bytes);
                return;
            }
            if (n == 8)
            {
                const int64_t pos = int32_t(a1) + (a2 == 0 ? 0 : a2 == 1 ? file.position : file.bytes);
                if (pos < 0 || pos > INT32_MAX)
                    ret(0xffffffffu);
                else
                {
                    file.position = uint32_t(pos);
                    ret(file.position);
                }
                return;
            }
        }
        throw std::runtime_error(std::format("unsupported native IRX import {}:{} at 0x{:x}", lib, n, c.pc));
    }
    CompiledLibraries::CompiledLibraries(libs::Libraries &l, uint32_t heapStart)
        : impl_(std::make_unique<Impl>(l, heapStart))
    {
    }
    CompiledLibraries::~CompiledLibraries() = default;
    void CompiledLibraries::addImage(Image image)
    {
        auto &p = *impl_;
        if (image.bytes.empty() || uint64_t(image.base) + image.bytes.size() > p.heapStart ||
            image.entry < image.base || uint64_t(image.entry) >= uint64_t(image.base) + image.bytes.size())
            throw std::runtime_error("native image outside its reserved data region");
        for (const auto &other : p.images)
        {
            if (other.name == image.name || (uint64_t(image.base) < uint64_t(other.base) + other.bytes.size() &&
                                             uint64_t(other.base) < uint64_t(image.base) + image.bytes.size()))
                throw std::runtime_error("overlapping or duplicate native images");
        }
        {
            const auto found = std::find_if(registrations().begin(), registrations().end(),
                                            [&](const auto &r)
                                            {
                                                return r.name == image.name && r.hash == image.hash &&
                                                       r.base == image.base && r.size == image.bytes.size();
                                            });
            if (found == registrations().end())
                throw std::runtime_error("no compiled registration for native image " + image.name);
        }
        p.copy(image.base, image.bytes.data(), uint32_t(image.bytes.size()));
        for (const auto &i : image.imports)
            p.imports.emplace(i.address, i);
        for (const auto &e : image.exports)
        {
            p.exports.emplace(std::make_pair(e.library, e.ordinal), e.address);
            // Libraries may call exports through a function pointer rather than an import stub.
            if (image.nativeLibsd && audio::Libsd::handles(e.ordinal))
                p.imports[e.address] = e;
        }
        p.images.push_back(std::move(image));
    }
    void CompiledLibraries::bindNative(uint32_t address, std::function<void(Env &)> function)
    {
        auto &p = *impl_;
        const auto image = std::find_if(p.images.begin(), p.images.end(), [&](const auto &i) {
            return address >= i.base && uint64_t(address) < uint64_t(i.base) + i.bytes.size();
        });
        if (!function || image == p.images.end() || !p.functions.contains(address) || p.imports.contains(address))
            throw std::runtime_error("native binding requires an admitted linked game entry");
        if (!p.started.empty() || p.invokeDepth || !p.tasks.empty())
            throw std::runtime_error("native bindings must be installed before library execution");
        p.functions.at(address) = std::move(function);
    }
    int32_t CompiledLibraries::startImage(const std::string &name)
    {
        auto &p = *impl_;
        if (auto it = p.started.find(name); it != p.started.end())
            return it->second;
        const auto image =
            std::find_if(p.images.begin(), p.images.end(), [&](const auto &i) { return i.name == name; });
        if (image == p.images.end())
            throw std::runtime_error("no native implementation for module " + name);
        if (image->nativeLibsd)
            p.libsd.init();
        else
            p.invoke(image->entry, image->gp);
        p.runReady();
        const int32_t id = int32_t(p.started.size() + 1);
        p.started[name] = id;
        return id;
    }
    void CompiledLibraries::reset()
    {
        auto &p = *impl_;
        p.tasks.clear();
        p.events.clear();
        p.interrupts.clear();
        p.timers.clear();
        p.semas.clear();
        p.started.clear();
        p.libraries.sif.servers.clear();
        p.libraries.sif.bound.clear();
        p.nextStack = 0x1fe000;
        p.invokeDepth = 0;
        p.nextTask = 1;
        p.nextSema = 1;
        p.nextTimer = 1;
        p.current = 0;
        p.callbackTask = 0;
        p.exclusions.clear();
        p.deferredTarget = 0;
        p.transferSerial = 0;
        p.cycle = 0;
        p.origin = false;
        std::fill(p.memory.begin(), p.memory.end(), 0);
        for (const auto &image : p.images)
            p.copy(image.base, image.bytes.data(), uint32_t(image.bytes.size()));
        p.heap.reset(p.heapStart, 0x180000);
        p.libsd.init();
        p.mixer.advanceTo(0);
    }
    audio::Mixer *CompiledLibraries::advanceTo(uint64_t eeCycle)
    {
        auto &p = *impl_;
        p.syncEe(eeCycle);
        return &p.mixer;
    }
    void CompiledLibraries::writeBuffer(uint32_t address, const uint8_t *data, uint32_t bytes)
    {
        impl_->copy(address, data, bytes);
    }
    uint32_t CompiledLibraries::allocateBuffer(uint32_t bytes) { return impl_->heap.allocate(bytes); }
    bool CompiledLibraries::freeBuffer(uint32_t address) { return impl_->heap.release(address); }
    uint64_t CompiledLibraries::nativeCalls() const { return impl_->calls; }
} // namespace bozo::native::irx
