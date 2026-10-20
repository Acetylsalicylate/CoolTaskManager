#pragma once
#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "imgui.h"

// One row of the process table; the fields come straight from PROCESSENTRY32W.
struct ProcessInfo {
    DWORD pid = 0;      // th32ProcessID
    DWORD ppid = 0;     // th32ParentProcessID
    DWORD threads = 0;  // cntThreads
    std::string name;   // szExeFile, converted to UTF-8
};

// Lists running processes via a Toolhelp32 snapshot. The list is refreshed by the Update button or once per second
// when automatic refresh is enabled.
class ProcessTab {
public:
    ProcessTab() { refresh(); }

    void render() {
        if (ImGui::Button("Update")) {
            refresh();
        }

        ImGui::SameLine();
        // Periodic refresh on/off. Restarting the period on toggle avoids an immediate second refresh.
        if (ImGui::Button(autoRefresh_ ? "Auto: ON (1 s)" : "Auto: OFF")) {
            autoRefresh_ = !autoRefresh_;
            lastRefresh_ = Clock::now();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%zu processes", processes_.size());

        if (!status_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", status_.c_str());
        }

        if (autoRefresh_ && Clock::now() - lastRefresh_ >= kRefreshInterval) {
            refresh();
        }

        ImGui::Spacing();

        float table_height = ImGui::GetContentRegionAvail().y;
        if (table_height < 50.0f) table_height = 50.0f;

        const ImGuiTableFlags flags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

        if (ImGui::BeginTable("ProcessTable", 4, flags, ImVec2(0.0f, table_height))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("PPID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Process name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& process : processes_) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%lu", process.pid);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%lu", process.ppid);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%lu", process.threads);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(process.name.c_str());
            }
            ImGui::EndTable();
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kRefreshInterval = std::chrono::seconds(1);

    // Rebuilds the list from a fresh Toolhelp32 snapshot. The snapshot is a point-in-time copy, so it has to be
    // taken again for every refresh.
    void refresh() {
        processes_.clear();
        status_.clear();

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            status_ = std::format("process snapshot failed (Win32 error {})", GetLastError());
            lastRefresh_ = Clock::now();
            return;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        // Process32FirstW fills the first entry, so the loop starts with do/while: otherwise the first process
        // (usually [System Process]) would be skipped.
        if (Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                processes_.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.cntThreads,
                                      toUtf8(entry.szExeFile)});
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }

        CloseHandle(snapshot);
        lastRefresh_ = Clock::now();
    }

    // Toolhelp32 returns UTF-16 names, while ImGui (and the rest of the UI) expects UTF-8.
    static std::string toUtf8(const wchar_t* text) {
        if (text == nullptr) return {};

        // First pass: required buffer size, including the terminating null.
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return {};

        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
        result.pop_back();  // drop the terminating null
        return result;
    }

    std::vector<ProcessInfo> processes_{};
    std::string status_{};  // non-empty only when the last refresh failed
    bool autoRefresh_ = false;
    Clock::time_point lastRefresh_ = Clock::now();
};
