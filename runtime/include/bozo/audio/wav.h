// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A WAV file of 16-bit stereo samples (the mixer's output) written as they come. The header is brought up to date
// every second of sound and at close, so a run that is killed leaves a readable file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bozo::audio
{
    class WavWriter
    {
    public:
        WavWriter() = default;
        ~WavWriter() { close(); }
        WavWriter(const WavWriter &) = delete;
        WavWriter &operator=(const WavWriter &) = delete;

        bool open(const std::string &path, uint32_t rate = 48000u);
        bool isOpen() const { return m_file != nullptr; }
        // Interleaved stereo frames (left, right).
        void write(const int16_t *frames, size_t count);
        void close();
        uint64_t frames() const { return m_bytes / 4u; }

    private:
        void header();
        std::FILE *m_file = nullptr;
        uint32_t m_rate = 48000u;
        uint64_t m_bytes = 0;
    };
}
