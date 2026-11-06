// The hardware registers TMB still touches (libraries.md section 11). Every address answered here was settled on the
// call logs' io-load and io-store records (their values, and the interrupts raised inside them).
#include "bozo/libs/io.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include <stdexcept>
#include <cstring>

namespace bozo::libs
{
    namespace
    {
        // The DMA channels' register blocks (CHCR at +0, MADR +0x10, QWC +0x20, TADR +0x30).
        constexpr uint32_t kChannelBase[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                               0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};
        constexpr uint32_t kIpuTo = 4;

        int channelOf(uint32_t a, uint32_t &reg)
        {
            for (int ch = 0; ch < 10; ++ch)
                if (a >= kChannelBase[ch] && a < kChannelBase[ch] + 0x40u && (a & 0xFu) == 0)
                {
                    reg = (a - kChannelBase[ch]) >> 4;
                    return ch;
                }
            return -1;
        }

        constexpr uint64_t kNtscHblank = 18743; // a line of the default field rate, without a dispatcher

        // Timer n's register block: T0 0x10000000, T1 0x10000800, T2 0x10001000, T3 0x10001800.
        int timerOf(uint32_t a, uint32_t &reg)
        {
            if (a < 0x10000000u || a >= 0x10002000u)
                return -1;
            const uint32_t off = a & 0x7FFu;
            if (off != 0 && off != 0x10u && off != 0x20u)
                return -1;
            reg = off >> 4;
            return static_cast<int>((a - 0x10000000u) >> 11);
        }

        // A timer's period in EE cycles: its clock (Tn_MODE bits 0-1) is BUSCLK (half the EE's), /16, /256, the
        // horizontal blank.
        uint64_t timerPeriod(Libraries &l, uint32_t mode)
        {
            switch (mode & 3u)
            {
            case 0: return 2;
            case 1: return 32;
            case 2: return 512;
            default: return l.dispatcher() ? l.dispatcher()->kernel().hblank() : kNtscHblank;
            }
        }

        // A transfer completes: CHCR's STR clears, its D_STAT bit is set, the DMAC interrupt is raised (the kernel
        // runs the channel's handlers if it is enabled, else keeps it pending).
        void complete(Libraries &l, uint32_t ch)
        {
            IoState &io = l.io;
            IoState::Channel &c = io.channels[ch];
            c.running = false;
            c.chcr &= ~0x100u;
            io.stat |= 1u << ch;
            io.cpcondDone |= 1u << ch;
            l.dispatcher()->kernel().irq(true, ch);
        }

    }

    bool ioKnown(uint32_t address, uint32_t size, bool store)
    {
        const uint32_t a = address & 0x1FFFFFFFu;
        uint32_t reg = 0;
        if (timerOf(a, reg) >= 0)
            return size == 4;
        if (channelOf(a, reg) >= 0)
            return size == 1 || size == 2 || size == 4;
        if (a >= 0x1000E000u && a <= 0x1000E060u && (a & 0xFu) == 0)
            return size == 4;
        if (a == 0x1000F520u || a == 0x1000F590u)
            return size == 4;
        if (a == 0x1000D080u || a == 0x1000D480u)
            return size == 4;
        if (store && a >= 0x12000000u && a <= 0x120000F0u && (a & 0xFu) == 0)
            return size == 8;
        if (a == 0x12001000u) // FIELD reads; obsolete CSR acknowledgement stores have no effect
            return size == 8 || size == 4;
        if (a == 0x12001010u || a == 0x12001080u) // IMR, SIGLBLID: no device behind them, nothing raises INTC 0
            return store && (size == 8 || size == 4);
        if (a >= 0x10002000u && a <= 0x10002030u && (a & 0xFu) == 0) // IPU_CMD, IPU_CTRL, IPU_BP, IPU_TOP: an idle decoder
            return size == 4 && (!store || a == 0x10002010u);
        if (a == 0x10003020u || a == 0x10003800u || a == 0x10003C00u) // GIF_STAT, VIF0_STAT, VIF1_STAT: idle
            return !store && size == 4;
        if (a == 0x10003010u || a == 0x10003820u || a == 0x10003C20u) // GIF_MODE, VIF0_ERR, VIF1_ERR: no path, no error
            return store && size == 4;
        return false;
    }

