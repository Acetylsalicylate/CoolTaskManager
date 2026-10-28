#pragma once
#include "ProcessTab.h"
#include "HardwareTab.h"
#include "imgui.h"

// Root of the UI: a single borderless window that always fills the screen, with one tab per page. The window is not
// resizable or movable on purpose, so the layout never has to deal with a floating window.
class MainWindow {
public:
    MainWindow() = default;

    void render() {
        // Nothing sensible can be laid out in a zero-sized window (minimized or just restored): skip the frame.
        const ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (display_size.x < 1.0f || display_size.y < 1.0f) return;

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(display_size, ImGuiCond_Always);
        
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | 
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;

        ImGui::Begin("MainWindow", nullptr, flags);

        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem("Processes")) {
                processTab.render();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hardware")) {
                hardwareTab.render();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    // Releases GPU resources owned by the tabs. Called from main() while the OpenGL context is still current.
    void releaseGpuResources() { processTab.releaseIcons(); }

private:
    ProcessTab processTab;
    HardwareTab hardwareTab;
};