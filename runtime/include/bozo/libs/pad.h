// libpad as the native runtime answers it: DualShock 2 controllers in ports 1 and 2, and with a multitap in port 1
// (the game's sceMtap library, answered by its port, sets PadState::multitap) four players on that port's slots. Their
// state comes from an input source: a frame-keyed script (the reference runs' format) or the PC's devices.
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

    // The pressure bytes' order in the pad data, as the reference fills it: right, left, up, down, triangle, circle,
    // cross, square, L1, L2, R1, R2.
    constexpr uint16_t kPadPressureOrder[12] = {kPadRight, kPadLeft, kPadUp,   kPadDown, kPadTriangle, kPadCircle,
                                                kPadCross, kPadSquare, kPadL1, kPadL2,   kPadR1,       kPadR2};

    // What a player's controller reports at one read.
    struct PadInputState
    {
        // Whether the player has a controller. The pads in ports 1 and 2 are always there; this decides only for the
        // multitap's slots 2-4 (players 2-4).
        bool connected = false;
        uint16_t buttons = 0;                    // PadButton bits
        uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
        std::array<uint8_t, 12> pressure{};      // kPadPressureOrder; 0-255
    };

    // Where controller state comes from, by player (0-3). `frame` is the game frames ended so far.
    class PadInput
    {
    public:
        virtual ~PadInput() = default;
        virtual void read(uint32_t frame, int player, PadInputState &out) = 0;
        // The vibration the game sets (scePadSetActDirect through scePadSetActAlign): the small motor (on / off) and
        // the large one (0-255), for the PC controller's rumble.
        virtual void rumble(int /*player*/, bool /*small*/, uint8_t /*large*/) {}
    };

    // The player a socket is (libraries.md section 6): with the multitap, port 1's slots are players 1-4 and port 2
    // has no player (its pad is there, idle); without, port 1 slot 0 is player 1 and port 2 slot 0 player 2. -1: none.
    int padPlayer(int port, int slot, bool multitap);

    // A frame-keyed script: "frame:BUTTON/frames" entries separated by commas (harness.md section 1); the button is
    // held for reads at frames [frame, frame + frames). Buttons: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 UP DOWN
    // LEFT RIGHT START SELECT L3 R3. Player 1 always has a controller; players 2-4 only when they have a script (as
    // the reference's PS2X_PAD2_SCRIPT..PS2X_PAD4_SCRIPT). The D-pad also moves the left stick to its end (as the
    // reference's input does) and every button pressed reports full pressure.
    class ScriptInput final : public PadInput
    {
    public:
        // Parses `text` as player `player`'s script (0-3); false with `error` on a malformed entry.
        bool load(int player, const std::string &text, std::string &error);
        void read(uint32_t frame, int player, PadInputState &out) override;

    private:
        struct Press
        {
            uint32_t from = 0, until = 0;
            uint16_t button = 0;
        };
        std::map<int, std::vector<Press>> presses_;
    };

    // Hot-reloaded native test input. Each valid file update starts a script at frame zero;
    // e.g. "0:CROSS/6" presses Cross for six game frames. Invalid partial writes keep the last script.
    class LiveScriptInput final : public PadInput {
    public:
        explicit LiveScriptInput(std::string path) : path_(std::move(path)) {}
        void read(uint32_t frame,int player,PadInputState &out) override;
    private:
        std::string path_,lastText_;
        ScriptInput script_;
        uint32_t lastFrame_=UINT32_MAX,origin_=0;
    };

    struct PadState
    {
        PadInput *input = nullptr; // null: the pads in ports 1 and 2 connected and idle, no multitap slot used

        struct Port
        {
            bool open = false;
            uint32_t dmaBuffer = 0;     // scePadPortOpen's buffer
            bool analog = false;        // main mode (set by scePadSetMainMode)
            bool locked = false;
            bool pressureMode = false;  // scePadEnterPressMode
            bool busy = false;          // a mode command in progress
            uint32_t commandFrame = 0;  // the frame it was given in
            uint8_t align[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; // scePadSetActAlign (0xff: no motor)
            uint8_t direct[6] = {};     // scePadSetActDirect: the motor levels
        };
        std::array<std::array<Port, 4>, 2> ports{};
        bool initialized = false;
        uint64_t reads = 0;
        std::array<std::array<uint64_t, 4>, 2> socketReads{}; // the reads that found a controller, by port and slot
        // A multitap in port 1 (sceMtapPortOpen through a port's override: the game's sceMtap library is answered by
        // the port): players 2-4 on its slots 1-3, port 2's pad idle.
        bool multitap = false;
    };
}
