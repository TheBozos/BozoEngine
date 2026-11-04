// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The keyboard, mouse and gamepads as the players' DualShock 2s (bozo/app/device_pad.h). Written for BozoEngine on
// SDL3; the gamepad order, the players' gamepads and the mouse's handling follow the team's fork (ps2_pad.cpp's
// gamepad discovery, ps2_input_bindings.cpp's frameUpdate).
#include "bozo/app/device_pad.h"

#include "bozo/app/app.h"
#include "bozo/app/input_bindings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bozo::app
{
    namespace
    {
        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && v[0] == '1';
        }
    }

    struct DevicePad::Impl
    {
        static constexpr int kPlaces = 4;
        // The gamepads in the order they took their places (a pulled-out one leaves its place empty).
        std::array<SDL_Gamepad *, kPlaces> gamepads{};
        std::array<SDL_JoystickID, kPlaces> ids{};
        bool mappingsLoaded = false;
        bool listChanged = true;

        // The mouse: motion since the last update, smoothed speed, the wheel's last notch.
        float dx = 0.0f, dy = 0.0f, speedX = 0.0f, speedY = 0.0f;
        uint64_t wheelUpUntil = 0, wheelDownUntil = 0; // SDL_GetTicks
        bool captured = false;

        // What the game reads (game thread), written each update (window thread).
        std::mutex mutex;
        std::array<libs::PadInputState, input::kPlayers> states{};

        // The rumble the game asks for, per player: small << 8 | large; what was sent, and when.
        std::array<std::atomic<uint32_t>, input::kPlayers> wanted{};
        std::array<uint32_t, input::kPlayers> sent{};
        std::array<uint64_t, input::kPlayers> sentAt{};
        std::array<SDL_Gamepad *, input::kPlayers> rumbling{};
        bool rumbleOn = true;
        bool background = false;

        // The gamepad of a player: the one input.ini names, else the player's among the connected ones (player n the
        // nth, as the fork gave port n the nth raylib gamepad).
        SDL_Gamepad *gamepadOf(int player, int chosen) const
        {
            if (chosen >= 0 && chosen < kPlaces)
                return gamepads[static_cast<size_t>(chosen)];
            int n = 0;
            for (SDL_Gamepad *g : gamepads)
                if (g && n++ == player)
                    return g;
            return nullptr;
        }

        void loadMappings()
        {
            mappingsLoaded = true;
            std::vector<std::filesystem::path> candidates;
            if (const char *path = std::getenv("PS2X_GAMEPAD_DB"); path && *path)
                candidates.emplace_back(path);
            else
            {
                candidates.push_back(executableDirectory() / "gamecontrollerdb.txt");
                if (!userDirectory().empty())
                    candidates.push_back(userDirectory() / "gamecontrollerdb.txt");
            }
            std::error_code ec;
            for (const std::filesystem::path &path : candidates)
                if (std::filesystem::is_regular_file(path, ec))
                {
                    const int added = SDL_AddGamepadMappingsFromFile(path.string().c_str());
                    std::fprintf(stderr, "[pad] %d gamepad mappings loaded from %s\n", added, path.string().c_str());
                    return;
                }
        }

        void logList()
        {
            listChanged = false;
            int n = 0;
            for (int place = 0; place < kPlaces; ++place)
                if (SDL_Gamepad *g = gamepads[static_cast<size_t>(place)])
                {
                    const char *name = SDL_GetGamepadName(g);
                    std::fprintf(stderr, "[pad] gamepad %d: %s (player %d unless input.ini chooses)\n", place, name ? name : "?", ++n);
                }
            if (n == 0)
                std::fprintf(stderr, "[pad] no gamepad: the keyboard is player 1\n");
        }
    };

    DevicePad::DevicePad() : impl_(new Impl)
    {
        const char *rumble = std::getenv("PS2X_RUMBLE");
        impl_->rumbleOn = !(rumble && rumble[0] == '0');
        impl_->background = envOn("PS2X_INPUT_BACKGROUND");
        if (impl_->background)
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    DevicePad::~DevicePad()
    {
        for (SDL_Gamepad *g : impl_->gamepads)
            if (g)
            {
                SDL_RumbleGamepad(g, 0, 0, 0);
                SDL_CloseGamepad(g);
            }
        delete impl_;
    }

    void DevicePad::handle(const SDL_Event &event)
    {
        Impl &m = *impl_;
        switch (event.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
        {
            for (int place = 0; place < Impl::kPlaces; ++place)
                if (m.gamepads[static_cast<size_t>(place)] && m.ids[static_cast<size_t>(place)] == event.gdevice.which)
                    return; // already open
            for (int place = 0; place < Impl::kPlaces; ++place)
                if (!m.gamepads[static_cast<size_t>(place)])
                {
                    if (SDL_Gamepad *g = SDL_OpenGamepad(event.gdevice.which))
                    {
                        m.gamepads[static_cast<size_t>(place)] = g;
                        m.ids[static_cast<size_t>(place)] = event.gdevice.which;
                        m.listChanged = true;
                    }
                    else
                        std::fprintf(stderr, "[pad] a gamepad could not be opened: %s\n", SDL_GetError());
                    return;
                }
            std::fprintf(stderr, "[pad] a fifth gamepad is not used (four players at most)\n");
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (int place = 0; place < Impl::kPlaces; ++place)
                if (m.gamepads[static_cast<size_t>(place)] && m.ids[static_cast<size_t>(place)] == event.gdevice.which)
                {
                    for (SDL_Gamepad *&r : m.rumbling)
                        if (r == m.gamepads[static_cast<size_t>(place)])
                            r = nullptr;
                    SDL_CloseGamepad(m.gamepads[static_cast<size_t>(place)]);
                    m.gamepads[static_cast<size_t>(place)] = nullptr;
                    m.listChanged = true;
                }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            m.dx += event.motion.xrel;
            m.dy += event.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            // A wheel notch holds its binding pressed for a moment (the game polls the pad at its own pace).
            if (event.wheel.y > 0.0f)
                m.wheelUpUntil = SDL_GetTicks() + 120;
            else if (event.wheel.y < 0.0f)
                m.wheelDownUntil = SDL_GetTicks() + 120;
            break;
        default:
            break;
        }
    }

    void DevicePad::update(bool focused)
    {
        Impl &m = *impl_;
        if (!m.mappingsLoaded)
            m.loadMappings();
        if (m.listChanged)
            m.logList();
        const bool allowed = focused || m.background;

        input::Config &config = input::config(); // input.ini, loaded at the first update
        input::MouseSettings mouse;
        std::array<int, input::kPlayers> chosen{};
        {
            std::lock_guard<std::mutex> lock(input::configMutex());
            mouse = config.mouse;
            for (int player = 0; player < input::kPlayers; ++player)
                chosen[static_cast<size_t>(player)] = config.players[player].gamepad;
        }

        // Cursor capture while playing: the motion is then unbounded (no screen edge), and the cursor hidden.
        const bool capture = mouse.capture && focused;
        if (capture != m.captured)
        {
            if (SDL_Window *w = SDL_GetKeyboardFocus())
                SDL_SetWindowRelativeMouseMode(w, capture);
            m.captured = capture;
        }
        // Mouse speed (pixels a loop, smoothed over a few) as stick deflection.
        m.speedX = m.speedX * 0.5f + m.dx * 0.5f;
        m.speedY = m.speedY * 0.5f + m.dy * 0.5f;
        m.dx = m.dy = 0.0f;
        const float scale = mouse.sensitivity / 10.0f;

        input::Devices devices;
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        const uint64_t now = SDL_GetTicks();
        if (allowed)
        {
            devices.keys = keys;
            devices.keyCount = keyCount;
            devices.mouseButtons = SDL_GetMouseState(nullptr, nullptr);
            devices.mouseX = std::clamp(m.speedX * scale, -1.0f, 1.0f);
            devices.mouseY = std::clamp(m.speedY * scale * (mouse.invertY ? -1.0f : 1.0f), -1.0f, 1.0f);
            devices.wheelUp = now < m.wheelUpUntil;
            devices.wheelDown = now < m.wheelDownUntil;
        }

        std::array<libs::PadInputState, input::kPlayers> states{};
        std::array<SDL_Gamepad *, input::kPlayers> pads{};
        for (int player = 0; player < input::kPlayers; ++player)
        {
            SDL_Gamepad *g = m.gamepadOf(player, chosen[static_cast<size_t>(player)]);
            pads[static_cast<size_t>(player)] = g;
            // Players 2-4 only with a gamepad of their own, never the keyboard (as the fork).
            if (allowed && (player == 0 || g))
                states[static_cast<size_t>(player)] = input::padState(player, devices, g);
            states[static_cast<size_t>(player)].connected = player == 0 || g != nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(m.mutex);
            m.states = states;
        }

        // Rumble: what the game asks, sent when it changes and again before the last one runs out; nothing while the
        // window is in the background.
        for (int player = 0; player < input::kPlayers; ++player)
        {
            SDL_Gamepad *g = pads[static_cast<size_t>(player)];
            uint32_t want = m.rumbleOn && allowed ? m.wanted[static_cast<size_t>(player)].load(std::memory_order_relaxed) : 0;
            SDL_Gamepad *&was = m.rumbling[static_cast<size_t>(player)];
            if (was && was != g)
            {
                SDL_RumbleGamepad(was, 0, 0, 0);
                was = nullptr;
                m.sent[static_cast<size_t>(player)] = 0;
            }
            if (!g)
                continue;
            const bool again = want && now - m.sentAt[static_cast<size_t>(player)] > 1000;
            if (want == m.sent[static_cast<size_t>(player)] && !again)
                continue;
            const uint16_t low = static_cast<uint16_t>((want & 0xFF) * 257u);
            const uint16_t high = (want >> 8) ? 0xFFFF : 0;
            SDL_RumbleGamepad(g, low, high, want ? 2000 : 0);
            m.sent[static_cast<size_t>(player)] = want;
            m.sentAt[static_cast<size_t>(player)] = now;
            was = want ? g : nullptr;
        }
    }

    void DevicePad::read(uint32_t, int player, libs::PadInputState &out)
    {
        if (player < 0 || player >= input::kPlayers)
        {
            out = libs::PadInputState{};
            return;
        }
        std::lock_guard<std::mutex> lock(impl_->mutex);
        out = impl_->states[static_cast<size_t>(player)];
    }

    void DevicePad::rumble(int player, bool small, uint8_t large)
    {
        if (player >= 0 && player < input::kPlayers)
            impl_->wanted[static_cast<size_t>(player)].store((small ? 0x100u : 0u) | large, std::memory_order_relaxed);
    }
}
