// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The settings window's Profiler tab (F2): the sampling profiler (bozo/app/profiler.h) started and stopped, each
// sampled thread's most sampled functions with their share, refreshed twice a second while it runs, and a report saved
// to the user folder's profiles/ (the current folder's without one).
#include "bozo/app/app.h"
#include "bozo/app/profiler.h"
#include "bozo/app/settings_tabs.h"
#include "bozo/app/window.h"

#include "imgui.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace bozo::app
{
    std::string profilesFolder()
    {
        const std::filesystem::path &user = userDirectory();
        return ((user.empty() ? std::filesystem::path(".") : user) / "profiles").string();
    }

    void registerProfilerTab()
    {
        ui::registerTab("Profiler", [] {
            static profiler::Profile shown;
            static std::chrono::steady_clock::time_point refreshed{};
            static int thread = 0;
            static std::string saved;
            if (!profiler::available())
            {
                ImGui::TextWrapped("Sampling is not available on this platform.");
                return;
            }
            ImGui::TextWrapped("Samples where the game's threads spend their time, about every millisecond: the function each "
                               "sample falls in (recompiled game functions carry their PS2 address). Start it, play the slow "
                               "part, stop it.");
            ImGui::Spacing();
            const bool running = profiler::running();
            if (ImGui::Button(running ? "Stop" : "Start", ImVec2(120.0f * ImGui::GetStyle().FontScaleDpi, 0.0f)))
            {
                if (running)
                    profiler::stop();
                else
                {
                    profiler::start();
                    saved.clear();
                }
                refreshed = {};
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(shown.threads.empty());
            if (ImGui::Button("Save report"))
            {
                const std::string path = profiler::reportPath(profilesFolder());
                saved = profiler::writeReport(path) ? "Saved to " + path : "Could not write " + path;
                notify(saved);
            }
            ImGui::EndDisabled();
            if (!saved.empty())
                ImGui::TextDisabled("%s", saved.c_str());

            const auto now = std::chrono::steady_clock::now();
            if (refreshed == std::chrono::steady_clock::time_point{} || (running && now - refreshed > std::chrono::milliseconds(500)))
            {
                shown = profiler::profile(60);
                refreshed = now;
            }
            ImGui::Text("%s: %.1f s", running ? "Sampling" : "Sampled", shown.seconds);
            if (shown.threads.empty())
            {
                ImGui::TextDisabled("No thread registered yet (the game's thread registers when it starts).");
                return;
            }
            if (thread >= static_cast<int>(shown.threads.size()))
                thread = 0;
            for (int i = 0; i < static_cast<int>(shown.threads.size()); ++i)
            {
                if (i)
                    ImGui::SameLine();
                const std::string label = shown.threads[static_cast<size_t>(i)].name + " (" +
                                          std::to_string(shown.threads[static_cast<size_t>(i)].samples) + ")";
                ImGui::RadioButton(label.c_str(), &thread, i);
            }
            const profiler::Thread &t = shown.threads[static_cast<size_t>(thread)];
            if (ImGui::BeginTable("profile", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 70.0f * ImGui::GetStyle().FontScaleDpi);
                ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 80.0f * ImGui::GetStyle().FontScaleDpi);
                ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const profiler::Function &f : t.functions)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%5.1f%%", t.samples ? 100.0 * double(f.samples) / double(t.samples) : 0.0);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(f.samples));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(f.name.c_str());
                }
                ImGui::EndTable();
            }
        });
    }
} // namespace bozo::app