    uint64_t ioLoad(Libraries &l, uint32_t address, uint32_t, uint32_t &timerPending, uint64_t cycle)
    {
        IoState &io = l.io;
        const uint32_t a = address & 0x1FFFFFFFu;
        uint64_t v = 0;
        uint32_t reg = 0;
        if (const int n = timerOf(a, reg); n >= 0)
        {
            IoState::Timer &t = io.timers[static_cast<uint32_t>(n)];
            if (reg == 0)
            {
                // Tn_COUNT: the count written last plus the periods since; reading the same count again (a poll loop)
                // charges a period at the next checkpoint (calllog.md section 2).
                const uint64_t period = timerPeriod(l, t.mode);
                const uint32_t count = static_cast<uint32_t>((t.start + (cycle - t.base) / period) & 0xFFFFu);
                if (count == t.lastRead)
                    timerPending = static_cast<uint32_t>(period);
                t.lastRead = count;
                v = count;
            }
            else
                v = reg == 1 ? t.mode : t.comp;
        }
        else if (a == 0x12001000u)
        {
            // CSR: FIELD (bit 13) is the vblank count's parity (read inside TMB's vblankHandler override, boot #729700
            // on: gp - 0x36d4 alternates 1, 0, ... from an odd count). Nothing else of it is observed.
            v = static_cast<uint64_t>(l.dispatcher()->kernel().vblankCount() & 1u) << 13;
        }
        else if (const int ch = channelOf(a, reg); ch >= 0)
        {
            // CHCR reads back (STR clear once the transfer completed); MADR, QWC and TADR read 0, as the reference
            // answers them (TMB reads channel 4's MADR, always 0 in the logs, also before any transfer).
            v = reg == 0 ? io.channels[static_cast<uint32_t>(ch)].chcr : 0;
        }
        else if (a == 0x1000E000u)
            v = io.ctrl;
        else if (a == 0x1000E010u)
            v = io.stat;
        else if (a == 0x1000E020u)
            v = io.pcr;
        else if (a == 0x1000D080u)
            v = io.sprAddress[0];
        else if (a == 0x1000D480u)
            v = io.sprAddress[1];
        else if (a == 0x1000F520u)
            v = 0; // D_ENABLER: no transfer is ever suspended (none runs)
        return v;
    }

    void ioStore(Libraries &l, uint32_t address, uint32_t size, uint64_t value, uint64_t cycle)
    {
        IoState &io = l.io;
        const uint32_t a = address & 0x1FFFFFFFu;
        uint32_t v = static_cast<uint32_t>(value);
        uint32_t reg = 0;
        // Channel 4 (to the IPU) completes as the reference completed it: at the game's first store after libmpeg's
        // decoder took the transfer (asked for more data), before the store's own effect (libraries.md section 11).
        if (io.channels[kIpuTo].running && io.ipuDemand && !io.hostAccess)
            complete(l, kIpuTo);
        if (const int n = timerOf(a, reg); n >= 0)
        {
            IoState::Timer &t = io.timers[static_cast<uint32_t>(n)];
            if (reg == 0)
            {
                t.start = v & 0xFFFFu;
                t.base = cycle;
                // A restart keeps the last read, the port's (TMB's frame end: match1p #34736003) and the game's alike:
                // the next read of the same count is a poll (stage-level0 #13062314: the game read 0, wrote 0, read 0
                // again).
            }
            else if (reg == 1)
                t.mode = v;
            else
                t.comp = v;
        }
        else if (const int ch = channelOf(a, reg); ch >= 0)
        {
            IoState::Channel &c = io.channels[static_cast<uint32_t>(ch)];
            if (size < 4)
            {
                const uint32_t mask = size == 1 ? 0xffu : 0xffffu;
                const uint32_t old = reg == 0 ? c.chcr : reg == 1 ? c.madr : reg == 2 ? c.qwc : c.tadr;
                v = (old & ~mask) | (v & mask);
            }
            switch (reg)
            {
            case 0:
                c.chcr = v;
                if (!(v & 0x100u))
                    c.running = false; // STR cleared: the channel stops (TMB stops channel 4 at a movie's end)
                else if (!c.running)
                {
                    if (ch == 0)
                        l.vu0.kick(v, c.tadr); // VIF0: the microcode image VU0 now holds
                    if (static_cast<uint32_t>(ch) == kIpuTo)
                    {
                        c.running = true;
                        io.ipuDemand = false;
                    }
                    else
                    {
                        complete(l, static_cast<uint32_t>(ch)); // VIF0, VIF1, GIF: done at once (no DMA runs)
                    }
                }
                break;
            case 1: c.madr = v; break;
            case 2: c.qwc = v & 0xFFFFu; break;
            default: c.tadr = v; break;
            }
        }
        else if (a == 0x1000D080u)
            io.sprAddress[0] = v;
        else if (a == 0x1000D480u)
            io.sprAddress[1] = v;
        else if (a == 0x1000E000u)
            io.ctrl = v;
        else if (a == 0x1000E010u)
        {
            io.stat &= ~(v & 0x3FFu);               // the status bits written 1 clear
            io.cpcondDone &= ~(v & 0x3FFu);
            io.stat ^= v & 0x3FF0000u;               // the mask bits written 1 toggle
        }
        else if (a == 0x1000E020u)
        {
            io.pcr = v;
            io.pcrWritten = true;
        }
        else if (a == 0x1000E030u)
            io.sqwc = v;
        else if (a == 0x1000E040u)
            io.rbsr = v;
        else if (a == 0x1000E050u)
            io.rbor = v;
        else if (a == 0x1000E060u)
            io.stadr = v;
        else if (a == 0x1000F590u)
        {
            io.enable = v;
        }
        // Native presentation owns the display. Obsolete display-register stores are
        // acknowledged without retaining a GS register bank.
    }

