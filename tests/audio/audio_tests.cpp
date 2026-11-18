// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// bozo-audio-tests: the native mixer and libsd (runtime/audio, docs/specs/libraries.md section 15.14): the voice-status
// rules a driver sees (ENVX after KeyOn, a sample's end with and without repeat, the release after KeyOff, ENDX), the
// clock (a sample per 768 IOP cycles), libsd's entries and batches, and the reference's samples (mixer_scenario.h).
#include "bozo/audio/libsd.h"
#include "bozo/audio/mixer.h"
#include "bozo/audio/port_stream.h"
#include "mixer_scenario.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    using bozo::audio::Libsd;
    using bozo::audio::LibsdHost;
    using bozo::audio::Mixer;

    int g_failures = 0;
    void eq(const char *what, long long got, long long want)
    {
        if (got == want)
            return;
        ++g_failures;
        std::printf("FAIL %s: %lld (0x%llx), expected %lld (0x%llx)\n", what, got, static_cast<unsigned long long>(got), want,
                    static_cast<unsigned long long>(want));
    }

    // A sample of `blocks` blocks of loud data (filter 0, shift 0, nibbles 7): the first flagged loop start, the last
    // end (| repeat).
    void writeTone(Mixer &m, uint32_t address, uint32_t blocks, bool repeat)
    {
        std::vector<uint8_t> data(blocks * 16u, 0x77u);
        for (uint32_t b = 0; b < blocks; ++b)
        {
            data[b * 16u] = 0x00u;
            data[b * 16u + 1u] = static_cast<uint8_t>((b == 0u ? 4u : 0u) | (b + 1u == blocks ? (repeat ? 3u : 1u) : 0u));
        }
        m.write(address, data.data(), static_cast<uint32_t>(data.size()));
    }

    // A clocked mixer whose clock origin is cycle 0; run(n) makes n more samples.
    struct Clocked
    {
        Mixer m;
        uint64_t samples = 0;
        Clocked()
        {
            m.setClocked(true);
            m.advanceTo(0u);
        }
        void run(uint64_t n)
        {
            samples += n;
            m.advanceTo(samples * Mixer::kIopCyclesPerSample);
        }
    };

    void testAttack()
    {
        Clocked c;
        writeTone(c.m, 0x1000u, 400u, true);
        c.m.setStart(0, 0, 0x1000u);
        c.m.setStart(0, 1, 0x1000u);
        c.m.setPitch(0, 0, 0x1000u);
        c.m.setPitch(0, 1, 0x1000u);
        c.m.setAdsr(0, 0, 0, 0x28ffu); // attack linear, rate 0x28: shift 10, step 7 << 1 a sample
        c.m.setAdsr(0, 1, 0, 0x50ffu); // attack linear, rate 0x50: shift 20, step 7 every 512 samples
        c.m.keyOn(0, 3u);
        eq("ENVX at KeyOn", c.m.envelope(0, 0), 0);
        c.run(1u);
        eq("ENVX after one sample (rate 0x28)", c.m.envelope(0, 0), 14);
        c.run(1u);
        eq("ENVX after two samples (rate 0x28)", c.m.envelope(0, 0), 28);
        c.run(198u);
        eq("ENVX after 200 samples (rate 0x28)", c.m.envelope(0, 0), 2800);
        eq("ENVX after 200 samples (rate 0x50: still 0 at 989SND's next tick)", c.m.envelope(0, 1), 0);
        c.run(311u);
        eq("ENVX after 511 samples (rate 0x50)", c.m.envelope(0, 1), 0);
        c.run(1u);
        eq("ENVX after 512 samples (rate 0x50)", c.m.envelope(0, 1), 7);
    }

    void testEnd(bool repeat)
    {
        Clocked c;
        writeTone(c.m, 0x2000u, 2u, repeat);
        c.m.setStart(1, 5, 0x2000u);
        c.m.setPitch(1, 5, 0x1000u);
        c.m.setAdsr(1, 5, 0, 0x00ffu);
        c.m.setAdsr(1, 5, 1, 0x1fcdu);
        c.m.keyOn(1, 1u << 5);
        c.run(55u);
        eq(repeat ? "repeat: ENDX before the end" : "ENDX before the end", c.m.ended(1), 0);
        eq(repeat ? "repeat: ENVX before the end" : "ENVX before the end", c.m.envelope(1, 5), 0x7FFF);
        eq("NAX on the last block", c.m.position(1, 5), 0x2010);
        c.run(1u);
        eq(repeat ? "repeat: ENDX at the end" : "ENDX at the end", c.m.ended(1), 1u << 5);
        eq(repeat ? "repeat: ENVX at the end (plays on)" : "ENVX at the end (silent at once)", c.m.envelope(1, 5),
           repeat ? 0x7FFF : 0);
        eq("NAX at the loop point", c.m.position(1, 5), 0x2000);
        c.m.keyOn(1, 1u << 5);
        eq("KeyOn clears the voice's ENDX bit", c.m.ended(1), 0);
    }

    void testRelease()
    {
        Clocked c;
        writeTone(c.m, 0x3000u, 2000u, true);
        c.m.setStart(0, 9, 0x3000u);
        c.m.setPitch(0, 9, 0x1000u);
        c.m.setAdsr(0, 9, 0, 0x00ffu); // attack rate 0: 0x7FFF in 3 samples; sustain level 0xF; sustain rate 0x7F: holds
        c.m.setAdsr(0, 9, 1, 0x1fcdu); // release linear, rate 13: -8 every 4 samples
        c.m.keyOn(0, 1u << 9);
        c.run(10u);
        eq("ENVX at sustain", c.m.envelope(0, 9), 0x7FFF);
        c.m.keyOff(0, 1u << 9);
        eq("ENVX right after KeyOff", c.m.envelope(0, 9), 0x7FFF);
        c.run(4u);
        eq("ENVX after the first release step", c.m.envelope(0, 9), 0x7FF7);
        c.run(16379u);
        eq("ENVX 1 sample before the release ends", c.m.envelope(0, 9), 7);
        c.run(1u);
        eq("ENVX when the release ends (16384 samples, 0.341 s)", c.m.envelope(0, 9), 0);
        eq("a release does not set ENDX", c.m.ended(0), 0);
        c.m.keyOff(0, 1u << 9);
        c.run(10u);
        eq("KeyOff of a silent voice", c.m.envelope(0, 9), 0);
    }

    void testClock()
    {
        Mixer m;
        m.setClocked(true);
        m.advanceTo(1000u);
        eq("origin", static_cast<long long>(m.samplesMade()), 1);
        m.advanceTo(768u * 10u - 1u);
        eq("samples to 7679", static_cast<long long>(m.samplesMade()), 9);
        m.advanceTo(768u * 10u);
        eq("samples to 7680", static_cast<long long>(m.samplesMade()), 10);
        uint64_t hash = 0;
        uint32_t count = 0;
        m.takeMixed(hash, count);
        eq("mixed so far", count, 9);
        m.advanceTo(768u * (10u + 20000u));
        m.takeMixed(hash, count);
        eq("one advance mixes at most kQueueFrames", count, static_cast<long long>(Mixer::kQueueFrames));
        eq("the clock skips the rest", static_cast<long long>(m.samplesMade()), 20010);
        m.advanceTo(5u * 768u);
        eq("a clock restart only sets the origin", static_cast<long long>(m.samplesMade()), 5);
    }

    struct Host : LibsdHost
    {
        std::vector<uint8_t> ram = std::vector<uint8_t>(2u * 1024u * 1024u, 0u);
        uint64_t cycle = 0;
        struct Call
        {
            uint32_t function, a0, a1, gp, delay;
        };
        std::vector<Call> calls;
        uint64_t iopCycle() override { return cycle; }
        uint8_t *iopRam() override { return ram.data(); }
        uint32_t iopRamBytes() override { return static_cast<uint32_t>(ram.size()); }
        void interruptCall(uint32_t function, uint32_t a0, uint32_t a1, uint32_t gp, uint32_t delay) override
        {
            calls.push_back(Call{function, a0, a1, gp, delay});
        }
        void w16(uint32_t at, uint16_t v) { std::memcpy(&ram[at & 0x1FFFFFu], &v, 2); }
        void w32(uint32_t at, uint32_t v) { std::memcpy(&ram[at & 0x1FFFFFu], &v, 4); }
        uint32_t r32(uint32_t at) const
        {
            uint32_t v;
            std::memcpy(&v, &ram[at & 0x1FFFFFu], 4);
            return v;
        }
    };

    // sceSdInit's mixes: every voice of both cores dry and wet, MMIX 0xFF0 / 0xFFC.
    void testInitMixing()
    {
        Mixer m;
        Host host;
        Libsd sd(m, host);
        m.setClocked(true);
        m.advanceTo(0u);
        uint32_t r = 0;
        const uint32_t none[5] = {};
        sd.call(4u, none, 0u, r);
        for (uint32_t core = 0; core < 2u; ++core)
            for (uint32_t sw : {0x1800u, 0x1900u, 0x1A00u, 0x1B00u})
            {
                const uint32_t get[5] = {sw | core};
                sd.call(8u, get, 0u, r);
                eq("sceSdInit's VMIX switches", r, 0xFFFFFF);
            }
        const uint32_t mmix0[5] = {0x0800u}, mmix1[5] = {0x0801u};
        sd.call(6u, mmix0, 0u, r);
        eq("sceSdInit's core 0 MMIX", r, 0xFF0);
        sd.call(6u, mmix1, 0u, r);
        eq("sceSdInit's core 1 MMIX", r, 0xFFC);

        auto peakOfCore0Voice = [&](bool clearDry)
        {
            writeTone(m, 0x5000u, 400u, true);
            const uint32_t voice = 2u << 1u;
            const uint32_t params[][2] = {{0x0000u | voice, 0x3FFFu}, {0x0100u | voice, 0x3FFFu}, {0x0200u | voice, 0x1000u},
                                          {0x0300u | voice, 0x00FFu}, {0x0400u | voice, 0x1FC0u}, {0x0980u, 0x3FFFu},
                                          {0x0A80u, 0x3FFFu},         {0x0981u, 0x3FFFu},         {0x0A81u, 0x3FFFu}};
            for (const auto &p : params)
            {
                const uint32_t a[5] = {p[0], p[1]};
                sd.call(5u, a, 0u, r);
            }
            const uint32_t ssa[5] = {0x2040u | voice, 0x5000u};
            sd.call(9u, ssa, 0u, r);
            if (clearDry)
                for (uint32_t sw : {0x1800u, 0x1A00u})
                {
                    const uint32_t a[5] = {sw, 0u};
                    sd.call(7u, a, 0u, r);
                }
            const uint32_t kon[5] = {0x1500u, 1u << 2u};
            sd.call(7u, kon, 0u, r);
            m.stats();
            m.advanceTo(768u * 200u);
            std::vector<int16_t> out(2u * 400u);
            m.take(out.data(), 400u);
            return static_cast<long long>(m.stats().peak);
        };
        eq("a core-0 voice after sceSdInit alone is heard", peakOfCore0Voice(false) > 0, 1);
        sd.call(4u, none, 0u, r);
        m.advanceTo(0u);
        eq("the same voice with core 0's dry switches cleared is silent", peakOfCore0Voice(true), 0);
    }

    void testLibsd()
    {
        Mixer m;
        Host host;
        Libsd sd(m, host);
        m.setClocked(true);
        uint32_t r = 0;
        const uint32_t none[5] = {};
        eq("sceSdInit handled", sd.call(4u, none, 0u, r), 1);
        // An upload: 8 blocks of tone at IOP 0x80000 to sound memory 0x5000, the transfer callback after it.
        for (uint32_t b = 0; b < 8u; ++b)
            for (uint32_t i = 0; i < 16u; ++i)
                host.ram[0x80000u + b * 16u + i] = i == 0u ? 0u : (i == 1u ? (b == 7u ? 1u : 0u) : 0x77u);
        const uint32_t cb[5] = {0u, 0x12340u};
        sd.call(21u, cb, 0x9999u, r);
        const uint32_t trans[5] = {0u, 0u, 0x80080000u, 0x5000u, 128u};
        sd.call(17u, trans, 0u, r);
        eq("sceSdVoiceTrans returns the size", r, 128);
        eq("transfer callback queued", static_cast<long long>(host.calls.size()), 1);
        eq("transfer callback function", host.calls.empty() ? 0 : host.calls[0].function, 0x12340);
        eq("transfer callback gp", host.calls.empty() ? 0 : host.calls[0].gp, 0x9999);
        eq("transfer callback delay", host.calls.empty() ? 0 : host.calls[0].delay, Libsd::kTransferDelay);
        // Voice 3 of core 1 by a batch (SSA, pitch, ADSR1, KON), then ENVX by sceSdGetParam.
        const uint32_t batch = 0x90000u;
        const uint32_t entries[4][3] = {{0x03u, 0x2040u | (3u << 1u) | 1u, 0x5000u},
                                        {0x01u, 0x0200u | (3u << 1u) | 1u, 0x1000u},
                                        {0x01u, 0x0300u | (3u << 1u) | 1u, 0x28ffu},
                                        {0x02u, 0x1500u | 1u, 1u << 3u}};
        for (uint32_t i = 0; i < 4u; ++i)
        {
            host.w16(batch + i * 8u, static_cast<uint16_t>(entries[i][0]));
            host.w16(batch + i * 8u + 2u, static_cast<uint16_t>(entries[i][1]));
            host.w32(batch + i * 8u + 4u, entries[i][2]);
        }
        host.cycle = 768u * 100u + 17u;
        const uint32_t proc[5] = {batch, 0x91000u, 4u};
        sd.call(15u, proc, 0u, r);
        eq("sceSdProcBatch returns the count", r, 4);
        eq("SSA read back", m.start(1, 3), 0x5000);
        host.cycle = 768u * 300u + 5u; // 200 samples after the KON: 989SND's next tick
        const uint32_t envx[5] = {0x0500u | (3u << 1u) | 1u};
        sd.call(6u, envx, 0u, r);
        eq("ENVX through sceSdGetParam, 200 samples after KON", r, 2800);
        host.cycle = 768u * 400u;
        const uint32_t endx[5] = {0x1700u | 1u};
        sd.call(8u, endx, 0u, r);
        eq("ENDX through sceSdGetSwitch (8 blocks at pitch 0x1000: ended at sample 224)", r, 1u << 3);
        const uint32_t clear[5] = {0x1700u | 1u, 0u};
        sd.call(7u, clear, 0u, r);
        sd.call(8u, endx, 0u, r);
        eq("sceSdSetSwitch(ENDX) clears the core's bits", r, 0);
        sd.call(6u, envx, 0u, r);
        eq("ENVX after the end without repeat", r, 0);
        // A parameter libsd keeps as set.
        const uint32_t avol[5] = {0x0D80u | 1u, 0x1234u};
        sd.call(5u, avol, 0u, r);
        const uint32_t avolGet[5] = {0x0D80u | 1u};
        sd.call(6u, avolGet, 0u, r);
        eq("AVOL kept as set", r, 0x1234);
        // The input: a looping block transfer of 0x2000 bytes; its position after 300 samples.
        host.cycle = 768u * 1000u;
        const uint32_t block[5] = {0u, 0x10u, 0xA0000u, 0x2000u, 0u};
        sd.call(18u, block, 0u, r);
        eq("sceSdBlockTrans", r, 0);
        host.cycle = 768u * 1300u;
        const uint32_t status[5] = {0u, 0u};
        sd.call(20u, status, 0u, r);
        eq("sceSdBlockTransStatus after 300 samples", r, 0xA0000u + 1024u + 44u * 2u);
        // Two signalled halves unread: each status read answers one, then the live position again.
        host.cycle = 768u * 3100u;
        sd.call(20u, status, 0u, r);
        eq("sceSdBlockTransStatus answers the first half unread", r, (1u << 24u) | (0xA0000u + 0x1000u));
        host.cycle = 768u * 4100u;
        sd.call(20u, status, 0u, r);
        eq("sceSdBlockTransStatus answers the second half unread", r, 0xA0000u);
        sd.call(20u, status, 0u, r);
        eq("sceSdBlockTransStatus answers the half signalled since", r, (1u << 24u) | (0xA0000u + 0x1000u));
        sd.call(20u, status, 0u, r);
        eq("sceSdBlockTransStatus then reads the input again", r, (1u << 24u) | (0xA0000u + 0x1000u + 28u * 2u));
        const uint32_t handler[5] = {1u, 0x4000u, 0x77u};
        sd.call(26u, handler, 0u, r);
        const uint32_t channel1[5] = {1u};
        sd.call(28u, channel1, 0u, r);
        eq("sceSdGetTransIntrHandlerArgument", r, 0x77);
        sd.call(30u, status, 0u, r);
        eq("sceSdStopTrans", r, 0);
        eq("sceSdStopTrans stops the input", m.inputPlaying(0), 0);
        sd.call(20u, status, 0u, r);
        eq("a stopped input's status", r, 0);
        eq("sceSdNote2Pitch of the center note", Libsd::note2Pitch(60u, 0u, 60u, 0u), 0x1000);
        eq("sceSdNote2Pitch an octave up", Libsd::note2Pitch(60u, 0u, 72u, 0u), 0x2000);
        eq("sceSdNote2Pitch an octave down", Libsd::note2Pitch(60u, 0u, 48u, 0u), 0x800);
        eq("sceSdPitch2Note of 0x1000", Libsd::pitch2Note(60u, 0u, 0x1000u), 60 << 8);
        // Results of the SDK's own conversions.
        eq("sceSdNote2Pitch fine", Libsd::note2Pitch(60u, 0u, 60u, 5u), 0x1009);
        eq("sceSdNote2Pitch a semitone up", Libsd::note2Pitch(60u, 0u, 61u, 0u), 0x10F4);
        eq("sceSdNote2Pitch a semitone down", Libsd::note2Pitch(60u, 0u, 59u, 0u), 0xF1A);
        eq("sceSdNote2Pitch negative fine", Libsd::note2Pitch(60u, 64u, 60u, 0xFF9Cu), 0xFBE);
        eq("sceSdNote2Pitch below the center", Libsd::note2Pitch(69u, 0u, 60u, 0u), 0x983);
        eq("sceSdNote2Pitch fine carry", Libsd::note2Pitch(48u, 127u, 72u, 127u), 0x47C5);
        eq("sceSdNote2Pitch top of the range", Libsd::note2Pitch(60u, 0u, 84u, 0u), 0x4000);
        eq("sceSdNote2Pitch two octaves down", Libsd::note2Pitch(60u, 0u, 36u, 0u), 0x400);
        eq("sceSdPitch2Note an octave down", Libsd::pitch2Note(60u, 0u, 0x800u), 0x3000);
        eq("sceSdPitch2Note between notes", Libsd::pitch2Note(60u, 0u, 0x1234u), 0x3E1E);
        eq("sceSdPitch2Note center fine", Libsd::pitch2Note(60u, 32u, 0x1000u), 0x3C20);
        eq("sceSdPitch2Note clamps to 0x3FFF", Libsd::pitch2Note(48u, 0u, 0x3FFFu), 0x4800);
        eq("sceSdPitch2Note of 0", Libsd::pitch2Note(60u, 0u, 0u), 0xAC00);
        const uint32_t conversion[5] = {60u, 0u, 72u, 0u};
        sd.call(13u, conversion, 0u, r);
        eq("export 13", r, 0x2000);
        const uint32_t reverse[5] = {60u, 0u, 0x2000u};
        sd.call(14u, reverse, 0u, r);
        eq("export 14", r, 72 << 8);
        eq("libsd handles 27", Libsd::handles(27u), 1);
        const auto effect = [&](uint32_t mode, uint16_t left, uint16_t right, uint32_t delay, uint32_t feedback) {
            host.w32(0x90004u, mode);
            host.w16(0x90008u, left);
            host.w16(0x9000Au, right);
            host.w32(0x9000Cu, delay);
            host.w32(0x90010u, feedback);
        };
        const uint32_t attr[5] = {0u, 0x90000u}, read[5] = {0u, 0x90100u};
        const auto ram16 = [&](uint32_t at) { return host.r32(at) & 0xFFFFu; };
        effect(2u, 0x1111u, 0x2222u, 5u, 6u);
        sd.call(23u, attr, 0u, r);
        effect(3u, 0x3333u, 0x3333u, 7u, 8u);
        eq("sceSdSetEffectMode", sd.call(32u, attr, 0u, r) && r == 0u, 1);
        sd.call(24u, read, 0u, r);
        eq("SetEffectMode: the mode", host.r32(0x90104u), 3);
        eq("SetEffectMode: delay 0", host.r32(0x9010Cu), 0);
        eq("SetEffectMode: feedback 0", host.r32(0x90110u), 0);
        eq("SetEffectMode: depth 0", ram16(0x90108u), 0);
        effect(10u, 0u, 0u, 0u, 0u);
        sd.call(32u, attr, 0u, r);
        eq("SetEffectMode: a bad mode", static_cast<int32_t>(r), -100);
        effect(1u, 0x4444u, 0x5555u, 9u, 10u);
        sd.call(33u, attr, 0u, r);
        eq("SetEffectModeParams: another mode", static_cast<int32_t>(r), -100);
        effect(3u, 0x4444u, 0x5555u, 9u, 10u);
        eq("sceSdSetEffectModeParams", sd.call(33u, attr, 0u, r) && r == 0u, 1);
        sd.call(24u, read, 0u, r);
        eq("SetEffectModeParams, mode 3: the delay stays", host.r32(0x9010Cu), 0);
        eq("SetEffectModeParams: the depth", ram16(0x90108u), 0x4444);
        eq("SetEffectModeParams: the right depth", ram16(0x9010Au), 0x5555);
        effect(7u, 0u, 0u, 0u, 0u);
        sd.call(32u, attr, 0u, r);
        effect(7u, 0x1000u, 0x1000u, 9u, 10u);
        sd.call(33u, attr, 0u, r);
        sd.call(24u, read, 0u, r);
        eq("SetEffectModeParams, mode 7: the delay", host.r32(0x9010Cu), 9);
        eq("SetEffectModeParams, mode 7: the feedback", host.r32(0x90110u), 10);
        eq("libsd handles 31 (sceSdCleanEffectWorkArea)", Libsd::handles(31u), 1);
        // The blocking clear (25) returns when done; the clean (31) reports its end as its DMA channel's transfer
        // interrupt: channel 1's handler (set above) with its argument. VP2's Csm chains its reverb change on it.
        const size_t before = host.calls.size();
        const uint32_t clearArea[5] = {0u, 1u, 7u};
        eq("sceSdClearEffectWorkArea", sd.call(25u, clearArea, 0u, r) && r == 0u, 1);
        eq("sceSdClearEffectWorkArea queues no interrupt", static_cast<long long>(host.calls.size() - before), 0);
        eq("sceSdCleanEffectWorkArea", sd.call(31u, clearArea, 0u, r) && r == 0u, 1);
        eq("sceSdCleanEffectWorkArea queues its channel's interrupt", static_cast<long long>(host.calls.size() - before), 1);
        if (host.calls.size() > before)
        {
            const auto &end = host.calls.back();
            eq("clean end: channel 1's handler", end.function, 0x4000);
            eq("clean end: the channel", end.a0, 1);
            eq("clean end: the handler's argument", end.a1, 0x77);
            eq("clean end: delay", end.delay, Libsd::kTransferDelay);
        }
        eq("libsd handles 13", Libsd::handles(13u), 1);
        eq("libsd does not handle 29", Libsd::handles(29u), 0);
    }

    void testReference()
    {
        // The digests the fork's mixer (the reference's) gives for mixer_scenario.h.
        Mixer a;
        eq("free-running scenario: the reference's samples and voice status",
           static_cast<long long>(bozo::audio::test::freeRun(a)), static_cast<long long>(0x27e0a39a032c3bc1ull));
        const Mixer::Stats stats = a.stats();
        eq("the scenario is not silent", stats.peak > 1000, 1);
        Mixer b;
        eq("clocked scenario: the reference's samples and voice status",
           static_cast<long long>(bozo::audio::test::clockedRun(b)), static_cast<long long>(0x4eee5deb242f84e6ull));
    }
}

