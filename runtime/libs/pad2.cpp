// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos.
// libpad2 and libvib over libdbc, as a game on a later SDK reads its DualShock 2 controllers (The Third Age): sockets
// on a port and slot, a state, a button profile, the pad data, the button info, the vibration profile and the motors.
// The controllers are libpad's (pad.cpp): the same ports, players and input source. libdbc's own calls (the DBCMAN
// driver's RPC) are not reached once these are native; sceDbcInit/sceDbcEnd answer for a game that calls them itself.
//
// The pad data scePad2Read writes is the DualShock 2's report without libpad's two-byte header: the buttons (two bytes,
// active low, libpad's bit order), the sticks (RX, RY, LX, LY) and the twelve pressures in the controller's order
// (right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2). The button profile is the DualShock 2's:
// four bytes, every button id 0..31 present.
#include "bozo/libs/libraries.h"

#include <algorithm>

namespace bozo::libs
{
    namespace
    {
        constexpr uint16_t kPad2PressureOrder[12] = {kPadRight, kPadLeft, kPadUp,   kPadDown, kPadTriangle, kPadCircle,
                                                     kPadCross, kPadSquare, kPadL1, kPadR1,   kPadL2,       kPadR2};
        constexpr uint32_t kPad2DataBytes = 18;

        const PadState::Pad2Socket *socket(Libraries &l, uint32_t number)
        {
            const auto it = l.pad.pad2Sockets.find(static_cast<int32_t>(number));
            return it == l.pad.pad2Sockets.end() ? nullptr : &it->second;
        }

        // A socket's controller, as libpad answers the same port and slot: the pads in ports 1 and 2 are always there;
        // a multitap slot has one when its player has an input.
        PadInputState controller(Libraries &l, const PadState::Pad2Socket &s, int &player)
        {
            PadInputState st;
            player = padPlayer(s.port, s.slot, l.pad.multitap);
            if (player >= 0 && l.pad.input)
                l.pad.input->read(l.framesEnded, player, st);
            st.connected = (s.slot == 0 && (s.port == 0 || s.port == 1)) || (player >= 0 && st.connected);
            return st;
        }

        void dbcInit(Libraries &, Guest &g) { g.ret(1); }
        void dbcEnd(Libraries &, Guest &g) { g.ret(1); }

        // scePad2Init(mode), scePad2End(): 1.
        void init(Libraries &, Guest &g) { g.ret(1); }
        void end(Libraries &l, Guest &g)
        {
            l.pad.pad2Sockets.clear();
            g.ret(1);
        }

        // scePad2CreateSocket(param, buffer): param {flag, port, slot, number, name[16]} (words, then the name); a
        // null param is port 0, slot 0. The buffer (64-byte aligned, -1 otherwise as the library prints) is not used.
        // Returns the socket's number.
        void createSocket(Libraries &l, Guest &g)
        {
            if (g.arg(1) & 0x3fu)
            {
                g.ret(-1);
                return;
            }
            PadState::Pad2Socket s;
            if (const uint32_t param = g.arg(0))
            {
                s.port = static_cast<int32_t>(g.read32(param + 4));
                s.slot = static_cast<int32_t>(g.read32(param + 8));
            }
            const int32_t number = l.pad.pad2Next++;
            l.pad.pad2Sockets[number] = s;
            g.ret(number);
        }

        // scePad2DeleteSocket(socket): 1, a socket that is not there -1.
        void deleteSocket(Libraries &l, Guest &g)
        {
            g.ret(l.pad.pad2Sockets.erase(static_cast<int32_t>(g.arg(0))) ? 1 : -1);
        }

        // scePad2GetState(socket): 1 with a controller (the game reads then), 0 without (it resets the port).
        void getState(Libraries &l, Guest &g)
        {
            const auto *s = socket(l, g.arg(0));
            int player = -1;
            g.ret(s && controller(l, *s, player).connected ? 1 : 0);
        }

        // scePad2GetButtonProfile(socket, profile): the DualShock 2's, four bytes with every id 0..31; their count, or
        // -1 without a controller.
        void getButtonProfile(Libraries &l, Guest &g)
        {
            const auto *s = socket(l, g.arg(0));
            int player = -1;
            if (!s || !controller(l, *s, player).connected)
            {
                g.ret(-1);
                return;
            }
            const uint8_t profile[4] = {0xff, 0xff, 0xff, 0xff};
            g.write(g.arg(1), profile, sizeof(profile));
            g.ret(4);
        }

