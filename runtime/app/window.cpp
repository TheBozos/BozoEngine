// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The real-time window (bozo/app/window.h). Written for BozoEngine on SDL3 (the fork's window and present loop lived in
// a file that cannot move: phase4-inventory.md section 2.2).
#include "bozo/app/window.h"

#include "bozo/app/app.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/frame_pacer.h"
#include "bozo/app/input_bindings.h"
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
#include <cstring>
#include <ctime>
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
            void readOffscreen(bool offscreen) { m_offscreen = offscreen; }
            ~CaptureWriter()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop = true;
                }
                m_wake.notify_all();
                m_thread.join();
            }

            // The back buffer (pixels w x h) now, saved as path; `compressed`: through SDL_SavePNG (a player's
            // screenshot, F12; a save state's picture, scaled down to `maxWidth` when it is set), else the quick
            // uncompressed writer (captures, many a second).
            void capture(const std::string &path, int w, int h, bool compressed = false, int maxWidth = 0)
            {
                const render::gl::Api &g = render::gl::api();
                Job job;
                job.path = path;
                job.width = w;
                job.height = h;
                job.compressed = compressed;
                job.maxWidth = maxWidth;
                job.pixels.resize(static_cast<size_t>(w) * h * 4u);
                // From the back buffer, or from the offscreen picture the window presents into when nothing may depend
                // on the window being visible (WindowOptions::captureScene / hidden, PS2X_CAPTURE_SCENE=1).
                g.ReadBuffer(m_offscreen ? render::gl::GLenum(0x8CE0) /* GL_COLOR_ATTACHMENT0 */ : render::gl::kBack);
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
                bool compressed = false;
                int maxWidth = 0;
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
                    bool ok = false;
                    if (job.compressed)
                    {
                        for (size_t i = 3; i < rows.size(); i += 4)
                            rows[i] = 0xFF; // the picture's alpha is the renderer's, not the player's
                        if (SDL_Surface *surface = SDL_CreateSurfaceFrom(job.width, job.height, SDL_PIXELFORMAT_RGBA32,
                                                                         rows.data(), static_cast<int>(stride)))
                        {
                            SDL_Surface *saved = surface;
                            if (job.maxWidth > 0 && job.width > job.maxWidth)
                                if (SDL_Surface *small = SDL_ScaleSurface(surface, job.maxWidth,
                                                                          std::max(1, job.height * job.maxWidth / job.width),
                                                                          SDL_SCALEMODE_LINEAR))
                                    saved = small;
                            ok = SDL_SavePNG(saved, job.path.c_str());
                            if (saved != surface)
                                SDL_DestroySurface(saved);
                            SDL_DestroySurface(surface);
                        }
                    }
                    else
                        ok = writePng(job.path, rows.data(), static_cast<uint32_t>(job.width), static_cast<uint32_t>(job.height));
                    std::fprintf(stderr, "[window] %s %s\n", ok ? "saved" : "could not save", job.path.c_str());
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_wake;
            std::deque<Job> m_jobs;
            bool m_stop = false;
            bool m_offscreen = false; // captures read the offscreen picture (WindowOptions::captureScene)
            std::thread m_thread;
        };

        // A save state's picture is taken at the next present after its save was asked (F5 or the Save states tab).
        std::atomic<int> g_pictureSlot{-1};

        // F12's file: <user folder>/screenshots/screenshot_<date>_<time>.png (PS2X_SCREENSHOT_DIR another folder), a
        // number added when two fall in the same second.
        std::string screenshotPath()
        {
            std::filesystem::path dir;
            if (const char *env = std::getenv("PS2X_SCREENSHOT_DIR"); env && *env)
                dir = env;
            else
                dir = userDirectory().empty() ? std::filesystem::path("screenshots") : userDirectory() / "screenshots";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
            // The last name given counts as taken too: its file may not be written yet (the writer's thread).
            static std::filesystem::path s_last;
            std::filesystem::path path = dir / ("screenshot_" + std::string(stamp) + ".png");
            for (int n = 2; (path == s_last || std::filesystem::exists(path, ec)) && n < 100; ++n)
                path = dir / ("screenshot_" + std::string(stamp) + "_" + std::to_string(n) + ".png");
            s_last = path;
            return path.string();
        }

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
                                                  (options.hidden ? SDL_WINDOW_HIDDEN : 0) |
                                                  (!videoSettings && !options.hidden && options.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
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
        const auto saveState = [save = game.saveState](int slot)
        {
            save(slot);
            g_pictureSlot.store(slot, std::memory_order_relaxed);
        };
        if (game.saveState && game.loadState)
            registerStateTab(saveState, game.loadState);

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
        // PS2X_SCREENSHOT_AT=<s>[,<s>...]: F12 pressed at those seconds.
        std::vector<double> screenshotAt = parseSeconds(std::getenv("PS2X_SCREENSHOT_AT"));
        size_t nextScreenshotAt = 0;
        // PS2X_STATE_SAVE_AT=<s>[,<s>...]: F5 pressed at those seconds (the selected slot, its picture).
        std::vector<double> stateSaveAt = parseSeconds(std::getenv("PS2X_STATE_SAVE_AT"));
        size_t nextStateSaveAt = 0;
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
        bool screenshot = false; // F12: the next present's game picture
        CaptureWriter writer;
        // The window presents into an offscreen picture (then copied to the window when it is shown) when captures must
        // not depend on the window being visible: a hidden or covered window's back buffer reads back black.
        static const bool sceneByEnv = [] { const char *v = std::getenv("PS2X_CAPTURE_SCENE"); return v && *v == '1'; }();
        const bool offscreen = options.captureScene || options.hidden || sceneByEnv;
        writer.readOffscreen(offscreen);
        render::gl::GLuint offFbo = 0, offColour = 0, offDepth = 0;
        int offWidth = 0, offHeight = 0;
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
        uint64_t lockstepFrames = 0; // lockstep: the frames ended at the last present
        int swapRate = 0; // the frame rate the swap interval was set for
        Clock::time_point nextPresent{};
        // The cursor over the game hides after two seconds without moving (shown again by any motion or button, and
        // while the settings window is open). PS2X_KEEP_CURSOR=1 leaves it shown.
        static const bool keepCursor = [] { const char *v = std::getenv("PS2X_KEEP_CURSOR"); return v && *v == '1'; }();
        Clock::time_point cursorMoved = start;
        bool cursorHidden = false;
        uint32_t windowActions = 0; // the window's actions held at the last pad update (input::windowActions)
        while (!quit)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_WHEEL)
                    cursorMoved = Clock::now();
                panel.handleEvent(event);
                if (videoSettings)
                    video::handleEvent(event);
                if (pad)
                    pad->handle(event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    quit = true;
                else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
                {
                    if (event.key.scancode == SDL_SCANCODE_F5 && game.saveState)
                        saveState(stateSlot());
                    else if (event.key.scancode == SDL_SCANCODE_F7 && game.loadState)
                        game.loadState(stateSlot());
                    else if (event.key.scancode == SDL_SCANCODE_F6 && game.saveState)
                    {
                        setStateSlot(stateSlot() + ((event.key.mod & SDL_KMOD_SHIFT) ? -1 : 1));
                        notify(stateSlotSummary(stateSlot()));
                    }
                    else if (event.key.scancode == SDL_SCANCODE_F12)
                        screenshot = true;
                    else if (event.key.scancode == SDL_SCANCODE_F8 && sequenceLeft == 0)
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
            // PS2X_VIRTUAL_PAD=<s>:<button>[,...] (scripted checks of what a gamepad does to the window: its bindings, the
            // settings window's gamepad navigation): a virtual SDL gamepad, attached at the start, presses each button
            // (SDL's names: guide, a, b, dpdown, touchpad, ...; or input.ini's: MIDDLE, RIGHT_FACE_LEFT, ...) for 0.15 s
            // at that second from the start. A hidden window has no focus: with PS2X_INPUT_BACKGROUND=1 the devices are
            // read anyway.
            {
                struct Press
                {
                    double at;
                    SDL_GamepadButton button;
                    int stage; // 0 waiting, 1 down, 2 done
                };
                static std::vector<Press> s_presses;
                static SDL_Joystick *s_virtual = nullptr;
                static const bool s_wanted = [&]
                {
                    const char *text = std::getenv("PS2X_VIRTUAL_PAD");
                    for (std::string rest = text ? text : ""; !rest.empty();)
                    {
                        const size_t comma = rest.find(',');
                        const std::string step = rest.substr(0, comma);
                        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                        const size_t colon = step.find(':');
                        const std::string name = colon == std::string::npos ? std::string() : step.substr(colon + 1);
                        SDL_GamepadButton button = name.empty() ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(name.c_str());
                        input::Binding binding; // or input.ini's name (RIGHT_FACE_LEFT, MIDDLE, ...)
                        if (button == SDL_GAMEPAD_BUTTON_INVALID && !name.empty() && input::parseBinding("pad:" + name, binding) &&
                            binding.source == input::Source::GamepadButton && binding.code < SDL_GAMEPAD_BUTTON_COUNT)
                            button = static_cast<SDL_GamepadButton>(binding.code);
                        if (button != SDL_GAMEPAD_BUTTON_INVALID)
                            s_presses.push_back({std::atof(step.c_str()), button, 0});
                        else
                            std::fprintf(stderr, "[window] PS2X_VIRTUAL_PAD: no button in '%s'\n", step.c_str());
                    }
                    if (s_presses.empty())
                        return false;
                    SDL_VirtualJoystickDesc desc;
                    SDL_INIT_INTERFACE(&desc);
                    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
                    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    desc.name = "Virtual gamepad (PS2X_VIRTUAL_PAD)";
                    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
                    s_virtual = id ? SDL_OpenJoystick(id) : nullptr;
                    std::fprintf(stderr, "[window] virtual gamepad %s\n", s_virtual ? "attached" : SDL_GetError());
                    return s_virtual != nullptr;
                }();
                if (s_wanted)
                {
                    const double at = seconds(start, Clock::now());
                    for (Press &p : s_presses)
                        if (p.stage == 0 && at >= p.at)
                        {
                            SDL_SetJoystickVirtualButton(s_virtual, p.button, true);
                            p.stage = 1;
                            std::fprintf(stderr, "[window] virtual gamepad: %s\n", SDL_GetGamepadStringForButton(p.button));
                        }
                        else if (p.stage == 1 && at >= p.at + 0.15)
                        {
                            SDL_SetJoystickVirtualButton(s_virtual, p.button, false);
                            p.stage = 2;
                        }
                }
            }
            if (!keepCursor && !options.hidden)
            {
                const bool hide = !panel.settingsOpen() && Clock::now() - cursorMoved > std::chrono::seconds(2) &&
                                  (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS);
                if (hide != cursorHidden)
                {
                    hide ? SDL_HideCursor() : SDL_ShowCursor();
                    cursorHidden = hide;
                }
            }
            // The player's "Pause in the background": the game holds at its frame end while the window has no focus
            // (or is minimised); the window keeps presenting the last picture.
            {
                const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
                const bool away = !(flags & SDL_WINDOW_INPUT_FOCUS) || (flags & SDL_WINDOW_MINIMIZED);
                const bool hold = videoSettings && video::current().pauseInBackground && away && !quit;
                if (FramePacer::backgroundHold().exchange(hold, std::memory_order_acq_rel) != hold)
                    std::fprintf(stderr, "[window] %s\n", hold ? "in the background: the game holds" : "the game goes on");
            }
            // Fast forward: while a player's FastForward binding is held (input.ini fast_forward, the Controls tab; player
            // 1's ` key, left of 1, by default; the pad's last update: focused, not while the panel takes the keyboard),
            // the game runs at the player's speed (video::Settings::fastForward; FramePacer::speedPercent).
            // PS2X_FAST_FORWARD_AT=<from>-<to>[,...]: held between those seconds from the start (scripted checks).
            {
                static const std::vector<std::pair<double, double>> s_spans = []
                {
                    std::vector<std::pair<double, double>> spans;
                    const char *text = std::getenv("PS2X_FAST_FORWARD_AT");
                    for (std::string rest = text ? text : ""; !rest.empty();)
                    {
                        const size_t comma = rest.find(',');
                        double from = 0.0, to = 0.0;
                        if (std::sscanf(rest.substr(0, comma).c_str(), "%lf-%lf", &from, &to) == 2 && to > from)
                            spans.emplace_back(from, to);
                        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                    }
                    return spans;
                }();
                bool held = (windowActions & (1u << (input::FastForward - input::kFirstWindowAction))) != 0;
                const double at = seconds(start, Clock::now());
                for (const auto &[from, to] : s_spans)
                    held = held || (at >= from && at < to);
                // Without the player's video settings (a scripted run): PS2X_FAST_FORWARD alone, 3x by default.
                static const int s_fallback = []
                {
                    int percent = 300;
                    if (const char *fast = std::getenv("PS2X_FAST_FORWARD"); fast && *fast)
                        video::parseFastForward(fast, percent);
                    return percent;
                }();
                const uint32_t speed = held ? static_cast<uint32_t>(videoSettings ? video::current().fastForward : s_fallback) : 100u;
                if (FramePacer::speedPercent().exchange(speed, std::memory_order_relaxed) != speed)
                    std::fprintf(stderr, "[window] fast forward %s\n", held ? video::fastForwardName(static_cast<int>(speed)).c_str() : "off");
            }
            if (pad)
            {
                pad->setHeld(panel.holdsKeyboard(), panel.holdsMouse(), panel.holdsGamepads());
                pad->update((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
                // The window's actions bound like the pad's buttons (input.ini, the Controls tab): the settings window,
                // save and load a state at their press (F2, F5, F7's); fast forward while held (above, the next loop).
                const uint32_t now = pad->windowActions();
                const uint32_t pressed = now & ~windowActions;
                windowActions = now;
                const auto bit = [](int action) { return 1u << (action - input::kFirstWindowAction); };
                if (pressed & bit(input::SettingsWindow))
                    panel.toggleSettings();
                if ((pressed & bit(input::SaveState)) && game.saveState)
                    saveState(stateSlot());
                if ((pressed & bit(input::LoadState)) && game.loadState)
                    game.loadState(stateSlot());
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
            {
                isFresh = renderer.waitForFrame(seen, 1.0 / 30.0);
                if (options.lockstep)
                {
                    if (!isFresh)
                        continue; // lockstep: no frame presented twice
                    // A frame publishes its picture before it counts as ended: the present waits for that end (a second
                    // at most), so its frame count (the captures' names) is the picture's own frame.
                    const auto limit = Clock::now() + std::chrono::seconds(1);
                    while (game.framesEnded() == lockstepFrames && Clock::now() < limit && !thread.done.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    lockstepFrames = game.framesEnded();
                }
            }
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
            // The frames ended before this present: its picture is at least the last of them (a frame publishes its
            // picture before it counts as ended). The captures by frame use this count, read before the present.
            const uint64_t framesBefore = game.framesEnded();
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            pw = std::max(pw, 1);
            ph = std::max(ph, 1);
            if (offscreen && (offFbo == 0 || offWidth != pw || offHeight != ph))
            {
                if (offFbo == 0)
                {
                    g.GenFramebuffers(1, &offFbo);
                    g.GenTextures(1, &offColour);
                    g.GenRenderbuffers(1, &offDepth);
                }
                g.BindTexture(render::gl::kTexture2D, offColour);
                g.TexImage2D(render::gl::kTexture2D, 0, static_cast<render::gl::GLint>(render::gl::kRgba8), pw, ph, 0, render::gl::kRgba,
                             render::gl::kUnsignedByte, nullptr);
                g.BindRenderbuffer(render::gl::kRenderbuffer, offDepth);
                g.RenderbufferStorage(render::gl::kRenderbuffer, render::gl::kDepth24Stencil8, pw, ph);
                g.BindFramebuffer(render::gl::kFramebuffer, offFbo);
                g.FramebufferTexture2D(render::gl::kFramebuffer, render::gl::kColorAttachment0, render::gl::kTexture2D, offColour, 0);
                g.FramebufferRenderbuffer(render::gl::kFramebuffer, render::gl::kDepthStencilAttachment, render::gl::kRenderbuffer, offDepth);
                offWidth = pw;
                offHeight = ph;
            }
            g.BindFramebuffer(render::gl::kFramebuffer, offscreen ? offFbo : 0);
            g.Viewport(0, 0, pw, ph);
            g.Disable(render::gl::kScissorTest);
            g.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            g.Clear(render::gl::kColorBufferBit | render::gl::kDepthBufferBit);
            // The picture letterboxed to its shape, at the exact (fractional) place and size, as the fork's window put it:
            // in a 640x448 window its 4:3 picture covers columns 21-618 (x 21.33, width 597.33), not 22-618 as a rounded
            // one did (milestone M5's pixel comparison).
            float rw = static_cast<float>(pw), rh = static_cast<float>(ph);
            if (rw > rh * options.aspect)
                rw = rh * options.aspect;
            else
                rh = rw / options.aspect;
            renderer.present((static_cast<float>(pw) - rw) * 0.5f, (static_cast<float>(ph) - rh) * 0.5f, rw, rh);
            ++presents;
            ++ratesPresents;
            if (options.lockstep && options.onPresented) // (interpolating: reported too, so a holding host never stalls)
                options.onPresented(seen);

            const uint64_t frames = game.framesEnded();
            const double elapsed = seconds(start, now);
            const unsigned long long ms = static_cast<unsigned long long>(elapsed * 1000.0);
            // The game's picture alone (before the panel).
            bool captureFrame = false;
            while (nextCapture < captureFrames.size() && framesBefore >= captureFrames[nextCapture])
            {
                captureFrame = true;
                ++nextCapture;
            }
            if (captureFrame)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(framesBefore));
                writer.capture(captureDir + "/" + name, pw, ph);
                if (options.onCaptured)
                    options.onCaptured(framesBefore);
            }
            while (nextScreenshotAt < screenshotAt.size() && elapsed >= screenshotAt[nextScreenshotAt])
            {
                ++nextScreenshotAt;
                screenshot = true;
            }
            while (nextStateSaveAt < stateSaveAt.size() && elapsed >= stateSaveAt[nextStateSaveAt])
            {
                ++nextStateSaveAt;
                if (game.saveState)
                    saveState(stateSlot());
            }
            if (const int slot = g_pictureSlot.exchange(-1, std::memory_order_relaxed); slot >= 0)
                writer.capture(stateSlotPicturePath(slot), pw, ph, true, 320);
            if (screenshot)
            {
                screenshot = false;
                const std::string path = screenshotPath();
                writer.capture(path, pw, ph, true);
                notify("Screenshot: " + std::filesystem::path(path).filename().string());
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
            if (offscreen)
            {
                // The offscreen picture to the window (shown or not), then the panel goes on top as usual.
                g.BindFramebuffer(render::gl::kReadFramebuffer, offFbo);
                g.BindFramebuffer(render::gl::kDrawFramebuffer, 0);
                g.BlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph, render::gl::kColorBufferBit, render::gl::GLenum(0x2600) /* GL_NEAREST */);
                g.BindFramebuffer(render::gl::kFramebuffer, 0);
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
        FramePacer::backgroundHold().store(false, std::memory_order_release); // a held game must reach its stop
        FramePacer::speedPercent().store(100u, std::memory_order_relaxed);
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