    namespace
    {
        void access(Libraries &l, Guest &g)
        {
            IoAccess &a = l.ioAccess;
            // A port's patch of the game's register stores (bozo/port.h): the store is still the game's out-call, its
            // effect is not made (match1p #62516600: TMB's dialog kick, logged, no completion after it).
            if (a.store)
            {
                if (const port::StorePatch patch = port::storePatch();
                    patch && a.size <= 4 && patch(a.pc, a.address, static_cast<uint32_t>(a.value)))
                    return;
                ioStore(l, a.address, a.size, a.value, l.now(g));
            }
            else
                a.value = ioLoad(l, a.address, a.size, a.timerPending, l.now(g));
        }
    }

    const Function &Libraries::ioFunction()
    {
        static const Function f{"hardware register", kIo, access};
        return f;
    }

    namespace
    {
        uint32_t sdkChannel(Guest &g)
        {
            const uint32_t handle = g.arg(0) & 0x1fffffffu;
            for (uint32_t ch = 0; ch < 10; ++ch)
                if (handle == kChannelBase[ch])
                    return ch;
            throw std::runtime_error("native libdma: invalid channel handle");
        }

        constexpr uint32_t kFromScratchpad = 8, kToScratchpad = 9;

        void scratchpadCopy(Libraries &l, Guest &g, uint32_t ch)
        {
            uint32_t at = l.io.sprAddress[ch - kFromScratchpad];
            uint32_t address = g.arg(1);
            for (uint32_t i = 0; i < g.arg(2); ++i, address += 16, at += 16)
            {
                const uint32_t spr = 0x70000000u | (at & 0x3FF0u);
                if (ch == kFromScratchpad)
                    g.copy(address, spr, 16);
                else
                    g.copy(spr, address, 16);
            }
        }

        template <uint32_t Mode>
        void submitDma(Libraries &l, Guest &g)
        {
            const uint32_t ch = sdkChannel(g);
            if ((ch == kFromScratchpad || ch == kToScratchpad) && (Mode & 0xcu) == 0)
            {
                scratchpadCopy(l, g, ch);
                g.ret(0);
                return;
            }
            if (ch > 2 && ch != kIpuTo)
                throw std::runtime_error("native libdma: transfer needs a native data producer");
            if (!l.dispatcher())
                throw std::runtime_error("native libdma: no SDK scheduler attached");
            if (ch < 3u && port::nativeGraphics(&l.dispatcher()->runtime()))
            {
                ++l.io.nativeSubmissions[ch];
                l.io.nativeCompleted[ch] = true;
                if (ch == 0u && (Mode & 0xcu) == 4u) l.vu0.kick(Mode, g.arg(1));
                // Native work is complete. Notify the SDK scheduler directly; do not
                // manufacture CHCR, D_STAT, or CPCOND0 state for a retired device.
                l.dispatcher()->kernel().irq(true, ch);
                g.ret(0);
                return;
            }
            auto &c = l.io.channels[ch];
            if (c.running)
                throw std::runtime_error("native libdma: previous native transfer is still pending");
            c.chcr = (c.chcr & ~0xcu) | Mode;
            if (ch == 0 && (Mode & 0xcu) == 4u)
                l.vu0.kick(Mode, g.arg(1)); // select a native program image by its known source, no chain read
            if (ch == kIpuTo)
            {
                // Native libmpeg supplies the stream to FFmpeg. Keep only its outstanding
                // completion until the decoder asks for more data, as for direct SDK stores.
                c.running = true;
                l.io.ipuDemand = false;
            }
            else
            {
                ++l.io.nativeSubmissions[ch];
                // Renderer work was already submitted by the port. Retain no packet address,
                // DMA length or transfer chain, and never run libdma's busy-polling code.
                complete(l, ch);
            }
            g.ret(0);
        }

