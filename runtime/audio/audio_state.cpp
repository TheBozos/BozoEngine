// SPDX-License-Identifier: MIT
// Save states of the native mixer and libsd (bozo/state.h). Built into bozo_libs, which has the archive.
#include "bozo/audio/libsd.h"
#include "bozo/audio/mixer.h"
#include "bozo/state.h"

namespace bozo::audio
{
    using state::fields;
    using state::pod;

    void Mixer::serialize(state::Archive &a, const uint8_t *inputBase, size_t inputBytes)
    {
        std::scoped_lock lock(m_mutex, m_queueMutex, m_digestMutex);
        a.begin("mixer");
        pod(a, m_stats);
        fields(a, m_clocked, m_clockStarted, m_samplesMade, m_memory, m_digestHash, m_digestCount);
        for (Group &g : m_groups)
        {
            for (Voice &v : g.voices)
                pod(a, v);
            fields(a, g.ended, g.dry, g.wet, g.mix);
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
        if (a.loading())
        {
            // What was mixed for the host belongs to the old state.
            m_queueRead = m_queueFill = 0;
        }
    }

    void Libsd::serialize(state::Archive &a)
    {
        a.begin("libsd");
        State &s = m_state;
        fields(a, s.params, s.switches, s.addresses, s.coreAttributes, s.transferAddress, s.irqCallback, s.blockBuffer,
               s.blockSize, s.halves, s.halvesPending, s.effectEnd);
        for (Callback &c : s.transCallback)
            pod(a, c);
        for (Callback &c : s.transHandler)
            pod(a, c);
        for (auto &e : s.effect)
            pod(a, e);
        a.end();
    }
}
