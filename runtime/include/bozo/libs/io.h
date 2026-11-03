// The hardware registers TMB still loads and stores (libraries.md section 11), answered by a native model of only what
// the game observes: no DMA, VIF, GIF, IPU or GS runs behind them. A transfer the game starts completes (its D_STAT
// bit, its DMAC interrupt) as the reference completed it; the timer counts EE time; the rest keeps what is written.
#pragma once

#include <array>
#include <cstdint>

namespace bozo::libs
{
    class Libraries;

    struct IoState
    {
        // ---- Timer 0 (T0_COUNT, T0_MODE; TMB polls the count) ----
        uint32_t t0Mode = 0;
        uint64_t t0Base = 0;      // the EE cycle the count was last written
        uint32_t t0Start = 0;     // the count written then
        uint32_t t0LastRead = ~0u; // the last value read (a poll that reads it again charges a period)
        // TMB's frame boundary (a port override) restarts the count inside it; while that override is answered from a
        // log, the next load of the count re-bases it on the log's value (bozo-run).
        bool t0Rebase = false;

        // ---- The DMA controller ----
        struct Channel
        {
            uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0;
            bool running = false; // started, not complete yet (channel 4: the reference completes it later)
        };
        std::array<Channel, 10> channels{};
        // libmpeg's decoder asked the game for more data (it queued the game's nodata callback) since channel 4 was
        // last started: the IPU has taken that transfer, which completes at the game's next store.
        bool ipuDemand = false;
        uint32_t ctrl = 0, stat = 0, pcr = 0, sqwc = 0, rbsr = 0, rbor = 0, stadr = 0, enable = 0;

        // ---- The GS's privileged registers (PMODE ... BGCOLOR): what the game sets, for the renderer ----
        std::array<uint64_t, 16> gsPrivileged{};

        bool pcrWritten = false;

        // CPCOND0 (BC0F/BC0T) as the reference answers it (replay.md section 3): every channel D_PCR names has
        // completed (D_STAT); before the game writes D_PCR, any channel has.
        bool cpcond0() const
        {
            if (!pcrWritten)
                return (stat & 0x3FFu) != 0;
            return (pcr & 0x3FFu) != 0 && (stat & pcr & 0x3FFu) == (pcr & 0x3FFu);
        }
    };

    // The access being made (bozo-run hands it to the I/O library's function, ioFunction()).
    struct IoAccess
    {
        uint32_t address = 0, size = 0;
        bool store = false;
        uint64_t value = 0;        // a store's value; a load's result
        uint32_t timerPending = 0; // the timer-poll cycles pending (in: before the access; out: after it)
    };

    // Whether the model answers an access (a register it knows, of a size it answers).
    bool ioKnown(uint32_t address, uint32_t size, bool store);
    // A load: its value (the low 64 bits; the upper ones 0) and the timer-poll cycles pending after it.
    uint64_t ioLoad(Libraries &l, uint32_t address, uint32_t size, uint32_t &timerPending);
    // A store: the register's new state and what it starts (a transfer's completion: D_STAT, the DMAC interrupt).
    void ioStore(Libraries &l, uint32_t address, uint32_t size, uint64_t value);
}
