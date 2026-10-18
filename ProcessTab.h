#pragma once
#include <vector>
#include <string>
#include <windows.h>
#include <tlhelp32.h>
#include "imgui.h"

struct ProcessInfo {
    DWORD pid;
    DWORD ppid;
    DWORD threads;
    std::string name;
};

class ProcessTab {
public:
    ProcessTab() {
        loadProcesses();
    }

    void loadProcesses() {
        processes.clear();
        HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapShot == INVALID_HANDLE_VALUE) return;

        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(hSnapShot, &processEntry) != FALSE) {
            while (Process32NextW(hSnapShot, &processEntry) != FALSE) {
                char buf[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, processEntry.szExeFile, -1, buf, MAX_PATH, nullptr, nullptr);

                processes.push_back({
                    processEntry.th32ProcessID,
                    processEntry.th32ParentProcessID,
                    processEntry.cntThreads,
                    std::string(buf)
                    });
            }
        }
        CloseHandle(hSnapShot);
    }

    void render() {
        if (ImGui::Button("Update")) {
            loadProcesses();
        }

        ImGui::Spacing();

        if (ImGui::BeginTable("ProcessTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 550))) {
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("PPID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Process name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& p : processes) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%lu", p.pid);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%lu", p.ppid);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%lu", p.threads);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", p.name.c_str());
            }
            ImGui::EndTable();
        }
    }

private:
    std::vector<ProcessInfo> processes;
};