#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <hwinfo/cpu.h>
#include <hwinfo/disk.h>
#include <hwinfo/gpu.h>
#include <hwinfo/monitoring.h>
#include <hwinfo/ram.h>

#include "imgui.h"

// Hardware tab: static machine information plus live utilization, collected from hwinfo.
//
// Threading: everything that talks to hwinfo runs on a single background thread (gpus() may load vendor libraries such
// as NVML/CUDA, and sampling queries performance counters, so both can block for a while). The worker publishes
// ready-made text snapshots; render() only copies those, so the UI thread never waits for the hardware.
class HardwareTab {
public:
    HardwareTab() { worker_ = std::thread([this] { workerLoop(); }); }

    ~HardwareTab() {
        // Stop the worker and wait for it: it may be in the middle of a hwinfo call, and it uses members of this object.
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
        // Manual refresh: the worker samples right away, without waiting for the next tick.
        if (ImGui::Button("Update")) {
            request_.store(true, std::memory_order_relaxed);
            cv_.notify_all();
        }

        ImGui::SameLine();
        // Periodic refresh on/off; while it is off the worker only samples on request.
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

        ImGui::Separator();

        // Copy the published snapshots only when they changed (shared strings are guarded by mutex_).
        if (staticDisplayed_.empty() && staticReady_.load(std::memory_order_acquire)) {
            std::lock_guard lock(mutex_);
            staticDisplayed_ = staticInfo_;
        }
        if (generation_.load(std::memory_order_acquire) != displayedGeneration_) {
            std::lock_guard lock(mutex_);
            dynamicDisplayed_ = dynamicInfo_;
            displayedGeneration_ = generation_.load(std::memory_order_acquire);
        }

        ImGui::BeginChild("HardwareScroll", ImVec2(0.0f, 0.0f), false);

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "=== STATIC INFO ===");
        if (staticDisplayed_.empty()) {
            ImGui::TextDisabled("Collecting hardware information...");
        } else {
            ImGui::TextUnformatted(staticDisplayed_.c_str());
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "=== LIVE DATA ===");
        if (dynamicDisplayed_.empty()) {
            ImGui::TextDisabled("Waiting for the first sample...");
        } else {
            ImGui::TextUnformatted(dynamicDisplayed_.c_str());
        }

        ImGui::EndChild();
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kUpdateInterval = std::chrono::seconds(1);

    void workerLoop() {
        // Static information (especially gpus(), which may load vendor libraries) is collected here as well: it can
        // take a moment and would otherwise freeze the window while it is being created.
        {
            std::string static_info = buildStaticInfo();
            std::lock_guard lock(mutex_);
            staticInfo_ = std::move(static_info);
        }
        staticReady_.store(true, std::memory_order_release);

        hwinfo::CpuSampler sampler;
        (void)sampler.sample();  // baseline, so the first interval is not close to zero

        // Sleep until there is something to do: a manual request, automatic mode, or shutdown.
        std::unique_lock lock(mutex_);
        while (!stop_) {
            cv_.wait(lock, [this] { return stop_ || request_ || autoRefresh_; });
            if (stop_) break;

            if (autoRefresh_) {
                // Wait for the next periodic tick; a manual request, disabling the mode or shutdown wakes us earlier.
                cv_.wait_for(lock, kUpdateInterval, [this] { return stop_ || request_ || !autoRefresh_; });
                if (stop_) break;
                if (!autoRefresh_ && !request_) continue;
            }

            request_ = false;
            sampling_.store(true, std::memory_order_relaxed);
            // Sampling is the slow part: do it without holding the lock, so render() can read the previous snapshot.
            lock.unlock();

            std::string snapshot = buildSnapshot(sampler);

            lock.lock();
            dynamicInfo_ = std::move(snapshot);
            lastSample_ = Clock::now();
            hasSample_ = true;
            generation_.fetch_add(1, std::memory_order_release);
            sampling_.store(false, std::memory_order_relaxed);
        }
    }

    // Runs once, on the worker thread, before the first sample. Fills the lists the snapshots are built from.
    std::string buildStaticInfo() {
        std::string info;

        if (const auto cpus = hwinfo::cpus()) {
            for (const auto& cpu : *cpus) {
                info += std::format("CPU : {}\n", cpu);
            }
        } else {
            info += std::format("CPU : information unavailable ({})\n", cpus.error());
        }

        if (const auto memory = hwinfo::memory()) {
            info += std::format("RAM : {} total\n", memory->total);
        } else {
            info += std::format("RAM : information unavailable ({})\n", memory.error());
        }

        if (const auto disks = hwinfo::disks()) {
            for (const auto& disk : *disks) {
                info += std::format("Disk: [{}] {}\n", disk.index, disk);
                for (const auto& mount_point : disk.mount_points) {
                    mountPoints_.push_back(mount_point.path);
                }
            }
        } else {
            info += std::format("Disk: information unavailable ({})\n", disks.error());
        }

        if (const auto gpus = hwinfo::gpus()) {
            for (const auto& gpu : *gpus) {
                info += std::format("GPU : [{}] {}\n", gpu.index, gpu);
                gpuSamplers_.emplace_back(gpu);
            }
        } else {
            info += std::format("GPU : information unavailable ({})\n", gpus.error());
        }

        return info;
    }

