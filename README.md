<p align="right">
  🇷🇺 <a href="README_RU.md">Русский</a> | 🇬🇧 <b>English</b>
</p>

# Cool Task Manager

A lightweight, modern system monitoring and task management application for Windows, built with C++23, Dear ImGui, GLFW, and OpenGL.

## Features

* **Process Viewer Tab:**
  * Displays a comprehensive list of running system processes.
  * Tracks **PID**, **PPID**, owner **user**, **session**, **CPU usage**, **RAM**, **thread** and **handle** counts, **priority**, and the executable **path**.
  * Hovering a process name shows its start time, total CPU time, and detailed memory counters (working set, private bytes, pagefile).
  * Click a column header to sort by it; columns are auto-fitted to their content and can be resized.
  * Manual refresh via the **Update** button, plus an optional automatic refresh every second (**Auto** toggle).
* **Hardware Monitoring Tab:**
  * The left pane lists every component separately: computer, operating system, mainboard, each CPU, RAM with its modules, each disk and its volumes, each GPU, each network adapter, and the battery.
  * The details pane shows everything hwinfo reports for the selected component: vendor, model, serial numbers, firmware, caches, memory modules, PCI IDs, driver versions, link speeds.
  * **CPU:** average load, per-thread utilization, clock rates, a load graph and per-thread load history.
  * **RAM:** used, available, and free memory with a usage graph.
  * **Disks:** free and total space of every volume with a usage bar.
  * **GPU:** utilization, memory usage, temperature, power draw, fan, clocks, PCIe link, plus utilization, memory and temperature graphs.
  * **Network:** link speed and receive/transmit throughput, sampled through WinAPI (`GetIfEntry2`) and plotted over time.
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

### Application icon

The window, taskbar and Explorer icon comes from `icon.ico` and is embedded into the executable by the resource script `CoolTaskManager.rc`; CMake compiles it with the Windows SDK resource compiler (enabled by `enable_language(RC)`). The script stores the same file under two names: `IDI_APP` for the executable and `GLFW_ICON`, which GLFW loads as its window class icon. To change the icon, replace `icon.ico` and rebuild — nothing else needs to be edited. Keep the `.rc` file name ASCII-only: resource compilers older than 14.4x reject non-ASCII paths.

> If the old icon stays in the taskbar after a rebuild, unpin the application: Windows caches icons of pinned and recently used items.
