// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// See bozo/audio/wav.h.
#include "bozo/audio/wav.h"

#include <algorithm>

namespace bozo::audio
{
    namespace
    {
        void put32(uint8_t *p, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                p[i] = static_cast<uint8_t>(v >> (8 * i));
        }
        void put16(uint8_t *p, uint16_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
        }
    }

    bool WavWriter::open(const std::string &path, uint32_t rate)
    {
        close();
        m_file = std::fopen(path.c_str(), "wb");
        if (!m_file)
            return false;
        m_rate = rate;
        m_bytes = 0;
        header();
        return true;
    }

    void WavWriter::header()
    {
        // The data's size is capped at what a RIFF size can say (4 GB: about 6 hours of sound).
        const uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(m_bytes, 0xFFFFFFFFull - 36u));
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        put32(h + 4, 36u + data);
        put32(h + 16, 16u);
        put16(h + 20, 1u);          // PCM
        put16(h + 22, 2u);          // stereo
        put32(h + 24, m_rate);
        put32(h + 28, m_rate * 4u); // bytes a second
        put16(h + 32, 4u);          // bytes a frame
        put16(h + 34, 16u);
        h[36] = 'd', h[37] = 'a', h[38] = 't', h[39] = 'a';
        put32(h + 40, data);
        std::fseek(m_file, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, m_file);
        std::fseek(m_file, 0, SEEK_END);
    }

    void WavWriter::write(const int16_t *frames, size_t count)
    {
        if (!m_file || !count)
            return;
        // Little-endian on disk, as the host's int16 on every target BozoEngine builds for.
        std::fwrite(frames, 4u, count, m_file);
        const uint64_t before = m_bytes;
        m_bytes += 4u * count;
        if (before / (m_rate * 4u) != m_bytes / (m_rate * 4u))
        {
            header();
            std::fflush(m_file);
        }
    }

    void WavWriter::close()
    {
        if (!m_file)
            return;
        header();
        std::fclose(m_file);
        m_file = nullptr;
    }
}
