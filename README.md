# Cool Task Manager

A lightweight, modern system monitoring and task management application for Windows, built with C++20, Dear ImGui, GLFW, and OpenGL.

## Features

* **Process Viewer Tab:** 
  * Displays a comprehensive list of running system processes.
  * Tracks **PID**, **PPID**, **Thread count**, and executable **Process names**.
  * Manual refresh capability via an update button.
* **Hardware Monitoring Tab:**
  * **CPU:** Real-time average load, per-thread utilization, and core frequencies.
  * **RAM:** Live tracking of free and available memory.
  * **Disks:** Storage capacity and free space.
  * **GPU:** Real-time utilization, memory usage and temperature.
* **Modern UI:** Sleek dark-themed graphical interface powered by Dear ImGui.

## Tech Stack & Dependencies

* **Language:** C++20 (utilizing modern features like `std::format` and `std::chrono`)
* **GUI Framework:** [Dear ImGui](https://github.com/ocornut/imgui) (v1.90.4)
* **Windowing & Rendering:** GLFW & OpenGL
* **Hardware Information:** [hwinfo](https://github.com/WorkspaceONE/hwinfo) library
* **Platform API:** Windows API (`tlhelp32.h`, `windows.h`) for process enumeration

## System Requirements

* **Operating System:** Windows (Windows-only due to WinAPI dependencies)
* **Compiler:** A C++20 compatible compiler (MSVC recommended)
* **Build System:** CMake 3.16 or higher

## Building and Running

1. Clone the repository with submodules:
   ```bash
   git clone --recursive [https://github.com/Acetylsalicylate/CoolTaskManager.git](https://github.com/Acetylsalicylate/CoolTaskManager.git)
   cd CoolTaskManager
2. Create and configure the build directory using CMake:
   ```bash
   mkdir build
   cd build
   cmake ..
3. Build the project:
   ```bash
   cmake --build . --config Release
