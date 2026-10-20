<p align="right">
  🇷🇺 <a href="README_RU.md">Русский</a> | 🇬🇧 <b>English</b>
</p>

# Cool Task Manager

A lightweight, modern system monitoring and task management application for Windows, built with C++23, Dear ImGui, GLFW, and OpenGL.

## Features

* **Process Viewer Tab:**
  * Displays a comprehensive list of running system processes.
  * Tracks **PID**, **PPID**, **Thread count**, and executable **Process names**.
  * Manual refresh via the **Update** button, plus an optional automatic refresh every second (**Auto** toggle).
* **Hardware Monitoring Tab:**
  * **CPU:** Real-time average load, per-thread utilization, and core frequencies.
  * **RAM:** Used, available, and total physical memory.
  * **Disks:** Storage capacity and free space of every mount point.
  * **GPU:** Real-time utilization, memory usage and temperature.
  * Hardware data is collected on a background thread, so the window stays responsive while the system is polled.
* **Modern UI:** Sleek dark-themed graphical interface powered by Dear ImGui.

## Tech Stack & Dependencies

* **Language:** C++23 (utilizing modern features like `std::format`, `std::chrono` and `std::expected`)
* **GUI Framework:** [Dear ImGui](https://github.com/ocornut/imgui) (v1.90.4, fetched by CMake)
* **Windowing & Rendering:** [GLFW](https://github.com/glfw/glfw) & OpenGL
* **Hardware Information:** [hwinfo](https://github.com/lfreist/hwinfo) library
* **Platform API:** Windows API (`tlhelp32.h`, `windows.h`) for process enumeration

## System Requirements

* **Operating System:** Windows (Windows-only due to WinAPI dependencies)
* **Compiler:** A C++23 compatible compiler (MSVC with Visual Studio 2022 or newer)
* **Build System:** CMake 3.22 or higher
* **Network:** the first CMake configure clones Dear ImGui; later builds work offline.

## Building and Running

1. Clone the repository with submodules:

   ```bash
   git clone --recursive https://github.com/Acetylsalicylate/CoolTaskManager.git
   cd CoolTaskManager
   ```

   For an existing clone, fetch the submodules with:

   ```bash
   git submodule update --init --recursive
   ```

2. Configure and build the project:

   ```bash
   cmake -S . -B build
   cmake --build build --config Release
   ```

3. Run `build/Release/CoolTaskManager.exe` (Visual Studio generator).

> hwinfo is linked as shared libraries, so the `hwinfo_*.dll` files have to stay next to the executable. CMake already puts them into the same output directory as the application.
