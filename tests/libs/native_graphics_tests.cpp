// Regression coverage for the packet-free native graphics boundary.
#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace
{
    template <typename T> concept HasGsState = requires(T t) { t.gs; };
    template <typename T> concept HasGsRegisters = requires(T t) { t.gsPrivileged; };
    static_assert(!HasGsState<bozo::libs::Libraries>);
    static_assert(!HasGsRegisters<bozo::libs::IoState>);
    int failures = 0;
    void check(bool pass, const char *message)
    {
        if (!pass) { ++failures; std::printf("FAIL %s\n", message); }
    }

    struct Machine
    {
        std::unique_ptr<Runtime> rt = std::make_unique<Runtime>();
        R5900Context ctx;
        bozo::libs::Libraries libs;
        void call(const char *name, std::initializer_list<uint32_t> args)
        {
            int i = 0;
            for (auto a : args) ctx.r[4 + i++].ud[0] = a;
            bozo::libs::Guest g(*rt, &ctx);
            const auto *fn = libs.find(name);
            if (!fn) throw std::runtime_error(name);
            fn->handler(libs, g);
        }
        uint64_t word(uint32_t at)
        {
            uint64_t v;
            std::memcpy(&v, rt->memory.ram() + at, sizeof(v));
            return v;
        }
    };

    void sdkBoundary()
    {
        Machine m;
        // An inaccessible packet address must be irrelevant to native submit/upload/sync.
        for (const char *name : {"sceGsPutDrawEnv", "sceGsPutDispEnv", "sceGsExecLoadImage", "sceGsSyncPath"})
        {
            m.ctx.r[2].ud[0] = 123;
            m.call(name, {0x12000000, 0x12000000});
            check(m.ctx.r[2].sw[0] == 0, name);
        }
        std::memset(m.rt->memory.ram() + 0x101000, 0x5a, 256);
        bool rejected = false;
        try { m.call("sceGsExecStoreImage", {0x12000000, 0x101000}); }
        catch (const std::runtime_error &e)
        {
            rejected = std::strstr(e.what(), "native renderer readback") != nullptr;
        }
        check(rejected, "readback requires a native implementation without reading a packet");
        for (unsigned i = 0; i < 256; ++i)
            check(m.rt->memory.ram()[0x101000 + i] == 0x5a, "unsupported readback leaves destination intact");

        // SDK output builders remain usable; no separate engine GS state stores their results.
        m.call("sceGsResetGraph", {0, 1, 2, 0});
        m.call("sceGsSetDefDispEnv", {0x100000, 0, 640, 448, 0, 0});
        check(m.word(0x100008) == 1, "interlaced frame display template");
        m.call("sceGsResetGraph", {0, 0, 3, 1});
        m.call("sceGsSetDefDispEnv", {0x100000, 0, 640, 448, 0, 0});
        check(m.word(0x100008) == 2, "progressive display template");
        check((m.word(0x100018) & 0xfff) == 656, "PAL template uses SDK video parameters");
        m.call("sceGsSetDefDrawEnv", {0x102000, 0, 640, 448, 2, 0});
        check(m.ctx.r[2].sw[0] == 8, "draw-template ABI result");
        check(m.word(0x102040) == 1 && m.word(0x102050) == 1 && m.word(0x102060) == 0,
              "draw template initializes flags without reading old packet contents");
        m.libs.io.ctrl = 1;
        m.libs.io.channels[2].running = true;
        m.libs.io.stat = m.libs.io.cpcondDone = 4;
        m.call("sceDmaReset", {0});
        check(m.ctx.r[2].sw[0] == 1 && m.libs.io.ctrl == 1, "native DMA reset returns previous enable state");
        check(!m.libs.io.channels[2].running && m.libs.io.stat == 0 && m.libs.io.cpcondDone == 0,
              "native DMA reset clears completion bookkeeping without accessing devices");
        std::array<uint8_t, 20> config{};
        for (unsigned i = 4; i < config.size(); ++i) config[i] = static_cast<uint8_t>(i * 7);
        std::memcpy(m.rt->memory.ram() + 0x104000, config.data(), config.size());
        m.call("sceDmaPutEnv", {0x104000});
        check(m.ctx.r[2].sw[0] == 0, "native DMA SDK environment accepted");
        for (unsigned field = 0; field < 4; ++field)
        {
            m.rt->memory.ram()[0x104000 + field] = field == 3 ? 7 : 10;
            m.call("sceDmaPutEnv", {0x104000});
            check(m.ctx.r[2].sw[0] == -static_cast<int>(field) - 1, "DMA SDK environment validation result");
            m.rt->memory.ram()[0x104000 + field] = 0;
        }
        m.call("sceDmaGetEnv", {0x104100});
        check(std::memcmp(m.rt->memory.ram() + 0x104100, config.data(), config.size()) == 0,
              "native DMA SDK environment round-trips without device registers");
    }

    struct Host : bozo::ee::Host
    {
        void libraryCall(bozo::ee::Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("unexpected library call"); }
        void vu0Microprogram(bozo::ee::Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("unexpected VU0 call"); }
        void ioRead(bozo::ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override { throw std::runtime_error("unexpected device read"); }
        void ioWrite(bozo::ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override { throw std::runtime_error("unexpected device write"); }
        void fault(bozo::ee::Dispatcher &, R5900Context *, const std::string &what) override { throw std::runtime_error(what); }
    };
    struct NativePort : bozo::port::Host
    {
        bozo::libs::Libraries &l;
        explicit NativePort(bozo::libs::Libraries &value) : l(value) {}
        bozo::libs::Libraries &libraries() override { return l; }
        void frameEnd(Runtime *, R5900Context *) override {}
        bool callLibrary(Runtime *, R5900Context *, uint32_t) override { return false; }
        bool nativeGraphics() const override { return true; }
    };
    struct CompletionObserver : bozo::ee::KernelObserver
    {
        bozo::libs::Libraries &l;
        explicit CompletionObserver(bozo::libs::Libraries &value) : l(value) {}
        bool dmacStatusSet(uint32_t cause) override { return cause < 3 && l.io.nativeCompleted[cause]; }
    };
    void nativeJobs()
    {
        Host host;
        bozo::ee::Dispatcher d(host);
        bozo::libs::Libraries l;
        l.attach(d);
        NativePort port(l);
        d.runtime().port = &port;
        CompletionObserver observer(l);
        d.kernel().setObserver(&observer);
        d.kernel().addHandler(true, 2, 0x123400, -1, 0, 0, 0);
        d.kernel().causeEnable(true, 2, false);
        l.io.stat = 0x1234;
        l.io.cpcondDone = 0x2345;
        auto &old = l.io.channels[2];
        old.chcr = old.madr = old.qwc = old.tadr = 0xabcdef;
        old.running = true; // a stale compatibility bank must not govern native SDK jobs
        R5900Context ctx;
        bozo::libs::Guest guest(d.runtime(), &ctx);
        for (const char *name : {"sceGsSetDefLoadImage", "sceGsSetDefStoreImage"})
        {
            // Neither the inaccessible output nor stack arguments may be read: the
            // packet has no consumer in native graphics mode.
            ctx.r[4].uw[0] = ctx.r[29].uw[0] = 0x12000000;
            ctx.r[2].sw[0] = -99;
            l.find(name)->handler(l, guest);
            check(ctx.r[2].sw[0] == 0, "native image builder retires unused output without touching it");
            std::memset(d.runtime().memory.ram() + 0x100000, 0x5a, 256);
            ctx.r[4].uw[0] = 0x100000;
            l.find(name)->handler(l, guest);
            for (unsigned i = 0; i < 256; ++i)
                check(d.runtime().memory.ram()[0x100000 + i] == 0x5a, "native image builder leaves a valid output buffer unchanged");
        }
        ctx.r[4].uw[0] = 0x1000a000;
        ctx.r[5].uw[0] = 0x12000000;
        l.find("sceDmaSend")->handler(l, guest);
        check(!d.kernel().handlerQueued(), "masked native completion callback waits");
        d.kernel().causeEnable(true, 2, true);
        check(d.kernel().handlerQueued(), "native completion callback survives masking without a device status bank");
        ctx.r[5].uw[0] = 0;
        l.find("sceDmaSync")->handler(l, guest);
        check(ctx.r[2].sw[0] == 0, "native job completion ignores retired channel status");
        check(old.chcr == 0xabcdef && old.madr == 0xabcdef && old.qwc == 0xabcdef && old.tadr == 0xabcdef && old.running &&
                  l.io.stat == 0x1234 && l.io.cpcondDone == 0x2345,
              "native jobs do not touch retired DMA registers or completion conditions");
        l.find("sceDmaReset")->handler(l, guest);
        check(!l.io.nativeCompleted[2], "native reset clears native SDK job completion");
        check(l.io.stat == 0x1234 && old.chcr == 0xabcdef, "native reset leaves retired register bank untouched");
    }
    void completionBoundary()
    {
        Host host;
        bozo::ee::Dispatcher d(host);
        bozo::libs::Libraries l;
        l.attach(d);
        // PRE=1 previously selected the packet-reading/drop path. Both forms now acknowledge
        // native work identically. Neither a DMA chain nor a GIF packet executes.
        for (uint64_t tag : {0ull, 1ull << 46})
        {
            const uint32_t dmaTag = 0x10000001;
            std::memcpy(d.runtime().memory.ram() + 0x100000, &dmaTag, 4);
            std::memcpy(d.runtime().memory.ram() + 0x100010, &tag, 8);
            bozo::libs::ioStore(l, 0x1000a030, 4, 0x100000, 0);
            bozo::libs::ioStore(l, 0x1000a000, 4, 0x105, 0);
            uint32_t pending = 0;
            check((bozo::libs::ioLoad(l, 0x1000a000, 4, pending, 0) & 0x100) == 0,
                  "graphics completion clears busy without parsing PRE");
            check((l.io.stat & 4) != 0, "graphics completion acknowledges status");
            bozo::libs::ioStore(l, 0x1000e010, 4, 4, 0);
        }
        bozo::libs::ioStore(l, 0x12000000, 8, 0x1234, 0);
        check(bozo::libs::ioKnown(0x12000000, 8, true), "obsolete display store is acknowledged");
        l.io.channels[1].chcr = 0x30005;
        bozo::libs::ioStore(l, 0x10009000, 1, 0x41, 0);
        check(l.io.channels[1].chcr == 0x30041, "byte channel update preserves upper completion flags");
        bozo::libs::ioStore(l, 0x10009000, 2, 0x71, 0);
        check(l.io.channels[1].chcr == 0x30071, "halfword channel update preserves upper completion flags");

        R5900Context ctx;
        bozo::libs::Guest guest(d.runtime(), &ctx);
        d.kernel().addHandler(true, 1, 0x123400, -1, 0, 0, 0);
        for (const char *name : {"sceDmaSend", "sceDmaSendN", "sceDmaSendI"})
            for (uint32_t channel : {1u, 2u})
            {
                const auto *send = l.find(name);
                check(send != nullptr, "native SDK submission binding");
                if (!send) continue;
                auto &state = l.io.channels[channel];
                state.madr = 0x1111;
                state.tadr = 0x2222;
                state.qwc = 0x3333;
                ctx.r[4].uw[0] = channel == 1 ? 0x10009000 : 0x1000a000;
                ctx.r[5].uw[0] = 0x12000000; // inaccessible source/chain must never be read
                ctx.r[6].uw[0] = 0xffffffff;
                send->handler(l, guest);
                check(!state.running && !(state.chcr & 0x100), "native SDK graphics work completes immediately");
                check((l.io.stat & (1u << channel)) != 0, "native SDK completion preserves IRQ status");
                check(state.madr == 0x1111 && state.tadr == 0x2222 && state.qwc == 0x3333,
                      "native SDK does not retain unused packet addresses or lengths");
                ctx.r[5].uw[0] = 0;
                l.find("sceDmaSync")->handler(l, guest);
                check(ctx.r[2].sw[0] == 0, "native graphics sync does not poll a hardware register");
            }
        check(d.kernel().handlerQueued(), "native submission queues the registered completion handler");
        check(l.io.nativeSubmissions[1] == 3 && l.io.nativeSubmissions[2] == 3 && l.io.nativeSynchronizations == 6,
              "native graphics service counts all submissions and waits");
        bool rejected = false;
        ctx.r[4].uw[0] = 0x10009010; // field address, not a channel handle
        try { l.find("sceDmaSend")->handler(l, guest); }
        catch (const std::runtime_error &) { rejected = true; }
        check(rejected, "native SDK rejects an invalid channel handle");
        rejected = false;
        ctx.r[4].uw[0] = 0x1000c000; // an unrelated producer cannot be silently discarded
        try { l.find("sceDmaSend")->handler(l, guest); }
        catch (const std::runtime_error &) { rejected = true; }
        check(rejected, "native SDK requires a data producer for unsupported channels");
    }
}
int main()
{
    sdkBoundary();
    completionBoundary();
    nativeJobs();
    std::printf("native graphics boundary: %d failures\n", failures);
    return failures ? 1 : 0;
}
