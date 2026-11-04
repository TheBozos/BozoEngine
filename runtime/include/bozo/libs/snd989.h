// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// 989SND's behaviour (docs/specs/989snd.md) on BozoEngine's own mixer: the voice pool and its stealing, the 240 Hz
// tick and its 256-tick end counter, program sounds, MIDI sequences and VAG streams, driven by the IOP clock of
// docs/specs/iop-timing.md. The SIF side (requests, replies, the IOP heap, bank parsing) stays in snd.h's SndDriver,
// which feeds every command here in order, with the EE cycle it was made at.
#pragma once

#include "bozo/audio/libsd.h"
#include "bozo/audio/mixer.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace bozo::libs::snd989
{
    // Reads sectors of the disc image (false: no image or out of range).
    using DiscRead = std::function<bool(uint32_t lbn, uint32_t sectors, uint8_t *out)>;

    // The IOP's time (iop-timing.md) as the engine runs it. The defaults are the points a pinned run takes from the
    // reference's logs (libraries.md 15.13c); freeRun() gives free mode's own (15.13c, "Free mode").
    struct Timing
    {
        uint64_t eeReset = 4927984;   // the IOP reset's EE cycle (P = (EE - reset) / 8 + G)
        uint64_t g = 0;               // unpaid IOP time since the reset (section 3)
        // The first tick due kTick + this after the first command (989SND's start): fitted on boot's sound records (the
        // title music's first frames equal the reference's for 27,752-27,792; libraries.md 15.13d).
        uint32_t firstTickPhase = 27776;
        uint32_t reverbEnableDelay = 0; // the reverb's enable this many IOP cycles after its tick (a debugging aid)
        int64_t firstBankPhase = -1;  // >= 0: the next tick due this many IOP cycles after the first bank load (0x03)
        // The level start: the next tick due this many IOP cycles after 0x2a (1.5 ms); off, the ticks keep their grid.
        bool streamInitRephase = true;
        uint32_t streamInitPhase = 55296;
        // A stream's KeyOn, IOP cycles after its 0x2c command (the streaming threads' work in the reference): a
        // two-chunk mono file, a long stereo one.
        uint32_t streamKeyOnMono = 1280, streamKeyOnStereo = 2470;

        // Free mode: the IOP reset where the game made it (sceSifRebootIop's EE cycle), no unpaid IOP time (no IOP
        // runs ahead of the EE), the tick grid kept through the level start, a stream keyed on at its command's end.
        static Timing freeRun(uint64_t eeReset)
        {
            Timing t;
            t.eeReset = eeReset;
            t.g = 0;
            t.streamInitRephase = false;
            t.streamKeyOnMono = t.streamKeyOnStereo = 0;
            t.firstTickPhase = 0;
            return t;
        }
    };

    class Engine final : public audio::LibsdHost
    {
    public:
        Engine();
        ~Engine() override;
        Engine(const Engine &) = delete;
        Engine &operator=(const Engine &) = delete;

        void setDisc(DiscRead read) { m_disc = std::move(read); }
        Timing timing;

        // The IOP's RAM as the engine models it (2 MB; the uploads' staging area lies above it).
        static constexpr uint32_t kIopRamBytes = 0x200000u;

        // One command at EE cycle `ee` (the IOP is first brought up to it). `result` is what 989SND would reply;
        // the return value says whether the command writes a result.
        bool command(uint32_t id, const uint32_t a[7], uint64_t ee, uint32_t &result);

        // A bank the driver loaded at IOP address `address`: its header chunk, samples and MIDI chunk (with its
        // 16-byte header), as in the file.
        void bankLoaded(uint32_t address, const std::vector<uint8_t> &header, const std::vector<uint8_t> &samples,
                        const std::vector<uint8_t> &midi);
        void bankUnloaded(uint32_t address);

        // The IOP's clock, the ticks and the mixer brought up to EE cycle `ee` (between commands: the output's
        // samples so far, and what a frame's sound record hashes). Before the first command only the mixer's clock
        // moves (silence). Changes nothing a later command sees: ticks run at the times the next command would run them.
        void advanceTo(uint64_t ee);
        // Data the EE sends to the IOP's RAM (sceSifSetDma) at EE cycle `ee`: the mixer is brought up to then first,
        // so the samples played before it read the buffer as it was.
        void iopWrite(uint64_t ee, uint32_t address, const uint8_t *data, uint32_t bytes);
        // The movie's sound buffer the driver allocated for the next InitMovieSound (0x3b).
        void setMovieBuffer(uint32_t address) { m_movieBuffer = address; }

        audio::Mixer &mixer() { return *m_mixer; }
        uint64_t iopClock() const { return m_clock; }
        uint64_t ticks() const { return m_ticks; }

        // LibsdHost
        uint64_t iopCycle() override { return m_now; }
        uint8_t *iopRam() override { return m_ram.data(); }
        uint32_t iopRamBytes() override { return static_cast<uint32_t>(m_ram.size()); }
        void interruptCall(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}

        struct Impl;

    private:
        void advance(uint64_t P);
        void runTick(uint64_t service);
        void bringUp(uint64_t cycle);
        uint64_t paid(uint64_t ee) const { return ee > timing.eeReset ? (ee - timing.eeReset) / 8 + timing.g : 0; }

        std::unique_ptr<audio::Mixer> m_mixer;
        std::unique_ptr<audio::Libsd> m_libsd;
        std::vector<uint8_t> m_ram;
        DiscRead m_disc;
        uint64_t m_clock = 0, m_now = 0, m_nextDue = 0, m_ticks = 0;
        bool m_started = false, m_dumped = false;
        uint32_t m_movieBuffer = 0;
        bool m_bankRephased = false;
        std::unique_ptr<Impl> m_impl;
        friend struct Impl;
    };
}