        void syncDma(Libraries &l, Guest &g)
        {
            const uint32_t ch = sdkChannel(g);
            ++l.io.nativeSynchronizations;
            if (ch < 3u && l.dispatcher() && port::nativeGraphics(&l.dispatcher()->runtime()))
            {
                g.ret(0);
                return;
            }
            const bool busy = l.io.channels[ch].running;
            if (busy && g.arg(1) != 1u)
                throw std::runtime_error("native libdma: blocking wait needs a native completion service");
            g.ret(busy ? 1 : 0);
        }

        // Replace libdma's device-register reset loop at its SDK entry point. Nothing is
        // transferred; only outstanding native completion acknowledgements are cleared.
        void resetDma(Libraries &l, Guest &g)
        {
            l.dmaEnvironment.fill(0);
            if (l.dispatcher() && port::nativeGraphics(&l.dispatcher()->runtime()))
            {
                l.io.nativeCompleted.fill(false);
                g.ret(0);
                return;
            }
            auto &io = l.io;
            const uint32_t enabled = io.ctrl & 1u;
            io.channels = {};
            io.stat = io.cpcondDone = io.pcr = io.sqwc = io.rbsr = io.rbor = io.stadr = 0;
            io.pcrWritten = true;
            io.ipuDemand = false;
            io.ctrl = g.arg(0) == 1 ? 1u : enabled;
            g.ret(enabled);
        }

        void putDmaEnvironment(Libraries &l, Guest &g)
        {
            const auto *bytes = g.in(g.arg(0), l.dmaEnvironment.size());
            for (unsigned i = 0; i < 4; ++i)
                if (bytes[i] >= (i == 3 ? 7u : 10u))
                {
                    g.ret(-static_cast<int32_t>(i) - 1);
                    return;
                }
            std::memcpy(l.dmaEnvironment.data(), bytes, l.dmaEnvironment.size());
            g.ret(0);
        }
        void getDmaEnvironment(Libraries &l, Guest &g)
        {
            g.write(g.arg(0), l.dmaEnvironment.data(), l.dmaEnvironment.size());
            g.ret(0);
        }

        void getChannel(Libraries &, Guest &g) { g.retU(g.arg(0) < 10 ? kChannelBase[g.arg(0)] : 0); }

        void watchDma(Libraries &, Guest &g)
        {
            sdkChannel(g);
            g.ret(0);
        }

        void pauseDma(Libraries &l, Guest &g)
        {
            auto &c = l.io.channels[sdkChannel(g)];
            const uint32_t old = c.chcr;
            c.chcr = old & ~0x100u;
            c.running = false;
            g.retU(old);
        }

        void restartDma(Libraries &l, Guest &g)
        {
            const uint32_t ch = sdkChannel(g);
            auto &c = l.io.channels[ch];
            const uint32_t old = c.chcr;
            c.chcr = g.arg(1);
            c.running = ch == kIpuTo && (c.chcr & 0x100u) != 0;
            g.ret((old >> 8) & 1);
        }

        void putStallAddress(Libraries &l, Guest &g)
        {
            const uint32_t old = l.io.stadr;
            l.io.stadr = g.arg(0);
            g.retU(old);
        }
    }

    void addDma(std::vector<Function> &out)
    {
        out.push_back({"sceDmaReset", kIo, resetDma});
        out.push_back({"sceDmaSend", kIo, submitDma<0x105u>});
        out.push_back({"sceDmaSendN", kIo, submitDma<0x101u>});
        out.push_back({"sceDmaSendI", kIo, submitDma<0x109u>});
        out.push_back({"sceDmaSync", kIo, syncDma});
        out.push_back({"sceDmaPutEnv", kIo, putDmaEnvironment});
        out.push_back({"sceDmaGetEnv", kIo, getDmaEnvironment});
        out.push_back({"sceDmaGetChan", kIo, getChannel});
        out.push_back({"sceDmaRecv", kIo, submitDma<0x104u>});
        out.push_back({"sceDmaRecvN", kIo, submitDma<0x100u>});
        out.push_back({"sceDmaRecvI", kIo, submitDma<0x108u>});
        out.push_back({"sceDmaWatch", kIo, watchDma});
        out.push_back({"sceDmaPause", kIo, pauseDma});
        out.push_back({"sceDmaRestart", kIo, restartDma});
        out.push_back({"sceDmaPutStallAddr", kIo, putStallAddress});
    }
}
