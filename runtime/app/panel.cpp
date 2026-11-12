// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The settings window (F2) and the frame rate counter (F3) (bozo/app/panel.h).
// - The host (ImGui's context and backends, the keys, the tab bar, the counter) is written fresh for BozoEngine: the
//   fork's panel host lived in a file that cannot move (phase4-inventory.md section 2.2).
// - The Settings window's Video and Controls tabs are moved from the team's lines of the fork's ps2_debug_panel.cpp
//   (2230-2519, phase4-inventory.md section 2.1): raylib's key reads replaced by SDL3 events
//   (input::bindingFromEvent), four players instead of two, no GS scale (kept in video.ini, not applied).
#include "bozo/app/panel.h"

#include "bozo/app/app.h"
#include "bozo/app/input_bindings.h"
#include "bozo/app/settings_tabs.h"
#include "bozo/app/video_settings.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace bozo::app
{
    struct Panel::Impl
    {
        SDL_Window *window = nullptr;
        bool ready = false;
        bool settings = false; // F2
        bool overlay = false;  // F3
        std::string openTab;   // PS2X_SETTINGS_OPEN's tab, selected once
        std::string iniPath;   // imgui.ini (ImGui keeps the pointer)
        int controlsPlayer = 0;
        int bindingAction = -1; // the action an Add waits an input for (-1: none)
        double gameFps = 0.0, presentFps = 0.0, frameMs = 0.0;

        void drawSettings();
        void drawVideoTab();
        void drawControlsTab();
        void drawOverlay();
    };

    Panel::Panel() : impl_(new Impl) {}
    Panel::~Panel()
    {
        shutdown();
        delete impl_;
    }

    bool Panel::init(SDL_Window *window, void *glContext)
    {
        Impl &m = *impl_;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        const std::filesystem::path &user = userDirectory();
        m.iniPath = (user.empty() ? std::filesystem::path("imgui.ini") : user / "imgui.ini").string();
        io.IniFilename = m.iniPath.c_str();
        ImGui::StyleColorsDark();
        // The monitor's scale (a 4K screen at 150 %: the panel at 150 %).
        const float scale = std::clamp(SDL_GetWindowDisplayScale(window), 1.0f, 4.0f);
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext) || !ImGui_ImplOpenGL3_Init("#version 330"))
        {
            std::fprintf(stderr, "[panel] ImGui's SDL3 / OpenGL 3 backends could not start: no settings window\n");
            ImGui::DestroyContext();
            return false;
        }
        m.window = window;
        m.ready = true;
        if (const char *show = std::getenv("PS2X_SHOW_FPS"); show && *show == '1')
            m.overlay = true;
        if (const char *open = std::getenv("PS2X_SETTINGS_OPEN"); open && *open)
        {
            m.settings = true;
            m.openTab = open;
        }
        return true;
    }

    void Panel::shutdown()
    {
        Impl &m = *impl_;
        if (!m.ready)
            return;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m.ready = false;
    }

    void Panel::handleEvent(const SDL_Event &event)
    {
        Impl &m = *impl_;
        if (!m.ready)
            return;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
        {
            if (event.key.scancode == SDL_SCANCODE_F2)
            {
                m.settings = !m.settings;
                if (!m.settings)
                    m.bindingAction = -1;
                return;
            }
            if (event.key.scancode == SDL_SCANCODE_F3)
            {
                m.overlay = !m.overlay;
                return;
            }
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
        // Controls: the first input since Add was clicked (mouse input only over the game, not over the panel).
        if (m.settings && m.bindingAction >= 0)
        {
            input::Binding binding;
            if (input::bindingFromEvent(event, binding))
            {
                const bool mouse = binding.source == input::Source::MouseButton || binding.source == input::Source::MouseWheel ||
                                   binding.source == input::Source::MouseMove;
                if (!(mouse && ImGui::GetIO().WantCaptureMouse))
                {
                    // config() takes the same (non-recursive) mutex: fetched before the lock, as the other callers do.
                    // Under the lock it threw "resource deadlock would occur" at the first input after Add.
                    input::Config &config = input::config();
                    std::lock_guard<std::mutex> lock(input::configMutex());
                    std::vector<input::Binding> &list = config.players[m.controlsPlayer].actions[m.bindingAction];
                    if (std::find(list.begin(), list.end(), binding) == list.end())
                        list.push_back(binding);
                    m.bindingAction = -1;
                }
            }
        }
    }

    void Panel::setRates(double gameFramesPerSecond, double presentsPerSecond, double frameMs)
    {
        impl_->gameFps = gameFramesPerSecond;
        impl_->presentFps = presentsPerSecond;
        impl_->frameMs = frameMs;
    }

    bool Panel::settingsOpen() const { return impl_->ready && impl_->settings; }
    bool Panel::overlayShown() const { return impl_->ready && impl_->overlay; }
    bool Panel::holdsKeyboard() const
    {
        return impl_->ready && impl_->settings && (impl_->bindingAction >= 0 || ImGui::GetIO().WantTextInput);
    }
    bool Panel::holdsMouse() const { return impl_->ready && impl_->settings; }

    void Panel::draw()
    {
        Impl &m = *impl_;
        if (!m.ready)
            return;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (m.settings)
            m.drawSettings();
        if (m.overlay)
            m.drawOverlay();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    // ---- F3: the frame rate counter ----

    void Panel::Impl::drawOverlay()
    {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const float pad = 10.0f * ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad), ImGuiCond_Always,
                                ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.45f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##fps", nullptr, flags))
        {
            // The game's frames a second: what the player gets (the presents can only show those).
            const ImVec4 colour = gameFps >= 59.0 ? ImVec4(0.55f, 1.0f, 0.55f, 1.0f)
                                  : gameFps >= 50.0 ? ImVec4(1.0f, 0.85f, 0.35f, 1.0f)
                                                    : ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
            ImGui::TextColored(colour, "%.1f FPS", gameFps);
            ImGui::TextDisabled("%.1f ms a frame, %.0f presents/s", frameMs, presentFps);
        }
        ImGui::End();
    }

    // ---- F2: the settings window ----

    void Panel::Impl::drawSettings()
    {
        const float unit = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextWindowSize(ImVec2(640.0f * unit, 600.0f * unit), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Settings (F2)", &settings))
        {
            ImGui::End();
            return;
        }
        if (!settings)
            bindingAction = -1;
        if (ImGui::BeginTabBar("settings-tabs"))
        {
            // PS2X_SETTINGS_OPEN's tab, selected once.
            const auto flags = [this](const char *name)
            {
                if (openTab.empty() || openTab != name)
                    return ImGuiTabItemFlags_None;
                openTab.clear();
                return ImGuiTabItemFlags_SetSelected;
            };
            if (ImGui::BeginTabItem("Controls", nullptr, flags("Controls")))
            {
                drawControlsTab();
                ImGui::EndTabItem();
            }
            else
                bindingAction = -1;
            if (ImGui::BeginTabItem("Video", nullptr, flags("Video")))
            {
                drawVideoTab();
                ImGui::EndTabItem();
            }
            // The port's own tabs (bozo::app::ui::registerTab).
            for (const ui::Tab &tab : ui::tabs())
                if (ImGui::BeginTabItem(tab.name.c_str(), nullptr, flags(tab.name.c_str())))
                {
                    if (tab.draw)
                        tab.draw();
                    ImGui::EndTabItem();
                }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void Panel::Impl::drawVideoTab()
    {
        video::Settings &edited = video::edited();
        const video::Settings &current = video::current();
        const float unit = ImGui::GetStyle().FontScaleDpi;
        // What the environment sets wins over video.ini at the next launch too.
        const auto envNote = [](const char *name)
        {
            if (const char *value = std::getenv(name); value && *value)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s=%s at launch)", name, value);
            }
        };

        ImGui::SeparatorText("Display");
        static const char *kModes[] = {"Windowed", "Fullscreen", "Borderless window"};
        int mode = static_cast<int>(edited.mode);
        ImGui::SetNextItemWidth(240.0f * unit);
        if (ImGui::Combo("Mode", &mode, kModes, IM_ARRAYSIZE(kModes)))
            edited.mode = static_cast<video::WindowMode>(mode);
        envNote("PS2X_WINDOW_MODE");

        const std::vector<std::pair<int, int>> sizes = video::resolutions();
        const auto [monitorWidth, monitorHeight] = video::monitorSize();
        const auto label = [&](int width, int height)
        {
            const int divisor = std::gcd(width, height);
            int ratioW = divisor ? width / divisor : 0, ratioH = divisor ? height / divisor : 0;
            if (ratioW == 8 && ratioH == 5)
                ratioW = 16, ratioH = 10;
            std::string text = std::to_string(width) + " x " + std::to_string(height);
            if (ratioW != 0 && ratioW <= 21 && ratioH <= 10)
                text += "  (" + std::to_string(ratioW) + ":" + std::to_string(ratioH) + ")";
            else if (std::abs(width * 9 - height * 16) < width / 50)
                text += "  (16:9)";
            if (width == monitorWidth && height == monitorHeight)
                text += "  monitor";
            return text;
        };
        const bool borderless = edited.mode == video::WindowMode::Borderless;
        ImGui::BeginDisabled(borderless);
        ImGui::SetNextItemWidth(240.0f * unit);
        if (ImGui::BeginCombo("Resolution", label(edited.width, edited.height).c_str()))
        {
            for (const auto &[width, height] : sizes)
            {
                const bool selected = width == edited.width && height == edited.height;
                if (ImGui::Selectable(label(width, height).c_str(), selected))
                    edited.width = width, edited.height = height;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        envNote("PS2X_WINDOW_SIZE");
        if (borderless)
            ImGui::TextDisabled("Borderless covers the monitor (%d x %d); the resolution is the window's when it leaves.", monitorWidth,
                                monitorHeight);
        else if (edited.mode == video::WindowMode::Fullscreen)
            ImGui::TextDisabled("Fullscreen sets the monitor to this resolution.");
        else
            ImGui::TextDisabled("The window can also be resized with the mouse.");

        ImGui::SeparatorText("Rendering");
        static const float kScales[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        static const char *kScaleNames[] = {"50%", "75%", "100% (the window's)", "150%", "200% (supersampled)"};
        int scaleIndex = 2;
        for (int i = 0; i < IM_ARRAYSIZE(kScales); ++i)
            if (std::fabs(edited.renderScale - kScales[i]) < 0.01f)
                scaleIndex = i;
        ImGui::SetNextItemWidth(240.0f * unit);
        if (ImGui::Combo("Render scale", &scaleIndex, kScaleNames, IM_ARRAYSIZE(kScaleNames)))
            edited.renderScale = kScales[scaleIndex];
        envNote("PS2X_RENDER_SCALE");
        ImGui::TextDisabled("The 3D against the window's resolution (applies at once).");

        // The window's frame rate: the game's 60, or the monitor's with the frames between the game's interpolated (a set
        // rate, from video.ini or PS2X_FRAME_RATE, is offered as it is).
        const int refresh = video::monitorRefresh();
        std::vector<std::pair<int, std::string>> rates = {{0, "60 (the game's)"},
                                                          {-1, "Match the monitor (" + std::to_string(refresh) + " Hz), smoothed (experimental)"}};
        for (const int set : {current.frameRate, edited.frameRate})
            if (set > 0 && std::none_of(rates.begin(), rates.end(), [&](const auto &r) { return r.first == set; }))
                rates.emplace_back(set, std::to_string(set) + " a second, smoothed (no vsync, experimental)");
        int rateIndex = 0;
        for (size_t i = 0; i < rates.size(); ++i)
            if (rates[i].first == edited.frameRate)
                rateIndex = static_cast<int>(i);
        ImGui::SetNextItemWidth(240.0f * unit);
        if (ImGui::BeginCombo("Frame rate", rates[static_cast<size_t>(rateIndex)].second.c_str()))
        {
            for (size_t i = 0; i < rates.size(); ++i)
                if (ImGui::Selectable(rates[i].second.c_str(), static_cast<int>(i) == rateIndex))
                    edited.frameRate = rates[i].first;
            ImGui::EndCombo();
        }
        envNote("PS2X_FRAME_RATE");
        ImGui::TextDisabled("The game runs at 60. Above that, each frame is drawn between the game's two latest:");
        ImGui::TextDisabled("smoother on a high-refresh monitor, one game frame (17 ms) more input lag. Applies at once.");
        ImGui::TextDisabled("Experimental: a few effects (rain, lens flares, some trails) still move at 60.");
        ImGui::TextDisabled("The picture keeps its 4:3, centred with bars, whatever the window's shape.");

        ImGui::Separator();
        const auto [windowWidth, windowHeight] = video::windowSize();
        ImGui::Text("Now: %d x %d, %s, render scale %d%%, frame rate %s; window %d x %d", current.width, current.height,
                    video::modeName(current.mode), static_cast<int>(std::lround(current.renderScale * 100.0f)),
                    video::frameRateName(current.frameRate).c_str(), windowWidth, windowHeight);
        static std::string s_status;
        if (ImGui::Button("Apply"))
        {
            video::requestApply();
            s_status = "applied (not saved)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply and save"))
        {
            video::requestApply();
            s_status = video::save() ? "saved to " + video::configPath() : "could not write " + video::configPath();
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            edited = current;
            s_status.clear();
        }
        ImGui::TextUnformatted(s_status.c_str());
        ImGui::TextDisabled("F11 or Alt+Enter switches between the window and full screen.");
    }

    void Panel::Impl::drawControlsTab()
    {
        input::Config &config = input::config();
        std::mutex &mutex = input::configMutex();
        const float unit = ImGui::GetStyle().FontScaleDpi;

        for (int player = 0; player < input::kPlayers; ++player)
        {
            if (player)
                ImGui::SameLine();
            char name[16];
            std::snprintf(name, sizeof(name), "Player %d", player + 1);
            if (ImGui::RadioButton(name, &controlsPlayer, player))
                bindingAction = -1;
        }
        input::Profile &profile = config.players[controlsPlayer];
        {
            std::lock_guard<std::mutex> lock(mutex);
            ImGui::SameLine(0.0f, 30.0f * unit);
            ImGui::SetNextItemWidth(90.0f * unit);
            ImGui::InputInt("Gamepad (-1: auto)", &profile.gamepad);
            profile.gamepad = std::clamp(profile.gamepad, -1, 3);
        }
        ImGui::TextWrapped("Add: then press a key or a gamepad button, push a stick or trigger, or click, roll the wheel or "
                           "move the mouse over the game picture. Click a binding to remove it.");

        if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, ImGui::GetContentRegionAvail().y - 120.0f * unit)))
        {
            ImGui::TableSetupColumn("PS2", ImGuiTableColumnFlags_WidthFixed, 130.0f * unit);
            ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f * unit);
            for (int action = 0; action < input::ActionCount; ++action)
            {
                ImGui::PushID(action);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(input::actionLabel(action));
                ImGui::TableNextColumn();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    std::vector<input::Binding> &list = profile.actions[action];
                    for (size_t i = 0; i < list.size(); ++i)
                    {
                        ImGui::PushID(static_cast<int>(i));
                        if (i)
                            ImGui::SameLine();
                        if (ImGui::SmallButton(input::bindingName(list[i]).c_str()))
                        {
                            list.erase(list.begin() + static_cast<long>(i));
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::TableNextColumn();
                if (bindingAction == action)
                {
                    if (ImGui::SmallButton("Cancel"))
                        bindingAction = -1;
                }
                else if (ImGui::SmallButton("Add"))
                    bindingAction = action; // the next input event (Panel::handleEvent)
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Mouse");
        {
            std::lock_guard<std::mutex> lock(mutex);
            ImGui::SetNextItemWidth(200.0f * unit);
            ImGui::SliderFloat("Sensitivity", &config.mouse.sensitivity, 0.1f, 5.0f, "%.2f");
            ImGui::SameLine();
            ImGui::Checkbox("Invert Y", &config.mouse.invertY);
            ImGui::SameLine();
            ImGui::Checkbox("Capture the cursor while playing", &config.mouse.capture);
        }
        ImGui::Separator();
        static std::string s_status;
        if (ImGui::Button("Save"))
            s_status = input::save() ? "saved to " + input::configPath() : "could not write " + input::configPath();
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults"))
        {
            input::resetToDefaults();
            s_status = "defaults (not saved yet)";
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(s_status.c_str());
    }
}
