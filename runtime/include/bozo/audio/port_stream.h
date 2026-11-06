// A port's PCM into the mixer: 16-bit stereo at 48 kHz, added to the output after the voices, sample by sample as the
// mixer's clock makes them. No device behind it. Spec: docs/specs/libraries.md section 15.14.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace bozo::audio
{
    class PortStream
    {
    public:
        static constexpr size_t kFrames = 96000u; // 2 s

        // Interleaved left, right. Returns the frames taken: what did not fit is the port's to push again.
        size_t push(const int16_t *frames, size_t count);
        size_t queued() const;
        void clear();
        // The next frame; false (out untouched) when none is queued: the stream adds silence and is not waited for.
        bool pull(int16_t out[2]);

    private:
        mutable std::mutex m_mutex;
        std::vector<int16_t> m_ring = std::vector<int16_t>(2u * kFrames);
        size_t m_read = 0, m_fill = 0;
    };
}
