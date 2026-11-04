// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The real-time window (bozo/app/window.h). Written for BozoEngine on SDL3 (the fork's window and present loop lived in
// a file that cannot move: phase4-inventory.md section 2.2).
#include "bozo/app/window.h"

#include "bozo/app/app.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/panel.h"
#include "bozo/app/png.h"
#include "bozo/app/video_settings.h"
#include "bozo/render/gl.h"
#include "bozo/render/renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace bozo::app
{
    namespace
    {
        struct GameThread
        {
            const Game *game = nullptr;
            int result = 0;
            std::atomic<bool> done{false};
        };

        int SDLCALL gameThreadMain(void *data)
        {
            auto *t = static_cast<GameThread *>(data);
            t->result = t->game->run();
            t->done.store(true, std::memory_order_release);
            return t->result;
        }

        void *procAddress(const char *name) { return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name)); }

        double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
        {
            return std::chrono::duration<double>(b - a).count();
        }

        // The PNGs written on a thread of their own: the window's thread only reads the pixels back (a burst of ten a
        // second, a sequence of consecutive presents, would otherwise hitch the presents).
        class CaptureWriter
        {
        public:
            CaptureWriter() : m_thread([this] { run(); }) {}
            ~CaptureWriter()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop = true;
                }
                m_wake.notify_all();
                m_thread.join();
            }

            // The back buffer (pixels w x h) now, saved as path.
            void capture(const std::string &path, int w, int h)
            {
                const render::gl::Api &g = render::gl::api();
                Job job;
                job.path = path;
                job.width = w;
                job.height = h;
                job.pixels.resize(static_cast<size_t>(w) * h * 4u);
                g.ReadBuffer(render::gl::kBack);
                g.PixelStorei(render::gl::kPackAlignment, 4);
                g.ReadPixels(0, 0, w, h, render::gl::kRgba, render::gl::kUnsignedByte, job.pixels.data());
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_jobs.push_back(std::move(job));
                }
                m_wake.notify_one();
            }

        private:
            struct Job
            {
                std::string path;
                int width = 0, height = 0;
                std::vector<uint8_t> pixels; // bottom-up, as GL reads them
            };

            void run()
            {
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                        if (m_jobs.empty())
                            return; // stopping, everything written
                        job = std::move(m_jobs.front());
                        m_jobs.pop_front();
                    }
                    const size_t stride = static_cast<size_t>(job.width) * 4u;
                    std::vector<uint8_t> rows(job.pixels.size());
                    for (int y = 0; y < job.height; ++y) // bottom-up to top-down
                        std::copy_n(job.pixels.data() + static_cast<size_t>(job.height - 1 - y) * stride, stride,
                                    rows.data() + static_cast<size_t>(y) * stride);
                    const bool ok = writePng(job.path, rows.data(), static_cast<uint32_t>(job.width), static_cast<uint32_t>(job.height));
                    std::fprintf(stderr, "[window] %s %s\n", ok ? "saved" : "could not save", job.path.c_str());
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_wake;
            std::deque<Job> m_jobs;
            bool m_stop = false;
            std::thread m_thread;
        };

        std::vector<double> parseSeconds(const char *text)
        {
            std::vector<double> list;
            std::stringstream in(text ? text : "");
            for (std::string item; std::getline(in, item, ',');)
                if (!item.empty())
                    list.push_back(std::atof(item.c_str()));
            std::sort(list.begin(), list.end());
            return list;
        }

        std::string envString(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v ? std::string(v) : std::string();
        }
    }

    int runWindow(const WindowOptions &options, const Game &game, DevicePad *pad)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            std::fprintf(stderr, "[window] SDL_Init: %s\n", SDL_GetError());
            return 3;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        // With the video settings (video.ini) the window opens as a window at their size, then takes their mode.
        const bool videoSettings = options.videoSettings;
        const int openWidth = videoSettings ? video::edited().width : options.width;
        const int openHeight = videoSettings ? video::edited().height : options.height;
        SDL_Window *window = SDL_CreateWindow(options.title.c_str(), openWidth, openHeight,
                                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                  (!videoSettings && options.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!window)
        {
            std::fprintf(stderr, "[window] SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            return 3;
        }
        SDL_GLContext context = SDL_GL_CreateContext(window);
        const char *missing = nullptr;
        if (!context || !SDL_GL_MakeCurrent(window, context) || !render::gl::load(&procAddress, &missing))
        {
            std::fprintf(stderr, "[window] OpenGL 3.3: %s\n", missing ? missing : SDL_GetError());
            if (context)
                SDL_GL_DestroyContext(context);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 3;
        }
        SDL_GL_SetSwapInterval(options.vsync ? 1 : 0);
        const render::gl::Api &g = render::gl::api();
        std::fprintf(stderr, "[window] OpenGL %s, %s (%s)\n", reinterpret_cast<const char *>(g.GetString(render::gl::kVersion)),
                     reinterpret_cast<const char *>(g.GetString(render::gl::kRenderer)),
                     reinterpret_cast<const char *>(g.GetString(render::gl::kVendor)));
        render::Renderer &renderer = render::Renderer::instance();
        if (videoSettings)
            video::applyAtStartup(window);
        else
            renderer.setRenderScale(options.renderScale);
        Panel panel;
        if (options.panel)
            panel.init(window, context);

        GameThread thread;
        thread.game = &game;
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, reinterpret_cast<void *>(&gameThreadMain));
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 256ll << 20);
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &thread);
        SDL_Thread *gameThread = SDL_CreateThreadWithProperties(props);
        SDL_DestroyProperties(props);
        if (!gameThread)
        {
            std::fprintf(stderr, "[window] the game's thread: %s\n", SDL_GetError());
            panel.shutdown();
            video::detach();
            renderer.releaseGpu();
            SDL_GL_DestroyContext(context);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 3;
        }

        // Captures. Where: --capture-dir, else PS2X_CAPTURE_WINDOW_DIR, else <user folder>/captures (the current folder
        // without one).
        // - --capture-frames: the game's picture at the first present after each of those game frames (frame_<n>.png),
        //   without the panel.
        // - F8 (and PS2X_CAPTURE_SEQUENCE_AT=<s>[,<s>...]): PS2X_CAPTURE_SEQUENCE consecutive presents (1 by default) as
        //   seq<k>_<i>.png in PS2X_CAPTURE_SEQUENCE_DIR (else the capture folder), with seq<k>.txt (each one's time and
        //   the game frame it shows); the window as it is shown, panel included.
        // - F9 (and PS2X_CAPTURE_BURST_AT=<s>[,<s>...]): a burst on / off, ten captures a second (burst_<ms>.png, ms
        //   since the window opened), panel included.
        // - PS2X_CAPTURE_DIR + PS2X_CAPTURE_INTERVAL=<s>: the game's picture every s seconds (interval_<ms>.png).
        std::string captureDir = options.captureDir.empty() ? envString("PS2X_CAPTURE_WINDOW_DIR") : options.captureDir;
        if (captureDir.empty())
            captureDir = userDirectory().empty() ? std::string(".") : (userDirectory() / "captures").string();
        std::string sequenceDir = envString("PS2X_CAPTURE_SEQUENCE_DIR");
        if (sequenceDir.empty())
            sequenceDir = captureDir;
        const std::string intervalDir = envString("PS2X_CAPTURE_DIR");
        const double interval = intervalDir.empty() ? 0.0 : std::max(0.1, std::atof(envString("PS2X_CAPTURE_INTERVAL").c_str()));
        const int sequenceLength = std::max(1, std::atoi(envString("PS2X_CAPTURE_SEQUENCE").c_str()));
        std::vector<double> sequenceAt = parseSeconds(std::getenv("PS2X_CAPTURE_SEQUENCE_AT"));
        size_t nextSequenceAt = 0;
        // PS2X_CAPTURE_BURST_AT=<s>[,<s>...]: F9 pressed at those seconds (unattended checks of the bursts).
        std::vector<double> burstAt = parseSeconds(std::getenv("PS2X_CAPTURE_BURST_AT"));
        size_t nextBurstAt = 0;
        const auto ensureDir = [](const std::string &dir)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        };
        if (!options.captureFrames.empty())
            ensureDir(captureDir);
        std::vector<uint64_t> captureFrames = options.captureFrames;
        std::sort(captureFrames.begin(), captureFrames.end());
        size_t nextCapture = 0;
        int sequences = 0, sequenceLeft = 0, sequenceIndex = 0;
        std::ofstream sequenceLog;
        bool burst = false;
        CaptureWriter writer;
        bool quit = false;

        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        Clock::time_point statsSince = start, lastFresh{}, lastBurst{}, lastInterval = start, ratesSince = start;
        uint64_t statsFrames = game.framesEnded(), presents = 0, fresh = 0, over = 0, intervals = 0;
        uint64_t ratesFrames = statsFrames, ratesPresents = 0, ratesFresh = 0;
        double intervalSum = 0.0, intervalMax = 0.0, ratesIntervalSum = 0.0;
        // The whole run, from the first published frame: game frames over seconds.
        uint64_t runFirstFrames = 0;
        Clock::time_point runFirst{};
        uint64_t seen = 0;
        int swapRate = 0; // the frame rate the swap interval was set for
        Clock::time_point nextPresent{};
        while (!quit)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                panel.handleEvent(event);
                if (videoSettings)
                    video::handleEvent(event);
                if (pad)
                    pad->handle(event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    quit = true;
                else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
                {
                    if (event.key.scancode == SDL_SCANCODE_F8 && sequenceLeft == 0)
                        sequenceLeft = -1; // starts at this present
                    else if (event.key.scancode == SDL_SCANCODE_F9)
                    {
                        burst = !burst;
                        std::fprintf(stderr, "[window] capture burst %s (%s)\n", burst ? "on" : "off", captureDir.c_str());
                        if (burst)
                            ensureDir(captureDir);
                    }
                    else if (!videoSettings && (event.key.scancode == SDL_SCANCODE_F11 ||
                                                (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
                        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                }
            }
            if (videoSettings)
                video::frameUpdate();
            if (pad)
            {
                pad->setHeld(panel.holdsKeyboard(), panel.holdsMouse());
                pad->update((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
            }
            if (thread.done.load(std::memory_order_acquire))
                break;

            // The frame rate (Renderer::setFrameRate, the Video settings): the game's, each frame the game publishes
            // shown once as it comes (waiting a 30th of a second at most: the window stays responsive); the monitor's,
            // presented at each vsync; n a second, presented on that schedule without vsync. Above the game's, the
            // renderer draws the frames between the game's (interpolation).
            const int rate = renderer.interpolating() ? renderer.frameRate() : 0;
            if (rate != swapRate)
            {
                SDL_GL_SetSwapInterval(rate > 0 ? 0 : rate < 0 ? 1 : (options.vsync ? 1 : 0));
                swapRate = rate;
                nextPresent = Clock::now();
            }
            bool isFresh;
            if (rate == 0)
                isFresh = renderer.waitForFrame(seen, 1.0 / 30.0);
            else
            {
                if (rate > 0)
                {
                    // Sleep most of the wait, then spin the last stretch (as the frame pacer); a late present starts the
                    // schedule again from now.
                    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
                    nextPresent += period;
                    Clock::time_point t = Clock::now();
                    if (t > nextPresent + period)
                        nextPresent = t;
                    if (nextPresent - t > std::chrono::milliseconds(2))
                        std::this_thread::sleep_until(nextPresent - std::chrono::milliseconds(1));
                    while (Clock::now() < nextPresent)
                        std::this_thread::yield();
                }
                const uint64_t publishes = renderer.publishes();
                isFresh = publishes != seen;
                seen = publishes;
            }
            const Clock::time_point now = Clock::now();
            if (isFresh)
            {
                ++fresh;
                ++ratesFresh;
                if (lastFresh != Clock::time_point{})
                {
                    const double between = seconds(lastFresh, now);
                    intervalSum += between;
                    ratesIntervalSum += between;
                    intervalMax = std::max(intervalMax, between);
                    over += between > 0.020 ? 1u : 0u;
                    ++intervals;
                }
                lastFresh = now;
                if (runFirst == Clock::time_point{})
                {
                    runFirst = now;
                    runFirstFrames = game.framesEnded();
                }
            }
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            pw = std::max(pw, 1);
            ph = std::max(ph, 1);
            g.BindFramebuffer(render::gl::kFramebuffer, 0);
            g.Viewport(0, 0, pw, ph);
            g.Disable(render::gl::kScissorTest);
            g.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            g.Clear(render::gl::kColorBufferBit | render::gl::kDepthBufferBit);
            // The picture letterboxed to its shape.
            float rw = static_cast<float>(pw), rh = static_cast<float>(ph);
            if (rw > rh * options.aspect)
                rw = std::round(rh * options.aspect);
            else
                rh = std::round(rw / options.aspect);
            renderer.present(std::round((static_cast<float>(pw) - rw) * 0.5f), std::round((static_cast<float>(ph) - rh) * 0.5f), rw, rh);
            ++presents;
            ++ratesPresents;

            const uint64_t frames = game.framesEnded();
            const double elapsed = seconds(start, now);
            const unsigned long long ms = static_cast<unsigned long long>(elapsed * 1000.0);
            // The game's picture alone (before the panel).
            bool captureFrame = false;
            while (nextCapture < captureFrames.size() && frames >= captureFrames[nextCapture])
            {
                captureFrame = true;
                ++nextCapture;
            }
            if (captureFrame)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(frames));
                writer.capture(captureDir + "/" + name, pw, ph);
            }
            if (interval > 0.0 && seconds(lastInterval, now) >= interval)
            {
                lastInterval = now;
                ensureDir(intervalDir);
                char name[64];
                std::snprintf(name, sizeof(name), "interval_%08llu.png", ms);
                writer.capture(intervalDir + "/" + name, pw, ph);
            }

            // The overlay's rates, every half second.
            if (seconds(ratesSince, now) >= 0.5)
            {
                const double span = seconds(ratesSince, now);
                const uint64_t betweenCount = ratesFresh > 1 ? ratesFresh - 1 : 0;
                panel.setRates(static_cast<double>(frames - ratesFrames) / span, static_cast<double>(ratesPresents) / span,
                               betweenCount ? 1000.0 * ratesIntervalSum / static_cast<double>(ratesFresh) : 0.0);
                ratesSince = now;
                ratesFrames = frames;
                ratesPresents = ratesFresh = 0;
                ratesIntervalSum = 0.0;
            }
            panel.draw();

            // The window as shown (the panel included).
            while (nextSequenceAt < sequenceAt.size() && elapsed >= sequenceAt[nextSequenceAt])
            {
                ++nextSequenceAt;
                if (sequenceLeft == 0)
                    sequenceLeft = -1;
            }
            if (sequenceLeft != 0)
            {
                if (sequenceLeft < 0)
                {
                    sequenceLeft = sequenceLength;
                    sequenceIndex = 0;
                    ++sequences;
                    ensureDir(sequenceDir);
                    sequenceLog.close();
                    sequenceLog.open(sequenceDir + "/seq" + std::to_string(sequences) + ".txt", std::ios::trunc);
                }
                char name[64];
                std::snprintf(name, sizeof(name), "seq%d_%03d.png", sequences, sequenceIndex);
                writer.capture(sequenceDir + "/" + name, pw, ph);
                if (sequenceLog)
                {
                    sequenceLog << name << " t=" << elapsed << " s, game frame " << frames << (isFresh ? "" : " (held)");
                    // Interpolating: the two published frames drawn between and how far (1: the newer as it is).
                    if (renderer.interpolating())
                    {
                        const render::Renderer::PresentInfo info = renderer.lastPresent();
                        sequenceLog << ", drawn between frames " << info.from << " and " << info.to << " at " << info.alpha << " ("
                                    << info.interpolatedBatches << " of " << info.keyedBatches << " keyed batches, "
                                    << info.trackedInterpolated << " of " << info.trackedTriangles << " tracked triangles moved)";
                    }
                    sequenceLog << "\n";
                }
                ++sequenceIndex;
                if (--sequenceLeft == 0)
                    sequenceLog.close();
            }
            while (nextBurstAt < burstAt.size() && elapsed >= burstAt[nextBurstAt])
            {
                ++nextBurstAt;
                burst = !burst;
                std::fprintf(stderr, "[window] capture burst %s (%s)\n", burst ? "on" : "off", captureDir.c_str());
                if (burst)
                    ensureDir(captureDir);
            }
            if (burst && seconds(lastBurst, now) >= 0.1)
            {
                // Ten a second on the burst's own schedule (a late present does not push the next one back).
                lastBurst = seconds(lastBurst, now) < 0.2 ? lastBurst + std::chrono::milliseconds(100) : now;
                char name[64];
                std::snprintf(name, sizeof(name), "burst_%08llu.png", ms);
                writer.capture(captureDir + "/" + name, pw, ph);
            }
            SDL_GL_SwapWindow(window);

            if (options.stats && seconds(statsSince, now) >= 2.0)
            {
                const double span = seconds(statsSince, now);
                std::fprintf(stderr,
                             "[window] t=%.1f s, game frame %llu: %.2f game frames/s, %.1f presents/s, %llu new pictures; time between "
                             "pictures %.2f ms avg, %.2f ms max, %llu over 20 ms\n",
                             elapsed, static_cast<unsigned long long>(frames), (frames - statsFrames) / span, presents / span,
                             static_cast<unsigned long long>(fresh), intervals ? 1000.0 * intervalSum / intervals : 0.0,
                             1000.0 * intervalMax, static_cast<unsigned long long>(over));
                statsSince = now;
                statsFrames = frames;
                presents = fresh = over = intervals = 0;
                intervalSum = intervalMax = 0.0;
            }
            if (options.quitAfterFrames && frames >= options.quitAfterFrames)
                quit = true;
        }
        if (runFirst != Clock::time_point{})
        {
            const double span = seconds(runFirst, Clock::now());
            const uint64_t frames = game.framesEnded() - runFirstFrames;
            std::fprintf(stderr, "[window] %llu game frames in %.2f s since the first picture: %.2f frames/s\n",
                         static_cast<unsigned long long>(frames), span, span > 0.0 ? frames / span : 0.0);
        }
        game.stop();
        int result = 0;
        SDL_WaitThread(gameThread, &result);
        panel.shutdown();
        video::detach();
        renderer.releaseGpu();
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return thread.result;
    }
}
