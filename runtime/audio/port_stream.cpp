#include "bozo/audio/port_stream.h"

#include <algorithm>

namespace bozo::audio
{
    size_t PortStream::push(const int16_t *frames, size_t count)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t taken = std::min(count, kFrames - m_fill);
        for (size_t i = 0; i < taken; ++i)
        {
            const size_t at = (m_read + m_fill + i) % kFrames;
            m_ring[at * 2u] = frames[i * 2u];
            m_ring[at * 2u + 1u] = frames[i * 2u + 1u];
        }
        m_fill += taken;
        return taken;
    }

    size_t PortStream::queued() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_fill;
    }

    void PortStream::clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_read = m_fill = 0;
    }

    bool PortStream::pull(int16_t out[2])
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fill == 0)
            return false;
        out[0] = m_ring[m_read * 2u];
        out[1] = m_ring[m_read * 2u + 1u];
        m_read = (m_read + 1u) % kFrames;
        --m_fill;
        return true;
    }
}
