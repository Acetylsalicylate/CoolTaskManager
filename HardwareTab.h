#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h must not pull in the legacy winsock.h, otherwise ws2ipdef.h (needed for MIB_IF_ROW2 / GetIfEntry2)
// cannot be included afterwards. The same define is set project-wide in CMakeLists.txt.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <winsock2.h>  // winsock types required by ws2ipdef.h
#include <ws2ipdef.h>
#include <iphlpapi.h>  // GetIfEntry2: link speed and traffic counters, which hwinfo does not report

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <hwinfo/battery.h>
#include <hwinfo/computer.h>
#include <hwinfo/cpu.h>
#include <hwinfo/disk.h>
#include <hwinfo/gpu.h>
#include <hwinfo/mainboard.h>
#include <hwinfo/monitoring.h>
#include <hwinfo/network.h>
#include <hwinfo/os.h>
#include <hwinfo/ram.h>
#include <hwinfo/resource_limits.h>
#include <hwinfo/units.h>
#include <hwinfo/virtualization.h>

#include "imgui.h"

namespace hw = hwinfo;

// Everything that does not change while the program runs, collected once on the worker thread.
struct HardwareStatic {
    std::optional<hw::Computer> computer;
    std::optional<hw::Os> os;
    std::optional<hw::Mainboard> mainboard;
    std::optional<hw::Virtualization> virtualization;
    std::optional<hw::ResourceLimits> limits;
    std::vector<hw::Cpu> cpus;
    std::optional<hw::Memory> memory;
    std::vector<hw::Disk> disks;
    std::vector<hw::Gpu> gpus;
    std::vector<hw::NetworkInterface> network;
    std::vector<hw::Battery> batteries;
    std::vector<std::string> notes;  // components whose query failed, with the hwinfo error text
};

// Link speed and throughput of one network interface, sampled through GetIfEntry2 (WinAPI).
struct NetworkLive {
    std::optional<std::uint64_t> link_speed_bps;
    std::optional<double> receive_bps;
    std::optional<double> transmit_bps;
};

// One live sample. Deliberately small: it is copied from the worker to the UI once per second.
struct HardwareLive {
    std::optional<hw::CpuLoad> cpu_load;
    std::optional<std::vector<hw::Hertz>> cpu_frequencies;
    std::optional<hw::MemoryUsage> memory_usage;
    std::optional<hw::PowerProfile> power_profile;
    std::optional<hw::PowerSource> power_source;
    std::optional<std::chrono::seconds> uptime;
    std::vector<std::vector<std::optional<hw::DiskSpace>>> disks;  // [disk][mount point]
    std::vector<std::optional<hw::GpuStatus>> gpus;
    std::vector<std::optional<hw::BatteryStatus>> batteries;
    std::vector<NetworkLive> network;
};

namespace hardware_ui {

// Recent samples of one value. PlotLines wants a contiguous array, so the oldest value is dropped from the front.
class History {
public:
    explicit History(std::size_t capacity = 180) : capacity_(capacity) {}

    void push(float value) {
        values_.push_back(value);
        if (values_.size() > capacity_) {
            values_.erase(values_.begin(), values_.begin() + static_cast<std::ptrdiff_t>(values_.size() - capacity_));
        }
    }

    [[nodiscard]] bool hasData() const { return values_.size() >= 2; }
    [[nodiscard]] int count() const { return static_cast<int>(values_.size()); }
    [[nodiscard]] const float* data() const { return values_.data(); }

private:
    std::size_t capacity_;
    std::vector<float> values_;
};

// Sizes are relative to the font, which already follows the monitor DPI.
inline float em(float value) { return ImGui::GetFontSize() * value; }

inline std::string textOr(const std::optional<std::string>& value, const char* fallback = "-") {
    return value.has_value() ? *value : fallback;
}

template <typename T>
inline std::string str(const T& value) {
    return std::format("{}", value);
}

template <typename T>
inline std::string optStr(const std::optional<T>& value) {
    return value.has_value() ? std::format("{}", *value) : "-";
}

// hwinfo returns std::expected<std::optional-like, error>; live samples keep only the value.
template <typename T>
inline std::optional<T> optionalOf(hw::result<T>&& result) {
    if (!result) return std::nullopt;
    return std::optional<T>(std::move(*result));
}

inline std::string yesNo(bool value) { return value ? "yes" : "no"; }

inline std::string duration(std::chrono::seconds seconds) {
    const auto total = static_cast<std::int64_t>(seconds.count());
    return std::format("{} d {:02} h {:02} min", total / 86400, (total % 86400) / 3600, (total % 3600) / 60);
}

inline std::string localTime(std::chrono::sys_seconds time_point) {
    const std::time_t time = std::chrono::system_clock::to_time_t(time_point);
    std::tm local{};
    if (localtime_s(&local, &time) != 0) return "-";
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

inline std::string rate(double bytes_per_second) {
    if (bytes_per_second >= 1024.0 * 1024.0) return std::format("{:.2f} MiB/s", bytes_per_second / (1024.0 * 1024.0));
    if (bytes_per_second >= 1024.0) return std::format("{:.1f} KiB/s", bytes_per_second / 1024.0);
    return std::format("{:.0f} B/s", bytes_per_second);
}

inline std::string linkSpeed(std::uint64_t bits_per_second) {
    if (bits_per_second >= 1'000'000'000ull) return std::format("{:.1f} Gbps", bits_per_second / 1.0e9);
    if (bits_per_second >= 1'000'000ull) return std::format("{:.0f} Mbps", bits_per_second / 1.0e6);
    if (bits_per_second >= 1'000ull) return std::format("{:.0f} kbps", bits_per_second / 1.0e3);
    return std::format("{} bps", bits_per_second);
}

// Two-column "field name / field value" table.
inline void beginFields(const char* id) {
    ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX);
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
}

inline void field(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value.c_str());
}

// For long values: paths, feature flag lists, driver names.
inline void fieldWrapped(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", value.c_str());
}

inline void endFields() { ImGui::EndTable(); }

// Fixed scale, e.g. 0..100 %.
inline void plotFixed(const char* id, const History& history, float height, float max_value,
                      const char* overlay = nullptr) {
    if (!history.hasData()) {
        ImGui::TextDisabled("collecting...");
        return;
    }
    // The default dark theme draws a grey line on a blue frame background, which is hard to read.
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.45f, 0.80f, 1.00f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.08f, 0.10f, 1.0f));
    ImGui::PlotLines(id, history.data(), history.count(), 0, overlay, 0.0f, max_value,
                     ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), height));
    ImGui::PopStyleColor(2);
}

