// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The pad bindings (bozo/app/input_bindings.h), moved from the team's ps2_input_bindings.cpp: input.ini's format, the
// defaults and the evaluation kept; raylib's devices replaced by SDL3's. Each name table keeps the raylib code the
// fork wrote for numbers ("key:283"), so its files read the same.
#include "bozo/app/input_bindings.h"

#include "bozo/app/app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace bozo::app::input
{
    namespace
    {
        // A name in input.ini (null: the fork wrote this one as its raylib number), raylib's code (-1: none), SDL's.
        struct Name
        {
            const char *name;
            int raylib;
            int sdl;
        };

        const Name kKeys[] = {
            {"A", 65, SDL_SCANCODE_A}, {"B", 66, SDL_SCANCODE_B}, {"C", 67, SDL_SCANCODE_C}, {"D", 68, SDL_SCANCODE_D},
            {"E", 69, SDL_SCANCODE_E}, {"F", 70, SDL_SCANCODE_F}, {"G", 71, SDL_SCANCODE_G}, {"H", 72, SDL_SCANCODE_H},
            {"I", 73, SDL_SCANCODE_I}, {"J", 74, SDL_SCANCODE_J}, {"K", 75, SDL_SCANCODE_K}, {"L", 76, SDL_SCANCODE_L},
            {"M", 77, SDL_SCANCODE_M}, {"N", 78, SDL_SCANCODE_N}, {"O", 79, SDL_SCANCODE_O}, {"P", 80, SDL_SCANCODE_P},
            {"Q", 81, SDL_SCANCODE_Q}, {"R", 82, SDL_SCANCODE_R}, {"S", 83, SDL_SCANCODE_S}, {"T", 84, SDL_SCANCODE_T},
            {"U", 85, SDL_SCANCODE_U}, {"V", 86, SDL_SCANCODE_V}, {"W", 87, SDL_SCANCODE_W}, {"X", 88, SDL_SCANCODE_X},
            {"Y", 89, SDL_SCANCODE_Y}, {"Z", 90, SDL_SCANCODE_Z},
            {"0", 48, SDL_SCANCODE_0}, {"1", 49, SDL_SCANCODE_1}, {"2", 50, SDL_SCANCODE_2}, {"3", 51, SDL_SCANCODE_3},
            {"4", 52, SDL_SCANCODE_4}, {"5", 53, SDL_SCANCODE_5}, {"6", 54, SDL_SCANCODE_6}, {"7", 55, SDL_SCANCODE_7},
            {"8", 56, SDL_SCANCODE_8}, {"9", 57, SDL_SCANCODE_9},
            {"F1", 290, SDL_SCANCODE_F1}, {"F2", 291, SDL_SCANCODE_F2}, {"F3", 292, SDL_SCANCODE_F3},
            {"F4", 293, SDL_SCANCODE_F4}, {"F5", 294, SDL_SCANCODE_F5}, {"F6", 295, SDL_SCANCODE_F6},
            {"F7", 296, SDL_SCANCODE_F7}, {"F8", 297, SDL_SCANCODE_F8}, {"F9", 298, SDL_SCANCODE_F9},
            {"F10", 299, SDL_SCANCODE_F10}, {"F11", 300, SDL_SCANCODE_F11}, {"F12", 301, SDL_SCANCODE_F12},
            {"SPACE", 32, SDL_SCANCODE_SPACE}, {"ESCAPE", 256, SDL_SCANCODE_ESCAPE}, {"ENTER", 257, SDL_SCANCODE_RETURN},
            {"TAB", 258, SDL_SCANCODE_TAB}, {"BACKSPACE", 259, SDL_SCANCODE_BACKSPACE}, {"INSERT", 260, SDL_SCANCODE_INSERT},
            {"DELETE", 261, SDL_SCANCODE_DELETE}, {"RIGHT", 262, SDL_SCANCODE_RIGHT}, {"LEFT", 263, SDL_SCANCODE_LEFT},
            {"DOWN", 264, SDL_SCANCODE_DOWN}, {"UP", 265, SDL_SCANCODE_UP}, {"PAGE_UP", 266, SDL_SCANCODE_PAGEUP},
            {"PAGE_DOWN", 267, SDL_SCANCODE_PAGEDOWN}, {"HOME", 268, SDL_SCANCODE_HOME}, {"END", 269, SDL_SCANCODE_END},
            {"CAPS_LOCK", 280, SDL_SCANCODE_CAPSLOCK},
            {"LEFT_SHIFT", 340, SDL_SCANCODE_LSHIFT}, {"LEFT_CONTROL", 341, SDL_SCANCODE_LCTRL},
            {"LEFT_ALT", 342, SDL_SCANCODE_LALT}, {"RIGHT_SHIFT", 344, SDL_SCANCODE_RSHIFT},
            {"RIGHT_CONTROL", 345, SDL_SCANCODE_RCTRL}, {"RIGHT_ALT", 346, SDL_SCANCODE_RALT},
            {"KP_0", 320, SDL_SCANCODE_KP_0}, {"KP_1", 321, SDL_SCANCODE_KP_1}, {"KP_2", 322, SDL_SCANCODE_KP_2},
            {"KP_3", 323, SDL_SCANCODE_KP_3}, {"KP_4", 324, SDL_SCANCODE_KP_4}, {"KP_5", 325, SDL_SCANCODE_KP_5},
            {"KP_6", 326, SDL_SCANCODE_KP_6}, {"KP_7", 327, SDL_SCANCODE_KP_7}, {"KP_8", 328, SDL_SCANCODE_KP_8},
            {"KP_9", 329, SDL_SCANCODE_KP_9}, {"KP_DECIMAL", 330, SDL_SCANCODE_KP_PERIOD},
            {"KP_DIVIDE", 331, SDL_SCANCODE_KP_DIVIDE}, {"KP_MULTIPLY", 332, SDL_SCANCODE_KP_MULTIPLY},
            {"KP_SUBTRACT", 333, SDL_SCANCODE_KP_MINUS}, {"KP_ADD", 334, SDL_SCANCODE_KP_PLUS},
            {"KP_ENTER", 335, SDL_SCANCODE_KP_ENTER},
            {"APOSTROPHE", 39, SDL_SCANCODE_APOSTROPHE}, {"COMMA", 44, SDL_SCANCODE_COMMA}, {"MINUS", 45, SDL_SCANCODE_MINUS},
            {"PERIOD", 46, SDL_SCANCODE_PERIOD}, {"SLASH", 47, SDL_SCANCODE_SLASH}, {"SEMICOLON", 59, SDL_SCANCODE_SEMICOLON},
            {"EQUAL", 61, SDL_SCANCODE_EQUALS}, {"LEFT_BRACKET", 91, SDL_SCANCODE_LEFTBRACKET},
            {"BACKSLASH", 92, SDL_SCANCODE_BACKSLASH}, {"RIGHT_BRACKET", 93, SDL_SCANCODE_RIGHTBRACKET},
            {"GRAVE", 96, SDL_SCANCODE_GRAVE},
            // Keys the fork had no name for: it wrote their raylib number, and so does this.
            {nullptr, 281, SDL_SCANCODE_SCROLLLOCK}, {nullptr, 282, SDL_SCANCODE_NUMLOCKCLEAR},
            {nullptr, 283, SDL_SCANCODE_PRINTSCREEN}, {nullptr, 284, SDL_SCANCODE_PAUSE},
            {nullptr, 336, SDL_SCANCODE_KP_EQUALS}, {nullptr, 343, SDL_SCANCODE_LGUI},
            {nullptr, 347, SDL_SCANCODE_RGUI}, {nullptr, 348, SDL_SCANCODE_APPLICATION}};

        // raylib's names (its codes 1-17), and SDL3's buttons raylib did not have.
        const Name kPadButtons[] = {
            {"LEFT_FACE_UP", 1, SDL_GAMEPAD_BUTTON_DPAD_UP},       {"LEFT_FACE_RIGHT", 2, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
            {"LEFT_FACE_DOWN", 3, SDL_GAMEPAD_BUTTON_DPAD_DOWN},   {"LEFT_FACE_LEFT", 4, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
            {"RIGHT_FACE_UP", 5, SDL_GAMEPAD_BUTTON_NORTH},        {"RIGHT_FACE_RIGHT", 6, SDL_GAMEPAD_BUTTON_EAST},
            {"RIGHT_FACE_DOWN", 7, SDL_GAMEPAD_BUTTON_SOUTH},      {"RIGHT_FACE_LEFT", 8, SDL_GAMEPAD_BUTTON_WEST},
            {"LEFT_TRIGGER_1", 9, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"LEFT_TRIGGER_2", 10, kLeftTriggerButton},
            {"RIGHT_TRIGGER_1", 11, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"RIGHT_TRIGGER_2", 12, kRightTriggerButton},
            {"MIDDLE_LEFT", 13, SDL_GAMEPAD_BUTTON_BACK},           {"MIDDLE", 14, SDL_GAMEPAD_BUTTON_GUIDE},
            {"MIDDLE_RIGHT", 15, SDL_GAMEPAD_BUTTON_START},         {"LEFT_THUMB", 16, SDL_GAMEPAD_BUTTON_LEFT_STICK},
            {"RIGHT_THUMB", 17, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
            {"MISC1", -1, SDL_GAMEPAD_BUTTON_MISC1},                {"TOUCHPAD", -1, SDL_GAMEPAD_BUTTON_TOUCHPAD},
            {"RIGHT_PADDLE1", -1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"LEFT_PADDLE1", -1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
            {"RIGHT_PADDLE2", -1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}, {"LEFT_PADDLE2", -1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2}};

        // raylib's axis numbers are SDL's.
        const Name kPadAxes[] = {{"LEFT_X", 0, SDL_GAMEPAD_AXIS_LEFTX},       {"LEFT_Y", 1, SDL_GAMEPAD_AXIS_LEFTY},
                                 {"RIGHT_X", 2, SDL_GAMEPAD_AXIS_RIGHTX},     {"RIGHT_Y", 3, SDL_GAMEPAD_AXIS_RIGHTY},
                                 {"LEFT_TRIGGER", 4, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
                                 {"RIGHT_TRIGGER", 5, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}};

        const Name kMouseButtons[] = {{"LEFT", 0, SDL_BUTTON_LEFT},   {"RIGHT", 1, SDL_BUTTON_RIGHT},
                                      {"MIDDLE", 2, SDL_BUTTON_MIDDLE}, {"SIDE", 3, SDL_BUTTON_X1},
                                      {"EXTRA", 4, SDL_BUTTON_X2},     {"FORWARD", 5, SDL_BUTTON_X2},
                                      {"BACK", 6, SDL_BUTTON_X1}};

        const char *const kActionNames[ActionCount] = {
            "up", "down", "left", "right", "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3",
            "r3", "start", "select", "left_stick_left", "left_stick_right", "left_stick_up", "left_stick_down",
            "right_stick_left", "right_stick_right", "right_stick_up", "right_stick_down"};
        const char *const kActionLabels[ActionCount] = {
            "Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3",
            "R3", "Start", "Select", "Left stick left", "Left stick right", "Left stick up", "Left stick down",
            "Right stick left", "Right stick right", "Right stick up", "Right stick down"};
        // The PS2 buttons in Action order for the 16 buttons.
        const uint16_t kButtonBits[16] = {libs::kPadUp,  libs::kPadDown, libs::kPadLeft,  libs::kPadRight,
                                          libs::kPadCross, libs::kPadCircle, libs::kPadSquare, libs::kPadTriangle,
                                          libs::kPadL1,  libs::kPadR1,   libs::kPadL2,    libs::kPadR2,
                                          libs::kPadL3,  libs::kPadR3,   libs::kPadStart, libs::kPadSelect};

        // The name SDL's code has in input.ini (null: none).
        const Name *byCode(const Name *table, size_t count, int sdl)
        {
            for (size_t i = 0; i < count; ++i)
                if (table[i].sdl == sdl)
                    return &table[i];
            return nullptr;
        }

        // A name, or a number (raylib's code, as the fork wrote it).
        bool codeOf(const Name *table, size_t count, const std::string &name, int &code)
        {
            for (size_t i = 0; i < count; ++i)
                if (table[i].name && name == table[i].name)
                {
                    code = table[i].sdl;
                    return true;
                }
            char *end = nullptr;
            const long value = std::strtol(name.c_str(), &end, 10);
            if (end == name.c_str() || *end != '\0')
                return false;
            for (size_t i = 0; i < count; ++i)
                if (table[i].raylib == value)
                {
                    code = table[i].sdl;
                    return true;
                }
            return false;
        }

        bool isTrigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

        std::mutex g_mutex;
        Config g_config;
        bool g_loaded = false;

        Binding key(int code) { return {Source::Key, code, 1}; }
        Binding pad(int code) { return {Source::GamepadButton, code, 1}; }
        Binding axis(int code, int direction) { return {Source::GamepadAxis, code, direction}; }

        void defaults(Config &config)
        {
            config = Config{};
            for (int player = 0; player < kPlayers; ++player)
            {
                Profile &p = config.players[player];
                const struct { int action; int button; } kPad[] = {
                    {Up, SDL_GAMEPAD_BUTTON_DPAD_UP}, {Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
                    {Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
                    {Cross, SDL_GAMEPAD_BUTTON_SOUTH}, {Circle, SDL_GAMEPAD_BUTTON_EAST},
                    {Square, SDL_GAMEPAD_BUTTON_WEST}, {Triangle, SDL_GAMEPAD_BUTTON_NORTH},
                    {L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
                    {L2, kLeftTriggerButton}, {R2, kRightTriggerButton},
                    {L3, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
                    {Start, SDL_GAMEPAD_BUTTON_START}, {Select, SDL_GAMEPAD_BUTTON_BACK}};
                for (const auto &entry : kPad)
                    p.actions[entry.action].push_back(pad(entry.button));
                p.actions[LeftStickLeft].push_back(axis(SDL_GAMEPAD_AXIS_LEFTX, -1));
                p.actions[LeftStickRight].push_back(axis(SDL_GAMEPAD_AXIS_LEFTX, 1));
                p.actions[LeftStickUp].push_back(axis(SDL_GAMEPAD_AXIS_LEFTY, -1));
                p.actions[LeftStickDown].push_back(axis(SDL_GAMEPAD_AXIS_LEFTY, 1));
                p.actions[RightStickLeft].push_back(axis(SDL_GAMEPAD_AXIS_RIGHTX, -1));
                p.actions[RightStickRight].push_back(axis(SDL_GAMEPAD_AXIS_RIGHTX, 1));
                p.actions[RightStickUp].push_back(axis(SDL_GAMEPAD_AXIS_RIGHTY, -1));
                p.actions[RightStickDown].push_back(axis(SDL_GAMEPAD_AXIS_RIGHTY, 1));
            }
            // Player 1's keyboard (US key positions), as before.
            Profile &p1 = config.players[0];
            const struct { int action; int keys[2]; } kKeyboard[] = {
                {Up, {SDL_SCANCODE_UP, SDL_SCANCODE_W}}, {Down, {SDL_SCANCODE_DOWN, SDL_SCANCODE_S}},
                {Left, {SDL_SCANCODE_LEFT, SDL_SCANCODE_A}}, {Right, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D}},
                {Cross, {SDL_SCANCODE_X, SDL_SCANCODE_SPACE}}, {Circle, {SDL_SCANCODE_C, SDL_SCANCODE_ESCAPE}},
                {Square, {SDL_SCANCODE_Z, SDL_SCANCODE_KP_0}}, {Triangle, {SDL_SCANCODE_V, SDL_SCANCODE_KP_1}},
                {L1, {SDL_SCANCODE_Q, 0}}, {R1, {SDL_SCANCODE_E, 0}}, {L2, {SDL_SCANCODE_LSHIFT, 0}},
                {R2, {SDL_SCANCODE_RSHIFT, 0}}, {Start, {SDL_SCANCODE_RETURN, 0}}, {Select, {SDL_SCANCODE_TAB, 0}},
                {L3, {SDL_SCANCODE_F, 0}}, {R3, {SDL_SCANCODE_G, 0}},
                // The movement keys push the left stick fully too (games that move with it), the right stick on I/J/K/L.
                {LeftStickUp, {SDL_SCANCODE_UP, SDL_SCANCODE_W}}, {LeftStickDown, {SDL_SCANCODE_DOWN, SDL_SCANCODE_S}},
                {LeftStickLeft, {SDL_SCANCODE_LEFT, SDL_SCANCODE_A}}, {LeftStickRight, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D}},
                {RightStickUp, {SDL_SCANCODE_I, 0}}, {RightStickDown, {SDL_SCANCODE_K, 0}},
                {RightStickLeft, {SDL_SCANCODE_J, 0}}, {RightStickRight, {SDL_SCANCODE_L, 0}}};
            for (const auto &entry : kKeyboard)
                for (int code : entry.keys)
                    if (code)
                        p1.actions[entry.action].push_back(key(code));
        }

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        }

        void load()
        {
            defaults(g_config);
            std::ifstream file(configPath());
            if (!file)
                return;
            std::string line;
            int player = -1;
            bool mouse = false;
            bool cleared[kPlayers][ActionCount] = {};
            bool seen[kPlayers] = {};
            while (std::getline(file, line))
            {
                line = trim(line.substr(0, line.find('#')));
                if (line.empty())
                    continue;
                if (line.front() == '[')
                {
                    const std::string section = line.substr(1, line.find(']') - 1);
                    player = -1;
                    for (int p = 0; p < kPlayers; ++p)
                        if (section == "player" + std::to_string(p + 1))
                            player = p;
                    // Players 3 and 4 start from player 2's bindings (the fork gave them player 2's): its file has
                    // no section of theirs.
                    if (player >= 0)
                        seen[player] = true;
                    mouse = section == "mouse";
                    continue;
                }
                const size_t equal = line.find('=');
                if (equal == std::string::npos)
                    continue;
                const std::string name = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
                if (mouse)
                {
                    if (name == "sensitivity")
                        g_config.mouse.sensitivity = static_cast<float>(std::atof(value.c_str()));
                    else if (name == "invert_y")
                        g_config.mouse.invertY = value == "true" || value == "1";
                    else if (name == "capture")
                        g_config.mouse.capture = value == "true" || value == "1";
                    continue;
                }
                if (player < 0)
                    continue;
                Profile &profile = g_config.players[player];
                if (name == "gamepad")
                {
                    profile.gamepad = value == "auto" ? -1 : std::clamp(std::atoi(value.c_str()), -1, 3);
                    continue;
                }
                for (int action = 0; action < ActionCount; ++action)
                {
                    if (name != kActionNames[action])
                        continue;
                    // A line replaces the action's defaults (an empty one unbinds it).
                    if (!cleared[player][action])
                    {
                        profile.actions[action].clear();
                        cleared[player][action] = true;
                    }
                    std::stringstream list(value);
                    std::string item;
                    while (std::getline(list, item, ','))
                    {
                        Binding binding;
                        item = trim(item);
                        if (!item.empty() && parseBinding(item, binding))
                            profile.actions[action].push_back(binding);
                        else if (!item.empty())
                            std::fprintf(stderr, "[input] %s: unknown binding '%s' for %s\n", configPath().c_str(), item.c_str(), name.c_str());
                    }
                }
            }
            for (int p = 2; p < kPlayers; ++p)
                if (!seen[p])
                {
                    for (int action = 0; action < ActionCount; ++action)
                        g_config.players[p].actions[action] = g_config.players[1].actions[action];
                }
        }

        void ensureLoaded()
        {
            if (!g_loaded)
            {
                g_loaded = true;
                load();
            }
        }

        float axisValue(SDL_Gamepad *gamepad, int code)
        {
            const float v = static_cast<float>(SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(code))) / 32767.0f;
            return std::clamp(v, -1.0f, 1.0f);
        }

        // How far a binding is pushed, 0..1.
        float strength(const Binding &binding, const Devices &devices, SDL_Gamepad *gamepad)
        {
            switch (binding.source)
            {
            case Source::Key:
                return devices.keys && binding.code >= 0 && binding.code < devices.keyCount && devices.keys[binding.code] ? 1.0f : 0.0f;
            case Source::GamepadButton:
                if (!gamepad)
                    return 0.0f;
                if (binding.code == kLeftTriggerButton || binding.code == kRightTriggerButton)
                    return axisValue(gamepad, binding.code == kLeftTriggerButton ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                                 : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
                return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(binding.code)) ? 1.0f : 0.0f;
            case Source::GamepadAxis:
            {
                if (!gamepad)
                    return 0.0f;
                float value = axisValue(gamepad, binding.code);
                // A resting stick's drift stays centred (raylib's own dead zone did this before).
                if (!isTrigger(binding.code) && std::fabs(value) < 0.1f)
                    value = 0.0f;
                return std::clamp(value * static_cast<float>(binding.direction), 0.0f, 1.0f);
            }
            case Source::MouseButton:
                return devices.mouseButtons & SDL_BUTTON_MASK(binding.code) ? 1.0f : 0.0f;
            case Source::MouseWheel:
                return (binding.direction > 0 ? devices.wheelUp : devices.wheelDown) ? 1.0f : 0.0f;
            case Source::MouseMove:
            {
                const float value = binding.code == 0 ? devices.mouseX : devices.mouseY;
                return std::clamp(value * static_cast<float>(binding.direction), 0.0f, 1.0f);
            }
            }
            return 0.0f;
        }
    }

    std::mutex &configMutex() { return g_mutex; }

    Config &config()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensureLoaded();
        return g_config;
    }

    void resetToDefaults()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_loaded = true;
        defaults(g_config);
    }

    std::string configPath()
    {
        if (const char *path = std::getenv("PS2X_INPUT_CONFIG"); path && *path)
            return path;
        // The port's user folder (bozo/app/app.h), else the current one.
        const std::filesystem::path &dir = userDirectory();
        return (dir.empty() ? std::filesystem::path("input.ini") : dir / "input.ini").string();
    }

    bool save()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensureLoaded();
        std::ofstream file(configPath(), std::ios::trunc);
        if (!file)
            return false;
        file << "# PS2 pad bindings (the Controls window, F2, writes this file). One line per PS2 button or stick\n"
                "# direction, any number of bindings separated by commas: key:<KEY>, pad:<BUTTON>, axis:<AXIS>+ or -,\n"
                "# mouse:<BUTTON>, wheel:UP / wheel:DOWN, mousemove:X+ / X- / Y+ / Y-. An empty line unbinds.\n";
        for (int player = 0; player < kPlayers; ++player)
        {
            const Profile &profile = g_config.players[player];
            file << "\n[player" << (player + 1) << "]\ngamepad = "
                 << (profile.gamepad < 0 ? std::string("auto") : std::to_string(profile.gamepad)) << "\n";
            for (int action = 0; action < ActionCount; ++action)
            {
                file << kActionNames[action] << " =";
                for (size_t i = 0; i < profile.actions[action].size(); ++i)
                    file << (i ? ", " : " ") << bindingName(profile.actions[action][i]);
                file << "\n";
            }
        }
        file << "\n[mouse]\nsensitivity = " << g_config.mouse.sensitivity << "\ninvert_y = "
             << (g_config.mouse.invertY ? "true" : "false") << "\ncapture = " << (g_config.mouse.capture ? "true" : "false") << "\n";
        return static_cast<bool>(file);
    }

    libs::PadInputState padState(int player, const Devices &devices, SDL_Gamepad *gamepad)
    {
        player = std::clamp(player, 0, kPlayers - 1);
        float value[ActionCount] = {};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ensureLoaded();
            const Profile &profile = g_config.players[player];
            for (int action = 0; action < ActionCount; ++action)
                for (const Binding &binding : profile.actions[action])
                    value[action] = std::max(value[action], strength(binding, devices, gamepad));
        }
        libs::PadInputState state;
        for (int button = 0; button < 16; ++button)
            if (value[button] > 0.5f)
                state.buttons |= kButtonBits[button];
        auto stick = [&value](int negative, int positive)
        {
            const float v = std::clamp(value[positive] - value[negative], -1.0f, 1.0f);
            return static_cast<uint8_t>(std::lround(127.5f + v * 127.5f));
        };
        state.lx = stick(LeftStickLeft, LeftStickRight);
        state.ly = stick(LeftStickUp, LeftStickDown);
        state.rx = stick(RightStickLeft, RightStickRight);
        state.ry = stick(RightStickUp, RightStickDown);
        // A pressed button's pressure is how far its binding is pushed: full for keys and digital buttons, the travel
        // of an analog trigger bound to it.
        for (int i = 0; i < 12; ++i)
            for (int button = 0; button < 16; ++button)
                if (kButtonBits[button] == libs::kPadPressureOrder[i] && (state.buttons & kButtonBits[button]))
                    state.pressure[static_cast<size_t>(i)] = static_cast<uint8_t>(std::lround(255.0f * std::min(value[button], 1.0f)));
        // PS2X_INPUT_LOG=1: each change of a player's buttons.
        static const bool log = std::getenv("PS2X_INPUT_LOG") != nullptr;
        static uint16_t s_lastButtons[kPlayers] = {};
        if (log && state.buttons != s_lastButtons[player])
        {
            s_lastButtons[player] = state.buttons;
            std::fprintf(stderr, "[input] t=%.2f player %d buttons %04x\n", static_cast<double>(SDL_GetTicks()) / 1000.0, player + 1,
                         state.buttons);
        }
        return state;
    }

    const char *actionName(int action) { return action >= 0 && action < ActionCount ? kActionNames[action] : "?"; }
    const char *actionLabel(int action) { return action >= 0 && action < ActionCount ? kActionLabels[action] : "?"; }

    std::string bindingName(const Binding &binding)
    {
        // A name, else the fork's number (raylib's code), else SDL's code after "SC" (only this reads that).
        const auto named = [&binding](const char *kind, const Name *table, size_t count)
        {
            const Name *n = byCode(table, count, binding.code);
            if (n && n->name)
                return std::string(kind) + n->name;
            if (n && n->raylib >= 0)
                return std::string(kind) + std::to_string(n->raylib);
            return std::string(kind) + "SC" + std::to_string(binding.code);
        };
        switch (binding.source)
        {
        case Source::Key:
            return named("key:", kKeys, std::size(kKeys));
        case Source::GamepadButton:
            return named("pad:", kPadButtons, std::size(kPadButtons));
        case Source::GamepadAxis:
            return named("axis:", kPadAxes, std::size(kPadAxes)) + (binding.direction > 0 ? "+" : "-");
        case Source::MouseButton:
            return named("mouse:", kMouseButtons, std::size(kMouseButtons));
        case Source::MouseWheel:
            return binding.direction > 0 ? "wheel:UP" : "wheel:DOWN";
        case Source::MouseMove:
            return std::string("mousemove:") + (binding.code == 0 ? "X" : "Y") + (binding.direction > 0 ? "+" : "-");
        }
        return "?";
    }

    bool parseBinding(const std::string &text, Binding &binding)
    {
        const size_t colon = text.find(':');
        if (colon == std::string::npos)
            return false;
        const std::string kind = text.substr(0, colon);
        std::string name = text.substr(colon + 1);
        int direction = 1;
        if ((kind == "axis" || kind == "mousemove") && !name.empty() && (name.back() == '+' || name.back() == '-'))
        {
            direction = name.back() == '+' ? 1 : -1;
            name.pop_back();
        }
        const auto lookup = [&name](const Name *table, size_t count, int &code)
        {
            if (name.size() > 2 && name.compare(0, 2, "SC") == 0)
            {
                char *end = nullptr;
                const long value = std::strtol(name.c_str() + 2, &end, 10);
                if (end != name.c_str() + 2 && *end == '\0')
                {
                    code = static_cast<int>(value);
                    return true;
                }
            }
            return codeOf(table, count, name, code);
        };
        binding.direction = direction;
        if (kind == "key")
        {
            binding.source = Source::Key;
            return lookup(kKeys, std::size(kKeys), binding.code);
        }
        if (kind == "pad")
        {
            binding.source = Source::GamepadButton;
            return lookup(kPadButtons, std::size(kPadButtons), binding.code);
        }
        if (kind == "axis")
        {
            binding.source = Source::GamepadAxis;
            return lookup(kPadAxes, std::size(kPadAxes), binding.code);
        }
        if (kind == "mouse")
        {
            binding.source = Source::MouseButton;
            return lookup(kMouseButtons, std::size(kMouseButtons), binding.code);
        }
        if (kind == "wheel" && (name == "UP" || name == "DOWN"))
        {
            binding.source = Source::MouseWheel;
            binding.code = 0;
            binding.direction = name == "UP" ? 1 : -1;
            return true;
        }
        if (kind == "mousemove" && (name == "X" || name == "Y"))
        {
            binding.source = Source::MouseMove;
            binding.code = name == "X" ? 0 : 1;
            return true;
        }
        return false;
    }

    bool bindingFromEvent(const SDL_Event &event, Binding &binding)
    {
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat)
                return false;
            binding = key(event.key.scancode);
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            binding = {Source::MouseButton, event.button.button, 1};
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.y == 0.0f)
                return false;
            binding = {Source::MouseWheel, 0, event.wheel.y > 0.0f ? 1 : -1};
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            if (std::fabs(event.motion.xrel) <= 40.0f && std::fabs(event.motion.yrel) <= 40.0f)
                return false;
            {
                const bool x = std::fabs(event.motion.xrel) >= std::fabs(event.motion.yrel);
                const float v = x ? event.motion.xrel : event.motion.yrel;
                binding = {Source::MouseMove, x ? 0 : 1, v > 0.0f ? 1 : -1};
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            binding = pad(event.gbutton.button);
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            const float value = static_cast<float>(event.gaxis.value) / 32767.0f;
            if (std::fabs(value) <= 0.6f)
                return false;
            binding = axis(event.gaxis.axis, value > 0.0f ? 1 : -1);
            return true;
        }
        default:
            return false;
        }
    }
}
