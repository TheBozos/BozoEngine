// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// video.ini and the window's modes (bozo/app/video_settings.h). Moved from the team's ps2_video_settings.cpp (phase
// 4, milestone M4): the file's format, the environment, the sizes offered and the F11 switch are the fork's; the
// window and monitor calls are SDL3's (SDL_SetWindowFullscreenMode with a display mode for exclusive full screen, with
// none for the borderless one, which SDL3 calls desktop full screen); the GS scale and the frame rate are kept for the
// file only (no GS); the frame rate goes to the renderer (frames between the game's, phase 4 M4b).
#include "bozo/app/video_settings.h"

#include "bozo/app/app.h"
#include "bozo/render/renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace bozo::app::video
{
    namespace
    {
        Settings g_current;
        Settings g_edited;
        SDL_Window *g_window = nullptr;
        WindowMode g_lastFull = WindowMode::Borderless; // where F11 / Alt+Enter go from the window
        bool g_applyPending = false;                    // requestApply(): at the next frameUpdate, between two frames

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        }

        bool parseMode(std::string_view text, WindowMode &mode)
        {
            if (text == "windowed")
                mode = WindowMode::Windowed;
            else if (text == "fullscreen")
                mode = WindowMode::Fullscreen;
            else if (text == "borderless")
                mode = WindowMode::Borderless;
            else
                return false;
            return true;
        }

        bool parseSize(const char *text, int &width, int &height)
        {
            int w = 0, h = 0;
            if (std::sscanf(text, "%dx%d", &w, &h) != 2 || w < 160 || h < 120)
                return false;
            width = w;
            height = h;
            return true;
        }

        void sanitize(Settings &settings)
        {
            if (settings.frameRate > 0)
                settings.frameRate = std::clamp(settings.frameRate, 30, 1000);
            else if (settings.frameRate < 0)
                settings.frameRate = -1;
            settings.renderScale = std::clamp(settings.renderScale, 0.25f, 4.0f);
            settings.gsScale = std::clamp(settings.gsScale, 1, 4);
        }

        void load(Settings &settings)
        {
            std::ifstream file(configPath());
            if (!file)
                return;
            std::string line;
            while (std::getline(file, line))
            {
                line = trim(line.substr(0, line.find('#')));
                const size_t equal = line.find('=');
                if (line.empty() || line.front() == '[' || equal == std::string::npos)
                    continue;
                const std::string name = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
                if (name == "size")
                    parseSize(value.c_str(), settings.width, settings.height);
                else if (name == "mode")
                    parseMode(value, settings.mode);
                else if (name == "render_scale")
                    settings.renderScale = static_cast<float>(std::atof(value.c_str()));
                else if (name == "gs_scale")
                    settings.gsScale = std::atoi(value.c_str());
                else if (name == "frame_rate")
                    parseFrameRate(value.c_str(), settings.frameRate);
            }
        }

        SDL_DisplayID display()
        {
            const SDL_DisplayID id = g_window ? SDL_GetDisplayForWindow(g_window) : 0;
            return id ? id : SDL_GetPrimaryDisplay();
        }

        void applyWindow(const Settings &from, const Settings &to)
        {
            if (!g_window)
                return;
            // Back to a plain window first (SDL restores the size and place it had).
            if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)
            {
                SDL_SetWindowFullscreen(g_window, false);
                SDL_SyncWindow(g_window);
            }
            switch (to.mode)
            {
            case WindowMode::Windowed:
            {
                int w = 0, h = 0;
                SDL_GetWindowSize(g_window, &w, &h);
                if (w != to.width || h != to.height)
                {
                    SDL_SetWindowSize(g_window, to.width, to.height);
                    if (from.mode == WindowMode::Windowed || from.width != to.width || from.height != to.height)
                        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display()), SDL_WINDOWPOS_CENTERED_DISPLAY(display()));
                }
                break;
            }
            case WindowMode::Fullscreen:
            {
                // The monitor's mode nearest the chosen size (its refresh as the desktop's when it has one).
                SDL_DisplayMode mode{};
                const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display());
                if (SDL_GetClosestFullscreenDisplayMode(display(), to.width, to.height, desktop ? desktop->refresh_rate : 0.0f, true, &mode))
                    SDL_SetWindowFullscreenMode(g_window, &mode);
                else
                    SDL_SetWindowFullscreenMode(g_window, nullptr);
                SDL_SetWindowFullscreen(g_window, true);
                break;
            }
            case WindowMode::Borderless:
                SDL_SetWindowFullscreenMode(g_window, nullptr); // desktop full screen: the monitor as it is
                SDL_SetWindowFullscreen(g_window, true);
                break;
            }
            SDL_SyncWindow(g_window);
            if (to.mode != WindowMode::Windowed)
                g_lastFull = to.mode;
        }
    }

    bool parseFrameRate(const char *text, int &rate)
    {
        const std::string_view value(text);
        if (value == "game" || value == "60" || value == "0")
            rate = 0;
        else if (value == "monitor")
            rate = -1;
        else if (const int n = std::atoi(text); n > 0)
            rate = n;
        else
            return false;
        return true;
    }

    std::string frameRateName(int rate) { return rate == 0 ? "game" : rate < 0 ? "monitor" : std::to_string(rate); }

    const char *modeName(WindowMode mode)
    {
        switch (mode)
        {
        case WindowMode::Fullscreen:
            return "fullscreen";
        case WindowMode::Borderless:
            return "borderless";
        default:
            return "windowed";
        }
    }

    std::string configPath()
    {
        if (const char *path = std::getenv("PS2X_VIDEO_CONFIG"); path && *path)
            return path;
        const std::filesystem::path &dir = userDirectory();
        return (dir.empty() ? std::filesystem::path("video.ini") : dir / "video.ini").string();
    }

    void loadAtStartup(int defaultWidth, int defaultHeight)
    {
        Settings settings;
        settings.width = defaultWidth;
        settings.height = defaultHeight;
        load(settings);
        // The environment over the file.
        if (const char *size = std::getenv("PS2X_WINDOW_SIZE"); size && *size)
            parseSize(size, settings.width, settings.height);
        if (const char *mode = std::getenv("PS2X_WINDOW_MODE"); mode && *mode)
            parseMode(mode, settings.mode);
        if (const char *scale = std::getenv("PS2X_RENDER_SCALE"); scale && *scale)
            settings.renderScale = static_cast<float>(std::atof(scale));
        if (const char *rate = std::getenv("PS2X_FRAME_RATE"); rate && *rate)
            parseFrameRate(rate, settings.frameRate);
        sanitize(settings);
        g_current = settings;
        g_current.mode = WindowMode::Windowed; // the window opens as a window; applyAtStartup goes on from there
        g_edited = settings;
    }

    void applyAtStartup(SDL_Window *window)
    {
        g_window = window;
        sanitize(g_edited);
        Settings target = g_edited;
        if (target.mode != WindowMode::Windowed)
            applyWindow(g_current, target);
        render::Renderer::instance().setRenderScale(target.renderScale);
        render::Renderer::instance().setFrameRate(target.frameRate);
        g_current = target;
        std::fprintf(stderr, "[video] %dx%d %s, render scale %.2f, frame rate %s (%s)\n", g_current.width, g_current.height,
                     modeName(g_current.mode), g_current.renderScale, frameRateName(g_current.frameRate).c_str(), configPath().c_str());
    }

    void detach() { g_window = nullptr; }

    void handleEvent(const SDL_Event &event)
    {
        if (!g_window)
            return;
        // A windowed resize by hand is the window's size from then on (saved with the rest).
        if (event.type == SDL_EVENT_WINDOW_RESIZED && g_current.mode == WindowMode::Windowed &&
            !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN))
        {
            g_current.width = g_edited.width = event.window.data1;
            g_current.height = g_edited.height = event.window.data2;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.scancode == SDL_SCANCODE_F11 || (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
        {
            g_edited = g_current;
            g_edited.mode = g_current.mode == WindowMode::Windowed ? g_lastFull : WindowMode::Windowed;
            g_applyPending = true;
        }
    }

    void frameUpdate()
    {
        // PS2X_VIDEO_SCRIPT=<s>:<w>x<h>:<mode>[:<render scale>],...: changes applied at those seconds from the first
        // frame, as the Video tab would (unattended checks of the mode switches).
        static std::vector<std::pair<double, Settings>> s_script = []
        {
            std::vector<std::pair<double, Settings>> steps;
            const char *text = std::getenv("PS2X_VIDEO_SCRIPT");
            for (std::string rest = text ? text : ""; !rest.empty();)
            {
                const size_t comma = rest.find(',');
                const std::string step = rest.substr(0, comma);
                rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                double at = 0.0;
                int w = 0, h = 0;
                char mode[16] = {};
                float scale = 0.0f;
                if (std::sscanf(step.c_str(), "%lf:%dx%d:%15[a-z]:%f", &at, &w, &h, mode, &scale) < 4)
                    continue;
                Settings settings;
                settings.width = w;
                settings.height = h;
                settings.renderScale = scale;
                if (parseMode(mode, settings.mode))
                    steps.emplace_back(at, settings);
            }
            return steps;
        }();
        static const uint64_t s_start = SDL_GetTicks();
        while (!s_script.empty() && static_cast<double>(SDL_GetTicks() - s_start) / 1000.0 >= s_script.front().first)
        {
            const Settings &step = s_script.front().second;
            g_edited = g_current;
            g_edited.width = step.width;
            g_edited.height = step.height;
            g_edited.mode = step.mode;
            if (step.renderScale > 0.0f)
                g_edited.renderScale = step.renderScale;
            s_script.erase(s_script.begin());
            g_applyPending = true;
        }
        if (g_applyPending)
        {
            g_applyPending = false;
            apply();
        }
    }

    void requestApply() { g_applyPending = true; }

    const Settings &current() { return g_current; }
    Settings &edited() { return g_edited; }

    void apply()
    {
        sanitize(g_edited);
        if (g_edited.mode != g_current.mode || g_edited.width != g_current.width || g_edited.height != g_current.height)
            applyWindow(g_current, g_edited);
        render::Renderer::instance().setRenderScale(g_edited.renderScale);
        render::Renderer::instance().setFrameRate(g_edited.frameRate);
        g_current = g_edited;
        const auto [w, h] = windowSize();
        std::fprintf(stderr, "[video] applied %dx%d %s, render scale %.2f, frame rate %s (window %dx%d)\n", g_current.width,
                     g_current.height, modeName(g_current.mode), g_current.renderScale, frameRateName(g_current.frameRate).c_str(), w, h);
    }

    bool save()
    {
        std::ofstream file(configPath(), std::ios::trunc);
        if (!file)
            return false;
        const Settings &s = g_edited;
        char scale[32];
        std::snprintf(scale, sizeof(scale), "%g", s.renderScale);
        file << "# The window (the Video tab of the settings window, F2, writes this file; PS2X_WINDOW_SIZE, PS2X_WINDOW_MODE,\n"
                "# PS2X_RENDER_SCALE and PS2X_FRAME_RATE override it). size: the window's, or the full-screen video mode's.\n"
                "# mode: windowed, fullscreen (exclusive) or borderless (the monitor's size). render_scale: the 3D drawn at\n"
                "# that many times the window's size each way (0.25 to 4). gs_scale: the PS2Recomp build's GS renderer scale\n"
                "# (1 to 4; BozoEngine has no GS and keeps it as it is). frame_rate: game (60, each game frame shown once),\n"
                "# monitor (the monitor's refresh with vsync, the frames between the game's drawn interpolated, one game frame\n"
                "# later) or a number (that many a second, interpolated, without vsync).\n"
                "\n[video]\n"
             << "size = " << s.width << "x" << s.height << "\n"
             << "mode = " << modeName(s.mode) << "\n"
             << "render_scale = " << scale << "\n"
             << "gs_scale = " << s.gsScale << "\n"
             << "frame_rate = " << frameRateName(s.frameRate) << "\n";
        return static_cast<bool>(file);
    }

    std::pair<int, int> monitorSize()
    {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display());
        return mode ? std::pair<int, int>{mode->w, mode->h} : std::pair<int, int>{0, 0};
    }

    int monitorRefresh()
    {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display());
        return mode ? static_cast<int>(std::lround(mode->refresh_rate)) : 0;
    }

    std::pair<int, int> windowSize()
    {
        int w = 0, h = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
        return {w, h};
    }

    std::vector<std::pair<int, int>> resolutions()
    {
        static const std::pair<int, int> kCommon[] = {
            {640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2880, 2160}, // 4:3
            {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},           // 16:9
        };
        const auto [monitorWidth, monitorHeight] = monitorSize();
        std::vector<std::pair<int, int>> sizes;
        for (const auto &size : kCommon)
            if (monitorWidth <= 0 || (size.first <= monitorWidth && size.second <= monitorHeight))
                sizes.push_back(size);
        if (monitorWidth > 0)
            sizes.emplace_back(monitorWidth, monitorHeight);
        sizes.emplace_back(g_current.width, g_current.height);
        sizes.emplace_back(g_edited.width, g_edited.height);
        std::sort(sizes.begin(), sizes.end());
        sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
        return sizes;
    }
}
