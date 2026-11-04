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

    // The IOP's time (iop-timing.md) as the engine runs it: pinned points and the cost model.
    struct Timing
    {
        uint64_t eeReset = 4927984;   // the IOP reset's EE cycle (P = (EE - reset) / 8 + G)
        uint64_t g = 0;               // unpaid IOP time since the reset (section 3)
        uint32_t streamInitPhase = 55296; // the level start: the next tick due this many IOP cycles after 0x2a (1.5 ms)
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

        // One command at EE cycle `ee` (the IOP is first brought up to it). `result` is what 989SND would reply;
        // the return value says whether the command writes a result.
        bool command(uint32_t id, const uint32_t a[7], uint64_t ee, uint32_t &result);

        // A bank the driver loaded at IOP address `address`: its header chunk, samples and MIDI chunk (with its
        // 16-byte header), as in the file.
        void bankLoaded(uint32_t address, const std::vector<uint8_t> &header, const std::vector<uint8_t> &samples,
                        const std::vector<uint8_t> &midi);
        void bankUnloaded(uint32_t address);

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

        std::unique_ptr<audio::Mixer> m_mixer;
        std::unique_ptr<audio::Libsd> m_libsd;
        std::vector<uint8_t> m_ram;
        DiscRead m_disc;
        uint64_t m_clock = 0, m_now = 0, m_nextDue = 0, m_ticks = 0;
        bool m_started = false, m_dumped = false;
        std::unique_ptr<Impl> m_impl;
        friend struct Impl;
    };
}
