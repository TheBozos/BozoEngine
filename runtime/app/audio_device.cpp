// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// See bozo/app/audio_device.h.
#include "bozo/app/audio_device.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace bozo::app
{
    namespace
    {
        constexpr int kRate = 48000;
        constexpr int kFrameBytes = 4; // stereo 16-bit
        double ms(double frames) { return frames * 1000.0 / kRate; }
    }

    struct AudioDevice::State
    {
        SDL_AudioStream *stream = nullptr;
        std::atomic<bool> started{false};
        double level = -1;  // the queue's smoothed level (frames)
        float ratio = 1.0f;
        bool dropping = false;
        std::atomic<uint64_t> underruns{0}, missing{0};
        mutable std::mutex mutex; // the counters below (the game's thread and stats())
        uint64_t pushed = 0, dropped = 0;
        double queueMs = 0, minQueueMs = 1e9, maxQueueMs = 0, deviceMs = 0;
        std::chrono::steady_clock::time_point nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        uint64_t lastUnderruns = 0, lastDropped = 0;
    };

    namespace
    {
        // The device's thread asks for data: what the queue cannot give (additional_amount) is an underrun.
        void SDLCALL request(void *user, SDL_AudioStream *, int additional, int)
        {
            auto *s = static_cast<AudioDevice::State *>(user);
            if (additional > 0 && s->started)
            {
                s->underruns.fetch_add(1, std::memory_order_relaxed);
                s->missing.fetch_add(static_cast<uint64_t>(additional / kFrameBytes), std::memory_order_relaxed);
            }
        }
    }

    AudioDevice::AudioDevice(const AudioOptions &options) : m_options(options), m_state(std::make_unique<State>()) {}

    AudioDevice::~AudioDevice() { close(); }

    bool AudioDevice::isOpen() const { return m_state->stream != nullptr; }

    bool AudioDevice::open()
    {
        if (m_state->stream)
            return true;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            std::fprintf(stderr, "[audio] SDL audio: %s\n", SDL_GetError());
            return false;
        }
        const std::string frames = std::to_string(m_options.deviceFrames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames.c_str());
        const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, kRate};
        m_state->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, request, m_state.get());
        if (!m_state->stream)
        {
            std::fprintf(stderr, "[audio] no playback device: %s\n", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        SDL_AudioSpec device{};
        int deviceFrames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(m_state->stream), &device, &deviceFrames) && device.freq > 0)
            m_state->deviceMs = deviceFrames * 1000.0 / device.freq;
        std::fprintf(stderr, "[audio] %s: %d Hz, %d channels, a %d-frame buffer (%.1f ms); queue target %.0f ms\n",
                     SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(m_state->stream)), device.freq, device.channels,
                     deviceFrames, m_state->deviceMs, m_options.latencyMs);
        return true;
    }

    void AudioDevice::close()
    {
        if (!m_state->stream)
            return;
        // After SDL_Quit the stream is already gone.
        if (SDL_WasInit(SDL_INIT_AUDIO))
        {
            SDL_DestroyAudioStream(m_state->stream);
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
        m_state->stream = nullptr;
        m_state->started = false;
    }

    void AudioDevice::push(const int16_t *frames, size_t count)
    {
        State &s = *m_state;
        if (!s.stream || !count)
            return;
        const double target = m_options.latencyMs * kRate / 1000.0;
        const double queued = SDL_GetAudioStreamQueued(s.stream) / static_cast<double>(kFrameBytes);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pushed += count;
        // The game ran ahead of real time (a burst after a stall, the boot before the frames are held to their rate):
        // what comes while the queue is beyond its limit is dropped, so the latency comes back at once instead of
        // over the seconds the ratio would take.
        if (ms(queued) > m_options.maxQueueMs || (s.dropping && ms(queued) > m_options.latencyMs))
        {
            s.dropping = true;
            s.dropped += count;
            return;
        }
        s.dropping = false;
        SDL_PutAudioStreamData(s.stream, frames, static_cast<int>(count * kFrameBytes));
        // The level halfway between two pushes (a push comes once a frame: the queue saws between them).
        const double level = queued + count / 2.0;
        s.level = s.level < 0 ? level : s.level + 0.05 * (level - s.level);
        if (!s.started && queued + count >= target)
        {
            SDL_ResumeAudioStreamDevice(s.stream);
            s.started = true;
            s.minQueueMs = 1e9;
            s.maxQueueMs = 0;
        }
        if (s.started)
        {
            s.queueMs = ms(queued);
            s.minQueueMs = std::min(s.minQueueMs, ms(queued));
            s.maxQueueMs = std::max(s.maxQueueMs, ms(queued + count));
            // Proportional: a queue 50% over its target plays 1% faster.
            const double error = (s.level - target) / target;
            const float ratio = static_cast<float>(1.0 + std::clamp(error * 0.02, -0.01, 0.01));
            if (std::fabs(ratio - s.ratio) > 0.0001f)
            {
                SDL_SetAudioStreamFrequencyRatio(s.stream, ratio);
                s.ratio = ratio;
            }
        }
        if (m_options.stats && std::chrono::steady_clock::now() >= s.nextStats)
        {
            s.nextStats += std::chrono::seconds(2);
            const uint64_t underruns = s.underruns.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[audio] queue %.1f ms (%.1f-%.1f), device %.1f ms, ratio %.4f, underruns %llu (+%llu), dropped %llu "
                         "frames (+%llu)\n",
                         ms(s.level), s.minQueueMs, s.maxQueueMs, s.deviceMs, s.ratio, static_cast<unsigned long long>(underruns),
                         static_cast<unsigned long long>(underruns - s.lastUnderruns), static_cast<unsigned long long>(s.dropped),
                         static_cast<unsigned long long>(s.dropped - s.lastDropped));
            s.lastUnderruns = underruns;
            s.lastDropped = s.dropped;
        }
    }

    AudioDevice::Stats AudioDevice::stats() const
    {
        const State &s = *m_state;
        Stats out;
        std::lock_guard<std::mutex> lock(s.mutex);
        out.pushed = s.pushed;
        out.dropped = s.dropped;
        out.underruns = s.underruns.load(std::memory_order_relaxed);
        out.missing = s.missing.load(std::memory_order_relaxed);
        out.queueMs = s.level < 0 ? 0 : ms(s.level);
        out.minQueueMs = s.minQueueMs > 1e8 ? 0 : s.minQueueMs;
        out.maxQueueMs = s.maxQueueMs;
        out.deviceMs = s.deviceMs;
        out.ratio = s.ratio;
        return out;
    }
}
