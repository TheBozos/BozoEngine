// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The PC's keyboard and first gamepad as port 1's DualShock 2 (libpad's input source, bozo/libs/pad.h), through SDL3.
// Milestone M1's minimum: fixed bindings (the fork's default keys: arrows / WASD the D-pad and left stick, X / Space
// cross, C / Escape circle, Z square, V triangle, Q / E L1 / R1, the shifts L2 / R2, Enter start, Tab select; a
// gamepad's buttons by position). input.ini, hot-plug of several pads, rumble and multitap come with M2.
#pragma once

#include "bozo/libs/pad.h"

#include <atomic>
#include <cstdint>

union SDL_Event;

namespace bozo::app
{
    class DevicePad final : public libs::PadInput
    {
    public:
        DevicePad();
        ~DevicePad() override;
        // Window thread: a device event (a gamepad added or removed), then the devices' state read (each loop).
        void handle(const SDL_Event &event);
        void update(bool focused);
        // Game thread.
        void read(uint32_t frame, int port, int slot, libs::PadInputState &out) override;

    private:
        struct Gamepad;
        Gamepad *gamepad_ = nullptr;
        std::atomic<uint64_t> state_{0}; // buttons | lx << 16 | ly << 24 | rx << 32 | ry << 40
    };
}
