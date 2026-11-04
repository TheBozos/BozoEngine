// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The real-time platform layer's sound output (phase 4, milestone M3): an SDL3 audio stream at 48 kHz stereo 16-bit
// on the default playback device. The game's thread pushes what the mixer made on the EE clock (the free host's
// onAudio, once a game frame); the device plays it a short, steady time later.
//
// The EE clock and the PC's clock drift apart (an NTSC field is 1/59.94 s, the frames are held to 60.0 a second; the
// sound card's crystal is its own), so the stream's frequency ratio follows the queue: a little faster when it fills
// beyond the target, slower when it drains (at most 1%: no pitch change anyone hears). What arrives while the queue is
// far beyond the target (the game ran ahead of real time) is dropped; when the queue runs dry the device plays
// silence and the underrun is counted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bozo::app
{
    struct AudioOptions
    {
        double latencyMs = 32.0;   // the queue's target (the device's own buffer comes on top)
        uint32_t deviceFrames = 512; // the device's buffer asked of SDL (frames at 48 kHz: 10.7 ms)
        double maxQueueMs = 64.0;  // beyond this, what is pushed is dropped until the queue is back at its target
        bool stats = false;        // every 2 s on stderr: queue, latency, ratio, underruns, drops
    };

    class AudioDevice
    {
    public:
        explicit AudioDevice(const AudioOptions &options = {});
        ~AudioDevice();
        AudioDevice(const AudioDevice &) = delete;
        AudioDevice &operator=(const AudioDevice &) = delete;

        // Initialises SDL's audio and opens the default playback device (paused until the queue reaches its target).
        // False (and nothing plays) when there is no device.
        bool open();
        // Closes the device. Safe after SDL_Quit (the device is gone with it).
        void close();
        bool isOpen() const;

        // Any thread: interleaved stereo frames at 48 kHz.
        void push(const int16_t *frames, size_t count);

        struct Stats
        {
            uint64_t pushed = 0, dropped = 0;  // frames
            uint64_t underruns = 0;            // device requests the queue could not fill
            uint64_t missing = 0;              // frames of silence those requests got
            double queueMs = 0, minQueueMs = 0, maxQueueMs = 0; // the queue now, and its range since the device started
            double deviceMs = 0;               // the device's buffer
            double ratio = 1;                  // the stream's frequency ratio now
        };
        Stats stats() const;

        struct State;

    private:
        AudioOptions m_options;
        std::unique_ptr<State> m_state;
    };
}
