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
        // Called on the window's thread once a captureFrames picture is read back (frames: the game frames ended when
        // that present began, so its picture is at least that frame's). A host that holds its game at a capture frame
        // until then (tmb-check --window: the pinned run) gets exactly that frame's picture.
        std::function<void(uint64_t frames)> onCaptured;
        // Lockstep (reproducible runs; at the game's own frame rate, not interpolating): each published frame is
        // presented exactly once, in order. The window presents only a frame it has not presented (no repeat after
        // its 1/30 s wait), and calls onPresented with Renderer::publishes() of the frame just presented. A host that
        // holds its game after each publish until then gets every frame presented, none twice: what the pictures keep
        // from present to present (Material::kPreviousFrame, the render targets: TTA's Nazgul Scream ghosting)
        // then follows the game's frames instead of the window's timing.
        bool lockstep = false;
        std::function<void(uint64_t publishes)> onPresented;
        uint64_t quitAfterFrames = 0;       // close once the game ended this many frames (0: never)
        bool stats = true;                   // every 2 s on stderr: game frames and presents a second, frame times
        // The window's size, mode and render scale from video::edited() (the port called video::loadAtStartup and
        // applied its command line there) instead of width / height / fullscreen / renderScale.
        bool videoSettings = false;
        bool panel = true; // F2 / F3 (Dear ImGui)
        // A test run that shows nothing: the window is never shown (the game still renders into the renderer's target),
        // and captures read that target (captureScene: the renderer's picture, its size, no overlays; also
        // PS2X_CAPTURE_SCENE=1), since a hidden or covered window's back buffer reads back black.
        bool hidden = false;
        bool captureScene = false;
    };

    // What the window runs and how it talks to it.
    struct Game
    {
        std::function<int()> run;                // the game thread's body: runs until the game ends (its exit code)
        std::function<void()> stop;              // any thread: asks the game to end
        std::function<uint64_t()> framesEnded;   // any thread: the game frames ended so far
        // Save states (bozo/state.h), from the window's thread, by slot (0-9): F6 / Shift+F6 picks the slot, F5 saves
        // it, F7 loads it (unset: no keys). A FreeHost port sets them to requestSave / requestLoad of stateSlotPath(slot).
        std::function<void(int slot)> saveState, loadState;
    };

    // A save state slot's file: the user folder's states/slotN.state (made when missing), or the current folder's
    // when there is no user folder.
    std::string stateSlotPath(int slot);
    constexpr int kStateSlots = 10;
    // "Slot 3: saved 2026-11-18 14:02" or "Slot 3: empty".
    std::string stateSlotSummary(int slot);
    // A short line shown over the game for a few seconds (any thread): a save state's result.
    void notify(const std::string &text);

    // Opens the window, runs the game on its own thread (a 256 MB stack: recompiled code nests deeply), presents until
    // the window closes or the game ends, then ends the game and closes. Returns the game's exit code (3 when the
    // window or its context could not be made).
    int runWindow(const WindowOptions &options, const Game &game, DevicePad *pad);
}
