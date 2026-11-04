// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The PC's keyboard, mouse and gamepads as the game's DualShock 2 controllers (libpad's input source,
// bozo/libs/pad.h), through SDL3, for up to four players (milestone M2):
// - the bindings: input.ini (bozo/app/input_bindings.h); player 1 the keyboard and mouse and its gamepad, players
//   2-4 a gamepad each;
// - gamepads hot-plugged: one plugged in takes the first free place of four (the order players get them in: player
//   n the nth, unless input.ini's `gamepad =` names one), one pulled out leaves its place empty; a player with no
//   gamepad has no controller in a multitap slot (players 2-4), and an idle one in port 2;
// - SDL's controller database, plus gamecontrollerdb.txt (PS2X_GAMEPAD_DB, else beside the executable, else in the
//   user folder) for controllers it lacks;
// - rumble: the motors the game sets (scePadSetActDirect) on the player's gamepad, the large motor as its low
//   frequency rumble, the small one as its high frequency (PS2X_RUMBLE=0: none);
// - pressure: each pressure button reports how far its binding is pressed (an analog trigger's travel, full for keys
//   and digital buttons).
// The host's devices reach the game only while the window has the focus (PS2X_INPUT_BACKGROUND=1: always).
#pragma once

#include "bozo/libs/pad.h"

#include <cstdint>

union SDL_Event;

namespace bozo::app
{
    class DevicePad final : public libs::PadInput
    {
    public:
        DevicePad();
        ~DevicePad() override;
        // Window thread: each event (gamepads added and removed, the mouse's wheel and motion), then once a loop the
        // devices read through the bindings and the rumble applied.
        void handle(const SDL_Event &event);
        void update(bool focused);
        // Window thread: while the settings window (F2) holds them, the keyboard and the mouse do not reach the game
        // (the gamepads still do), and the cursor is not captured.
        void setHeld(bool keyboard, bool mouse);
        // Game thread.
        void read(uint32_t frame, int player, libs::PadInputState &out) override;
        void rumble(int player, bool small, uint8_t large) override;

    private:
        struct Impl;
        Impl *impl_;
    };
}
