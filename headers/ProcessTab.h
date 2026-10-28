#pragma once
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ProcessIcons.h"
#include "imgui.h"

#include <GLFW/glfw3.h>

// One row of the process table. The base fields come from PROCESSENTRY32W, everything else from per-process Win32
// queries (QueryFullProcessImageNameW, GetProcessTimes, GetProcessMemoryInfo, ...), which may fail for protected
// processes; such fields stay empty / zero and are shown as "-".
struct ProcessInfo {
    DWORD pid = 0;               // th32ProcessID
    DWORD ppid = 0;              // th32ParentProcessID
    DWORD threads = 0;           // cntThreads
    DWORD session = 0;           // ProcessIdToSessionId
    DWORD handles = 0;           // GetProcessHandleCount
    DWORD priority = 0;          // GetPriorityClass
    std::uint64_t creation = 0;  // process start time, FILETIME as 64-bit: used to detect PID reuse
    std::uint64_t cpu_time = 0;  // kernel + user time in 100 ns units
    double cpu_percent = 0.0;    // share of the whole machine, 0..100; only meaningful after a second refresh
    bool has_cpu_percent = false;
    std::size_t working_set = 0;
    std::size_t private_bytes = 0;
    std::size_t pagefile_bytes = 0;
    std::string name;  // szExeFile, converted to UTF-8
    std::string path;  // full image path
    std::string user;  // DOMAIN\User owning the process token
};

// Lists running processes via a Toolhelp32 snapshot. The list is refreshed by the Update button or once per second
// when automatic refresh is enabled.
class ProcessTab {
public:
    ProcessTab() { refresh(); }

    // Frees the icon textures. Must be called while the OpenGL context is still current (see main.cpp).
    void releaseIcons() { icons_.release(); }

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
        if (ImGui::Button(group_by_ppid_ ? "Group: PPID" : "Group: none")) {
            group_by_ppid_ = !group_by_ppid_;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(group_by_ppid_ ? "Parent/child tree; click again to show a flat list"
                                             : "Show processes as a parent/child tree built from PPID");
        }

        if (!status_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", status_.c_str());
        }

