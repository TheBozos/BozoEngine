// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The window as the player sets it: its size, windowed / fullscreen / borderless, and the render scale. Moved from the
// team's ps2_video_settings (phase 4, milestone M4), raylib's window and monitor calls replaced by SDL3's. Loaded from
// video.ini in the user folder (bozo/app/app.h; PS2X_VIDEO_CONFIG names another file) before the window opens, edited
// in the Video tab of the settings window (F2) and saved back there; the environment still overrides the file
// (PS2X_WINDOW_SIZE, PS2X_WINDOW_MODE, PS2X_RENDER_SCALE, PS2X_FRAME_RATE, PS2X_TEXTURE_UPSCALE, PS2X_TEXTURE_FILTER). F11 or Alt+Enter switch between the
// window and the last full-screen mode. Whatever the window's shape, the picture keeps its 4:3 and is centred with
// bars (the present loop).
//
// The file is the fork's, line for line: a video.ini the fork wrote reads the same, and the fork reads ours.
// - gs_scale (the fork's GS renderer's internal scale) is kept and written back as read: BozoEngine has no GS.
// - frame_rate (game, monitor or a number): the window's frame rate, live (Renderer::setFrameRate: above the game's
//   60, the frames between the game's drawn interpolated; the window's loop presents at the monitor's refresh with
//   vsync, or that many a second without).
// - texture_upscale (off, 2x or 4x) and texture_filter (edge or smooth): the texture assets drawn from scaled copies
//   (Renderer::setTextureUpscale, live); off by default. Not the render scale: that one is the 3D's resolution.
//
// Everything here runs on the window's thread, except shared() and post() (any thread: a game's own menu).
#pragma once

#include "bozo/render/texture_upscale.h"

#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace bozo::app::video
{
    enum class WindowMode : int
    {
        Windowed,
        Fullscreen, // exclusive: the monitor switches to the chosen size
        Borderless, // a window without borders over the whole monitor, at the monitor's own size
    };

    struct Settings
    {
        int width = 0, height = 0; // the window's client area (the full-screen video mode's size)
        WindowMode mode = WindowMode::Windowed;
        float renderScale = 1.0f; // the renderer's 3D against the window's size (live)
        int gsScale = 1;          // the fork's GS renderer's scale: kept for the file, unused
        // The window's frame rate (live): 0 the game's 60 (the default), -1 the monitor's refresh with vsync, n that many
        // a second without vsync; above the game's, the renderer draws frames between the game's.
        int frameRate = 0;
        render::TextureUpscale textureUpscale; // texture assets drawn from copies 2 or 4 times as large (live); off by default
        // Texture replacements (bozo/render/texture_replace.h): a folder of <identity>.png files; "" the user folder's
        // texture_pack when it exists, "off" none.
        std::string texturePack;
    };

    // The texture replacements' folder a texture_pack setting names ("" when off or when there is none).
    std::string texturePackFolder(const std::string &setting);

    // frame_rate / PS2X_FRAME_RATE: "game", "monitor" or a number (false for anything else).
    bool parseFrameRate(const char *text, int &rate);
    std::string frameRateName(int rate);

    // Before the window opens: the file then the environment over the defaults (the port's window size). The port may
    // then change edited() (its command line) before the window opens.
    void loadAtStartup(int defaultWidth, int defaultHeight);
    // Once the window is open (windowed, at edited()'s size): the mode and render scale loaded.
    void applyAtStartup(SDL_Window *window);
    // Each event (window thread): F11 / Alt+Enter, a windowed resize kept as the size.
    void handleEvent(const SDL_Event &event);
    // Once a frame, before drawing it: requestApply()'s change, PS2X_VIDEO_SCRIPT's steps.
    void frameUpdate();
    // The window is going away.
    void detach();

    // What is applied now, and the settings window's copy of it until Apply.
    const Settings &current();
    Settings &edited();
    // Applies `edited()` to the window at once; requestApply() at the next frameUpdate (from inside a frame: the
    // settings window).
    void apply();
    void requestApply();
    bool save();
    std::string configPath();

    const char *modeName(WindowMode mode);
    // Any thread (a port's in-game menu, on the game's thread): what is applied now, with the sizes and refresh to
    // offer, copied by the window's thread each time it changes; `open` false until the window is open.
    struct Shared
    {
        Settings applied;
        std::vector<std::pair<int, int>> resolutions;
        std::pair<int, int> monitor{0, 0};
        int monitorRefresh = 0;
        bool open = false;
    };
    Shared shared();
    // Any thread: `settings` applied at the next frameUpdate as the Video tab's Apply (it becomes edited() too), and
    // written to video.ini when `save`. A later post before that frame replaces it.
    void post(const Settings &settings, bool save);

    // Sizes to offer: common 4:3 and 16:9 ones that fit the monitor, the monitor's own, and the current one.
    std::vector<std::pair<int, int>> resolutions();
    std::pair<int, int> monitorSize();
    int monitorRefresh(); // Hz, 0 unknown
    // The window's client area now (pixels).
    std::pair<int, int> windowSize();
}
