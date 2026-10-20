#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <string>

#include <GLFW/glfw3.h>

#include "MainWindow.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"

// Entry point of the GUI subsystem build (the WIN32 target in CMakeLists.txt): no console window is created, so
// startup failures are reported with a message box instead of stderr.

namespace {

std::string g_lastGlfwError = "unknown error";

// GLFW reports every failure (window, context, monitor, ...) through this callback; the text ends up in the message box.
void glfwErrorCallback(int error, const char* description) {
    g_lastGlfwError = std::to_string(error) + ": " + (description != nullptr ? description : "unknown error");
}

void showError(const std::string& message) {
    const std::string text = "Cool Task Manager\n\n" + message;
    MessageBoxA(nullptr, text.c_str(), "Cool Task Manager", MB_OK | MB_ICONERROR);
}

// GLFW is per-monitor DPI aware, so on a 150% display a 1000x700 window would be tiny and the text blurry. The whole
// UI (window size, ImGui style and font) is therefore scaled by the content scale of the primary monitor.
float detectUiScale() {
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor == nullptr) return 1.0f;

    float x_scale = 1.0f;
    float y_scale = 1.0f;
    glfwGetMonitorContentScale(monitor, &x_scale, &y_scale);
    return x_scale > 1.0f ? std::min(x_scale, 3.0f) : 1.0f;
}

void loadFont(float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();

    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
        0x2DE0, 0x2DFF, // Cyrillic Extended-A
        0xA640, 0xA69F, // Cyrillic Extended-B
        0,
    };

    // Oversampling improves the hinted, downscaled glyphs that DPI scaling produces.
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;

    // Segoe UI has the best Cyrillic coverage; fall back to any installed UI font instead of failing.
    // The font is loaded at the scaled size, so it stays sharp on high-DPI displays.
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    for (const char* path : candidates) {
        if (io.Fonts->AddFontFromFileTTF(path, 16.0f * ui_scale, &config, ranges) != nullptr) return;
    }
    io.Fonts->AddFontDefault();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous_instance*/, PWSTR /*command_line*/,
                    int /*show_command*/) {
    glfwSetErrorCallback(glfwErrorCallback);

    if (glfwInit() == GLFW_FALSE) {
        showError("Could not initialize GLFW: " + g_lastGlfwError);
        return 1;
    }

    const float ui_scale = detectUiScale();

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(static_cast<int>(1000.0f * ui_scale), static_cast<int>(700.0f * ui_scale),
                                          "Cool Task Manager", nullptr, nullptr);
    if (window == nullptr) {
        showError("Could not create the application window: " + g_lastGlfwError);
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // fixed full-window layout: nothing to persist in imgui.ini

    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(ui_scale);
    loadFont(ui_scale);

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        showError("Could not initialize the GLFW backend.");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    const char* glsl_version = "#version 130";
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        showError("Could not initialize the OpenGL 3 backend.");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    MainWindow mainWindow;

    // One ImGui frame per swap; glfwSwapInterval(1) ties the loop to the display refresh rate.
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // New frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        mainWindow.render();

        // Render. The viewport uses the framebuffer size (not the window size) so the image is not stretched on
        // displays with a fractional scaling factor.
        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
