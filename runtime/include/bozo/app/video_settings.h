// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The window as the player sets it: its size, windowed / fullscreen / borderless, and the render scale. Moved from the
// team's ps2_video_settings (phase 4, milestone M4), raylib's window and monitor calls replaced by SDL3's. Loaded from
// video.ini in the user folder (bozo/app/app.h; PS2X_VIDEO_CONFIG names another file) before the window opens, edited
// in the Video tab of the settings window (F2) and saved back there; the environment still overrides the file
// (PS2X_WINDOW_SIZE, PS2X_WINDOW_MODE, PS2X_RENDER_SCALE, PS2X_FRAME_RATE). F11 or Alt+Enter switch between the
// window and the last full-screen mode. Whatever the window's shape, the picture keeps its 4:3 and is centred with
// bars (the present loop).
//
// The file is the fork's, line for line: a video.ini the fork wrote reads the same, and the fork reads ours.
// - gs_scale (the fork's GS renderer's internal scale) is kept and written back as read: BozoEngine has no GS.
// - frame_rate (game, monitor or a number): the window's frame rate, live (Renderer::setFrameRate: above the game's
//   60, the frames between the game's drawn interpolated; the window's loop presents at the monitor's refresh with
//   vsync, or that many a second without).
//
// Everything here runs on the window's thread.
#pragma once

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
    };

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
    // Sizes to offer: common 4:3 and 16:9 ones that fit the monitor, the monitor's own, and the current one.
    std::vector<std::pair<int, int>> resolutions();
    std::pair<int, int> monitorSize();
    int monitorRefresh(); // Hz, 0 unknown
    // The window's client area now (pixels).
    std::pair<int, int> windowSize();
}
