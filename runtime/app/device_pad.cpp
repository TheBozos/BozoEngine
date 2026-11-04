// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The keyboard and gamepad as a DualShock 2 (bozo/app/device_pad.h).
#include "bozo/app/device_pad.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace bozo::app
{
    struct DevicePad::Gamepad
    {
        SDL_Gamepad *pad = nullptr;
        SDL_JoystickID id = 0;
    };

    namespace
    {
        struct KeyBinding
        {
            SDL_Scancode a, b;
            uint16_t button;
        };
        constexpr KeyBinding kKeys[] = {
            {SDL_SCANCODE_UP, SDL_SCANCODE_W, libs::kPadUp},           {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, libs::kPadDown},
            {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, libs::kPadLeft},       {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, libs::kPadRight},
            {SDL_SCANCODE_X, SDL_SCANCODE_SPACE, libs::kPadCross},     {SDL_SCANCODE_C, SDL_SCANCODE_ESCAPE, libs::kPadCircle},
            {SDL_SCANCODE_Z, SDL_SCANCODE_KP_0, libs::kPadSquare},     {SDL_SCANCODE_V, SDL_SCANCODE_KP_1, libs::kPadTriangle},
            {SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN, libs::kPadL1},      {SDL_SCANCODE_E, SDL_SCANCODE_UNKNOWN, libs::kPadR1},
            {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UNKNOWN, libs::kPadL2}, {SDL_SCANCODE_RSHIFT, SDL_SCANCODE_UNKNOWN, libs::kPadR2},
            {SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, libs::kPadStart}, {SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, libs::kPadSelect},
            {SDL_SCANCODE_F, SDL_SCANCODE_UNKNOWN, libs::kPadL3},      {SDL_SCANCODE_G, SDL_SCANCODE_UNKNOWN, libs::kPadR3},
        };
        struct PadBinding
        {
            SDL_GamepadButton button;
            uint16_t ps2;
        };
        constexpr PadBinding kPadButtons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, libs::kPadCross},         {SDL_GAMEPAD_BUTTON_EAST, libs::kPadCircle},
            {SDL_GAMEPAD_BUTTON_WEST, libs::kPadSquare},         {SDL_GAMEPAD_BUTTON_NORTH, libs::kPadTriangle},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, libs::kPadL1},    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, libs::kPadR1},
            {SDL_GAMEPAD_BUTTON_START, libs::kPadStart},         {SDL_GAMEPAD_BUTTON_BACK, libs::kPadSelect},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, libs::kPadL3},       {SDL_GAMEPAD_BUTTON_RIGHT_STICK, libs::kPadR3},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, libs::kPadUp},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, libs::kPadDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, libs::kPadLeft},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, libs::kPadRight},
        };

        uint8_t stickByte(int16_t axis) { return static_cast<uint8_t>(std::clamp((static_cast<int>(axis) + 32768) >> 8, 0, 255)); }
    }

    DevicePad::DevicePad() : gamepad_(new Gamepad) {}

    DevicePad::~DevicePad()
    {
        if (gamepad_->pad)
            SDL_CloseGamepad(gamepad_->pad);
        delete gamepad_;
    }

    void DevicePad::handle(const SDL_Event &event)
    {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad_->pad)
        {
            gamepad_->pad = SDL_OpenGamepad(event.gdevice.which);
            gamepad_->id = event.gdevice.which;
        }
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad_->pad && event.gdevice.which == gamepad_->id)
        {
            SDL_CloseGamepad(gamepad_->pad);
            gamepad_->pad = nullptr;
        }
    }

    void DevicePad::update(bool focused)
    {
        uint16_t buttons = 0;
        int lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
        if (focused)
        {
            const bool *keys = SDL_GetKeyboardState(nullptr);
            for (const KeyBinding &k : kKeys)
                if (keys[k.a] || (k.b != SDL_SCANCODE_UNKNOWN && keys[k.b]))
                    buttons |= k.button;
            // The D-pad keys also move the left stick to its end; I J K L the right stick.
            lx = buttons & libs::kPadLeft ? 0x00 : buttons & libs::kPadRight ? 0xff : 0x80;
            ly = buttons & libs::kPadUp ? 0x00 : buttons & libs::kPadDown ? 0xff : 0x80;
            rx = keys[SDL_SCANCODE_J] ? 0x00 : keys[SDL_SCANCODE_L] ? 0xff : 0x80;
            ry = keys[SDL_SCANCODE_I] ? 0x00 : keys[SDL_SCANCODE_K] ? 0xff : 0x80;
        }
        if (SDL_Gamepad *pad = gamepad_->pad)
        {
            for (const PadBinding &b : kPadButtons)
                if (SDL_GetGamepadButton(pad, b.button))
                    buttons |= b.ps2;
            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000)
                buttons |= libs::kPadL2;
            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000)
                buttons |= libs::kPadR2;
            const auto stick = [&](SDL_GamepadAxis axis, int &value)
            {
                const int16_t v = SDL_GetGamepadAxis(pad, axis);
                if (std::abs(static_cast<int>(v)) > 6000) // a resting stick's drift stays centred
                    value = stickByte(v);
            };
            stick(SDL_GAMEPAD_AXIS_LEFTX, lx);
            stick(SDL_GAMEPAD_AXIS_LEFTY, ly);
            stick(SDL_GAMEPAD_AXIS_RIGHTX, rx);
            stick(SDL_GAMEPAD_AXIS_RIGHTY, ry);
        }
        state_.store(static_cast<uint64_t>(buttons) | static_cast<uint64_t>(lx) << 16 | static_cast<uint64_t>(ly) << 24 |
                         static_cast<uint64_t>(rx) << 32 | static_cast<uint64_t>(ry) << 40,
                     std::memory_order_relaxed);
    }

    void DevicePad::read(uint32_t, int port, int slot, libs::PadInputState &out)
    {
        out = libs::PadInputState{};
        out.connected = slot == 0 && (port == 0 || port == 1); // port 2: connected, idle
        if (port != 0 || slot != 0)
            return;
        const uint64_t s = state_.load(std::memory_order_relaxed);
        out.buttons = static_cast<uint16_t>(s);
        out.lx = static_cast<uint8_t>(s >> 16);
        out.ly = static_cast<uint8_t>(s >> 24);
        out.rx = static_cast<uint8_t>(s >> 32);
        out.ry = static_cast<uint8_t>(s >> 40);
        // Pressure in the pad data's order: right, left, up, down, triangle, circle, cross, square, L1, L2, R1, R2.
        constexpr uint16_t kOrder[12] = {libs::kPadRight, libs::kPadLeft,  libs::kPadUp,    libs::kPadDown,
                                         libs::kPadTriangle, libs::kPadCircle, libs::kPadCross, libs::kPadSquare,
                                         libs::kPadL1,    libs::kPadL2,    libs::kPadR1,    libs::kPadR2};
        for (int i = 0; i < 12; ++i)
            out.pressure[i] = out.buttons & kOrder[i] ? 0xff : 0x00;
    }
}
