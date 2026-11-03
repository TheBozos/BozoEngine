// libpad (libraries.md section 6): two DualShock 2 controllers, answered as the reference does in its deterministic
// runs, their buttons from a PadInput (a frame-keyed script in the checks).
#include "bozo/libs/libraries.h"

#include <algorithm>
#include <cstdlib>

namespace bozo::libs
{
    // ---- ScriptInput ---------------------------------------------------------------------------------------------

    namespace
    {
        uint16_t buttonByName(const std::string &n)
        {
            static const std::pair<const char *, uint16_t> names[] = {
                {"SELECT", kPadSelect}, {"L3", kPadL3}, {"R3", kPadR3}, {"START", kPadStart}, {"UP", kPadUp},
                {"RIGHT", kPadRight}, {"DOWN", kPadDown}, {"LEFT", kPadLeft}, {"L2", kPadL2}, {"R2", kPadR2},
                {"L1", kPadL1}, {"R1", kPadR1}, {"TRIANGLE", kPadTriangle}, {"CIRCLE", kPadCircle},
                {"CROSS", kPadCross}, {"SQUARE", kPadSquare}};
            for (const auto &[name, bit] : names)
                if (n == name)
                    return bit;
            return 0;
        }
    }

    bool ScriptInput::load(int port, const std::string &text, std::string &error)
    {
        std::vector<Press> &list = presses_[port];
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find_first_of(",\r\n", at);
            if (end == std::string::npos)
                end = text.size();
            const std::string e = text.substr(at, end - at);
            at = end + 1;
            if (e.empty())
                continue;
            const size_t colon = e.find(':'), slash = e.find('/');
            if (colon == std::string::npos || slash == std::string::npos || slash < colon)
            {
                error = "malformed pad script entry '" + e + "'";
                return false;
            }
            Press p;
            p.from = static_cast<uint32_t>(std::strtoul(e.substr(0, colon).c_str(), nullptr, 10));
            p.button = buttonByName(e.substr(colon + 1, slash - colon - 1));
            const uint64_t n = std::strtoull(e.substr(slash + 1).c_str(), nullptr, 10);
            p.until = static_cast<uint32_t>(std::min<uint64_t>(p.from + n, 0xFFFFFFFFull));
            if (!p.button)
            {
                error = "unknown button in pad script entry '" + e + "'";
                return false;
            }
            list.push_back(p);
        }
        return true;
    }

    void ScriptInput::read(uint32_t frame, int port, int slot, PadInputState &out)
    {
        out = PadInputState{};
        out.connected = slot == 0 && (port == 0 || port == 1);
        if (!out.connected)
            return;
        auto it = presses_.find(port);
        if (it == presses_.end())
            return;
        for (const Press &p : it->second)
            if (frame >= p.from && frame < p.until)
                out.buttons |= p.button;
        // The D-pad moves the left stick to its end; every pressed button reports full pressure.
        if (out.buttons & kPadLeft)
            out.lx = 0x00;
        if (out.buttons & kPadRight)
            out.lx = 0xFF;
        if (out.buttons & kPadUp)
            out.ly = 0x00;
        if (out.buttons & kPadDown)
            out.ly = 0xFF;
        // The reference's order puts R1 after L2 (match1p: L1 at byte 16, R1 at byte 18); L2 and R2 are never pressed
        // in the reference runs, so their places (17, 19) are the remaining ones.
        static const uint16_t order[12] = {kPadRight, kPadLeft,   kPadUp, kPadDown, kPadTriangle, kPadCircle,
                                           kPadCross, kPadSquare, kPadL1, kPadL2,   kPadR1,       kPadR2};
        for (int i = 0; i < 12; ++i)
            out.pressure[i] = (out.buttons & order[i]) ? 0xFF : 0x00;
    }

    // ---- libpad --------------------------------------------------------------------------------------------------

    namespace
    {
        PadState::Port *port(Libraries &l, Guest &g)
        {
            const uint32_t p = g.arg(0), s = g.arg(1);
            return p < 2 && s < 4 ? &l.pad.ports[p][s] : nullptr;
        }

        PadInputState current(Libraries &l, int p, int s)
        {
            PadInputState st;
            if (l.pad.input)
                l.pad.input->read(l.framesEnded, p, s, st);
            else
                st.connected = s == 0 && (p == 0 || p == 1);
            return st;
        }

        // A mode command (scePadSetMainMode, scePadEnterPressMode) keeps the pad busy (state 5, EXECCMD) in the frame
        // it is given and the next; it is stable (6) from the one after.
        bool busy(Libraries &l, PadState::Port &pt)
        {
            if (pt.busy && l.framesEnded > pt.commandFrame + 1)
                pt.busy = false;
            return pt.busy;
        }

        void command(Libraries &l, PadState::Port &pt)
        {
            pt.busy = true;
            pt.commandFrame = l.framesEnded;
        }

        void init(Libraries &l, Guest &g)
        {
            l.pad.initialized = true;
            g.ret(1);
        }

        // scePadPortOpen(port, slot, buffer): every slot opens (1), the multitap's too.
        void portOpen(Libraries &l, Guest &g)
        {
            if (PadState::Port *pt = port(l, g))
            {
                pt->open = true;
                pt->dmaBuffer = g.arg(2);
            }
            g.ret(1);
        }

        // scePadGetState(port, slot): 6 (stable), 5 (executing a command), 0 (nothing there: slots 1-3).
        void getState(Libraries &l, Guest &g)
        {
            PadState::Port *pt = port(l, g);
            if (!pt || !current(l, static_cast<int>(g.arg(0)), static_cast<int>(g.arg(1))).connected)
            {
                g.ret(0);
                return;
            }
            g.ret(busy(l, *pt) ? 5 : 6);
        }

        // scePadGetReqState: the last request completed (0).
        void getReqState(Libraries &, Guest &g) { g.ret(0); }

        // scePadInfoMode(port, slot, term, offset): the current or extended ID (1, 2): 7 (DualShock 2, analog) once
        // scePadSetMainMode has set analog mode, 4 (digital) before; the mode table (4): its size at offset -1 (2),
        // then 4 and 7.
        void infoMode(Libraries &l, Guest &g)
        {
            PadState::Port *pt = port(l, g);
            const int32_t term = static_cast<int32_t>(g.arg(2)), offset = static_cast<int32_t>(g.arg(3));
            if (!pt)
            {
                g.ret(0);
                return;
            }
            const bool analog = pt->analog && !busy(l, *pt);
            switch (term)
            {
            case 1:
            case 2: g.ret(analog ? 7 : 4); break;
            case 3: g.ret(analog ? 1 : 0); break; // the current mode table entry
            case 4: g.ret(offset == -1 ? 2 : offset == 0 ? 4 : offset == 1 ? 7 : 0); break;
            default: g.ret(0); break;
            }
        }

        void setMainMode(Libraries &l, Guest &g)
        {
            if (PadState::Port *pt = port(l, g))
            {
                pt->analog = g.arg(2) == 1;
                pt->locked = g.arg(3) == 3;
                command(l, *pt);
            }
            g.ret(1);
        }

        // scePadInfoAct(port, slot, actuator, term): two actuators (at actuator -1); their details are not asked.
        void infoAct(Libraries &, Guest &g) { g.ret(static_cast<int32_t>(g.arg(2)) == -1 ? 2 : 0); }

        void setActAlign(Libraries &l, Guest &g)
        {
            if (PadState::Port *pt = port(l, g))
                for (int i = 0; i < 6; ++i)
                    pt->align[i] = g.read8(g.arg(2) + static_cast<uint32_t>(i));
            g.ret(1);
        }

        void setActDirect(Libraries &l, Guest &g)
        {
            if (PadState::Port *pt = port(l, g))
            {
                for (int i = 0; i < 6; ++i)
                    pt->direct[i] = g.read8(g.arg(2) + static_cast<uint32_t>(i));
                if (l.pad.input)
                    l.pad.input->rumble(static_cast<int>(g.arg(0)), static_cast<int>(g.arg(1)), pt->direct);
            }
            g.ret(1);
        }

        void infoPressMode(Libraries &, Guest &g) { g.ret(1); }

        void enterPressMode(Libraries &l, Guest &g)
        {
            if (PadState::Port *pt = port(l, g))
            {
                pt->pressureMode = true;
                command(l, *pt);
            }
            g.ret(1);
        }

        // scePadRead(port, slot, data): the pad data (status 0, the mode ID, the buttons active low, then in analog
        // mode the sticks, in pressure mode the twelve pressures). Returns 1.
        void read(Libraries &l, Guest &g)
        {
            PadState::Port *pt = port(l, g);
            const PadInputState st = current(l, static_cast<int>(g.arg(0)), static_cast<int>(g.arg(1)));
            if (!pt || !st.connected)
            {
                g.ret(0);
                return;
            }
            ++l.pad.reads;
            uint8_t d[20] = {};
            const uint16_t released = static_cast<uint16_t>(~st.buttons);
            d[0] = 0;
            d[2] = static_cast<uint8_t>(released);
            d[3] = static_cast<uint8_t>(released >> 8);
            uint32_t bytes = 4;
            d[1] = 0x41;
            if (pt->analog)
            {
                d[1] = pt->pressureMode ? 0x79 : 0x73;
                d[4] = st.rx;
                d[5] = st.ry;
                d[6] = st.lx;
                d[7] = st.ly;
                bytes = 8;
                if (pt->pressureMode)
                {
                    for (int i = 0; i < 12; ++i)
                        d[8 + i] = st.pressure[static_cast<size_t>(i)];
                    bytes = 20;
                }
            }
            g.write(g.arg(2), d, bytes);
            g.ret(1);
        }
    }

    void addPad(std::vector<Function> &out)
    {
        out.push_back({"scePadInit", kPad, init});
        out.push_back({"scePadPortOpen", kPad, portOpen});
        out.push_back({"scePadGetState", kPad, getState});
        out.push_back({"scePadGetReqState", kPad, getReqState});
        out.push_back({"scePadInfoMode", kPad, infoMode});
        out.push_back({"scePadSetMainMode", kPad, setMainMode});
        out.push_back({"scePadInfoAct", kPad, infoAct});
        out.push_back({"scePadSetActAlign", kPad, setActAlign});
        out.push_back({"scePadSetActDirect", kPad, setActDirect});
        out.push_back({"scePadInfoPressMode", kPad, infoPressMode});
        out.push_back({"scePadEnterPressMode", kPad, enterPressMode});
        out.push_back({"scePadRead", kPad, read});
    }
}