        // Low integrity (a sandboxed or low-integrity launcher started the app) cannot drive Explorer, so the folder
        // button would silently fail: say so before the user clicks it and offer to restart with more rights.
        if (!integrity_checked_) {
            const std::wstring level = integrityLevel();
            low_integrity_ = level == L"Low" || level == L"Untrusted";
            integrity_checked_ = true;
        }
        if (low_integrity_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "low integrity: Explorer integration is blocked");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Windows does not let low-integrity processes open folders.\n"
                                  "Restarting as administrator lifts this restriction.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart as administrator")) {
                restartElevated();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Restart CoolTaskManager through a UAC prompt.\n"
                                  "An elevated process can open folders and read the details of protected processes.");
            }
        }

        if (autoRefresh_ && Clock::now() - lastRefresh_ >= kRefreshInterval) {
            refresh();
        }
        // CPU usage is a delta between two snapshots, so take a second sample shortly after startup to make the
        // percentages meaningful without waiting for a manual refresh.
        if (refresh_count_ == 1 && Clock::now() - lastRefresh_ >= kPrimingInterval) {
            refresh();
        }

        // Search row: matches the executable name (case-insensitive substring) and PID/PPID (digit substring).
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        ImGui::InputTextWithHint("##process-search", "Search by name, PID or PPID", search_, sizeof(search_));
        if (search_[0] != '\0') {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) search_[0] = '\0';
        }

        // Which rows pass the search; in the tree view the ancestors of a match stay visible to keep the context.
        const bool filtering = search_[0] != '\0';
        std::vector<char> visible(processes_.size(), 1);
        std::size_t matched = processes_.size();
        if (filtering) {
            matched = 0;
            for (std::size_t i = 0; i < processes_.size(); ++i) {
                visible[i] = matchesSearch(processes_[i]) ? 1 : 0;
                matched += visible[i];
            }
            if (group_by_ppid_) keepAncestors(visible);
        }

        ImGui::SameLine();
        const std::string count_label = filtering ? std::format("{} of {} processes", matched, processes_.size())
                                                  : std::format("{} processes", processes_.size());
        ImGui::TextDisabled("%s", count_label.c_str());

        ImGui::Spacing();

        float table_height = ImGui::GetContentRegionAvail().y;
        if (table_height < 50.0f) table_height = 50.0f;

        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;

        if (ImGui::BeginTable("ProcessTable", 11, flags, ImVec2(0.0f, table_height))) {
            // The PID column and the header stay visible while the rows scroll; all columns are fixed so that widening
            // one pushes the following ones to the right instead of squeezing them. Widths are auto-fitted by ImGui:
            // each column starts as wide as its longest value, or as its header if that is wider.
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("PPID", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Sess", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("RAM", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Handles", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            // Clicking a header changes the sort order; the list is re-sorted on every refresh as well, so the order
            // survives new snapshots. In grouped mode the same order is applied inside every sibling group.
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsDirty || resort_needed_) {
                    sortProcesses(*sort_specs);
                    sort_column_ = sort_specs->SpecsCount > 0 ? static_cast<int>(sort_specs->Specs[0].ColumnIndex) : 0;
                    sort_ascending_ = sort_specs->SpecsCount == 0 ||
                                      sort_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    sort_specs->SpecsDirty = false;
                    resort_needed_ = false;
                }
            }

            last_row_top_ = 0.0f;  // restart the row-geometry tracking for this frame

            if (group_by_ppid_) {
                renderGroupedRows(visible);
            } else {
                for (std::size_t i = 0; i < processes_.size(); ++i) {
                    if (!visible[i]) continue;
                    const ProcessInfo& process = processes_[i];
                    ImGui::TableNextRow();
                    highlightRowUnderMouse();
                    drawIdentityCells(process);
                    ImGui::TableSetColumnIndex(2);
                    drawIcon(process);
                    ImGui::TextUnformatted(process.name.c_str());
                    if (ImGui::IsItemHovered()) showDetails(process);
                    drawDetailCells(process);
                }
            }
            ImGui::EndTable();
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kRefreshInterval = std::chrono::seconds(1);
    static constexpr auto kPrimingInterval = std::chrono::milliseconds(250);

    // Per-PID state kept between refreshes to turn absolute CPU times into percentages.
    struct CpuSample {
        std::uint64_t creation = 0;
        std::uint64_t cpu_time = 0;
        Clock::time_point wall{};
    };

    // ----- row rendering -------------------------------------------------------------------------------------------

    // PID and PPID always come first; the Name cell is drawn separately because the grouped view puts a tree arrow
    // there and the flat view does not.
    void drawIdentityCells(const ProcessInfo& process) const {
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%lu", process.pid);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%lu", process.ppid);
    }

    // The shell icon is only available once the executable path is known (protected processes have none).
    void drawIcon(const ProcessInfo& process) {
        if (const ImTextureID icon = icons_.get(process.path)) {
            const float icon_size = ImGui::GetFontSize();
            ImGui::Image(icon, ImVec2(icon_size, icon_size));
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        }
    }

    void drawDetailCells(const ProcessInfo& process) {
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(process.user.empty() ? "-" : process.user.c_str());

        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%lu", process.session);

        ImGui::TableSetColumnIndex(5);
        if (process.has_cpu_percent) {
            const float share = static_cast<float>(process.cpu_percent);
            const ImVec4 color = share >= 50.0f   ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                                 : share >= 10.0f ? ImVec4(1.0f, 0.85f, 0.4f, 1.0f)
                                                  : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(color, "%.1f%%", share);
        } else {
            ImGui::TextUnformatted("-");
        }

        ImGui::TableSetColumnIndex(6);
        ImGui::TextUnformatted(process.working_set == 0 ? "-" : formatBytes(process.working_set).c_str());

        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%lu", process.threads);

        ImGui::TableSetColumnIndex(8);
        ImGui::Text("%lu", process.handles);

        ImGui::TableSetColumnIndex(9);
        ImGui::TextUnformatted(priorityName(process.priority));

        ImGui::TableSetColumnIndex(10);
        if (process.path.empty()) {
            ImGui::TextUnformatted("-");
        } else {
            ImGui::PushID(static_cast<int>(process.pid));
            if (ImGui::SmallButton("Open")) revealInExplorer(process.path);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the executable in Explorer");
            ImGui::PopID();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextUnformatted(process.path.c_str());
        }
    }

    // ----- search and row highlighting -----------------------------------------------------------------------------

    // Matches the name as a case-insensitive substring; PID and PPID match any substring of their decimal form.
    bool matchesSearch(const ProcessInfo& process) const {
        if (search_[0] == '\0') return true;

        const std::string query = lowered(search_);
        if (lowered(process.name).find(query) != std::string::npos) return true;
        return std::to_string(process.pid).find(query) != std::string::npos ||
               std::to_string(process.ppid).find(query) != std::string::npos;
    }

    static std::string lowered(const std::string& text) {
        std::string result = text;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    // Keeps the ancestors of every match visible, so a filtered tree still shows where a process belongs.
    void keepAncestors(std::vector<char>& visible) const {
        std::unordered_map<DWORD, std::size_t> index_by_pid;
        index_by_pid.reserve(processes_.size());
        for (std::size_t i = 0; i < processes_.size(); ++i) {
            index_by_pid[processes_[i].pid] = i;
        }

        for (std::size_t i = 0; i < processes_.size(); ++i) {
            if (!visible[i]) continue;
            DWORD parent = processes_[i].ppid;
            for (int depth = 0; depth < 64; ++depth) {
                const auto it = index_by_pid.find(parent);
                if (it == index_by_pid.end() || visible[it->second]) break;
                visible[it->second] = 1;
                parent = processes_[it->second].ppid;
            }
        }
    }

    // Highlights the row under the mouse. ImGui 1.90 has no public "hovered row" query, so the vertical band is
    // reconstructed from the previous row's height (all rows have the same height).
    void highlightRowUnderMouse() {
        const float top = ImGui::GetCursorScreenPos().y;
        if (last_row_top_ > 0.0f) last_row_height_ = top - last_row_top_;
        last_row_top_ = top;

        const float height = last_row_height_ > 0.0f ? last_row_height_ : ImGui::GetFrameHeight();
        const ImVec2 mouse = ImGui::GetMousePos();
        const float left = ImGui::GetWindowPos().x;
        const float right = left + ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize;
        if (mouse.x >= left && mouse.x < right && mouse.y >= top && mouse.y < top + height) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.5f));
        }
    }

    // ----- grouping by PPID ----------------------------------------------------------------------------------------

    struct Grouping {
        std::vector<std::vector<std::size_t>> children;
        std::vector<std::size_t> roots;
    };

    Grouping buildGrouping(const std::vector<char>& visible) const {
        Grouping grouping;
        grouping.children.resize(processes_.size());

        std::unordered_map<DWORD, std::size_t> index_by_pid;
        index_by_pid.reserve(processes_.size());
        for (std::size_t i = 0; i < processes_.size(); ++i) {
            if (visible[i]) index_by_pid[processes_[i].pid] = i;
        }

        // A visible process whose parent is hidden or absent (exited, protected, filtered out) becomes a root.
        for (std::size_t i = 0; i < processes_.size(); ++i) {
            if (!visible[i]) continue;
            const auto parent = index_by_pid.find(processes_[i].ppid);
            if (parent != index_by_pid.end() && parent->second != i) {
                grouping.children[parent->second].push_back(i);
            } else {
                grouping.roots.push_back(i);
            }
        }

        sortIndices(grouping.roots);
        for (auto& children : grouping.children) {
            sortIndices(children);
        }
        return grouping;
    }

    void sortIndices(std::vector<std::size_t>& indices) const {
        std::stable_sort(indices.begin(), indices.end(), [this](std::size_t a, std::size_t b) {
            const int result = compareByColumn(processes_[a], processes_[b], sort_column_);
            return sort_ascending_ ? result < 0 : result > 0;
        });
    }

    void renderGroupedRows(const std::vector<char>& visible) {
        const Grouping grouping = buildGrouping(visible);
        std::vector<char> visiting(processes_.size(), 0);
        std::vector<char> drawn(processes_.size(), 0);

        for (const std::size_t root : grouping.roots) {
            if (!drawn[root]) drawGroupedNode(root, grouping, visiting, drawn);
        }
        // PID reuse can create a parent/child cycle, which has no root: show the remaining processes anyway.
        for (std::size_t i = 0; i < processes_.size(); ++i) {
            if (visible[i] && !drawn[i]) drawGroupedNode(i, grouping, visiting, drawn);
        }
    }

    void drawGroupedNode(std::size_t index, const Grouping& grouping, std::vector<char>& visiting,
                         std::vector<char>& drawn) {
        const ProcessInfo& process = processes_[index];
        drawn[index] = 1;

        ImGui::TableNextRow();
        highlightRowUnderMouse();
        drawIdentityCells(process);

        ImGui::TableSetColumnIndex(2);
        const bool has_children = !grouping.children[index].empty();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
        if (!has_children) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        ImGui::PushID(static_cast<int>(process.pid));
        const bool open = ImGui::TreeNodeEx("##process", flags);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        drawIcon(process);
        ImGui::TextUnformatted(process.name.c_str());
        if (ImGui::IsItemHovered()) showDetails(process);

        drawDetailCells(process);

        if (has_children && open) {
            visiting[index] = 1;
            for (const std::size_t child : grouping.children[index]) {
                if (!drawn[child] && !visiting[child]) drawGroupedNode(child, grouping, visiting, drawn);
            }
            visiting[index] = 0;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ----- worker helpers ------------------------------------------------------------------------------------------

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

        const auto now = Clock::now();
        const std::size_t processor_count = std::max<std::size_t>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        std::unordered_map<DWORD, CpuSample> samples;
        samples.reserve(previous_.size() + 64);

        // Process32FirstW fills the first entry, so the loop starts with do/while: otherwise the first process
        // (usually [System Process]) would be skipped.
        if (Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                ProcessInfo process;
                process.pid = entry.th32ProcessID;
                process.ppid = entry.th32ParentProcessID;
                process.threads = entry.cntThreads;
                process.name = toUtf8(entry.szExeFile);
                ProcessIdToSessionId(process.pid, &process.session);

                collectDetails(process, now, processor_count);

                samples.emplace(process.pid, CpuSample{process.creation, process.cpu_time, now});
                processes_.push_back(std::move(process));
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }

        CloseHandle(snapshot);

        previous_ = std::move(samples);
        ++refresh_count_;
        resort_needed_ = true;  // the fresh snapshot comes in PID order, re-apply the clicked sort
        lastRefresh_ = Clock::now();
    }

    // Sorts the list according to the clicked column; with no sort spec it falls back to the PID order of the snapshot.
    void sortProcesses(const ImGuiTableSortSpecs& specs) {
        const ImGuiTableColumnSortSpecs* spec = specs.SpecsCount > 0 ? &specs.Specs[0] : nullptr;
        const bool ascending = spec == nullptr || spec->SortDirection == ImGuiSortDirection_Ascending;
        const int column = spec != nullptr ? static_cast<int>(spec->ColumnIndex) : 0;

        std::stable_sort(processes_.begin(), processes_.end(),
                         [column, ascending](const ProcessInfo& a, const ProcessInfo& b) {
                             const int result = compareByColumn(a, b, column);
                             return ascending ? result < 0 : result > 0;
                         });
    }

    // Column order must match the TableSetupColumn calls above.
    static int compareByColumn(const ProcessInfo& a, const ProcessInfo& b, int column) {
        switch (column) {
            case 0:
                return compareValues(a.pid, b.pid);
            case 1:
                return compareValues(a.ppid, b.ppid);
            case 2:
                return compareText(a.name, b.name);
            case 3:
                return compareText(a.user, b.user);
            case 4:
                return compareValues(a.session, b.session);
            case 5:
                return compareValues(a.cpu_percent, b.cpu_percent);
            case 6:
                return compareValues(a.working_set, b.working_set);
            case 7:
                return compareValues(a.threads, b.threads);
            case 8:
                return compareValues(a.handles, b.handles);
            case 9:
                return compareValues(priorityRank(a.priority), priorityRank(b.priority));
            case 10:
                return compareText(a.path, b.path);
            default:
                return 0;
        }
    }

    template <typename T>
    static int compareValues(const T& a, const T& b) {
        return a < b ? -1 : (b < a ? 1 : 0);
    }

    // Case-insensitive text comparison, matching what the table shows.
    static int compareText(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()); }

    // Table sizes are relative to the font, which follows the monitor DPI.
    static float em(float value) { return ImGui::GetFontSize() * value; }

    static std::wstring toWide(const std::string& utf8) {
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (size <= 1) return {};
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
        wide.pop_back();
        return wide;
    }

    // Shows the file in Explorer.
    //
    // The preferred call only asks the already running shell to select the item: no new explorer.exe is started, so it
    // cannot fail the way a spawned explorer does (STATUS_DLL_INIT_FAILED / 0xc0000142). If the shell refuses, the
    // "explore" verb on the containing folder is tried, and only if that fails as well the user gets the error codes.
    static void revealInExplorer(const std::string& utf8_path) {
        const std::wstring path = toWide(utf8_path);
        if (path.empty()) return;

        // Balance every successful CoInitializeEx with CoUninitialize, including on the early returns.
        struct ComScope {
            HRESULT result;
            ~ComScope() {
                if (SUCCEEDED(result)) CoUninitialize();
            }
        } com{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};

        HRESULT shell_view = E_FAIL;
        if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str())) {
            shell_view = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
            if (SUCCEEDED(shell_view)) return;
        }

        const std::filesystem::path folder = std::filesystem::path(path).parent_path();
        if (!folder.empty()) {
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            info.lpVerb = L"explore";
            info.lpFile = folder.c_str();
            info.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&info) != FALSE) return;

            const std::wstring message =
                std::format(L"Windows did not let Explorer open this folder.\n\nFile: {}\n\n"
                            L"CoInitializeEx: 0x{:08X}\nSHOpenFolderAndSelectItems: 0x{:08X}\n"
                            L"ShellExecuteEx(explore): 0x{:08X}\nIntegrity level: {}\nElevated process: {}\n"
                            L"Started by: {}",
                            path, static_cast<unsigned long>(com.result), static_cast<unsigned long>(shell_view),
                            static_cast<unsigned long>(GetLastError()), integrityLevel(),
                            isElevated() ? L"yes" : L"no", parentProcess());
            MessageBoxW(nullptr, message.c_str(), L"Cool Task Manager", MB_OK | MB_ICONWARNING);
        }
    }

    // Shows which process started us: a launcher that runs at low integrity (a sandboxed shell, the harness GUI) makes
    // its children low integrity as well, and those are not allowed to drive Explorer.
    static std::wstring parentProcess() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return L"unknown";

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD parent = 0;
        if (Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                if (entry.th32ProcessID == GetCurrentProcessId()) {
                    parent = entry.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }

        std::wstring name = L"unknown";
        if (parent != 0 && Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                if (entry.th32ProcessID == parent) {
                    name = entry.szExeFile;
                    break;
                }
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }

        CloseHandle(snapshot);
        return std::format(L"{} (PID {})", name, parent);
    }

    // Low integrity (sandboxed) processes are not allowed to drive the shell, which explains access denied errors.
    static std::wstring integrityLevel() {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) return L"unknown";

        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        std::wstring level = L"unknown";
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size) != FALSE) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
            const DWORD rid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
            if (rid >= SECURITY_MANDATORY_SYSTEM_RID) {
                level = L"System";
            } else if (rid >= SECURITY_MANDATORY_HIGH_RID) {
                level = L"High";
            } else if (rid >= SECURITY_MANDATORY_MEDIUM_RID) {
                level = L"Medium";
            } else if (rid >= SECURITY_MANDATORY_LOW_RID) {
                level = L"Low";
            } else {
                level = L"Untrusted";
            }
        }
        CloseHandle(token);
        return level;
    }

    // Restarts the application through a UAC prompt. The elevated instance can drive Explorer and read details of
    // protected processes; this one closes as soon as the new instance was started successfully.
    static void restartElevated() {
        wchar_t path[MAX_PATH * 4]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) return;

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = path;
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&info) != FALSE) {
            if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetWindowShouldClose(window, GLFW_TRUE);
            return;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) return;  // the user declined the UAC prompt
        const std::wstring message = std::format(L"Could not restart as administrator (error {}).", error);
        MessageBoxW(nullptr, message.c_str(), L"Cool Task Manager", MB_OK | MB_ICONWARNING);
    }

    // Elevation changes whether the shell can be driven from this process, so it is part of the error message.
    static bool isElevated() {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
        CloseHandle(token);
        return ok != FALSE && elevation.TokenIsElevated != 0;
    }

    // Fills everything that needs a process handle. OpenProcess fails for protected processes, and individual queries
    // may fail as well; in both cases the corresponding fields simply stay empty.
    void collectDetails(ProcessInfo& process, Clock::time_point now, std::size_t processor_count) {
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid);
        if (handle == nullptr) return;

        process.path = toUtf8(imagePath(handle));

        FILETIME creation{}, exit_time{}, kernel{}, user{};
        if (GetProcessTimes(handle, &creation, &exit_time, &kernel, &user) != FALSE) {
            process.creation = fileTimeToUint64(creation);
            process.cpu_time = fileTimeToUint64(kernel) + fileTimeToUint64(user);

            // CPU share = CPU time consumed since the previous refresh, divided by the wall time and the number of
            // logical processors, i.e. 100% means "all cores busy".
            if (const auto it = previous_.find(process.pid);
                it != previous_.end() && it->second.creation == process.creation) {
                const double wall = std::chrono::duration<double>(now - it->second.wall).count();
                if (wall > 0.0 && process.cpu_time >= it->second.cpu_time) {
                    const double cpu_seconds = static_cast<double>(process.cpu_time - it->second.cpu_time) / 1.0e7;
                    process.cpu_percent = cpu_seconds / (wall * static_cast<double>(processor_count)) * 100.0;
                    process.has_cpu_percent = true;
                }
            }
        }

        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (K32GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            fillMemory(process, counters);
        } else if (HANDLE full = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process.pid)) {
            // Some processes reject the limited handle for memory counters but allow full access.
            if (K32GetProcessMemoryInfo(full, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
                fillMemory(process, counters);
            }
            CloseHandle(full);
        }

        DWORD handle_count = 0;
        if (GetProcessHandleCount(handle, &handle_count) != FALSE) {
            process.handles = handle_count;
        }

        process.priority = GetPriorityClass(handle);
        process.user = userForProcess(handle);

        CloseHandle(handle);
    }

    static void fillMemory(ProcessInfo& process, const PROCESS_MEMORY_COUNTERS_EX& counters) {
        process.working_set = counters.WorkingSetSize;
        process.private_bytes = counters.PrivateUsage;
        process.pagefile_bytes = counters.PagefileUsage;
    }

    // Tooltip with the fields that do not fit into the table columns.
    static void showDetails(const ProcessInfo& process) {
        ImGui::BeginTooltip();
        ImGui::Text("PID %lu, PPID %lu", process.pid, process.ppid);
        ImGui::Separator();
        ImGui::TextUnformatted(std::format("Name:        {}", process.name).c_str());
        ImGui::TextUnformatted(std::format("Path:        {}", process.path.empty() ? "-" : process.path).c_str());
        ImGui::TextUnformatted(std::format("User:        {}", process.user.empty() ? "-" : process.user).c_str());
        ImGui::TextUnformatted(std::format("Session:     {}", process.session).c_str());
        ImGui::TextUnformatted(std::format("Priority:    {}", priorityName(process.priority)).c_str());
        ImGui::TextUnformatted(std::format("Started:     {}", formatStartTime(process.creation)).c_str());
        ImGui::TextUnformatted(std::format("CPU time:    {}", formatCpuTime(process.cpu_time)).c_str());
        ImGui::TextUnformatted(std::format("Working set: {}", formatBytes(process.working_set)).c_str());
        ImGui::TextUnformatted(std::format("Private:     {}", formatBytes(process.private_bytes)).c_str());
        ImGui::TextUnformatted(std::format("Pagefile:    {}", formatBytes(process.pagefile_bytes)).c_str());
        ImGui::TextUnformatted(std::format("Threads:     {}", process.threads).c_str());
        ImGui::TextUnformatted(std::format("Handles:     {}", process.handles).c_str());
        ImGui::EndTooltip();
    }

    // Full image path; QueryFullProcessImageNameW reports a too small buffer via ERROR_INSUFFICIENT_BUFFER.
    static std::wstring imagePath(HANDLE process) {
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            DWORD size = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE) {
                path.resize(size);
                return path;
            }
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768) return {};
            path.resize(path.size() * 2);
        }
    }

    // Resolves the process token owner. LookupAccountSidW is the slowest call here, so results are cached by SID:
    // hundreds of processes usually belong to a handful of accounts.
    std::string userForProcess(HANDLE process) {
        HANDLE token = nullptr;
        if (OpenProcessToken(process, TOKEN_QUERY, &token) == FALSE) return {};

        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size == 0) {
            CloseHandle(token);
            return {};
        }

        std::vector<BYTE> buffer(size);
        const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
        CloseHandle(token);
        if (ok == FALSE) return {};

        const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        const DWORD sid_length = GetLengthSid(token_user->User.Sid);
        const std::string key(reinterpret_cast<const char*>(token_user->User.Sid), sid_length);
        if (const auto cached = user_cache_.find(key); cached != user_cache_.end()) return cached->second;

        std::string result;
        DWORD name_length = 0;
        DWORD domain_length = 0;
        SID_NAME_USE sid_type{};
        LookupAccountSidW(nullptr, token_user->User.Sid, nullptr, &name_length, nullptr, &domain_length, &sid_type);
        if (name_length > 0) {
            std::wstring name(name_length, L'\0');
            std::wstring domain(domain_length, L'\0');
            if (LookupAccountSidW(nullptr, token_user->User.Sid, name.data(), &name_length,
                                  domain_length > 0 ? domain.data() : nullptr, &domain_length, &sid_type) != FALSE) {
                name.resize(name_length);
                domain.resize(domain_length);
                result = toUtf8(domain.empty() ? name : domain + L"\\" + name);
            }
        }

        user_cache_.emplace(std::move(key), result);
        return result;
    }

    static std::uint64_t fileTimeToUint64(const FILETIME& time) {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return value.QuadPart;
    }

    // Numeric rank for sorting by priority class (the Win32 constants themselves are not ordered meaningfully).
    static int priorityRank(DWORD priority) {
        switch (priority) {
            case IDLE_PRIORITY_CLASS:
                return 0;
            case BELOW_NORMAL_PRIORITY_CLASS:
                return 1;
            case NORMAL_PRIORITY_CLASS:
                return 2;
            case ABOVE_NORMAL_PRIORITY_CLASS:
                return 3;
            case HIGH_PRIORITY_CLASS:
                return 4;
            case REALTIME_PRIORITY_CLASS:
                return 5;
            default:
                return -1;
        }
    }

    static const char* priorityName(DWORD priority) {
        switch (priority) {
            case IDLE_PRIORITY_CLASS:
                return "Idle";
            case BELOW_NORMAL_PRIORITY_CLASS:
                return "Below normal";
            case NORMAL_PRIORITY_CLASS:
                return "Normal";
            case ABOVE_NORMAL_PRIORITY_CLASS:
                return "Above normal";
            case HIGH_PRIORITY_CLASS:
                return "High";
            case REALTIME_PRIORITY_CLASS:
                return "Realtime";
            default:
                return "-";
        }
    }

    static std::string formatBytes(std::size_t bytes) {
        static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
        double value = static_cast<double>(bytes);
        std::size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < std::size(units)) {
            value /= 1024.0;
            ++unit;
        }
        return unit == 0 ? std::format("{:.0f} {}", value, units[unit])
                         : std::format("{:.1f} {}", value, units[unit]);
    }

    static std::string formatCpuTime(std::uint64_t time_100ns) {
        const std::uint64_t seconds = time_100ns / 10'000'000;
        return std::format("{}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    }

    static std::string formatStartTime(std::uint64_t creation) {
        FILETIME file_time{};
        file_time.dwLowDateTime = static_cast<DWORD>(creation & 0xFFFFFFFFu);
        file_time.dwHighDateTime = static_cast<DWORD>(creation >> 32);

        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (FileTimeToSystemTime(&file_time, &utc) == FALSE) return "-";
        if (SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) == FALSE) return "-";
        return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local.wYear, local.wMonth, local.wDay, local.wHour,
                           local.wMinute, local.wSecond);
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

    static std::string toUtf8(const std::wstring& text) { return toUtf8(text.c_str()); }

    std::vector<ProcessInfo> processes_{};
    std::string status_{};  // non-empty only when the last refresh failed
    ProcessIconCache icons_{};
    std::unordered_map<DWORD, CpuSample> previous_{};
    std::unordered_map<std::string, std::string> user_cache_{};  // SID -> "DOMAIN\User"
    unsigned int refresh_count_ = 0;
    bool resort_needed_ = false;
    bool integrity_checked_ = false;
    bool low_integrity_ = false;
    bool group_by_ppid_ = false;
    int sort_column_ = 0;
    bool sort_ascending_ = true;
    char search_[128] = {};
    float last_row_top_ = 0.0f;
    float last_row_height_ = 0.0f;
    bool autoRefresh_ = false;
    Clock::time_point lastRefresh_ = Clock::now();
};
