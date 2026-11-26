// CAW entry point for macOS and Linux: GLFW window + OpenGL 3 (Windows uses main.cpp / Direct3D 11).
#include "app.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>

static bool fileExists(const char* p) { struct stat st; return p && stat(p, &st) == 0; }
static const char* firstFont(std::initializer_list<const char*> c) { for (const char* p : c) if (fileExists(p)) return p; return nullptr; }

static void onDrop(GLFWwindow* w, int count, const char** paths) {
    if (count <= 0 || !paths[0]) return;
    const std::string p = paths[0];
    const bool project = p.size() > 4 && (p.compare(p.size() - 4, 4, ".caw") == 0 || p.compare(p.size() - 4, 4, ".CAW") == 0);
    if (project) { appOpenProjectPath(p.c_str()); return; }
    double x = 0, y = 0; glfwGetCursorPos(w, &x, &y);
    appQueueFileDrop(p.c_str(), (int)x, (int)y);
}
static void onClose(GLFWwindow* w) { glfwSetWindowShouldClose(w, GLFW_FALSE); appRequestClose(); }   // the app asks "save changes?" first

int main(int argc, char** argv) {
    if (!glfwInit()) return 1;
#ifdef __APPLE__
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    GLFWwindow* win = glfwCreateWindow(1400, 800, "CAW - Code Audio Workstation (build 20)", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetDropCallback(win, onDrop);
    glfwSetWindowCloseCallback(win, onClose);

    float dpi = 1.f;
#ifndef __APPLE__   // macOS: the framebuffer is already Retina-scaled, fonts stay at 1x
    { float sx = 1.f, sy = 1.f; glfwGetWindowContentScale(win, &sx, &sy); dpi = sx > 0.f ? sx : 1.f; }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    appSetupStyle(dpi);

#ifdef __APPLE__
    const char* uiPath   = firstFont({"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial.ttf"});
    const char* codePath = firstFont({"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf"});
    const char* codeB = nullptr; const char* codeI = nullptr;
#else
    const char* uiPath   = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"});
    const char* codePath = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf"});
    const char* codeB    = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"});
    const char* codeI    = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Oblique.ttf"});
#endif
    ImFont* uiFont = uiPath ? io.Fonts->AddFontFromFileTTF(uiPath, 17.f * dpi) : nullptr;
    if (!uiFont) uiFont = io.Fonts->AddFontDefault();
    io.FontDefault = uiFont;
    ImFont* codeRegular = codePath ? io.Fonts->AddFontFromFileTTF(codePath, 17.f * dpi) : nullptr;
    ImFont* codeBold    = codeB ? io.Fonts->AddFontFromFileTTF(codeB, 17.f * dpi) : nullptr;
    ImFont* codeItalic  = codeI ? io.Fonts->AddFontFromFileTTF(codeI, 17.f * dpi) : nullptr;
    appSetCodeFonts(codeRegular ? codeRegular : uiFont, codeBold, codeItalic);
    ImFont* markUi   = uiPath ? io.Fonts->AddFontFromFileTTF(uiPath, 76.f * dpi) : nullptr;
    ImFont* markCode = codePath ? io.Fonts->AddFontFromFileTTF(codePath, 76.f * dpi) : nullptr;
    appSetMarkFonts(markUi ? markUi : uiFont, markCode ? markCode : (codeRegular ? codeRegular : uiFont));
    appPrepareLogo();

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    auto present = [&](bool draw) {
        int w = 0, h = 0; glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h); glClearColor(0.06f, 0.06f, 0.07f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
        if (draw) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    };
    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
    appDrawStartupSplash();
    ImGui::Render(); present(true);
    appInit();
    if (argc > 1 && argv[1] && std::strncmp(argv[1], "-psn_", 5) != 0) appOpenProjectPath(argv[1]);   // macOS Finder passes -psn_*

    using clk = std::chrono::steady_clock;
    auto activeUntil = clk::now() + std::chrono::seconds(2);
    double lastX = -1, lastY = -1;
    while (true) {
        // same frame budget as Windows: 60 fps while active / playing, 30 engine idle, 5 still
        const auto now = clk::now();
        const bool busy = appNeedsResponsiveFrames() || now < activeUntil;
        const double wait = busy ? 0.016 : (appEnginesIdleRunning() ? 0.033 : 0.2);
        glfwWaitEventsTimeout(wait);   // sleeps until input or the timeout: no busy loop
        double cx, cy; glfwGetCursorPos(win, &cx, &cy);
        if (cx != lastX || cy != lastY || glfwGetWindowAttrib(win, GLFW_FOCUSED) && ImGui::GetIO().InputQueueCharacters.Size) {
            lastX = cx; lastY = cy; activeUntil = clk::now() + std::chrono::seconds(2);
        }
        for (int b = 0; b < 3; ++b) if (glfwGetMouseButton(win, b) == GLFW_PRESS) activeUntil = clk::now() + std::chrono::seconds(2);

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        const bool visible = !glfwGetWindowAttrib(win, GLFW_ICONIFIED);
        appFrame(io.DeltaTime, visible);
        if (appQuitRequested()) { ImGui::EndFrame(); break; }
        if (!visible) { ImGui::EndFrame(); continue; }
        ImGui::Render(); present(true);
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
