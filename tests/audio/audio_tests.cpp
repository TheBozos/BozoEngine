// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// bozo-audio-tests: the native mixer and libsd (runtime/audio, docs/specs/libraries.md section 15.14): the voice-status
// rules a driver sees (ENVX after KeyOn, a sample's end with and without repeat, the release after KeyOff, ENDX), the
// clock (a sample per 768 IOP cycles), libsd's entries and batches, and the reference's samples (mixer_scenario.h).
#include "bozo/audio/libsd.h"
#include "bozo/audio/mixer.h"
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
        eq("libsd handles 27", Libsd::handles(27u), 1);
        // The effect: sceSdSetEffectAttr sets everything, sceSdSetEffectMode the mode, delay and feedback (not the
        // depth), sceSdSetEffectModeParams delay, feedback and depth of the mode in place.
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
        eq("SetEffectMode: the delay", host.r32(0x9010Cu), 7);
        eq("SetEffectMode: the feedback", host.r32(0x90110u), 8);
        eq("SetEffectMode keeps the depth", ram16(0x90108u), 0x1111);
        effect(1u, 0x4444u, 0x5555u, 9u, 10u);
        eq("sceSdSetEffectModeParams", sd.call(33u, attr, 0u, r) && r == 0u, 1);
        sd.call(24u, read, 0u, r);
        eq("SetEffectModeParams keeps the mode", host.r32(0x90104u), 3);
        eq("SetEffectModeParams: the delay", host.r32(0x9010Cu), 9);
        eq("SetEffectModeParams: the depth", ram16(0x90108u), 0x4444);
        eq("SetEffectModeParams: the right depth", ram16(0x9010Au), 0x5555);
        eq("libsd handles 31 (sceSdCleanEffectWorkArea)", Libsd::handles(31u), 1);
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

int main()
{
    testAttack();
    testEnd(false);
    testEnd(true);
    testRelease();
    testClock();
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