static void testPortStream()
{
    using bozo::audio::PortStream;
    Mixer m;
    PortStream stream;
    m.setPortStream(&stream);
    m.setClocked(true);
    m.advanceTo(0);
    const int16_t frames[6] = {1000, -1000, 2000, -2000, 32000, -32000};
    eq("pushed", static_cast<long long>(stream.push(frames, 3)), 3);
    m.advanceTo(2 * Mixer::kIopCyclesPerSample);
    eq("taken as the clock makes samples", static_cast<long long>(stream.queued()), 1);
    int16_t out[8] = {};
    eq("two made", static_cast<long long>(m.take(out, 4)), 2);
    eq("left", out[0], 1000);
    eq("right", out[1], -1000);
    eq("the next", out[2], 2000);
    m.advanceTo(4 * Mixer::kIopCyclesPerSample);
    eq("two more", static_cast<long long>(m.take(out, 4)), 2);
    eq("the last", out[0], 32000);
    eq("then silence, not a wait", out[2], 0);

    std::vector<int16_t> big(2u * (PortStream::kFrames + 10u), 1);
    eq("it holds two seconds", static_cast<long long>(stream.push(big.data(), PortStream::kFrames + 10u)),
       static_cast<long long>(PortStream::kFrames));
    eq("nothing more", static_cast<long long>(stream.push(big.data(), 1)), 0);
    stream.clear();
    eq("cleared", static_cast<long long>(stream.queued()), 0);
    m.setPortStream(nullptr);
}

int main()
{
    testPortStream();
    testAttack();
    testEnd(false);
    testEnd(true);
    testRelease();
    testClock();
    testInitMixing();
    testLibsd();
    testReference();
    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("audio: all passed\n");
    return 0;
}
