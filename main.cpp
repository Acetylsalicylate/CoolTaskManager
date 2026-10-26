#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <string>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

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

    // Let the window follow the monitor it is moved to; the initial font/style scale above stays as it is.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    return x_scale > 1.0f ? std::min(x_scale, 3.0f) : 1.0f;
}

void loadFont(float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();

    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement (includes ° and µ)
        0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
        0x2000, 0x206F, // General punctuation: em dash, en dash, quotes, ellipsis (rendered as "?" otherwise)
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
    // SHOpenFolderAndSelectItems (the "Open" button of the process tab) needs COM on the calling thread; the shell
    // expects a single-threaded apartment. GLFW and Dear ImGui do not use COM, so this does not interfere with them.
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (void)com_result;

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

    // GLFW registers its window class with the icon named GLFW_ICON from the executable (CoolTaskManager.rc embeds it)
    // but never sets the small (16 px) class icon, which the title bar and Alt-Tab prefer. Fill it from the same
    // resource, so window creation has no chance to show GLFW's stock icon.
    if (HICON small_icon = static_cast<HICON>(LoadImageW(nullptr, MAKEINTRESOURCEW(32512), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR))) {
        SetClassLongPtrW(reinterpret_cast<HWND>(glfwGetWin32Window(window)), GCLP_HICONSM,
                         reinterpret_cast<LONG_PTR>(small_icon));
    }

    // Creating a window replaces the class icons with GLFW's generic ones. An empty image list puts the icons of the
    // window class back (GCLP_HICON / GCLP_HICONSM), i.e. IDI_APP = 32512 embedded from icon.ico by
    // CoolTaskManager.rc. Without this the taskbar, Alt-Tab and the title bar show the default application icon.
    glfwSetWindowIcon(window, 0, nullptr);

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

        // A minimized window has a zero-sized framebuffer and a zero DisplaySize. Building a frame from that produces
        // negative widget sizes (the hardware graphs and children would get a negative width/height), so wait until
        // the window is restored instead of rendering.
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

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

    mainWindow.releaseGpuResources();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (SUCCEEDED(com_result)) CoUninitialize();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
