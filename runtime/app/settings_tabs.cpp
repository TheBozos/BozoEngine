// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The settings window's port tabs (bozo/app/settings_tabs.h). Moved from the team's ps2_debug_ui.cpp.
#include "bozo/app/settings_tabs.h"

#include <mutex>

namespace bozo::app::ui
{
    namespace
    {
        std::mutex g_mutex;
        std::vector<Tab> g_tabs;
    }

    void registerTab(const std::string &name, std::function<void()> draw)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Tab &tab : g_tabs)
            if (tab.name == name)
            {
                tab.draw = std::move(draw);
                return;
            }
        g_tabs.push_back(Tab{name, std::move(draw)});
    }

    std::vector<Tab> tabs()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_tabs;
    }
}