    std::string buildSnapshot(hwinfo::CpuSampler& sampler) {
        // hwinfo's Bytes/Hertz formatters pick a readable unit by themselves ("{}" -> "15.9 GiB"), which keeps the
        // numbers short without any manual scaling here.
        std::string text;

        if (const auto cpu_load = sampler.sample()) {
            text += std::format("CPU avg : {:5.1f}{}\n", cpu_load->total * 100.0, "%");
            const auto frequencies = hwinfo::cpu_frequencies();
            for (std::size_t i = 0; i < cpu_load->per_thread.size(); ++i) {
                const double load = cpu_load->per_thread[i] * 100.0;
                if (frequencies && i < frequencies->size() && (*frequencies)[i].value != 0) {
                    text += std::format("  T{:02} : {:5.1f}{}  {:>10}\n", i, load, "%", (*frequencies)[i]);
                } else {
                    text += std::format("  T{:02} : {:5.1f}{}\n", i, load, "%");
                }
            }
        } else {
            text += std::format("CPU load unavailable ({})\n", cpu_load.error());
        }

        if (const auto memory = hwinfo::memory_usage()) {
            const auto used = memory->total - memory->available;
            // hwinfo reports free and available as the same number on Windows: GlobalMemoryStatusEx only exposes
            // ullAvailPhys, and that already includes the standby (cached) pages. So show the figure once.
            text += std::format("\nRAM used: {}  available: {}  total: {}\n", used, memory->available, memory->total);
        } else {
            text += std::format("\nRAM usage unavailable ({})\n", memory.error());
        }

        for (const auto& path : mountPoints_) {
            if (const auto space = hwinfo::disk_space(path)) {
                text += std::format("Disk [{}] free: {} of {}\n", path.string(), space->available, space->capacity);
            } else {
                text += std::format("Disk [{}] unavailable ({})\n", path.string(), space.error());
            }
        }

        for (std::size_t i = 0; i < gpuSamplers_.size(); ++i) {
            const auto status = gpuSamplers_[i].sample();
            if (!status) {
                text += std::format("GPU [{}] status unavailable ({})\n", i, status.error());
                continue;
            }
            if (status->suspended) {
                text += std::format("GPU [{}] : suspended (powered down)\n", i);
                continue;
            }

            const std::string utilization =
                status->utilization ? std::format("{:5.1f}{}", *status->utilization * 100.0, "%") : "    -";
            const std::string memory = status->memory_used ? std::format("{}", *status->memory_used) : "-";
            const std::string temperature =
                status->temperature ? std::format("{:.0f}", *status->temperature) + " °C" : "-";

            text += std::format("GPU [{}] : util: {} | mem: {} | temp: {}\n", i, utilization, memory, temperature);
        }

        return text;
    }

    double secondsSinceLastSample() {
        std::lock_guard lock(mutex_);
        if (!hasSample_) return -1.0;
        return std::chrono::duration<double>(Clock::now() - lastSample_).count();
    }

    // Shared between the UI thread and the worker.
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string staticInfo_{};
    std::string dynamicInfo_{};
    Clock::time_point lastSample_{};
    bool hasSample_ = false;
    bool stop_ = false;  // guarded by mutex_

    // UI thread only.
    std::string staticDisplayed_{};
    std::string dynamicDisplayed_{};
    unsigned int displayedGeneration_ = 0;

    // Worker thread only (filled before staticReady_ is set).
    std::vector<std::filesystem::path> mountPoints_{};
    std::vector<hwinfo::GpuSampler> gpuSamplers_{};

    // Flags shared between the threads: the worker inspects them while the UI thread changes them.
    std::atomic<bool> autoRefresh_{true};       // periodic sampling on/off
    std::atomic<bool> request_{false};          // one-shot sample requested by the Update button
    std::atomic<bool> sampling_{false};         // a sample is currently running
    std::atomic<bool> staticReady_{false};      // staticInfo_ has been published
    std::atomic<unsigned int> generation_{0};   // incremented after every new snapshot

    // Declared last: the thread is started in the constructor, when every member above already exists.
    std::thread worker_;
};