        // scePad2Read(socket, data): the 18-byte report; its size, or -1 without a controller.
        void read(Libraries &l, Guest &g)
        {
            const auto *s = socket(l, g.arg(0));
            int player = -1;
            const PadInputState st = s ? controller(l, *s, player) : PadInputState{};
            if (!s || !st.connected)
            {
                g.ret(-1);
                return;
            }
            ++l.pad.reads;
            if (s->port >= 0 && s->port < 2 && s->slot >= 0 && s->slot < 4)
                ++l.pad.socketReads[static_cast<size_t>(s->port)][static_cast<size_t>(s->slot)];
            uint8_t d[kPad2DataBytes] = {};
            const uint16_t released = static_cast<uint16_t>(~st.buttons);
            d[0] = static_cast<uint8_t>(released);
            d[1] = static_cast<uint8_t>(released >> 8);
            d[2] = st.rx;
            d[3] = st.ry;
            d[4] = st.lx;
            d[5] = st.ly;
            // The input's pressures are in libpad's order; each goes to its button's place in the controller's.
            for (int i = 0; i < 12; ++i)
            {
                const auto from = std::find(std::begin(kPadPressureOrder), std::end(kPadPressureOrder), kPad2PressureOrder[i]);
                d[6 + i] = st.pressure[static_cast<size_t>(from - std::begin(kPadPressureOrder))];
            }
            g.write(g.arg(1), d, sizeof(d));
            g.ret(static_cast<int32_t>(sizeof(d)));
        }

        // scePad2GetButtonInfo(socket, data, id): ids 0..15 the buttons (1 pressed, the data active low), 16..31 the
        // sticks and pressures (data[id - 14]); -1 for any other id.
        void getButtonInfo(Libraries &, Guest &g)
        {
            const uint32_t data = g.arg(1), id = g.arg(2);
            if (id < 16)
                g.ret((g.read8(data + id / 8) >> (id % 8)) & 1u ? 0 : 1);
            else if (id < 32)
                g.ret(g.read8(data + id - 14));
            else
                g.ret(-1);
        }

        // sceVibGetProfile(socket, profile): the DualShock 2's motors, one byte 0x03 (bit 0 the small one, bit 1 the
        // large one); 1, or -1 without a controller.
        void vibGetProfile(Libraries &l, Guest &g)
        {
            const auto *s = socket(l, g.arg(0));
            int player = -1;
            if (!s || !controller(l, *s, player).connected)
            {
                g.ret(-1);
                return;
            }
            g.write8(g.arg(1), 0x03);
            g.ret(1);
        }

        // sceVibSetActParam(socket, profile size, profile, data size, data): the motors, packed as libvib packs them
        // (data[0] bit 0 the small motor on or off, then eight bits of the large motor's level); 1.
        void vibSetActParam(Libraries &l, Guest &g)
        {
            const auto *s = socket(l, g.arg(0));
            int player = -1;
            if (s && controller(l, *s, player).connected && player >= 0 && l.pad.input && g.arg(3) >= 2)
            {
                const uint32_t data = g.arg(4);
                const uint8_t b0 = g.read8(data), b1 = g.read8(data + 1);
                l.pad.input->rumble(player, (b0 & 1u) != 0, static_cast<uint8_t>((b0 >> 1) | ((b1 & 1u) << 7)));
            }
            g.ret(1);
        }
    }

    void addPad2(std::vector<Function> &out)
    {
        out.push_back({"sceDbcInit", kPad, dbcInit});
        out.push_back({"sceDbcEnd", kPad, dbcEnd});
        out.push_back({"scePad2Init", kPad, init});
        out.push_back({"scePad2End", kPad, end});
        out.push_back({"scePad2CreateSocket", kPad, createSocket});
        out.push_back({"scePad2DeleteSocket", kPad, deleteSocket});
        out.push_back({"scePad2GetState", kPad, getState});
        out.push_back({"scePad2GetButtonProfile", kPad, getButtonProfile});
        out.push_back({"scePad2Read", kPad, read});
        out.push_back({"scePad2GetButtonInfo", kPad, getButtonInfo});
        out.push_back({"sceVibGetProfile", kPad, vibGetProfile});
        out.push_back({"sceVibSetActParam", kPad, vibSetActParam});
    }
}
