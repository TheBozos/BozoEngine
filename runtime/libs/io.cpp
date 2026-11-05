// The hardware registers TMB still touches (libraries.md section 11). Every address answered here was settled on the
// call logs' io-load and io-store records (their values, and the interrupts raised inside them).
#include "bozo/libs/io.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"

namespace bozo::libs
{
    namespace
    {
        // The DMA channels' register blocks (CHCR at +0, MADR +0x10, QWC +0x20, TADR +0x30).
        constexpr uint32_t kChannelBase[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                               0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};
        constexpr uint32_t kGif = 2, kIpuTo = 4;

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

        // Timer 0's period in EE cycles: its clock (T0_MODE bits 0-1) is BUSCLK (half the EE's), /16, /256; 3 (the
        // horizontal blank) is not modelled.
        uint64_t t0Period(uint32_t mode)
        {
            switch (mode & 3u)
            {
            case 0: return 2;
            case 1: return 32;
            case 2: return 512;
            default: return 0;
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

    namespace
    {
        // Whether a GIF chain's first packet draws: its GIF tag sets PRIM (PRE, bit 46). The reference completes a
        // GIF transfer of register writes at once (its interrupt raised), and drops one that draws (TMB's clear
        // before a loading screen's frame boundary): no completion, no interrupt (libraries.md section 11).
        bool drawsPrimitive(Libraries &l, const IoState::Channel &c)
        {
            if (((c.chcr >> 2) & 3u) != 1u)
                return false;
            Guest g(l.dispatcher()->runtime(), nullptr);
            try
            {
                const uint32_t tag = g.read32(c.tadr), addr = g.read32(c.tadr + 4);
                const uint32_t id = (tag >> 28) & 7u, qwc = tag & 0xFFFFu;
                if (qwc == 0)
                    return false;
                const uint32_t data = id == 0 || id == 3 || id == 4 ? addr & ~15u : c.tadr + 16;
                return (g.read32(data + 4) >> 14) & 1u;
            }
            catch (const BadAccess &)
            {
                return false;
            }
        }
    }

    bool ioKnown(uint32_t address, uint32_t size, bool store)
    {
        const uint32_t a = address & 0x1FFFFFFFu;
        uint32_t reg = 0;
        if (a == 0x10000000u || a == 0x10000010u)
            return size == 4;
        if (channelOf(a, reg) >= 0)
            return size == 4;
        if (a >= 0x1000E000u && a <= 0x1000E060u && (a & 0xFu) == 0)
            return size == 4;
        if (a == 0x1000F520u || a == 0x1000F590u)
            return size == 4;
        if (store && a >= 0x12000000u && a <= 0x120000F0u && (a & 0xFu) == 0)
            return size == 8;
        if (!store && a == 0x12001000u) // the GS's CSR (TMB's vblank handler reads its FIELD)
            return size == 8 || size == 4;
        return false;
    }

    uint64_t ioLoad(Libraries &l, uint32_t address, uint32_t, uint32_t &timerPending, uint64_t cycle)
    {
        IoState &io = l.io;
        const uint32_t a = address & 0x1FFFFFFFu;
        uint64_t v = 0;
        uint32_t reg = 0;
        if (a == 0x10000000u)
        {
            // T0_COUNT: the count written last plus the periods since; reading the same count again (a poll loop)
            // charges a period at the next checkpoint (calllog.md section 2).
            const uint64_t period = t0Period(io.t0Mode);
            const uint64_t now = cycle;
            const uint32_t count = period ? static_cast<uint32_t>((io.t0Start + (now - io.t0Base) / period) & 0xFFFFu) : io.t0Start;
            if (count == io.t0LastRead)
                timerPending = static_cast<uint32_t>(period);
            io.t0LastRead = count;
            v = count;
        }
        else if (a == 0x10000010u)
            v = io.t0Mode;
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
        else if (a == 0x1000F520u)
            v = 0; // D_ENABLER: no transfer is ever suspended (none runs)
        return v;
    }

    void ioStore(Libraries &l, uint32_t address, uint32_t, uint64_t value, uint64_t cycle)
    {
        IoState &io = l.io;
        const uint32_t a = address & 0x1FFFFFFFu;
        const uint32_t v = static_cast<uint32_t>(value);
        uint32_t reg = 0;
        // Channel 4 (to the IPU) completes as the reference completed it: at the game's first store after libmpeg's
        // decoder took the transfer (asked for more data), before the store's own effect (libraries.md section 11).
        if (io.channels[kIpuTo].running && io.ipuDemand && !io.hostAccess)
            complete(l, kIpuTo);
        if (a == 0x10000000u)
        {
            io.t0Start = v & 0xFFFFu;
            io.t0Base = cycle;
            // A restart keeps the last read, the port's (TMB's frame end: match1p #34736003) and the game's alike: the
            // next read of the same count is a poll (stage-level0 #13062314: the game read 0, wrote 0, read 0 again).
        }
        else if (a == 0x10000010u)
            io.t0Mode = v;
        else if (const int ch = channelOf(a, reg); ch >= 0)
        {
            IoState::Channel &c = io.channels[static_cast<uint32_t>(ch)];
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
                        if (static_cast<uint32_t>(ch) == kGif && drawsPrimitive(l, c))
                        {
                            c.running = false; // dropped: no completion, no interrupt (the reference's renderer path)
                            c.chcr &= ~0x100u;
                            break;
                        }
                        complete(l, static_cast<uint32_t>(ch)); // VIF0, VIF1, GIF: done at once (no DMA runs)
                    }
                }
                break;
            case 1: c.madr = v; break;
            case 2: c.qwc = v & 0xFFFFu; break;
            default: c.tadr = v; break;
            }
        }
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
        else if (a >= 0x12000000u && a <= 0x120000F0u)
            io.gsPrivileged[(a - 0x12000000u) >> 4] = value;
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
}
