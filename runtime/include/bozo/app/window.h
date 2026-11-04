// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The real-time platform layer's window (phase 4, milestone M1): an SDL3 window with an OpenGL 3.3 core context; the
// game runs on a thread of its own (the free host in real time: bozo/host/free_host.h) while the window's thread shows
// each frame the renderer publishes (Renderer::present), letterboxed to the picture's shape, and reads the PC's devices
// (bozo/app/device_pad.h).
//
// Keys (milestone M4): F2 the settings window and F3 the frame rate counter (bozo/app/panel.h); F8 a capture sequence
// and F9 a burst of captures (window.cpp says where and how); F11 or Alt+Enter switches fullscreen (with the video
// settings: to the last full-screen mode, bozo/app/video_settings.h). Closing the window ends the game at its next frame.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bozo::app
{
    class DevicePad;

    struct WindowOptions
    {
        std::string title = "BozoEngine";
        int width = 1280, height = 960; // the window's size in its own units (the picture letterboxed inside)
        bool fullscreen = false;
        bool vsync = true;
        float aspect = 4.0f / 3.0f;          // the picture's shape
        float renderScale = 1.0f;            // the 3D's resolution against the picture's (Renderer::setRenderScale)
        std::string captureDir;              // where F8 and captureFrames write (empty: the current folder)
        std::vector<uint64_t> captureFrames; // the window saved at the first present after each of these game frames
        uint64_t quitAfterFrames = 0;        // close once the game ended this many frames (0: never)
        bool stats = true;                   // every 2 s on stderr: game frames and presents a second, frame times
        // The window's size, mode and render scale from video::edited() (the port called video::loadAtStartup and
        // applied its command line there) instead of width / height / fullscreen / renderScale.
        bool videoSettings = false;
        bool panel = true; // F2 / F3 (Dear ImGui)
    };

    // What the window runs and how it talks to it.
    struct Game
    {
        std::function<int()> run;                // the game thread's body: runs until the game ends (its exit code)
        std::function<void()> stop;              // any thread: asks the game to end
        std::function<uint64_t()> framesEnded;   // any thread: the game frames ended so far
    };

    // Opens the window, runs the game on its own thread (a 256 MB stack: recompiled code nests deeply), presents until
    // the window closes or the game ends, then ends the game and closes. Returns the game's exit code (3 when the
    // window or its context could not be made).
    int runWindow(const WindowOptions &options, const Game &game, DevicePad *pad);
}
