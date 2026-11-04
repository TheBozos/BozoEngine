// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// Tabs a port adds to the settings window (F2, bozo/app/panel.h) beside Controls and Video: a name and a callback
// drawing ImGui widgets, called on the window's thread each frame the tab is open (between ImGui::BeginTabItem and
// EndTabItem). Register from the port's install (any thread, before or after the window opens). Moved from the team's
// ps2_debug_ui (phase 4, milestone M4). In bozo_ui (ImGui's core, no SDL3): a headless program keeps the tabs and never
// draws them; code that includes imgui.h guards it with BOZO_SETTINGS_UI (defined by linking bozo_ui).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bozo::app::ui
{
    struct Tab
    {
        std::string name;
        std::function<void()> draw;
    };

    // A second registration under the same name replaces the first.
    void registerTab(const std::string &name, std::function<void()> draw);
    // A copy, in registration order.
    std::vector<Tab> tabs();
}