// Scale from the data, for values without a natural maximum (temperatures, network rates).
inline void plotAuto(const char* id, const History& history, float height, const char* overlay = nullptr) {
    if (!history.hasData()) {
        ImGui::TextDisabled("collecting...");
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.45f, 0.80f, 1.00f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.08f, 0.10f, 1.0f));
    ImGui::PlotLines(id, history.data(), history.count(), 0, overlay, FLT_MAX, FLT_MAX,
                     ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), height));
    ImGui::PopStyleColor(2);
}

}  // namespace hardware_ui

// Hardware tab: a navigation pane with one entry per component and a details pane that shows everything hwinfo knows
// about the selected component, plus graphs of the live values.
//
// Threading: all hwinfo calls happen on one worker thread (gpus() may load vendor libraries such as NVML/CUDA, and
// sampling queries performance counters, so both can block). The worker publishes an immutable static description once
// and replaces the live sample every second; render() only copies those, so the UI never waits for the hardware.
class HardwareTab {
public:
    HardwareTab() { worker_ = std::thread([this] { workerLoop(); }); }

    ~HardwareTab() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    HardwareTab(const HardwareTab&) = delete;
    HardwareTab& operator=(const HardwareTab&) = delete;
    HardwareTab(HardwareTab&&) = delete;
    HardwareTab& operator=(HardwareTab&&) = delete;

