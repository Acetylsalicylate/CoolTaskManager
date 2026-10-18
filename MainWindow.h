#pragma once
#include "ProcessTab.h"
#include "HardwareTab.h"
#include "imgui.h"

class MainWindow {
public:
    MainWindow() = default;

    void render() {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        
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

private:
    ProcessTab processTab;
    HardwareTab hardwareTab;
};