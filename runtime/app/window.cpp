// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The real-time window (bozo/app/window.h). Written for BozoEngine on SDL3 (the fork's window and present loop lived in
// a file that cannot move: phase4-inventory.md section 2.2).
#include "bozo/app/window.h"

#include "bozo/app/device_pad.h"
#include "bozo/app/png.h"
#include "bozo/render/gl.h"
#include "bozo/render/renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

        // The window's back buffer (pixels w x h) as a PNG.
        bool capture(const std::string &path, int w, int h)
        {
            const render::gl::Api &g = render::gl::api();
            std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u), rows(pixels.size());
            g.ReadBuffer(render::gl::kBack);
            g.PixelStorei(render::gl::kPackAlignment, 4);
            g.ReadPixels(0, 0, w, h, render::gl::kRgba, render::gl::kUnsignedByte, pixels.data());
            const size_t stride = static_cast<size_t>(w) * 4u;
            for (int y = 0; y < h; ++y) // bottom-up to top-down
                std::copy_n(pixels.data() + static_cast<size_t>(h - 1 - y) * stride, stride, rows.data() + static_cast<size_t>(y) * stride);
            const bool ok = writePng(path, rows.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            std::fprintf(stderr, "[window] %s %s\n", ok ? "saved" : "could not save", path.c_str());
            return ok;
        }

        double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
        {
            return std::chrono::duration<double>(b - a).count();
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
        SDL_Window *window = SDL_CreateWindow(options.title.c_str(), options.width, options.height,
                                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                  (options.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
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
        renderer.setRenderScale(options.renderScale);

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
            renderer.releaseGpu();
            SDL_GL_DestroyContext(context);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 3;
        }

        const std::string captureDir = options.captureDir.empty() ? std::string(".") : options.captureDir;
        if (!options.captureDir.empty() || !options.captureFrames.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(captureDir, ec);
        }
        std::vector<uint64_t> captureFrames = options.captureFrames;
        std::sort(captureFrames.begin(), captureFrames.end());
        size_t nextCapture = 0;
        int captures = 0;
        bool captureNow = false, fullscreen = options.fullscreen, quit = false;

        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        Clock::time_point statsSince = start, lastFresh{};
        uint64_t statsFrames = game.framesEnded(), presents = 0, fresh = 0, over = 0, intervals = 0;
        double intervalSum = 0.0, intervalMax = 0.0;
        // The whole run, from the first published frame: game frames over seconds.
        uint64_t runFirstFrames = 0;
        Clock::time_point runFirst{};
        uint64_t seen = 0;
        while (!quit)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (pad)
                    pad->handle(event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    quit = true;
                else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
                {
                    if (event.key.scancode == SDL_SCANCODE_F8)
                        captureNow = true;
                    else if (event.key.scancode == SDL_SCANCODE_F11 ||
                             (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT)))
                    {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(window, fullscreen);
                    }
                }
            }
            if (pad)
                pad->update((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
            if (thread.done.load(std::memory_order_acquire))
                break;

            // The next frame the game publishes (a 30th of a second at most: the window stays responsive).
            const bool isFresh = renderer.waitForFrame(seen, 1.0 / 30.0);
            const Clock::time_point now = Clock::now();
            if (isFresh)
            {
                ++fresh;
                if (lastFresh != Clock::time_point{})
                {
                    const double interval = seconds(lastFresh, now);
                    intervalSum += interval;
                    intervalMax = std::max(intervalMax, interval);
                    over += interval > 0.020 ? 1u : 0u;
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

            const uint64_t frames = game.framesEnded();
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
                capture(captureDir + "/" + name, pw, ph);
            }
            if (captureNow)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "capture_%03d_frame_%06llu.png", ++captures, static_cast<unsigned long long>(frames));
                capture(captureDir + "/" + name, pw, ph);
                captureNow = false;
            }
            SDL_GL_SwapWindow(window);

            if (options.stats && seconds(statsSince, now) >= 2.0)
            {
                const double span = seconds(statsSince, now);
                std::fprintf(stderr,
                             "[window] t=%.1f s, game frame %llu: %.2f game frames/s, %.1f presents/s, %llu new pictures; time between "
                             "pictures %.2f ms avg, %.2f ms max, %llu over 20 ms\n",
                             seconds(start, now), static_cast<unsigned long long>(frames), (frames - statsFrames) / span, presents / span,
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
        renderer.releaseGpu();
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return thread.result;
    }
}