    void render() {
        renderToolbar();
        ImGui::Separator();

        std::shared_ptr<const HardwareStatic> static_info;
        bool new_sample = false;
        {
            std::lock_guard lock(mutex_);
            static_info = static_info_;
            if (generation_ != shown_generation_) {
                shown_live_ = live_;
                shown_generation_ = generation_;
                new_sample = true;
            }
        }

        if (!static_info) {
            ImGui::TextDisabled("Collecting hardware information...");
            return;
        }

        if (new_sample) updateHistories(shown_live_);

        const float pane_height = std::max(0.0f, ImGui::GetContentRegionAvail().y);
        ImGui::BeginChild("HardwareNav", ImVec2(hardware_ui::em(18.0f), pane_height), true);
        renderNavigation(*static_info);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("HardwareDetails", ImVec2(0.0f, pane_height), false);
        renderDetails(*static_info, shown_live_);
        ImGui::EndChild();
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kUpdateInterval = std::chrono::seconds(1);

    enum class Page { Computer, Os, Mainboard, Cpu, Memory, Disk, Gpu, Network, Battery };

    struct Selection {
        Page page = Page::Computer;
        std::size_t index = 0;
    };

    // Previous network counters, used to turn absolute byte counts into a rate.
    struct NicCounter {
        std::uint64_t receive_octets = 0;
        std::uint64_t transmit_octets = 0;
        Clock::time_point time{};
        bool valid = false;
    };

    struct WorkerContext {
        hw::CpuSampler cpu_sampler;
        std::vector<hw::GpuSampler> gpu_samplers;
        std::vector<NicCounter> nic_counters;
    };

    // ----- toolbar -------------------------------------------------------------------------------------------------

    void renderToolbar() {
        if (ImGui::Button("Update")) {
            request_.store(true, std::memory_order_relaxed);
            cv_.notify_all();
        }

        ImGui::SameLine();
        if (ImGui::Button(autoRefresh_ ? "Auto: ON (1 s)" : "Auto: OFF")) {
            autoRefresh_ = !autoRefresh_;
            cv_.notify_all();
        }

        ImGui::SameLine();
        if (sampling_.load(std::memory_order_relaxed)) {
            ImGui::TextDisabled("sampling...");
        } else if (const double age = secondsSinceLastSample(); age >= 0.0) {
            ImGui::TextDisabled("updated %.1f s ago", age);
        } else {
            ImGui::TextDisabled("no data yet");
        }
    }

    double secondsSinceLastSample() {
        std::lock_guard lock(mutex_);
        if (!has_sample_) return -1.0;
        return std::chrono::duration<double>(Clock::now() - last_sample_).count();
    }

    // ----- worker --------------------------------------------------------------------------------------------------

    void workerLoop() {
        auto static_info = std::make_shared<HardwareStatic>(collectStatic());
        {
            std::lock_guard lock(mutex_);
            static_info_ = static_info;
        }

        WorkerContext context;
        context.gpu_samplers.reserve(static_info->gpus.size());
        for (const auto& gpu : static_info->gpus) {
            context.gpu_samplers.emplace_back(gpu);
        }
        context.nic_counters.resize(static_info->network.size());
        (void)context.cpu_sampler.sample();  // baseline, so the first interval is not close to zero

        std::unique_lock lock(mutex_);
        while (!stop_) {
            cv_.wait(lock, [this] { return stop_ || request_ || autoRefresh_; });
            if (stop_) break;

            if (autoRefresh_) {
                cv_.wait_for(lock, kUpdateInterval, [this] { return stop_ || request_ || !autoRefresh_; });
                if (stop_) break;
                if (!autoRefresh_ && !request_) continue;
            }

            request_ = false;
            sampling_.store(true, std::memory_order_relaxed);
            lock.unlock();

            HardwareLive live = sampleLive(*static_info, context);

            lock.lock();
            live_ = std::move(live);
            last_sample_ = Clock::now();
            has_sample_ = true;
            generation_.fetch_add(1, std::memory_order_release);
            sampling_.store(false, std::memory_order_relaxed);
        }
    }

    // Reads everything that does not change: one hwinfo call per component, failures are recorded in notes.
    static HardwareStatic collectStatic() {
        HardwareStatic info;

        if (auto value = hw::computer()) {
            info.computer = std::move(*value);
        } else {
            info.notes.push_back(std::format("Computer: {}", value.error()));
        }
        if (auto value = hw::os()) {
            info.os = std::move(*value);
        } else {
            info.notes.push_back(std::format("OS: {}", value.error()));
        }
        if (auto value = hw::mainboard()) {
            info.mainboard = std::move(*value);
        } else {
            info.notes.push_back(std::format("Mainboard: {}", value.error()));
        }
        if (auto value = hw::virtualization()) {
            info.virtualization = std::move(*value);
        } else {
            info.notes.push_back(std::format("Virtualization: {}", value.error()));
        }
        if (auto value = hw::resource_limits()) {
            info.limits = std::move(*value);
        } else {
            info.notes.push_back(std::format("Resource limits: {}", value.error()));
        }
        if (auto value = hw::cpus()) {
            info.cpus = std::move(*value);
        } else {
            info.notes.push_back(std::format("CPU: {}", value.error()));
        }
        if (auto value = hw::memory()) {
            info.memory = std::move(*value);
        } else {
            info.notes.push_back(std::format("Memory: {}", value.error()));
        }
        if (auto value = hw::disks()) {
            info.disks = std::move(*value);
        } else {
            info.notes.push_back(std::format("Disks: {}", value.error()));
        }
        if (auto value = hw::gpus()) {
            info.gpus = std::move(*value);
        } else {
            info.notes.push_back(std::format("GPUs: {}", value.error()));
        }
        if (auto value = hw::network_interfaces()) {
            info.network = std::move(*value);
        } else {
            info.notes.push_back(std::format("Network: {}", value.error()));
        }
        if (auto value = hw::batteries()) {
            info.batteries = std::move(*value);
        } else {
            info.notes.push_back(std::format("Batteries: {}", value.error()));
        }

        return info;
    }

    static HardwareLive sampleLive(const HardwareStatic& info, WorkerContext& context) {
        HardwareLive live;
        const auto now = Clock::now();

        live.cpu_load = hardware_ui::optionalOf(context.cpu_sampler.sample());
        live.cpu_frequencies = hardware_ui::optionalOf(hw::cpu_frequencies());
        live.memory_usage = hardware_ui::optionalOf(hw::memory_usage());
        live.power_profile = hardware_ui::optionalOf(hw::power_profile());
        live.uptime = hardware_ui::optionalOf(hw::uptime());

        if (!info.batteries.empty()) {
            live.power_source = hardware_ui::optionalOf(hw::power_source());
        }

        // Free space of every mount point of every disk, aligned with the static description.
        live.disks.resize(info.disks.size());
        for (std::size_t disk = 0; disk < info.disks.size(); ++disk) {
            live.disks[disk].resize(info.disks[disk].mount_points.size());
            for (std::size_t mount = 0; mount < info.disks[disk].mount_points.size(); ++mount) {
                if (auto space = hw::disk_space(info.disks[disk].mount_points[mount].path)) {
                    live.disks[disk][mount] = *space;
                }
            }
        }

        live.gpus.resize(context.gpu_samplers.size());
        for (std::size_t gpu = 0; gpu < context.gpu_samplers.size(); ++gpu) {
            live.gpus[gpu] = hardware_ui::optionalOf(context.gpu_samplers[gpu].sample());
        }

        live.batteries.resize(info.batteries.size());
        for (std::size_t battery = 0; battery < info.batteries.size(); ++battery) {
            live.batteries[battery] = hardware_ui::optionalOf(hw::battery_status(info.batteries[battery].index));
        }

        live.network.resize(info.network.size());
        for (std::size_t nic = 0; nic < info.network.size(); ++nic) {
            live.network[nic] = sampleNetwork(info.network[nic], context.nic_counters[nic], now);
        }

        return live;
    }

    // Link speed and throughput; GetIfEntry2 is cheap and needs no elevation.
    static NetworkLive sampleNetwork(const hw::NetworkInterface& interface_info, NicCounter& counter,
                                     Clock::time_point now) {
        NetworkLive live;

        MIB_IF_ROW2 row{};
        row.InterfaceIndex = interface_info.index;
        if (GetIfEntry2(&row) != NO_ERROR) return live;

        live.link_speed_bps = row.TransmitLinkSpeed;

        if (counter.valid) {
            const double seconds = std::chrono::duration<double>(now - counter.time).count();
            if (seconds > 0.0 && row.InOctets >= counter.receive_octets && row.OutOctets >= counter.transmit_octets) {
                live.receive_bps = static_cast<double>(row.InOctets - counter.receive_octets) / seconds;
                live.transmit_bps = static_cast<double>(row.OutOctets - counter.transmit_octets) / seconds;
            }
        }

        counter.receive_octets = row.InOctets;
        counter.transmit_octets = row.OutOctets;
        counter.time = now;
        counter.valid = true;
        return live;
    }

    // ----- histories -----------------------------------------------------------------------------------------------

    void updateHistories(const HardwareLive& live) {
        if (live.cpu_load) {
            cpu_history_.push(static_cast<float>(live.cpu_load->total * 100.0));
            const auto& threads = live.cpu_load->per_thread;
            if (core_history_.size() < threads.size()) core_history_.resize(threads.size());
            for (std::size_t i = 0; i < threads.size(); ++i) {
                core_history_[i].push(static_cast<float>(threads[i] * 100.0));
            }
        }

        if (live.memory_usage && live.memory_usage->total.value > 0) {
            const double total = live.memory_usage->total.to(hw::ByteUnit::GiB);
            const double available = live.memory_usage->available.to(hw::ByteUnit::GiB);
            ram_history_.push(static_cast<float>(std::max(0.0, total - available)));
        }

        if (live.gpus.size() > gpu_util_history_.size()) {
            gpu_util_history_.resize(live.gpus.size());
            gpu_memory_history_.resize(live.gpus.size());
            gpu_temperature_history_.resize(live.gpus.size());
        }
        for (std::size_t i = 0; i < live.gpus.size(); ++i) {
            if (!live.gpus[i]) continue;
            const auto& status = *live.gpus[i];
            if (status.utilization) gpu_util_history_[i].push(static_cast<float>(*status.utilization * 100.0));
            if (status.memory_used) {
                gpu_memory_history_[i].push(static_cast<float>(status.memory_used->to(hw::ByteUnit::GiB)));
            }
            if (status.temperature) gpu_temperature_history_[i].push(static_cast<float>(*status.temperature));
        }

        if (live.network.size() > network_receive_history_.size()) {
            network_receive_history_.resize(live.network.size());
            network_transmit_history_.resize(live.network.size());
        }
        for (std::size_t i = 0; i < live.network.size(); ++i) {
            if (live.network[i].receive_bps) {
                network_receive_history_[i].push(static_cast<float>(*live.network[i].receive_bps / 1024.0));
            }
            if (live.network[i].transmit_bps) {
                network_transmit_history_[i].push(static_cast<float>(*live.network[i].transmit_bps / 1024.0));
            }
        }

        if (live.batteries.size() > battery_history_.size()) battery_history_.resize(live.batteries.size());
        for (std::size_t i = 0; i < live.batteries.size(); ++i) {
            if (live.batteries[i] && live.batteries[i]->charge) {
                battery_history_[i].push(static_cast<float>(*live.batteries[i]->charge * 100.0));
            }
        }
    }

    // ----- navigation ----------------------------------------------------------------------------------------------

    void selectable(const std::string& label, Page page, std::size_t index = 0) {
        const bool selected = selection_.page == page && selection_.index == index;
        if (ImGui::Selectable(label.c_str(), selected)) {
            selection_ = Selection{page, index};
        }
    }

    void renderNavigation(const HardwareStatic& info) {
        ImGui::TextDisabled("SYSTEM");
        selectable("Computer", Page::Computer);
        selectable("Operating system", Page::Os);
        selectable("Mainboard", Page::Mainboard);

        if (!info.cpus.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("PROCESSORS");
            for (std::size_t i = 0; i < info.cpus.size(); ++i) {
                selectable(std::format("CPU {} — {}", i, info.cpus[i].model), Page::Cpu, i);
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("MEMORY");
        selectable("RAM", Page::Memory);

        if (!info.disks.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("STORAGE");
            for (std::size_t i = 0; i < info.disks.size(); ++i) {
                selectable(std::format("Disk {} — {}", i, info.disks[i].model.value_or("unknown")), Page::Disk, i);
            }
        }

        if (!info.gpus.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("GRAPHICS");
            for (std::size_t i = 0; i < info.gpus.size(); ++i) {
                selectable(std::format("GPU {} — {}", i, info.gpus[i].name), Page::Gpu, i);
            }
        }

        if (!info.network.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("NETWORK");
            for (std::size_t i = 0; i < info.network.size(); ++i) {
                selectable(info.network[i].name, Page::Network, i);
            }
        }

        if (!info.batteries.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("POWER");
            for (std::size_t i = 0; i < info.batteries.size(); ++i) {
                selectable(std::format("Battery {} — {}", i, info.batteries[i].model.value_or("unknown")), Page::Battery,
                           i);
            }
        }
    }

    // ----- details -------------------------------------------------------------------------------------------------

    void renderDetails(const HardwareStatic& info, const HardwareLive& live) {
        switch (selection_.page) {
            case Page::Computer:
                renderComputer(info, live);
                break;
            case Page::Os:
                renderOs(info);
                break;
            case Page::Mainboard:
                renderMainboard(info);
                break;
            case Page::Cpu:
                renderCpu(info, live, selection_.index);
                break;
            case Page::Memory:
                renderMemory(info, live);
                break;
            case Page::Disk:
                renderDisk(info, live, selection_.index);
                break;
            case Page::Gpu:
                renderGpu(info, live, selection_.index);
                break;
            case Page::Network:
                renderNetwork(info, live, selection_.index);
                break;
            case Page::Battery:
                renderBattery(info, live, selection_.index);
                break;
        }
    }

    static void renderComputer(const HardwareStatic& info, const HardwareLive& live) {
        if (info.computer) {
            const auto& computer = *info.computer;
            ImGui::SeparatorText("Computer");
            hardware_ui::beginFields("computer");
            hardware_ui::field("Vendor", hardware_ui::textOr(computer.vendor));
            hardware_ui::field("Model", hardware_ui::textOr(computer.model));
            hardware_ui::field("Family", hardware_ui::textOr(computer.family));
            hardware_ui::field("Version", hardware_ui::textOr(computer.version));
            hardware_ui::field("SKU", hardware_ui::textOr(computer.sku));
            hardware_ui::field("Serial number", hardware_ui::textOr(computer.serial_number));
            const std::string chassis =
                computer.chassis_code
                    ? std::format("{} (SMBIOS code {})", hw::to_string(computer.chassis), *computer.chassis_code)
                    : std::string(hw::to_string(computer.chassis));
            hardware_ui::field("Chassis", chassis);
            hardware_ui::field("Boot time",
                               computer.boot_time ? hardware_ui::localTime(*computer.boot_time) : std::string("-"));
            hardware_ui::field("Uptime", live.uptime ? hardware_ui::duration(*live.uptime) : std::string("-"));
            hardware_ui::endFields();
        } else {
            ImGui::TextDisabled("Computer information is unavailable.");
        }

        ImGui::SeparatorText("Environment");
        hardware_ui::beginFields("environment");
        hardware_ui::field("Virtualization",
                           info.virtualization ? hw::to_string(*info.virtualization) : std::string("unavailable"));
        hardware_ui::field("Power plan", live.power_profile && live.power_profile->power_plan
                                             ? *live.power_profile->power_plan
                                             : std::string("-"));
        hardware_ui::field("Power mode", live.power_profile && live.power_profile->profile
                                             ? *live.power_profile->profile
                                             : std::string("-"));
        if (live.power_profile && !live.power_profile->governors.empty()) {
            std::string governors;
            for (const auto& governor : live.power_profile->governors) {
                governors += (governors.empty() ? "" : ", ") + governor;
            }
            hardware_ui::field("Governors", governors);
        }
        if (info.limits) {
            hardware_ui::field("CPU quota", info.limits->cpu_quota
                                                ? std::format("{:.2f} cores", *info.limits->cpu_quota)
                                                : std::string("unlimited"));
            hardware_ui::field("Memory limit", info.limits->memory
                                                   ? hardware_ui::str(*info.limits->memory)
                                                   : std::string("unlimited"));
            hardware_ui::field("Allowed CPUs", std::to_string(info.limits->allowed_cpus.size()));
        }
        hardware_ui::endFields();

        if (!info.notes.empty()) {
            ImGui::SeparatorText("Notes");
            for (const auto& note : info.notes) {
                ImGui::TextWrapped("%s", note.c_str());
            }
        }
    }

    static void renderOs(const HardwareStatic& info) {
        if (!info.os) {
            ImGui::TextDisabled("Operating system information is unavailable.");
            return;
        }
        const auto& os = *info.os;
        ImGui::SeparatorText("Operating system");
        hardware_ui::beginFields("os");
        hardware_ui::field("Family", std::string(hw::to_string(os.family)));
        hardware_ui::field("Name", os.name);
        hardware_ui::field("Marketing name", os.marketing_name.empty() ? std::string("-") : os.marketing_name);
        hardware_ui::field("Version", os.version);
        hardware_ui::field("Build", hardware_ui::textOr(os.build));
        hardware_ui::field("Kernel", os.kernel);
        hardware_ui::field("Architecture", os.architecture);
        hardware_ui::field("Bitness", std::format("{}-bit", os.bits));
        hardware_ui::endFields();
    }

    static void renderMainboard(const HardwareStatic& info) {
        if (!info.mainboard) {
            ImGui::TextDisabled("Mainboard information is unavailable.");
            return;
        }
        const auto& board = *info.mainboard;
        ImGui::SeparatorText("Mainboard");
        hardware_ui::beginFields("mainboard");
        hardware_ui::field("Vendor", hardware_ui::textOr(board.vendor));
        hardware_ui::field("Name", hardware_ui::textOr(board.name));
        hardware_ui::field("Version", hardware_ui::textOr(board.version));
        hardware_ui::field("Serial number", hardware_ui::textOr(board.serial_number));
        hardware_ui::field("Firmware vendor", hardware_ui::textOr(board.firmware_vendor));
        hardware_ui::field("Firmware version", hardware_ui::textOr(board.firmware_version));
        hardware_ui::endFields();
    }

    void renderCpu(const HardwareStatic& info, const HardwareLive& live, std::size_t index) {
        if (index >= info.cpus.size()) return;
        const auto& cpu = info.cpus[index];

        ImGui::SeparatorText(std::format("CPU {} — {}", index, cpu.model).c_str());

        hardware_ui::beginFields("cpu");
        hardware_ui::field("Vendor", cpu.vendor);
        hardware_ui::field("Model", cpu.model);
        hardware_ui::field("Socket", std::to_string(cpu.socket));
        hardware_ui::field("Physical cores", std::to_string(cpu.physical_cores));
        hardware_ui::field("Logical cores", std::to_string(cpu.logical_cores));

        std::optional<hw::Hertz> base_frequency;
        std::optional<hw::Hertz> max_frequency;
        for (const auto& core : cpu.cores) {
            if (core.base_frequency && (!base_frequency || *base_frequency < *core.base_frequency)) {
                base_frequency = core.base_frequency;
            }
            if (core.max_frequency && (!max_frequency || *max_frequency < *core.max_frequency)) {
                max_frequency = core.max_frequency;
            }
        }
        hardware_ui::field("Base clock", hardware_ui::optStr(base_frequency));
        hardware_ui::field("Max clock", hardware_ui::optStr(max_frequency));

        auto sumCache = [](const std::vector<hw::Core>& cores,
                           std::optional<hw::Bytes> hw::Cache::*member) -> std::optional<hw::Bytes> {
            hw::Bytes total{0};
            bool any = false;
            for (const auto& core : cores) {
                if (auto value = core.cache.*member) {
                    total += *value;
                    any = true;
                }
            }
            return any ? std::optional<hw::Bytes>(total) : std::nullopt;
        };
        // L3 is shared by all cores: hwinfo reports it per core, so summing would multiply it by the core count.
        auto maxCache = [](const std::vector<hw::Core>& cores,
                           std::optional<hw::Bytes> hw::Cache::*member) -> std::optional<hw::Bytes> {
            std::optional<hw::Bytes> largest;
            for (const auto& core : cores) {
                if (auto value = core.cache.*member; value && (!largest || *largest < *value)) {
                    largest = value;
                }
            }
            return largest;
        };
        hardware_ui::field("L1 data", hardware_ui::optStr(sumCache(cpu.cores, &hw::Cache::l1_data)));
        hardware_ui::field("L1 instruction", hardware_ui::optStr(sumCache(cpu.cores, &hw::Cache::l1_instruction)));
        hardware_ui::field("L2", hardware_ui::optStr(sumCache(cpu.cores, &hw::Cache::l2)));
        hardware_ui::field("L3", hardware_ui::optStr(maxCache(cpu.cores, &hw::Cache::l3)));
        hardware_ui::endFields();

        if (!cpu.flags.empty()) {
            std::string flags;
            for (const auto& flag : cpu.flags) {
                flags += (flags.empty() ? "" : ", ") + flag;
            }
            ImGui::Spacing();
            hardware_ui::beginFields("cpu-flags");
            hardware_ui::fieldWrapped("Feature flags", flags);
            hardware_ui::endFields();
        }

        ImGui::SeparatorText("Live");

        const std::size_t logical_cores =
            live.cpu_load && !live.cpu_load->per_thread.empty() ? live.cpu_load->per_thread.size() : cpu.logical_cores;
        hardware_ui::beginFields("cpu-live");
        hardware_ui::field("Logical processors", std::to_string(logical_cores));
        hardware_ui::field("Average load", live.cpu_load ? std::format("{:5.1f} %", live.cpu_load->total * 100.0)
                                                         : std::string("-"));
        if (live.cpu_frequencies && !live.cpu_frequencies->empty()) {
            hardware_ui::field("Current clock", std::format("{}", live.cpu_frequencies->front()));
        }
        hardware_ui::endFields();

        ImGui::Spacing();
        hardware_ui::plotFixed("##cpu-total", cpu_history_, hardware_ui::em(5.0f), 100.0f);

        if (live.cpu_load && !live.cpu_load->per_thread.empty()) {
            ImGui::Spacing();
            if (ImGui::BeginTable("cpu-threads", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(5.0f));
                ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(16.0f));
                ImGui::TableSetupColumn("Clock", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (std::size_t i = 0; i < live.cpu_load->per_thread.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("T%02zu", i);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::ProgressBar(static_cast<float>(live.cpu_load->per_thread[i]), ImVec2(-FLT_MIN, 0.0f),
                                       nullptr);
                    ImGui::TableSetColumnIndex(2);
                    if (live.cpu_frequencies && i < live.cpu_frequencies->size() &&
                        (*live.cpu_frequencies)[i].value != 0) {
                        ImGui::TextUnformatted(std::format("{:>10}", (*live.cpu_frequencies)[i]).c_str());
                    } else {
                        ImGui::TextUnformatted("-");
                    }
                }
                ImGui::EndTable();
            }
        }

        if (!core_history_.empty()) {
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Load history per thread")) {
                constexpr int kColumns = 4;
                if (ImGui::BeginTable("cpu-thread-history", kColumns, ImGuiTableFlags_SizingStretchSame)) {
                    for (std::size_t i = 0; i < core_history_.size(); ++i) {
                        if (i % kColumns == 0) ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(static_cast<int>(i % kColumns));
                        ImGui::TextDisabled("T%02zu", i);
                        hardware_ui::plotFixed(std::format("##core{}", i).c_str(), core_history_[i],
                                               hardware_ui::em(3.0f), 100.0f);
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    void renderMemory(const HardwareStatic& info, const HardwareLive& live) {
        ImGui::SeparatorText("Memory");

        hardware_ui::beginFields("memory");
        hardware_ui::field("Installed", info.memory ? hardware_ui::str(info.memory->total) : std::string("unavailable"));
        const bool modules_reported = info.memory && !info.memory->modules.empty();
        hardware_ui::field("Modules", !info.memory ? std::string("-")
                                       : modules_reported
                                           ? std::to_string(info.memory->modules.size())
                                           : std::string("not reported"));
        hardware_ui::endFields();

        if (live.memory_usage) {
            const auto& usage = *live.memory_usage;
            const hw::Bytes used = usage.total - usage.available;
            ImGui::SeparatorText("Live");
            hardware_ui::beginFields("memory-live");
            hardware_ui::field("Used", std::format("{} ({:.1f} %)", used,
                                                   usage.total.value ? 100.0 * used.to(hw::ByteUnit::B) /
                                                                           usage.total.to(hw::ByteUnit::B)
                                                                     : 0.0));
            // hwinfo reports free and available as the same number on Windows (GlobalMemoryStatusEx exposes only
            // ullAvailPhys), so only one of them is shown.
            hardware_ui::field("Available", hardware_ui::str(usage.available));
            hardware_ui::endFields();

            ImGui::Spacing();
            ImGui::TextDisabled("Used memory, GiB");
            hardware_ui::plotFixed("##ram-used", ram_history_, hardware_ui::em(6.0f),
                                   static_cast<float>(usage.total.to(hw::ByteUnit::GiB)));
        } else {
            ImGui::TextDisabled("Memory usage is unavailable.");
        }

        if (info.memory && !info.memory->modules.empty()) {
            ImGui::SeparatorText("Modules");
            if (ImGui::BeginTable("memory-modules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(2.5f));
                ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(9.0f));
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(7.0f));
                ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(7.0f));
                ImGui::TableSetupColumn("Vendor / part number", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (const auto& module : info.memory->modules) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", module.index);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(hardware_ui::textOr(module.name).c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(hardware_ui::optStr(module.size).c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(hardware_ui::optStr(module.frequency).c_str());
                    ImGui::TableSetColumnIndex(4);
                    const std::string vendor = module.vendor.value_or("-");
                    const std::string model = module.model.value_or("");
                    ImGui::TextUnformatted(model.empty() ? vendor.c_str() : (vendor + " " + model).c_str());
                }
                ImGui::EndTable();
            }
        }
    }

    void renderDisk(const HardwareStatic& info, const HardwareLive& live, std::size_t index) {
        if (index >= info.disks.size()) return;
        const auto& disk = info.disks[index];

        ImGui::SeparatorText(std::format("Disk {} — {}", index, disk.model.value_or("unknown")).c_str());
        hardware_ui::beginFields("disk");
        hardware_ui::field("Model", hardware_ui::textOr(disk.model));
        hardware_ui::field("Vendor", hardware_ui::textOr(disk.vendor));
        hardware_ui::field("Serial number", hardware_ui::textOr(disk.serial_number));
        hardware_ui::field("Size", hardware_ui::str(disk.size));
        hardware_ui::field("Bus", std::string(hw::to_string(disk.bus)));
        hardware_ui::field("Link speed", hardware_ui::optStr(disk.link_speed));
        hardware_ui::endFields();

        if (disk.mount_points.empty()) {
            ImGui::TextDisabled("No mounted filesystems were reported for this disk.");
            return;
        }

        ImGui::SeparatorText("Volumes");
        if (ImGui::BeginTable("disk-mounts", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Mount point", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(8.0f));
            ImGui::TableSetupColumn("File system", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(6.0f));
            ImGui::TableSetupColumn("Free", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(6.0f));
            ImGui::TableSetupColumn("Capacity", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(6.0f));
            ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(10.0f));
            ImGui::TableSetupColumn("Encrypted", ImGuiTableColumnFlags_WidthFixed, hardware_ui::em(5.0f));
            ImGui::TableHeadersRow();

            for (std::size_t mount = 0; mount < disk.mount_points.size(); ++mount) {
                const auto& mount_point = disk.mount_points[mount];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(mount_point.path.string().c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(mount_point.filesystem.value_or("-").c_str());

                std::optional<hw::DiskSpace> space;
                if (index < live.disks.size() && mount < live.disks[index].size()) {
                    space = live.disks[index][mount];
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(space ? hardware_ui::str(space->available).c_str() : "-");

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(space ? hardware_ui::str(space->capacity).c_str() : "-");

                ImGui::TableSetColumnIndex(4);
                if (space && space->capacity.value > 0) {
                    const float used_fraction =
                        1.0f - static_cast<float>(static_cast<double>(space->available.value) /
                                                  static_cast<double>(space->capacity.value));
                    const std::string overlay = std::format("{:.0f} %", used_fraction * 100.0f);
                    ImGui::ProgressBar(std::clamp(used_fraction, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f),
                                       overlay.c_str());
                } else {
                    ImGui::TextUnformatted("-");
                }

                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(mount_point.encrypted ? hardware_ui::yesNo(*mount_point.encrypted).c_str()
                                                             : "-");
            }
            ImGui::EndTable();
        }
    }

    void renderGpu(const HardwareStatic& info, const HardwareLive& live, std::size_t index) {
        if (index >= info.gpus.size()) return;
        const auto& gpu = info.gpus[index];

        ImGui::SeparatorText(std::format("GPU {} — {}", index, gpu.name).c_str());

        hardware_ui::beginFields("gpu");
        hardware_ui::field("Vendor", gpu.vendor);
        hardware_ui::field("Name", gpu.name);
        hardware_ui::field("Type", std::string(hw::to_string(gpu.type)));
        hardware_ui::field("Dedicated memory", hardware_ui::optStr(gpu.dedicated_memory));
        hardware_ui::field("Shared memory", hardware_ui::optStr(gpu.shared_memory));
        hardware_ui::field("Memory type", hardware_ui::textOr(gpu.memory_type));
        hardware_ui::field("Memory bus", gpu.memory_bus_width ? std::format("{}-bit", *gpu.memory_bus_width)
                                                              : std::string("-"));
        hardware_ui::field("L2 cache", hardware_ui::optStr(gpu.l2_cache));
        hardware_ui::field("Max clock", hardware_ui::optStr(gpu.max_frequency));
        hardware_ui::field("Max memory clock", hardware_ui::optStr(gpu.max_memory_frequency));
        hardware_ui::field("Power limit", hardware_ui::optStr(gpu.power_limit));
        hardware_ui::field("Cores", hardware_ui::optStr(gpu.cores));
        hardware_ui::field("Compute units", hardware_ui::optStr(gpu.compute_units));
        hardware_ui::field("Architecture", hardware_ui::textOr(gpu.architecture));
        hardware_ui::field("Compute capability", hardware_ui::textOr(gpu.compute_capability));
        hardware_ui::fieldWrapped("Driver", hardware_ui::textOr(gpu.driver));
        hardware_ui::fieldWrapped("Driver version", hardware_ui::textOr(gpu.driver_version));
        hardware_ui::field("VBIOS", hardware_ui::textOr(gpu.vbios_version));
        hardware_ui::field("Serial number", hardware_ui::textOr(gpu.serial));
        hardware_ui::field("Unified memory", gpu.unified_memory ? hardware_ui::yesNo(*gpu.unified_memory) : "-");

        if (!gpu.compute_apis.empty()) {
            std::string apis;
            for (const auto& api : gpu.compute_apis) {
                apis += (apis.empty() ? "" : ", ") + api.name + (api.version.empty() ? "" : " " + api.version);
            }
            hardware_ui::fieldWrapped("Compute APIs", apis);
        }

        if (gpu.pci) {
            hardware_ui::field("PCI ID", std::format("{:04x}:{:04x}", gpu.pci->vendor_id, gpu.pci->device_id));
            hardware_ui::field("PCI address", gpu.pci->address.value_or("-"));
            hardware_ui::field("PCIe link", hardware_ui::optStr(gpu.pci->max_link));
        }
        hardware_ui::endFields();

        std::optional<hw::GpuStatus> status;
        if (index < live.gpus.size()) status = live.gpus[index];

        ImGui::SeparatorText("Live");
        if (!status) {
            ImGui::TextDisabled("Live monitoring is unavailable for this GPU.");
            return;
        }
        if (status->suspended) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Suspended (powered down)");
        }

        hardware_ui::beginFields("gpu-live");
        hardware_ui::field("Utilization", status->utilization ? std::format("{:5.1f} %", *status->utilization * 100.0)
                                                              : std::string("-"));
        hardware_ui::field("Memory utilization", status->memory_utilization
                                                     ? std::format("{:5.1f} %", *status->memory_utilization * 100.0)
                                                     : std::string("-"));
        hardware_ui::field("Video engine", status->video_utilization
                                              ? std::format("{:5.1f} %", *status->video_utilization * 100.0)
                                              : std::string("-"));
        hardware_ui::field("Memory used", status->memory_used ? std::format("{}{}", *status->memory_used,
                                                                            status->memory_total
                                                                                ? std::format(" / {}", *status->memory_total)
                                                                                : std::string())
                                                              : std::string("-"));
        hardware_ui::field("Temperature", status->temperature ? std::format("{:.0f} °C", *status->temperature)
                                                              : std::string("-"));
        hardware_ui::field("Power draw", status->power ? hardware_ui::str(*status->power) : std::string("-"));
        hardware_ui::field("Fan", status->fan_speed ? std::format("{:.0f} %", *status->fan_speed * 100.0)
                                                    : std::string("-"));
        hardware_ui::field("Clock", status->frequency ? hardware_ui::str(*status->frequency) : std::string("-"));
        hardware_ui::field("Memory clock",
                           status->memory_frequency ? hardware_ui::str(*status->memory_frequency) : std::string("-"));
        hardware_ui::field("PCIe current link", hardware_ui::optStr(status->pcie_link));
        hardware_ui::endFields();

        if (index < gpu_util_history_.size()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Utilization, %");
            hardware_ui::plotFixed(std::format("##gpu-util{}", index).c_str(), gpu_util_history_[index],
                                   hardware_ui::em(5.0f), 100.0f);
        }
        if (index < gpu_memory_history_.size()) {
            ImGui::TextDisabled("Memory used, GiB");
            const float memory_max = gpu.dedicated_memory
                                         ? static_cast<float>(gpu.dedicated_memory->to(hw::ByteUnit::GiB))
                                         : 0.0f;
            if (memory_max > 0.0f) {
                hardware_ui::plotFixed(std::format("##gpu-mem{}", index).c_str(), gpu_memory_history_[index],
                                       hardware_ui::em(5.0f), memory_max);
            } else {
                hardware_ui::plotAuto(std::format("##gpu-mem{}", index).c_str(), gpu_memory_history_[index],
                                      hardware_ui::em(5.0f));
            }
        }
        if (index < gpu_temperature_history_.size()) {
            ImGui::TextDisabled("Temperature, °C");
            hardware_ui::plotAuto(std::format("##gpu-temp{}", index).c_str(), gpu_temperature_history_[index],
                                  hardware_ui::em(5.0f));
        }
    }

    void renderNetwork(const HardwareStatic& info, const HardwareLive& live, std::size_t index) {
        if (index >= info.network.size()) return;
        const auto& nic = info.network[index];

        ImGui::SeparatorText(std::format("Network — {}", nic.name).c_str());
        hardware_ui::beginFields("network");
        hardware_ui::field("Name", nic.name);
        hardware_ui::field("Description", hardware_ui::textOr(nic.description));
        hardware_ui::field("Interface index", std::to_string(nic.index));
        hardware_ui::field("Status", nic.is_up ? "up" : "down");
        hardware_ui::field("Loopback", hardware_ui::yesNo(nic.is_loopback));
        hardware_ui::field("MAC address", hardware_ui::textOr(nic.mac));
        hardware_ui::fieldWrapped("IPv4", nic.ipv4.empty() ? "-" : [&] {
            std::string list;
            for (const auto& address : nic.ipv4) list += (list.empty() ? "" : ", ") + address;
            return list;
        }());
        hardware_ui::fieldWrapped("IPv6", nic.ipv6.empty() ? "-" : [&] {
            std::string list;
            for (const auto& address : nic.ipv6) list += (list.empty() ? "" : ", ") + address;
            return list;
        }());

        std::optional<NetworkLive> stat;
        if (index < live.network.size()) stat = live.network[index];
        if (stat && stat->link_speed_bps) {
            hardware_ui::field("Link speed", hardware_ui::linkSpeed(*stat->link_speed_bps));
        }
        hardware_ui::field("Receive", stat && stat->receive_bps ? hardware_ui::rate(*stat->receive_bps)
                                                                : std::string("-"));
        hardware_ui::field("Transmit", stat && stat->transmit_bps ? hardware_ui::rate(*stat->transmit_bps)
                                                                  : std::string("-"));
        hardware_ui::endFields();

        if (index < network_receive_history_.size()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Receive, KiB/s");
            hardware_ui::plotAuto(std::format("##net-rx{}", index).c_str(), network_receive_history_[index],
                                  hardware_ui::em(5.0f));
            ImGui::TextDisabled("Transmit, KiB/s");
            hardware_ui::plotAuto(std::format("##net-tx{}", index).c_str(), network_transmit_history_[index],
                                  hardware_ui::em(5.0f));
        }
    }

    void renderBattery(const HardwareStatic& info, const HardwareLive& live, std::size_t index) {
        if (index >= info.batteries.size()) return;
        const auto& battery = info.batteries[index];

        ImGui::SeparatorText(std::format("Battery {} — {}", index, battery.model.value_or("unknown")).c_str());
        hardware_ui::beginFields("battery");
        hardware_ui::field("Vendor", hardware_ui::textOr(battery.vendor));
        hardware_ui::field("Model", hardware_ui::textOr(battery.model));
        hardware_ui::field("Serial number", hardware_ui::textOr(battery.serial_number));
        hardware_ui::field("Technology", hardware_ui::textOr(battery.technology));
        hardware_ui::field("Design capacity", hardware_ui::optStr(battery.design_capacity));
        hardware_ui::field("Full charge", hardware_ui::optStr(battery.full_charge_capacity));

        std::optional<hw::BatteryStatus> status;
        if (index < live.batteries.size()) status = live.batteries[index];
        if (status) {
            hardware_ui::field("State", std::string(hw::to_string(status->state)));
            hardware_ui::field("Charge", status->charge ? std::format("{:.0f} %", *status->charge * 100.0)
                                                        : std::string("-"));
        }
        if (live.power_source) {
            hardware_ui::field("Power source", std::string(hw::to_string(*live.power_source)));
        }
        hardware_ui::endFields();

        if (status && status->charge) {
            ImGui::Spacing();
            ImGui::ProgressBar(static_cast<float>(*status->charge), ImVec2(-FLT_MIN, 0.0f), nullptr);
        }
        if (index < battery_history_.size() && battery_history_[index].hasData()) {
            ImGui::TextDisabled("Charge, %");
            hardware_ui::plotFixed(std::format("##battery{}", index).c_str(), battery_history_[index],
                                   hardware_ui::em(5.0f), 100.0f);
        }
    }

    // ----- state ---------------------------------------------------------------------------------------------------

    // Shared between the worker and the UI thread.
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const HardwareStatic> static_info_;
    HardwareLive live_{};
    Clock::time_point last_sample_{};
    bool has_sample_ = false;
    bool stop_ = false;  // guarded by mutex_

    // UI thread only.
    HardwareLive shown_live_{};
    unsigned int shown_generation_ = 0;
    Selection selection_{};

    hardware_ui::History cpu_history_{};
    std::vector<hardware_ui::History> core_history_{};
    hardware_ui::History ram_history_{};
    std::vector<hardware_ui::History> gpu_util_history_{};
    std::vector<hardware_ui::History> gpu_memory_history_{};
    std::vector<hardware_ui::History> gpu_temperature_history_{};
    std::vector<hardware_ui::History> network_receive_history_{};
    std::vector<hardware_ui::History> network_transmit_history_{};
    std::vector<hardware_ui::History> battery_history_{};

    // Flags shared between the threads.
    std::atomic<bool> autoRefresh_{true};
    std::atomic<bool> request_{false};
    std::atomic<bool> sampling_{false};
    std::atomic<unsigned int> generation_{0};

    // Declared last: the thread is started in the constructor, when every member above already exists.
    std::thread worker_;
};
