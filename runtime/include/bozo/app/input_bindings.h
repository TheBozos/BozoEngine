// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// What drives each PS2 pad button and stick direction on the PC: keys, gamepad buttons and axes, mouse buttons, the
// wheel and mouse movement, any number per action, per player. Moved from the team's ps2_input_bindings (phase 4,
// milestone M2), raylib's key and gamepad codes replaced by SDL3's (scancodes, SDL_GamepadButton / SDL_GamepadAxis);
// the names in input.ini are unchanged, so a file the fork wrote reads the same (its numeric codes, raylib's, are
// converted), and the fork reads this one (it skips the [player3] / [player4] sections it does not know).
//
// Loaded from input.ini in the user folder (bozo/app/app.h; PS2X_INPUT_CONFIG overrides the path, no user folder: the
// current one). Without a file the defaults are the fork's: the keyboard and its gamepad for player 1, a gamepad each
// for players 2-4. The window's thread evaluates the bindings (DevicePad::update) on the devices it sampled.
#pragma once

#include "bozo/libs/pad.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Gamepad;
union SDL_Event;

namespace bozo::app::input
{
    constexpr int kPlayers = 4;

    // The PS2 side: the 16 buttons, then the two sticks' four directions.
    enum Action : int
    {
        Up, Down, Left, Right, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Start, Select,
        LeftStickLeft, LeftStickRight, LeftStickUp, LeftStickDown,
        RightStickLeft, RightStickRight, RightStickUp, RightStickDown,
        ActionCount
    };

    // The PC side.
    enum class Source : uint8_t
    {
        Key,           // code: SDL_Scancode
        GamepadButton, // code: SDL_GamepadButton, or kLeftTriggerButton / kRightTriggerButton
        GamepadAxis,   // code: SDL_GamepadAxis, direction: +1 / -1
        MouseButton,   // code: SDL's mouse button (SDL_BUTTON_LEFT ...)
        MouseWheel,    // direction: +1 up / -1 down
        MouseMove,     // code: 0 x, 1 y; direction: +1 / -1 (right / down)
    };

    // The triggers as buttons (input.ini's pad:LEFT_TRIGGER_2 / RIGHT_TRIGGER_2): how far they are pulled.
    constexpr int kLeftTriggerButton = 100, kRightTriggerButton = 101;

    struct Binding
    {
        Source source = Source::Key;
        int code = 0;
        int direction = 1;
        bool operator==(const Binding &other) const
        {
            return source == other.source && code == other.code && direction == other.direction;
        }
    };

    struct Profile
    {
        std::vector<Binding> actions[ActionCount];
        int gamepad = -1; // which gamepad (0-3, in the order they were plugged in) this player reads; -1: the player's
                          // among the connected ones (player n the nth)
    };

    struct MouseSettings
    {
        float sensitivity = 1.0f; // stick deflection per (pixels per frame / 10)
        bool invertY = false;
        bool capture = false;     // hide and hold the cursor while playing
    };

    struct Config
    {
        Profile players[kPlayers];
        MouseSettings mouse;
    };

    // The current configuration (loaded on first use). Changes apply at once (hold configMutex() while changing it,
    // the window's thread reads it); save() writes them.
    Config &config();
    std::mutex &configMutex();
    void resetToDefaults();
    bool save();
    std::string configPath();

    // The devices as the window's thread sampled them for one evaluation.
    struct Devices
    {
        const bool *keys = nullptr; // SDL_GetKeyboardState
        int keyCount = 0;
        uint32_t mouseButtons = 0;  // SDL_MouseButtonFlags
        float mouseX = 0.0f, mouseY = 0.0f; // the mouse's speed as stick deflection, -1..1
        bool wheelUp = false, wheelDown = false;
    };

    // A player's pad from its bindings: the buttons (pressed past half way), the sticks (0..255, 128 the centre), the
    // pressures (how far each pressure button is pressed, 0..255). `gamepad`: the player's gamepad (null: none).
    libs::PadInputState padState(int player, const Devices &devices, SDL_Gamepad *gamepad);

    // Names (input.ini, a Controls window).
    const char *actionName(int action);   // "cross", "left_stick_up", ...
    const char *actionLabel(int action);  // "Cross", "Left stick up", ...
    std::string bindingName(const Binding &binding); // "key:SPACE", "pad:RIGHT_FACE_DOWN", "mouse:LEFT", ...
    bool parseBinding(const std::string &text, Binding &binding);

    // A Controls window's capture: the binding an event is (a key, mouse button or gamepad button pressed, the wheel
    // turned, the mouse moved far, a gamepad axis pushed past 60%), if it is one.
    bool bindingFromEvent(const SDL_Event &event, Binding &binding);
}
