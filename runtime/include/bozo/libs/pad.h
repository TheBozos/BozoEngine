// libpad as the native runtime answers it: two DualShock 2 controllers on ports 1 and 2 (no multitap in TMB's runs),
// their state from an input source: a frame-keyed script (the reference runs' format) or, later, the PC's devices.
// Spec: docs/specs/libraries.md section 6.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bozo::libs
{
    // The DualShock 2's buttons, bit n set when pressed (the order of the pad data's button halfword).
    enum PadButton : uint16_t
    {
        kPadSelect = 1u << 0, kPadL3 = 1u << 1, kPadR3 = 1u << 2, kPadStart = 1u << 3,
        kPadUp = 1u << 4, kPadRight = 1u << 5, kPadDown = 1u << 6, kPadLeft = 1u << 7,
        kPadL2 = 1u << 8, kPadR2 = 1u << 9, kPadL1 = 1u << 10, kPadR1 = 1u << 11,
        kPadTriangle = 1u << 12, kPadCircle = 1u << 13, kPadCross = 1u << 14, kPadSquare = 1u << 15,
    };

    // What a controller reports at one read.
    struct PadInputState
    {
        bool connected = false;
        uint16_t buttons = 0;                    // PadButton bits
        uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
        // Pressure (0-255) in the pad data's order as the reference fills it: right, left, up, down, triangle,
        // circle, cross, square, L1, L2, R1, R2.
        std::array<uint8_t, 12> pressure{};
    };

    // Where controller state comes from. `frame` is the game frames ended so far.
    class PadInput
    {
    public:
        virtual ~PadInput() = default;
        virtual void read(uint32_t frame, int port, int slot, PadInputState &out) = 0;
        // Vibration the game asks for (the actuators' levels as it sets them): for the PC controller's rumble.
        virtual void rumble(int /*port*/, int /*slot*/, const uint8_t /*levels*/[6]) {}
    };

    // A frame-keyed script: "frame:BUTTON/frames" entries separated by commas (harness.md section 1); the button is
    // held for reads at frames [frame, frame + frames). Buttons: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 UP DOWN
    // LEFT RIGHT START SELECT L3 R3. Ports 1 and 2 are connected, slot 0 only; a port without a script is idle.
    // The D-pad also moves the left stick to its end (as the reference's input does) and every button pressed reports
    // full pressure.
    class ScriptInput final : public PadInput
    {
    public:
        // Parses `text` as port `port`'s script; false with `error` on a malformed entry.
        bool load(int port, const std::string &text, std::string &error);
        void read(uint32_t frame, int port, int slot, PadInputState &out) override;

    private:
        struct Press
        {
            uint32_t from = 0, until = 0;
            uint16_t button = 0;
        };
        std::map<int, std::vector<Press>> presses_;
    };

    struct PadState
    {
        PadInput *input = nullptr; // null: both ports connected and idle

        struct Port
        {
            bool open = false;
            uint32_t dmaBuffer = 0;     // scePadPortOpen's buffer
            bool analog = false;        // main mode (set by scePadSetMainMode)
            bool locked = false;
            bool pressureMode = false;  // scePadEnterPressMode
            bool busy = false;          // a mode command in progress
            uint32_t commandFrame = 0;  // the frame it was given in
            uint8_t align[6] = {};      // scePadSetActAlign
            uint8_t direct[6] = {};     // scePadSetActDirect: the motor levels
        };
        std::array<std::array<Port, 4>, 2> ports{};
        bool initialized = false;
        uint64_t reads = 0;
        // A multitap in port 1 (sceMtapPortOpen through a port's override: the game's sceMtap library is answered by
        // the port). The slots behind it (players 2-4 on port 1) come with the PC's pads (milestone M2).
        bool multitap = false;
    };
}
