// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// What the player sees over the game besides the picture (phase 4, milestone M4), drawn with Dear ImGui through its own
// SDL3 and OpenGL 3 backends on the window's thread:
// - F2: the settings window. Controls (every PS2 button and stick direction per player, rebound to keys, gamepad
//   buttons, sticks, triggers, mouse buttons, the wheel or mouse movement; input.ini), Video (window mode, resolution,
//   render scale; video.ini) and the port's own tabs (bozo/app/settings_tabs.h). PS2X_SETTINGS_OPEN=<tab> opens it on
//   that tab at launch.
// - F3: the frame rate counter at the top right (PS2X_SHOW_FPS=1 starts with it).
// Only player-facing parts: no debugger, no emulation views. ImGui's settings (window places) go to imgui.ini in the
// user folder.
#pragma once

#include <cstdint>

struct SDL_Window;
union SDL_Event;

namespace bozo::app
{
    class Panel
    {
    public:
        Panel();
        ~Panel();
        Panel(const Panel &) = delete;
        Panel &operator=(const Panel &) = delete;

        // The window and its OpenGL context current on this thread. False: no panel (the game still runs).
        bool init(SDL_Window *window, void *glContext);
        void shutdown();

        // Each event, first: F2 / F3, ImGui's input, a binding being captured.
        void handleEvent(const SDL_Event &event);
        // The overlay's numbers, once a present.
        void setRates(double gameFramesPerSecond, double presentsPerSecond, double frameMs);
        // Once a present, after the game's picture: the panel drawn over it (the current framebuffer, the window's).
        void draw();

        bool settingsOpen() const;
        bool overlayShown() const;
        // What the game's devices should not get now: the keyboard (a binding being captured, a text field typed in),
        // the mouse (the settings window is open: the cursor is the panel's).
        bool holdsKeyboard() const;
        bool holdsMouse() const;

        struct Impl;

    private:
        Impl *impl_;
    };
}
