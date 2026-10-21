#pragma once
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"

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
        // CPU usage is a delta between two snapshots, so take a second sample shortly after startup to make the
        // percentages meaningful without waiting for a manual refresh.
        if (refresh_count_ == 1 && Clock::now() - lastRefresh_ >= kPrimingInterval) {
            refresh();
        }

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
            // survives new snapshots.
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsDirty || resort_needed_) {
                    sortProcesses(*sort_specs);
                    sort_specs->SpecsDirty = false;
                    resort_needed_ = false;
                }
            }

            for (const auto& process : processes_) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%lu", process.pid);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%lu", process.ppid);

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(process.name.c_str());
                if (ImGui::IsItemHovered()) showDetails(process);

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
                ImGui::TextUnformatted(process.path.empty() ? "-" : process.path.c_str());
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
    std::unordered_map<DWORD, CpuSample> previous_{};
    std::unordered_map<std::string, std::string> user_cache_{};  // SID -> "DOMAIN\User"
    unsigned int refresh_count_ = 0;
    bool resort_needed_ = false;
    bool autoRefresh_ = false;
    Clock::time_point lastRefresh_ = Clock::now();
};
