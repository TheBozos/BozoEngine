// SPDX-License-Identifier: MIT
// Native SDK services for ahead-of-time compiled IRX sound libraries. See docs/native-irx.md.
#include "bozo/native/compiled_libraries.h"
#include "bozo/ee/dispatcher.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <set>
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
        // WaitVblankEnd's host job: a 60 Hz tick of the library clock (36.864 MHz), not a display.
        constexpr uint64_t kFrameTick = 36864000 / 60;
        constexpr uint32_t kCpuDisableScope = 0xffffffffu;
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
            // An RPC request served on this task: its parked RpcLoop state, kept while the handler runs above it.
            bool serving = false, replied = false;
            NativeCpu parked{};
            uint32_t reply = 0;
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
            Interrupt handler;
            bool configured = false, direct = false, alarm = false;
            uint64_t revision = 0;
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
            bool readyAudio = false;
            uint32_t ownedBuffer = 0;
            bool readyDisc = false;
        };
        std::deque<Event> events;
        bool readyAudioCompletions = false, deliveringReadyAudio = false;
        bool nativeDiscFiles = false, deliveringReadyDisc = false;
        uint64_t discRequestSerial = 0;
        std::set<uint64_t> discRequests; // Actual host read/copy invocations only.
        uint32_t discError = 0, discCallback = 0, discCallbackGp = 0;
        struct FrameListener { Interrupt callback;int32_t priority;uint64_t order; };
        std::map<uint32_t,FrameListener> frameListeners;
        std::deque<std::vector<std::pair<uint32_t,uint64_t>>> completedFrames;
        bool nativeFrameCallbacks = false, deliveringFrames = false;
        uint64_t frameListenerSerial = 0;
        uint32_t commandBuffer = 0, commandCapacity = 0, commandSerial = 0;
        std::map<uint32_t,Interrupt> commandHandlers;
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
            // Libsd calls this after its actual byte copy or mixer half consumption.
            events.push_back({cycle + (readyAudioCompletions ? 0 : delay), fn, a0, a1, gp, readyAudioCompletions});
        }
        void deliverReadyAudio()
        {
            if (!readyAudioCompletions || deliveringReadyAudio || !exclusions.empty()) return;
            deliveringReadyAudio = true;
            struct Scope { bool &active; ~Scope(){active=false;} } scope{deliveringReadyAudio};
            unsigned count = 0;
            while (true)
            {
                const auto found = std::find_if(events.begin(),events.end(),[&](const auto &event){
                    return event.readyAudio && event.due <= cycle;
                });
                if (found == events.end()) return;
                if (++count > 1024) throw std::runtime_error("native ready audio completion work limit");
                const auto event = *found;
                events.erase(found); // Remove before invoking reentrant native SDK/game code.
                if (event.function) invoke(event.function,event.gp,event.a0,event.a1);
            }
        }
        void deliverReadyDisc()
        {
            if (!nativeDiscFiles || deliveringReadyDisc || !exclusions.empty()) return;
            deliveringReadyDisc = true;
            struct Scope { bool &active; ~Scope(){active=false;} } scope{deliveringReadyDisc};
            unsigned work = 0;
            while (true)
            {
                const auto found=std::find_if(events.begin(),events.end(),[](const auto &event){return event.readyDisc;});
                if(found==events.end())return;
                if(++work>1024)throw std::runtime_error("native disc completion work limit");
                const auto event=*found;events.erase(found);
                invoke(event.function,event.gp,event.a0,event.a1);
            }
        }
        void deliverFrames()
        {
            if (!nativeFrameCallbacks || deliveringFrames || !exclusions.empty()) return;
            deliveringFrames = true;
            struct Scope { bool &active; ~Scope(){active=false;} } scope{deliveringFrames};
            unsigned work = 0;
            while (!completedFrames.empty())
            {
                if (++work > 1024) throw std::runtime_error("native frame callback work limit");
                auto listeners = std::move(completedFrames.front());completedFrames.pop_front();
                for (const auto &[function,order] : listeners)
                {
                    const auto found = frameListeners.find(function);
                    if (found == frameListeners.end() || found->second.order != order) continue;
                    const auto callback = found->second.callback;
                    const auto result = invoke(callback.function,callback.gp,callback.argument);
                    const auto after = frameListeners.find(function);
                    if (!result && after != frameListeners.end() && after->second.order == order)
                        frameListeners.erase(after);
                }
            }
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
                        owner.deliverReadyDisc();
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
                        owner.deliverReadyAudio();
                        owner.deliverReadyDisc();
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
                throw std::runtime_error(std::format("native library call 0x{:x} stopped at 0x{:x}; work {} budget {}, calls {}",
                    function,cpu.pc,env.executed,env.budget,calls));
            return cpu.gpr[2];
        }
        // An RPC request runs on its server task, as RpcLoop runs it: the handler may wait (DelayThread, semaphores),
        // the host's clock then goes on to the next deadline until the handler returns and the task parks again.
        uint32_t serve(int32_t server, uint32_t function, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2)
        {
            if (!server || current || invokeDepth)
                return invoke(function, gp, a0, a1, a2, server);
            if (!tasks.contains(server) || tasks.at(server).done)
                throw std::runtime_error("native RPC owner task is not available");
            {
                auto &t = tasks.at(server);
                if (t.serving || !t.waiting)
                    throw std::runtime_error("native RPC server task is not parked in its RPC loop");
                t.parked = t.cpu;
                t.cpu.pc = function;
                t.cpu.gpr[28] = gp;
                t.cpu.gpr[31] = kReturn;
                t.cpu.gpr[29] = (t.parked.gpr[29] - 0x40u) & ~15u;
                t.cpu.gpr[4] = a0;
                t.cpu.gpr[5] = a1;
                t.cpu.gpr[6] = a2;
                t.serving = true;
                t.replied = false;
                t.waiting = false;
                t.due = 0;
            }
            struct Restore
            {
                Impl &host;
                int32_t server;
                bool armed = true;
                ~Restore()
                {
                    if (!armed || !host.tasks.contains(server))
                        return;
                    auto &t = host.tasks.at(server);
                    if (t.serving)
                    {
                        t.cpu = t.parked;
                        t.serving = false;
                        t.waiting = true;
                        t.due = 0;
                    }
                    t.replied = false;
                    host.current = 0;
                }
            } restore{*this, server};
            for (unsigned work = 0;; ++work)
            {
                runReady();
                auto &t = tasks.at(server);
                if (t.replied)
                {
                    t.replied = false;
                    restore.armed = false;
                    return t.reply;
                }
                if (work >= 100000)
                    throw std::runtime_error("native RPC handler made no progress");
                uint64_t next = UINT64_MAX;
                for (const auto &[id, task] : tasks)
                    if (!task.done && task.waiting && task.due)
                        next = std::min(next, task.due);
                for (const auto &[id, timer] : timers)
                    if (timer.mode && timer.compare)
                        next = std::min(next, timer.due);
                for (const auto &event : events)
                    next = std::min(next, event.due);
                if (next == UINT64_MAX)
                    throw std::runtime_error(std::format("native RPC handler 0x{:x} waits with nothing to wake it", function));
                advance(std::max(next, cycle));
            }
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
                    {
                        if (task.serving)
                        {
                            task.reply = task.cpu.gpr[2];
                            task.cpu = task.parked;
                            task.serving = false;
                            task.replied = true;
                            task.waiting = true;
                        }
                        else
                            task.done = true;
                    }
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
        // The SDK's printf family on 32-bit arguments: argument slot k is a0-a3, then the caller's stack from sp + 16.
        std::string format(const NativeCpu &c, uint32_t formatAddress, uint32_t first)
        {
            const std::string f = string(formatAddress);
            uint32_t slot = first;
            const auto next = [&] { const uint32_t k = slot++; return k < 4 ? c.gpr[4 + k] : read(c.gpr[29] + 4 * k); };
            std::string out;
            for (size_t i = 0; i < f.size(); ++i)
            {
                if (f[i] != '%')
                {
                    out += f[i];
                    continue;
                }
                std::string spec = "%";
                ++i;
                while (i < f.size() && std::strchr("-+ #0", f[i]))
                    spec += f[i++];
                const auto number = [&]
                {
                    if (i < f.size() && f[i] == '*')
                    {
                        ++i;
                        spec += std::to_string(int32_t(next()));
                        return;
                    }
                    while (i < f.size() && std::isdigit(static_cast<unsigned char>(f[i])))
                        spec += f[i++];
                };
                number();
                if (i < f.size() && f[i] == '.')
                {
                    spec += f[i++];
                    number();
                }
                while (i < f.size() && (f[i] == 'l' || f[i] == 'h'))
                    ++i;
                if (i >= f.size())
                    break;
                const char conversion = f[i];
                const auto append = [&](const std::string &with, auto value)
                {
                    const int bytes = std::snprintf(nullptr, 0, with.c_str(), value);
                    if (bytes <= 0)
                        return;
                    std::string text(size_t(bytes) + 1, '\0');
                    std::snprintf(text.data(), text.size(), with.c_str(), value);
                    out.append(text.data(), size_t(bytes));
                };
                switch (conversion)
                {
                case 'd': case 'i':
                    append(spec + 'd', int32_t(next()));
                    break;
                case 'u': case 'x': case 'X': case 'o':
                    append(spec + conversion, unsigned(next()));
                    break;
                case 'p':
                    append("%x", unsigned(next()));
                    break;
                case 'c':
                    append(spec + 'c', int(uint8_t(next())));
                    break;
                case 's':
                {
                    const uint32_t at = next();
                    const std::string text = at ? string(at) : "(null)";
                    append(spec + 's', text.c_str());
                    break;
                }
                case '%':
                    out += '%';
                    break;
                default:
                    throw std::runtime_error(std::format("unsupported native printf conversion %{}", conversion));
                }
            }
            return out;
        }
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
            deliverFrames();
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
                // The mix stops at each input half so its handler and the tasks it wakes refill that half in time.
                // A task's charge may already have run past it: the mix then catches up to the cycle and stops there.
                if (const uint64_t half = mixer.nextInputHalfCycle(); half != 0 && half < next)
                {
                    next = std::max(half, cycle);
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
                    if (timer->second.direct)
                    {
                        const auto handler = timer->second.handler;
                        const auto revision = timer->second.revision;
                        if (handler.function)
                        {
                            const auto nextCompare = invoke(handler.function, handler.gp, handler.argument);
                            const auto after = timers.find(id);
                            if (after != timers.end() && after->second.revision == revision)
                            {
                                // Callback result supplies the next native period; zero cancels.
                                after->second.compare = nextCompare;
                                if (!nextCompare) after->second.mode = 0;
                                else after->second.due = cycle + after->second.period();
                            }
                        }
                        continue;
                    }
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
                    struct OwnedMessage {
                        Impl &owner;uint32_t buffer;
                        ~OwnedMessage(){if(buffer)owner.heap.release(buffer);}
                    } owned{*this,event.ownedBuffer};
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
                throw std::runtime_error(std::format("no native libsd export {}", n));
            ret(result);
            return;
        }
        if (lib == "loadcore")
        {
            // Native linking and cache coherence are supplied by the host executable.
            if (n == 5 || n == 6 || n == 7 || n == 12)
            {
                ret();
                return;
            }
        }
        if ((lib == "stdio" && n == 4) || (lib == "sysmem" && n == 14))
        {
            if (libraries.printOutput)
                std::fprintf(stdout, "[native-irx] %s", format(c, a0, 1).c_str());
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
            const auto compare = [&](uint32_t left, uint32_t right, uint32_t limit, bool strings) -> uint32_t
            {
                for (uint32_t i = 0; i < limit; ++i)
                {
                    const uint8_t x = memory[physical(left + i, 1)], y = memory[physical(right + i, 1)];
                    if (x != y)
                        return uint32_t(int32_t(x) - int32_t(y));
                    if (strings && !x)
                        break;
                }
                return 0;
            };
            if (n == 11)
            {
                ret(compare(a0, a1, a2, false));
                return;
            }
            if (n == 22)
            {
                ret(compare(a0, a1, 0xffffffffu, true));
                return;
            }
            if (n == 19)
            {
                const auto text = format(c, a1, 2);
                copy(a0, reinterpret_cast<const uint8_t *>(text.c_str()), uint32_t(text.size() + 1));
                ret(uint32_t(text.size()));
                return;
            }
            if (n == 20 || n == 23)
            {
                const auto text = string(a1, 0x100000);
                const uint32_t at = n == 20 ? a0 + uint32_t(string(a0, 0x100000).size()) : a0;
                copy(at, reinterpret_cast<const uint8_t *>(text.c_str()), uint32_t(text.size() + 1));
                ret(a0);
                return;
            }
            if (n == 30)
            {
                const auto text = string(a1, 0x100000);
                std::vector<uint8_t> bytes(a2, 0);
                std::memcpy(bytes.data(), text.data(), std::min<size_t>(text.size(), a2));
                if (a2)
                    copy(a0, bytes.data(), a2);
                ret(a0);
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
            if (n == 7)
            {
                ret(heap.largestFree());
                return;
            }
            if (n == 8)
            {
                ret(heap.totalFree());
                return;
            }
        }
        if (lib == "thsemap")
        {
            if(n==4)
            {
                const auto attr=read(a0),option=read(a0+4);
                const auto initial=static_cast<int32_t>(read(a0+8)),max=static_cast<int32_t>(read(a0+12));
                if(attr>1)throw std::runtime_error("native library semaphore supports FIFO/priority attributes only");
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
                if(!s.waiting.empty()) {
                    auto selected=s.waiting.begin();
                    if(s.attr==1)selected=std::min_element(s.waiting.begin(),s.waiting.end(),[&](int32_t a,int32_t b){
                        return tasks.at(a).priority<tasks.at(b).priority;
                    });
                    const auto id=*selected;s.waiting.erase(selected);wake(id,0);
                }
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
            if (n == 5 || n == 10)
            {
                const auto it = tasks.find(int32_t(a0));
                if (!a0 || int32_t(a0) == logicalTask())
                {
                    ret(uint32_t(-406));
                    return;
                }
                if (it == tasks.end())
                {
                    ret(uint32_t(-407));
                    return;
                }
                const bool dormant = !it->second.started || it->second.done;
                if (n == 5 && !dormant)
                {
                    ret(uint32_t(-414));
                    return;
                }
                if (n == 10 && dormant)
                {
                    ret(uint32_t(-413));
                    return;
                }
                for (auto &[id, sema] : semas)
                    std::erase(sema.waiting, it->first);
                if (n == 5)
                    tasks.erase(it);
                else
                {
                    auto &t = it->second;
                    t.done = true;
                    t.waiting = t.sleeping = false;
                    t.waitingSema = 0;
                    t.due = 0;
                }
                ret();
                return;
            }
            if (n == 8)
            {
                if (!current)
                    throw std::runtime_error("native root call attempted ExitThread");
                tasks.at(current).done = true;
                ret();
                return;
            }
            if (n == 27 || n == 28)
            {
                const auto it = tasks.find(a0 ? int32_t(a0) : logicalTask());
                if (it == tasks.end())
                {
                    ret(uint32_t(-407));
                    return;
                }
                ret(uint32_t(it->second.wakeups));
                it->second.wakeups = 0;
                return;
            }
            if (n == 6)
            {
                auto &t = tasks.at(int32_t(a0));
                if (t.done)
                {
                    const uint32_t gp = t.cpu.gpr[28];
                    t.cpu = NativeCpu{};
                    t.cpu.pc = t.entry;
                    t.cpu.gpr[28] = gp;
                    t.cpu.gpr[29] = t.stack + t.stackBytes;
                    t.cpu.gpr[31] = kReturn;
                    t.done = false;
                    t.wakeups = 0;
                }
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
            if (n == 35 || n == 36)
            {
                for (const auto &[id, t] : timers)
                    if (t.alarm && t.mode && t.handler.function == a1 && t.handler.argument == a2)
                    {
                        ret(uint32_t(-104));
                        return;
                    }
                const bool admitted = std::any_of(images.begin(), images.end(), [&](const auto &image) {
                    return a1 >= image.base && uint64_t(a1) < uint64_t(image.base) + image.bytes.size();
                });
                if (!admitted || !functions.contains(a1) || imports.contains(a1))
                    throw std::runtime_error("native alarm handler requires an admitted linked function");
                if (read(a0 + 4) || !read(a0))
                    throw std::runtime_error("native alarm interval outside 1..2^32-1 clocks");
                auto &t = timers[nextTimer++];
                t.alarm = t.direct = t.configured = true;
                t.handler = {a1, a2, c.gpr[28], false};
                t.compare = read(a0);
                t.mode = 1;
                t.due = cycle + t.period();
                ret();
                return;
            }
            if (n == 37 || n == 38)
            {
                for (auto it = timers.begin(); it != timers.end(); ++it)
                    if (it->second.alarm && it->second.handler.function == a0 && it->second.handler.argument == a1)
                    {
                        const bool active = it->second.mode != 0;
                        timers.erase(it);
                        ret(active ? 0u : uint32_t(-105));
                        return;
                    }
                ret(uint32_t(-105));
                return;
            }
            if (n == 39)
            {
                const uint64_t clocks = uint64_t(a0) * 36864 / 1000;
                write(a1, uint32_t(clocks));
                write(a1 + 4, uint32_t(clocks >> 32));
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
                if (token) exclusions.push_back({logicalTask(),token});
                ret(token ? 0u : uint32_t(-102));
                return;
            }
            if (n == 18)
            {
                if (exclusions.empty() || exclusions.back().owner != logicalTask() || a0 > 1u)
                    throw std::runtime_error("unbalanced native callback exclusion release");
                if (a0) exclusions.pop_back();
                ret();return;
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
            if (n == 8)
            {
                if (!exclusions.empty())
                {
                    ret(uint32_t(-102));
                    return;
                }
                exclusions.push_back({logicalTask(), kCpuDisableScope});
                ret();
                return;
            }
            if (n == 9)
            {
                if (!exclusions.empty())
                {
                    if (exclusions.back().token != kCpuDisableScope || exclusions.back().owner != logicalTask())
                        throw std::runtime_error("native CpuEnableIntr cannot replace a scoped callback release");
                    exclusions.pop_back();
                }
                ret();
                return;
            } // native callbacks are only delivered at safe points
        }
        if (lib == "vblank" && n == 5)
        {
            if (!current)
                throw std::runtime_error("native root WaitVblankEnd");
            if (!exclusions.empty()) throw std::runtime_error("native frame wait with callbacks excluded");
            auto &t = tasks.at(current);
            t.waiting = true;
            t.due = (cycle / kFrameTick + 1) * kFrameTick;
            ret();
            return;
        }
        if (lib == "vblank" && (n == 8 || n == 9) && nativeFrameCallbacks)
        {
            if (a0) throw std::runtime_error("native frames have no display-end phase");
            if (n == 9)
            {
                ret(frameListeners.erase(a1) ? 0 : uint32_t(-105));return;
            }
            if (!a2 || !functions.contains(a2) || imports.contains(a2) ||
                std::none_of(images.begin(),images.end(),[&](const auto &image){
                    return a2 >= image.base && uint64_t(a2) < uint64_t(image.base)+image.bytes.size();
                })) throw std::runtime_error("native frame listener requires an admitted linked function");
            if (frameListeners.contains(a2)) {ret(uint32_t(-104));return;}
            frameListeners.emplace(a2,FrameListener{{a2,a3,c.gpr[28],false},int32_t(a1),++frameListenerSerial});
            ret();return;
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
            if (n == 20)
            {
                if (timer.mode) { ret(uint32_t(-156));return; }
                if (a2)
                {
                    const bool admitted = std::any_of(images.begin(), images.end(), [&](const auto &image) {
                        return a2 >= image.base && uint64_t(a2) < uint64_t(image.base) + image.bytes.size();
                    });
                    if (!admitted || !functions.contains(a2) || imports.contains(a2))
                        throw std::runtime_error("native timer handler requires an admitted linked function");
                    if (!a1) throw std::runtime_error("native timer callback requires a nonzero period");
                }
                timer.direct = true;timer.handler = {a2,a3,c.gpr[28],false};timer.compare = a1;
                ++timer.revision;ret();return;
            }
            if (n == 22)
            {
                if (timer.mode) { ret(uint32_t(-154));return; }
                // Gated modes require an external signal; no device gate is synthesized.
                if (a2) throw std::runtime_error("unported native timer gate mode");
                if (a1 != 1 && a1 != 2 && a1 != 4)
                    throw std::runtime_error("unsupported native timer clock source");
                if (a3 != 1 && a3 != 8 && a3 != 16 && a3 != 256)
                    throw std::runtime_error("unsupported native timer clock divisor");
                timer.rate = a1 == 4 ? 15734 : a1 == 2 ? 13500000 : 36864000;
                timer.prescale = a3;timer.configured = true;++timer.revision;ret();return;
            }
            if (n == 23)
            {
                if (timer.mode) { ret(uint32_t(-154));return; }
                if (!timer.configured) { ret(uint32_t(-155));return; }
                if (!timer.direct || !timer.handler.function || !timer.compare)
                    throw std::runtime_error("native periodic timer requires a registered callback");
                timer.mode = 1;timer.due = cycle + timer.period();++timer.revision;ret();return;
            }
            if (n == 24)
            {
                if (!timer.mode) { ret(uint32_t(-156));return; }
                timer.mode = 0;++timer.revision;ret();return;
            }
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
            if (n == 10)
            {
                const uint64_t period = timer.period(), start = timer.due > period ? timer.due - period : 0;
                const uint64_t elapsed = timer.mode && cycle > start ? cycle - start : 0;
                ret(uint32_t(elapsed * timer.rate / (uint64_t(36864000) * timer.prescale)));
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
            if (n == 12 || n == 13)
            {
                if (a2<16 || a2>112) {ret();return;}
                if (a0&0x80000000u) throw std::runtime_error("native user messages do not process system commands");
                if (!libraries.sif.receiveCommand) throw std::runtime_error("native EE user-message receiver is not installed");
                const auto destination=read(c.gpr[29]+16),suppliedExtra=read(c.gpr[29]+20);
                const uint32_t extraBytes=int32_t(suppliedExtra)>0?suppliedExtra:0;
                if (extraBytes>0xffffffu || uint64_t(a1)+a2>memory.size() ||
                    (extraBytes && uint64_t(a3)+extraBytes>memory.size()))
                    throw std::runtime_error("native outgoing user-message data is outside owned storage");
                std::vector<uint8_t> data(memory.begin()+a1,memory.begin()+a1+a2),extra;
                if(extraBytes)extra.assign(memory.begin()+a3,memory.begin()+a3+extraBytes);
                const uint32_t header[3]={a2|(extraBytes<<8),extraBytes?destination:0,a0};
                std::memcpy(data.data(),header,sizeof(header));
                const auto job=libraries.sif.receiveCommand(a0,data,extraBytes?destination:0,extra);
                if(job)copy(a1,data.data(),a2);
                ret(job);return;
            }
            if (n == 8)
            {
                if (a1 > memory.size()/8 || (!a0 && a1) || uint64_t(a0)+uint64_t(a1)*8>memory.size())
                    throw std::runtime_error("invalid native user command table");
                if (a1) physical(a0,a1*8);
                commandBuffer=a0;commandCapacity=a1;commandHandlers.clear();ret();return;
            }
            if (n == 10 || n == 11)
            {
                if (a0 >= commandCapacity || !commandBuffer)
                    throw std::runtime_error("native user command outside registered table");
                if (n == 10 && (!a1 || !functions.contains(a1) || imports.contains(a1) ||
                    std::none_of(images.begin(),images.end(),[&](const auto &image){
                        return a1 >= image.base && uint64_t(a1) < uint64_t(image.base)+image.bytes.size();
                    }))) throw std::runtime_error("native user command requires an admitted linked handler");
                if (n == 10) commandHandlers[a0]={a1,a2,c.gpr[28],false};
                else commandHandlers.erase(a0);
                write(commandBuffer+a0*8,n==10?a1:0);write(commandBuffer+a0*8+4,n==10?a2:0);
                ret();return;
            }
            if (n == 14)
            {
                ret();
                return;
            }
            if (n == 23)
            {
                auto *dispatcher = libraries.dispatcher();
                if (!dispatcher) throw std::runtime_error("native EE data read requires attached EE data");
                auto &ee = dispatcher->runtime().memory;
                const auto *from = a3 ? ee.hostPointer(a1, a3) : nullptr;
                if (a3 && (!from || from < ee.ram() || uint64_t(from - ee.ram()) + a3 > 0x2000000u))
                    throw std::runtime_error("native EE data read source is outside EE RAM");
                if (a3) copy(a2, from, a3);
                ret(0);
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
                    const uint32_t reply = serve(ownerTask, function, gp, request.command, buffer, request.sendBytes);
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
            if (n == 15)
            {
                const auto found = libraries.sif.eeServers.find(a1);
                write(a0 + 0x24, a1 && found != libraries.sif.eeServers.end() ? a1 : 0);
                ret();
                return;
            }
            if (n == 16)
            {
                const uint32_t sp = c.gpr[29], sendBytes = read(sp + 16), receive = read(sp + 20),
                               receiveBytes = read(sp + 24), end = read(sp + 28), endArgument = read(sp + 32);
                const uint32_t service = read(a0 + 0x24);
                const auto found = libraries.sif.eeServers.find(service);
                if (!service || found == libraries.sif.eeServers.end())
                    throw std::runtime_error(std::format("native IOP call to an unbound EE RPC client 0x{:x}", a0));
                if (end && (!functions.contains(end) || imports.contains(end) ||
                            std::none_of(images.begin(), images.end(), [&](const auto &image) {
                                return end >= image.base && uint64_t(end) < uint64_t(image.base) + image.bytes.size();
                            })))
                    throw std::runtime_error("native EE RPC completion requires an admitted linked function");
                const auto from = physical(a3, sendBytes);
                const std::vector<uint8_t> sent(memory.begin() + from, memory.begin() + from + sendBytes);
                std::vector<uint8_t> reply(receiveBytes);
                if (receiveBytes)
                    physical(receive, receiveBytes);
                found->second(a1, sent, reply);
                if (receiveBytes)
                    copy(receive, reply.data(), receiveBytes);
                write(a0 + 0x10, a1);
                if (end)
                    events.push_back({cycle, end, endArgument, 0, c.gpr[28]});
                ret();
                return;
            }
            if (n == 24)
            {
                libraries.sif.servers.erase(read(a0 + 0x20));
                ret(a0);
                return;
            }
            if (n == 19 || n == 25)
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
        if (lib == "cdvdman" && nativeDiscFiles)
        {
            // SDK ordinals: ps2dev/ps2sdk iop/cdvd/cdvdman/include/cdvdman.h.
            // This is a synchronous native file backend. The request exists
            // only while actual host I/O and the bounded destination copy run.
            if(n==11){ret(!discRequests.empty());return;}
            if(n==8){ret(discError);return;}
            // Drive commands with nothing to move complete at once; the asynchronous ones notify with their
            // SCECdFunc code (seek 4, standby 5, stop 6, pause 7) as the read does with 1.
            auto commandDone=[&](uint32_t code)
            {
                if(discCallback)
                {
                    if(events.size()>=1024)throw std::runtime_error("native disc notification queue limit");
                    Event completion{cycle,discCallback,code,0,discCallbackGp};completion.readyDisc=true;
                    events.push_back(completion);
                }
                discError=0;ret(1);
            };
            if(n==4 || n==75){ret(1);return;}
            if(n==7){commandDone(4);return;}
            if(n==5){commandDone(5);return;}
            if(n==15){commandDone(6);return;}
            if(n==38){commandDone(7);return;}
            if(n==39){ret(1);return;}
            if(n==12){ret(0x14);return;}
            if(n==13){ret(2);return;}
            if(n==28){ret(0x0A);return;}
            if(n==14)
            {
                if(a1)write(a1,0);
                ret(1);return;
            }
            if(n==24)
            {
                int sec=0,min=0,hour=12,day=18,month=6,year=1;
                if(libraries.cd.hostClock)
                {
                    const std::time_t t=std::time(nullptr);
                    if(const std::tm *tm=std::localtime(&t))
                    {sec=tm->tm_sec;min=tm->tm_min;hour=tm->tm_hour;day=tm->tm_mday;month=tm->tm_mon+1;year=tm->tm_year%100;}
                }
                auto bcd=[](int v){return uint8_t(((v/10)<<4)|(v%10));};
                const uint8_t clock[8]={0,bcd(sec),bcd(min),bcd(hour),0,bcd(day),bcd(month),bcd(year)};
                std::copy(clock,clock+8,memory.begin()+physical(a0,8));
                ret(1);return;
            }
            if(n==37)
            {
                const bool admitted=std::any_of(images.begin(),images.end(),[&](const auto &image){
                    return a0>=image.base && uint64_t(a0)<uint64_t(image.base)+image.bytes.size();
                });
                if(a0 && (!admitted || !functions.contains(a0) || imports.contains(a0)))
                    throw std::runtime_error("native disc callback requires an admitted linked function");
                const auto old=discCallback;discCallback=a0;discCallbackGp=a0?gpAt(a0):0;
                ret(old);return;
            }
            if(n==6)
            {
                if(!a1 || a1>memory.size()/libs::kSectorBytes)
                    throw std::runtime_error("native disc read count outside buffer bounds");
                const uint32_t bytes=a1*libs::kSectorBytes;
                const auto destination=physical(a2,bytes),mode=physical(a3,3);
                // Only ordinary 2048-byte ISO data sectors are qualified.
                if(memory[mode+2]!=0)throw std::runtime_error("unported native disc sector format");
                if(discCallback && events.size()>=1024)throw std::runtime_error("native disc notification queue limit");
                const auto request=++discRequestSerial;discRequests.insert(request);
                struct Scope {std::set<uint64_t> &requests;uint64_t id;~Scope(){requests.erase(id);}} scope{discRequests,request};
                auto &disc=libraries.cd.disc;std::string error;
                if((!disc.isOpen() && !disc.open(libraries.cd.imagePath,error)) ||
                   uint64_t(a0)+a1>disc.sectors())
                {discError=0x30;ret(0);return;}
                std::vector<uint8_t> data(bytes);
                if(!disc.read(a0,a1,data.data())){discError=0x30;ret(0);return;}
                std::copy(data.begin(),data.end(),memory.begin()+destination);discError=0;
                if(discCallback)
                {
                    Event completion{cycle,discCallback,1,0,discCallbackGp};completion.readyDisc=true;
                    events.push_back(completion);
                }
                ret(1);return;
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
    int32_t CompiledLibraries::startImage(const std::string &name, std::span<const std::string> arguments)
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
        {
            uint32_t argc = 0, argv = 0;
            if (!arguments.empty())
            {
                argc = uint32_t(arguments.size()) + 1;
                const std::string module = name.substr(0, name.find(';'));
                uint32_t bytes = (argc + 1) * 4 + uint32_t(module.size()) + 1;
                for (const auto &argument : arguments)
                    bytes += uint32_t(argument.size()) + 1;
                argv = p.heap.allocate(bytes);
                if (!argv)
                    throw std::runtime_error("native module arguments do not fit the heap");
                uint32_t text = argv + (argc + 1) * 4, slot = argv;
                const auto put = [&](const std::string &value)
                {
                    p.write(slot, text);
                    slot += 4;
                    p.copy(text, reinterpret_cast<const uint8_t *>(value.c_str()), uint32_t(value.size()) + 1);
                    text += uint32_t(value.size()) + 1;
                };
                put(module);
                for (const auto &argument : arguments)
                    put(argument);
                p.write(slot, 0);
            }
            p.invoke(image->entry, image->gp, argc, argv);
        }
        p.runReady();
        const int32_t id = int32_t(p.started.size() + 1);
        p.started[name] = id;
        return id;
    }
    void CompiledLibraries::useReadyAudioCompletions()
    {
        auto &p = *impl_;
        if (!p.started.empty() || p.invokeDepth || !p.tasks.empty() || p.calls)
            throw std::runtime_error("native audio completion policy must be installed before execution");
        p.readyAudioCompletions = true;
    }
    void CompiledLibraries::useNativeFrameCallbacks()
    {
        auto &p = *impl_;
        if (!p.started.empty() || p.invokeDepth || !p.tasks.empty() || p.calls)
            throw std::runtime_error("native frame policy must be installed before execution");
        p.nativeFrameCallbacks = true;
    }
    void CompiledLibraries::useNativeDiscFiles()
    {
        auto &p=*impl_;
        if(!p.started.empty() || p.invokeDepth || !p.tasks.empty() || p.calls)
            throw std::runtime_error("native disc file policy must be installed before execution");
        p.nativeDiscFiles=true;
    }
    void CompiledLibraries::notifyFrameCompleted()
    {
        auto &p = *impl_;
        if (!p.nativeFrameCallbacks) throw std::runtime_error("native frame callbacks are not enabled");
        if (p.completedFrames.size() >= 1024) throw std::runtime_error("native frame callback queue limit");
        std::vector<std::pair<uint32_t,uint64_t>> listeners;
        for (const auto &[function,listener] : p.frameListeners) listeners.emplace_back(function,listener.order);
        std::stable_sort(listeners.begin(),listeners.end(),[&](const auto &a,const auto &b){
            const auto &first=p.frameListeners.at(a.first), &second=p.frameListeners.at(b.first);
            return first.priority != second.priority ? first.priority < second.priority : first.order < second.order;
        });
        p.completedFrames.push_back(std::move(listeners));
        if (!p.invokeDepth && !p.current) p.deliverFrames();
    }
    uint32_t CompiledLibraries::submitCommand(uint32_t command,std::span<const uint8_t> data)
    {
        auto &p = *impl_;
        const auto found=p.commandHandlers.find(command);
        if (command >= p.commandCapacity || found == p.commandHandlers.end())
            throw std::runtime_error("no registered native user command handler");
        if (data.size()<16 || data.size()>112)
            throw std::runtime_error("native user command data must be 16..112 bytes");
        const auto callback=found->second;
        if (p.read(p.commandBuffer+command*8)!=callback.function ||
            p.read(p.commandBuffer+command*8+4)!=callback.argument)
            throw std::runtime_error("native user command table changed outside registration");
        if (p.commandSerial==0x7fffffffu || p.events.size()>=1024)
            throw std::runtime_error("native user command queue exhausted");
        const auto buffer=p.heap.allocate(uint32_t(data.size()));
        if (!buffer) throw std::runtime_error("native user command allocation failed");
        p.copy(buffer,data.data(),uint32_t(data.size()));
        p.events.push_back({p.cycle,callback.function,buffer,callback.argument,callback.gp,false,buffer});
        return ++p.commandSerial;
    }
    void CompiledLibraries::reset()
    {
        auto &p = *impl_;
        p.tasks.clear();
        p.events.clear();
        p.frameListeners.clear();p.completedFrames.clear();p.frameListenerSerial=0;p.deliveringFrames=false;
        p.commandBuffer=0;p.commandCapacity=0;p.commandSerial=0;p.commandHandlers.clear();
        p.deliveringReadyAudio = false;
        p.discRequests.clear();p.discRequestSerial=0;p.discError=0;p.discCallback=0;p.discCallbackGp=0;p.deliveringReadyDisc=false;
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
    std::span<uint8_t> CompiledLibraries::allocatedBuffer(uint32_t address,uint32_t bytes)
    {
        auto &p = *impl_;
        if (!p.heap.contains(address,bytes))
            throw std::runtime_error(std::format("native buffer span is not a live allocation: 0x{:x}, {} bytes",address,bytes));
        return {p.memory.data()+address,bytes};
    }
    uint64_t CompiledLibraries::nativeCalls() const { return impl_->calls; }
} // namespace bozo::native::irx
