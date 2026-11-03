// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A fixed scenario for the mixer, written against its public interface only (a template, so the same steps can drive
// any mixer with that interface): pseudo-random ADPCM banks, voices keyed on and off with every kind of envelope and
// volume sweep, ends with and without repeat, both reverbs, the sound data input; the output samples and every voice's
// ENVX, ENDX and NAX read back along the way are folded into one FNV-1a 64 digest. The expected digests in
// audio_tests.cpp are what the mixer gave on the PS2Recomp fork (the reference's) for the same steps.
#pragma once

#include <cstdint>
#include <vector>

namespace bozo::audio::test
{
    struct Digest
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        void byte(uint8_t b) { hash = (hash ^ b) * 0x100000001B3ull; }
        void word(uint32_t w)
        {
            for (int i = 0; i < 4; ++i)
                byte(static_cast<uint8_t>(w >> (8 * i)));
        }
        void samples(const int16_t *s, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
            {
                byte(static_cast<uint8_t>(s[i]));
                byte(static_cast<uint8_t>(static_cast<uint16_t>(s[i]) >> 8));
            }
        }
    };

    struct Rng
    {
        uint32_t state;
        uint32_t next()
        {
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        }
    };

    // A sample of blocks 16-byte blocks at address: header (shift 0-12, filter 0-4), flags (last: end | repeat as
    // asked, loop start on block loopAt), 14 bytes of nibbles.
    template <class M>
    void writeSample(M &m, Rng &rng, uint32_t address, uint32_t blocks, bool repeat, uint32_t loopAt)
    {
        std::vector<uint8_t> data(blocks * 16u);
        for (uint32_t b = 0; b < blocks; ++b)
        {
            uint8_t *p = &data[b * 16u];
            p[0] = static_cast<uint8_t>((rng.next() % 13u) | ((rng.next() % 5u) << 4u));
            p[1] = static_cast<uint8_t>((b == loopAt ? 4u : 0u) | (b + 1u == blocks ? (repeat ? 3u : 1u) : 0u));
            for (int i = 2; i < 16; ++i)
                p[i] = static_cast<uint8_t>(rng.next());
        }
        m.write(address, data.data(), static_cast<uint32_t>(data.size()));
    }

    template <class M>
    void readBack(M &m, Digest &d)
    {
        for (int g = 0; g < 2; ++g)
        {
            d.word(m.ended(g));
            for (int v = 0; v < 24; ++v)
            {
                d.word(m.envelope(g, v));
                d.word(m.position(g, v));
            }
        }
    }

    // The voice settings and events of step `step` (both runs: free and clocked).
    template <class M>
    void events(M &m, Rng &rng, int step)
    {
        static const uint16_t kAdsr1[] = {0x00ff, 0x80ff, 0x28f0, 0x5044, 0x0a0a, 0x7f7f, 0x8a5c, 0x0000};
        static const uint16_t kAdsr2[] = {0x1fcd, 0x5fcd, 0x5fce, 0xc020, 0x0000, 0x4ae5, 0x9fc0, 0x1fe7};
        static const uint16_t kVolumes[] = {0x3fff, 0x1000, 0x8040, 0xa07f, 0xc030, 0xe010, 0x9020, 0x0800};
        for (int k = 0; k < 3; ++k)
        {
            const int g = static_cast<int>(rng.next() % 2u), v = static_cast<int>(rng.next() % 24u);
            switch (rng.next() % 6u)
            {
            case 0:
            case 1:
            {
                m.setStart(g, v, 0x5000u + (rng.next() % 12u) * 0x2000u);
                if (rng.next() % 4u == 0u)
                    m.setLoop(g, v, 0x5000u + (rng.next() % 12u) * 0x2000u + 0x40u);
                m.setPitch(g, v, static_cast<uint16_t>(0x400u + rng.next() % 0x3c00u));
                m.setAdsr(g, v, 0, kAdsr1[rng.next() % 8u]);
                m.setAdsr(g, v, 1, kAdsr2[rng.next() % 8u]);
                m.setVolume(g, v, 0, kVolumes[rng.next() % 8u]);
                m.setVolume(g, v, 1, kVolumes[rng.next() % 8u]);
                m.keyOn(g, 1u << v);
                break;
            }
            case 2: m.keyOff(g, 1u << v); break;
            case 3: m.clearEnded(g, 0xFFFFFFu); break;
            case 4: m.setPitch(g, v, static_cast<uint16_t>(rng.next() % 0x4000u)); break;
            default: m.setVolume(g, v, static_cast<int>(rng.next() % 2u), kVolumes[rng.next() % 8u]); break;
            }
        }
        if (step == 40)
            m.setEffect(0, 9, 0, 0, 0x1DFFFFu);
        if (step == 80)
        {
            m.setEffect(1, 7, 40, 90, 0x1FFFFFu);
            m.setMasterVolume(1, 0, 0x8050);
        }
        if (step == 150)
            m.stopInput(1);
    }

    template <class M>
    void setUp(M &m, Rng &rng, const std::vector<uint8_t> &input)
    {
        for (uint32_t i = 0; i < 12u; ++i)
            writeSample(m, rng, 0x5000u + i * 0x2000u, 20u + (rng.next() % 400u), i % 3u == 0u, i % 2u);
        for (int g = 0; g < 2; ++g)
        {
            m.setDryMask(g, 0, 0xFFFFFFu);
            m.setDryMask(g, 1, 0xFFFFFFu);
            m.setWetMask(g, 0, 0x00FF00u);
            m.setWetMask(g, 1, 0x0F0F0Fu);
            m.setMasterVolume(g, 0, 0x3fff);
            m.setMasterVolume(g, 1, 0x3fff);
            m.setEffectVolume(g, 0, 0x2000);
            m.setEffectVolume(g, 1, 0x2000);
            m.setEffectEnable(g, true);
        }
        m.setMix(0, 0xFF0u);
        m.setInputVolume(1, 0, 0x6000);
        m.setInputVolume(1, 1, 0x5000);
        m.startInput(1, input.data(), static_cast<uint32_t>(input.size()), 0x400u, true);
    }

    inline std::vector<uint8_t> inputStream()
    {
        Rng rng{77u};
        std::vector<uint8_t> input(0x4000u);
        for (uint8_t &b : input)
            b = static_cast<uint8_t>(rng.next());
        return input;
    }

    // Free-running: render() in chunks of 256 frames, 200 steps of events between them.
    template <class M>
    uint64_t freeRun(M &m)
    {
        Rng rng{12345u};
        const std::vector<uint8_t> input = inputStream();
        setUp(m, rng, input);
        Digest d;
        std::vector<int16_t> out(256u * 2u);
        for (int step = 0; step < 200; ++step)
        {
            events(m, rng, step);
            m.render(out.data(), 256u);
            d.samples(out.data(), out.size());
            readBack(m, d);
        }
        return d.hash;
    }

    // Clocked: advanceTo at irregular IOP cycles (the origin first), events between, the queue drained by render().
    template <class M>
    uint64_t clockedRun(M &m)
    {
        Rng rng{999u};
        const std::vector<uint8_t> input = inputStream();
        m.setClocked(true);
        setUp(m, rng, input);
        Digest d;
        uint64_t cycle = 5000u;
        m.advanceTo(cycle);
        std::vector<int16_t> out(4096u * 2u);
        for (int step = 0; step < 200; ++step)
        {
            events(m, rng, step);
            cycle += 1000u + rng.next() % 300000u;
            m.advanceTo(cycle);
            readBack(m, d);
            const size_t frames = 1u + rng.next() % 4096u;
            m.render(out.data(), frames);
            d.samples(out.data(), frames * 2u);
        }
        return d.hash;
    }
}
