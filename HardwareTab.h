#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <format>
#include <chrono>
#include <hwinfo/cpu.h>
#include <hwinfo/disk.h>
#include <hwinfo/gpu.h>
#include <hwinfo/monitoring.h>
#include <hwinfo/ram.h>
#include "imgui.h"

class HardwareTab {
public:
    HardwareTab() {
        initStaticInfo();
        lastUpdate = std::chrono::steady_clock::now();
        updateStats();
    }

    void render() {
        // Update 1\s
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() >= 1000) {
            updateStats();
            lastUpdate = now;
        }

        ImGui::BeginChild("HardwareScroll", ImVec2(0, 0), false);

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "=== STATIC INFO ===");
        ImGui::TextUnformatted(staticInfo.c_str());

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "=== LIVE DATA ===");
        ImGui::TextUnformatted(dynamicInfo.c_str());

        ImGui::EndChild();
    }

private:
    void updateStats() {
        dynamicInfo.clear();

        static hwinfo::CpuSampler cpu_sampler;
        auto cpu_load = cpu_sampler.sample();
        auto freqs = hwinfo::cpu_frequencies();

        if (cpu_load) {
            dynamicInfo += std::format("CPU avg : {:5.1f}%\n", cpu_load->total * 100.0);
            for (std::size_t i = 0; i < cpu_load->per_thread.size(); ++i) {
                double u = cpu_load->per_thread[i];
                std::string freq_str = (freqs && i < freqs->size()) ? std::format("{:>10}", (*freqs)[i]) : "";
                dynamicInfo += std::format("  T{:02} : {:5.1f}%  {}\n", i, u * 100.0, freq_str);
            }
        }
        else {
            dynamicInfo += "CPU load data unavailable.\n";
        }

        auto mem_usage = hwinfo::memory_usage();
        if (mem_usage) {
            dynamicInfo += std::format("\nRAM free: {:.2GiB}  available: {:.2GiB}\n", mem_usage->free, mem_usage->available);
        }

        for (const auto& path : mount_points) {
            auto space = hwinfo::disk_space(path);
            if (space) {
                dynamicInfo += std::format("Disk [{}] free: {:.2GiB} of {:.2GiB}\n", path.string(), space->available, space->capacity);
            }
        }

        for (std::size_t i = 0; i < gpu_samplers.size(); ++i) {
            auto gpu_status = gpu_samplers[i].sample();
            if (gpu_status) {
                if (gpu_status->suspended) {
                    dynamicInfo += std::format("GPU [{}] : suspended (powered down)\n", i);
                }
                else {
                    double util = gpu_status->utilization ? *gpu_status->utilization * 100.0 : 0.0;
                    dynamicInfo += std::format("GPU [{}] : util: {:5.1f}% | mem: {} | temp: {}\n",
                        i, util,
                        gpu_status->memory_used ? std::format("{:.2GiB}", *gpu_status->memory_used) : "-",
                        gpu_status->temperature ? std::format("{:.0f}°C", *gpu_status->temperature) : "-");
                }
            }
        }
    }

    void initStaticInfo() {
        const auto cpus = hwinfo::cpus();
        for (const auto& cpu : cpus.value_or(std::vector<hwinfo::Cpu>{})) {
            staticInfo += std::format("CPU : {}\n", cpu);
        }

        const auto memory = hwinfo::memory();
        if (memory) {
            staticInfo += std::format("RAM : {} total\n", memory->total);
        }

        const auto disks = hwinfo::disks();
        for (const auto& disk : disks.value_or(std::vector<hwinfo::Disk>{})) {
            staticInfo += std::format("Disk: [{}] {}\n", disk.index, disk);
            for (const auto& mp : disk.mount_points) {
                mount_points.push_back(mp.path);
            }
        }

        const auto gpus = hwinfo::gpus();
        for (const auto& gpu : gpus.value_or(std::vector<hwinfo::Gpu>{})) {
            staticInfo += std::format("GPU : [{}] {}\n", gpu.index, gpu);
            gpu_samplers.emplace_back(gpu);
        }
    }

    std::string staticInfo;
    std::string dynamicInfo;
    std::vector<std::filesystem::path> mount_points;
    std::vector<hwinfo::GpuSampler> gpu_samplers;
    std::chrono::steady_clock::time_point lastUpdate;
};