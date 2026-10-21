# hwinfo requires CMake 3.22, so the project cannot advertise an older minimum.
cmake_minimum_required(VERSION 3.22)
project(CoolTaskManager LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CoolTaskManager is currently supported only on Windows!")
endif()

# hwinfo's public API uses C++23 (<expected>, std::to_underlying), so the application is built as C++23 as well.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The parts of GLFW the application does not use.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# glfw and hwinfo are git submodules: clone with --recursive or run
# `git submodule update --init --recursive` inside an existing clone.
add_subdirectory(third_party/glfw)
add_subdirectory(third_party/hwinfo)

# ImGui is not vendored: it is cloned by the first configure, which therefore needs network access.
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_backend
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.h
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui_backend PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_backend PRIVATE glfw)

find_package(OpenGL REQUIRED)

# WIN32: GUI subsystem, so no console window is opened next to the application window.
add_executable(CoolTaskManager WIN32
    main.cpp
)

if(MSVC)
    # Sources are UTF-8: keep the degree sign and other non-ASCII literals intact.
    target_compile_options(CoolTaskManager PRIVATE /utf-8)
endif()

target_link_libraries(CoolTaskManager PRIVATE
    glfw
    hwinfo
    OpenGL::GL
    imgui_backend
    advapi32  # process token queries (OpenProcessToken, LookupAccountSidW) in ProcessTab
)
