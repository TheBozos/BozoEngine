// The port API. See bozo/port.h and docs/specs/kernel.md section 3b.
#include "bozo/port.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <set>

namespace bozo::port
{
    namespace
    {
        const Port *g_port = nullptr;
    }

    void Registry::registerFunction(uint32_t address, Function fn)
    {
        for (BozoFunctionEntry &e : functions_)
            if (e.address == address)
            {
                e.function = fn;
                return;
            }
        functions_.push_back({address, fn});
    }

    Function Registry::find(uint32_t address) const
    {
        for (const BozoFunctionEntry &e : functions_)
            if (e.address == address)
                return e.function;
        return nullptr;
    }

    void setPort(const Port *p) { g_port = p; }
    const Port *port() { return g_port; }

    std::vector<uint32_t> undeclared(const Registry &registry, const BozoExternalEntry *table, size_t count)
    {
        std::set<uint32_t> declared;
        for (size_t i = 0; i < count; ++i)
            declared.insert(table[i].address);
        std::vector<uint32_t> out;
        for (const BozoFunctionEntry &e : registry.functions())
            if (!declared.count(e.address))
                out.push_back(e.address);
        return out;
    }

    ee::Dispatcher &dispatcher(Runtime *rt) { return *static_cast<ee::Dispatcher *>(rt->services); }
    ee::Kernel &kernel(Runtime *rt) { return dispatcher(rt).kernel(); }
    Host &host(Runtime *rt) { return *rt->port; }
    libs::Libraries &libraries(Runtime *rt) { return rt->port->libraries(); }
    uint8_t *scratchpad(Runtime *rt) { return rt->memory.scratchpad(); }
    bool deterministic(Runtime *rt) { return !rt->port || rt->port->deterministic(); }
    bool realTime(Runtime *rt) { return rt->port && rt->port->realTime(); }

    uint64_t now(Runtime *rt) { return dispatcher(rt).now(); }

    void accountCycles(Runtime *rt, R5900Context *ctx, uint64_t cycles)
    {
        // As a checkpoint would, it takes the timer-poll cycles pending, which the time charged here covers (match1p
        // #17538393: a frame end charging a field leaves none pending after its T0 read; #17541951, charging nothing,
        // leaves its 512).
        ee::Dispatcher &d = dispatcher(rt);
        if (cycles == 0)
            return;
        d.setTimerPending(0);
        d.setNow(ctx, d.now(ctx) + cycles);
    }

    uint64_t vblankCount(Runtime *rt) { return kernel(rt).vblankCount(); }

    int32_t currentThread(Runtime *rt)
    {
        const ee::Thread *t = dispatcher(rt).currentThread();
        return t ? t->id : 0;
    }

    uint32_t currentPriority(Runtime *rt)
    {
        const ee::Thread *t = dispatcher(rt).currentThread();
        return t ? t->priority : 0;
    }

    int32_t changePriority(Runtime *rt, int32_t thread, uint32_t priority, uint32_t &old)
    {
        const int32_t r = kernel(rt).changePriority(thread, priority, false);
        if (r < 0)
            return r;
        old = static_cast<uint32_t>(r);
        return 0;
    }

    int32_t rotateReadyQueue(Runtime *rt, uint32_t priority) { return kernel(rt).rotateReadyQueue(priority, false); }

    bool transfer(Runtime *rt, R5900Context *ctx)
    {
        ee::Dispatcher &d = dispatcher(rt);
        if (!d.kernel().transfer(false))
            return false;
        d.leave(ctx, ctx->pc);
        return true;
    }

    uint32_t ioRead32(Runtime *rt, R5900Context *ctx, uint32_t address)
    {
        libs::IoState &io = libraries(rt).io;
        io.hostAccess = true;
        const uint32_t v = static_cast<uint32_t>(bozo_io_read(ctx, rt, address, 4));
        io.hostAccess = false;
        return v;
    }

    void ioWrite32(Runtime *rt, R5900Context *ctx, uint32_t address, uint32_t value)
    {
        libs::IoState &io = libraries(rt).io;
        io.hostAccess = true;
        bozo_io_write(ctx, rt, address, 4, value);
        io.hostAccess = false;
    }

    void frameEnd(Runtime *rt, R5900Context *ctx) { rt->port->frameEnd(rt, ctx); }

    void callLibrary(Runtime *rt, R5900Context *ctx, uint32_t address)
    {
        if (!rt->port->callLibrary(rt, ctx, address))
            static_cast<ee::Dispatcher *>(rt->services)->host().fault(dispatcher(rt), ctx,
                std::format("an override calls the library at 0x{:x}, which nothing native answers", address));
    }
}
