// SPDX-License-Identifier: MIT
// Save states of the native mixer and libsd (bozo/state.h). Built into bozo_libs, which has the archive.
#include "bozo/audio/libsd.h"
#include "bozo/audio/mixer.h"
#include "bozo/state.h"

namespace bozo::audio
{
    using state::fields;
    using state::pod;

    // A section read in the current layout, else (a state written before the field was added: the section's size check
    // fails) again from the same place in the older one.
    template <class Walk> void withOlderLayout(state::Archive &a, Walk walk, auto resetNewer)
    {
        if (!a.loading())
            return walk(true);
        const state::Archive::Mark mark = a.mark();
        try
        {
            walk(true);
        }
        catch (const state::Error &)
        {
            a.rewind(mark);
            resetNewer();
            walk(false);
        }
    }

    void Mixer::serialize(state::Archive &a, const uint8_t *inputBase, size_t inputBytes)
    {
        std::scoped_lock lock(m_mutex, m_queueMutex, m_digestMutex);
        // A state written before the SPU2 interrupt (3b79730) has no IRQ address or enable: the cores' stay off.
        withOlderLayout(a, [&](bool irq) { walk(a, inputBase, inputBytes, irq); },
                        [&]
                        {
                            for (Group &g : m_groups)
                            {
                                g.irqAddress = 0;
                                g.irqEnabled = false;
                            }
                        });
        if (a.loading())
        {
            // What was mixed for the host belongs to the old state.
            m_queueRead = m_queueFill = 0;
        }
    }

    void Mixer::walk(state::Archive &a, const uint8_t *inputBase, size_t inputBytes, bool irq)
    {
        a.begin("mixer");
        pod(a, m_stats);
        fields(a, m_clocked, m_clockStarted, m_samplesMade, m_memory, m_digestHash, m_digestCount);
        for (Group &g : m_groups)
        {
            for (Voice &v : g.voices)
                pod(a, v);
            fields(a, g.ended, g.dry, g.wet, g.mix);
            if (irq)
                fields(a, g.irqAddress, g.irqEnabled);
            for (Volume &v : g.master)
                pod(a, v);
            for (Volume &v : g.effect)
                pod(a, v);
            pod(a, g.reverb);
            Input &in = g.input;
            uint64_t offset = ~0ull;
            if (!a.loading() && in.memory)
            {
                if (!inputBase || in.memory < inputBase || in.memory >= inputBase + inputBytes)
                    throw state::Error("save state: a mixer input plays from memory outside the IOP's");
                offset = static_cast<uint64_t>(in.memory - inputBase);
            }
            fields(a, offset, in.samples, in.position, in.playing, in.loop, in.volume);
            if (a.loading())
            {
                if (offset != ~0ull && (!inputBase || offset >= inputBytes))
                    throw state::Error("save state: a mixer input outside the IOP's memory");
                in.memory = offset == ~0ull ? nullptr : inputBase + offset;
            }
        }
        a.end();
    }

    void Libsd::serialize(state::Archive &a)
    {
        // A state written before the SPU2 interrupt (3b79730) has no interrupt handler.
        withOlderLayout(a, [&](bool handler) { walk(a, handler); }, [&] { m_state.spu2Handler = Callback{}; });
    }

    void Libsd::walk(state::Archive &a, bool handler)
    {
        a.begin("libsd");
        State &s = m_state;
        fields(a, s.params, s.switches, s.addresses, s.coreAttributes, s.transferAddress, s.irqCallback, s.blockBuffer,
               s.blockSize, s.halves, s.halvesPending, s.effectEnd);
        for (Callback &c : s.transCallback)
            pod(a, c);
        for (Callback &c : s.transHandler)
            pod(a, c);
        if (handler)
            pod(a, s.spu2Handler);
        for (auto &e : s.effect)
            pod(a, e);
        a.end();
    }
}
