// macOS and Linux entry point: GLFW window + OpenGL 3. (Windows uses main.cpp with Direct3D 11.)
#include "app.h"
#include "platform.h"
#include "branding.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <chrono>

static GLFWwindow* g_window = nullptr;

static void onDrop(GLFWwindow*, int count, const char** paths) {
    if (count > 0 && paths && paths[0]) appOpenProjectPath(paths[0]);
}

static void onClose(GLFWwindow* w) {
    glfwSetWindowShouldClose(w, GLFW_FALSE);
    appRequestClose();   // asks "save changes?" in the program when needed, then the main loop ends
}

int main(int argc, char** argv) {
    glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); });
    if (!glfwInit()) return 1;

#ifdef __APPLE__
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    float dpi = 1.0f;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        float xs = 1.f, ys = 1.f;
        glfwGetMonitorContentScale(mon, &xs, &ys);
        if (xs > 0.f) dpi = xs;
    }
#ifdef __APPLE__
    dpi = 1.0f;   // macOS: window sizes are in points, the retina framebuffer is handled below
#endif

    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    g_window = glfwCreateWindow((int)(1400 * dpi), (int)(800 * dpi), "CAW - Code Audio Workstation", nullptr, nullptr);
    if (!g_window) { glfwTerminate(); return 1; }
#ifndef __APPLE__
    std::vector<unsigned char> icon = caw_brand::icon(64);
    GLFWimage windowIcon = {64, 64, icon.data()};
    glfwSetWindowIcon(g_window, 1, &windowIcon);
#endif
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);
    glfwSetDropCallback(g_window, onDrop);
    glfwSetWindowCloseCallback(g_window, onClose);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef __APPLE__
    io.ConfigMacOSXBehaviors = true;   // Cmd instead of Ctrl for copy / paste / undo in text boxes
#endif

    // Retina: render fonts at framebuffer resolution and scale the UI back down
    float fbScale = 1.0f;
    {
        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(g_window, &ww, &wh);
        glfwGetFramebufferSize(g_window, &fw, &fh);
        if (ww > 0 && fw > ww) fbScale = (float)fw / (float)ww;
    }
    appSetupStyle(dpi);
    const float fontPx = 17.0f * dpi * fbScale;
    ImFont* uiFont = nullptr;
    const std::string uiPath = plat::uiFontPath();
    if (!uiPath.empty()) uiFont = io.Fonts->AddFontFromFileTTF(uiPath.c_str(), fontPx);
    if (uiFont) io.FontDefault = uiFont;
    else { uiFont = io.Fonts->AddFontDefault(); io.FontDefault = uiFont; }
    auto loadCode = [&](int style) -> ImFont* {
        const std::string path = plat::codeFontPath(style);
        return path.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(path.c_str(), fontPx);
    };
    ImFont* codeRegular = loadCode(0);
    ImFont* codeBold = loadCode(1);
    ImFont* codeItalic = loadCode(2);
    appSetCodeFonts(codeRegular ? codeRegular : uiFont, codeBold, codeItalic);
    ImFont* markUi = !uiPath.empty() ? io.Fonts->AddFontFromFileTTF(uiPath.c_str(), 76.0f * dpi * fbScale) : nullptr;
    const std::string codePath = plat::codeFontPath(0);
    ImFont* markCode = !codePath.empty() ? io.Fonts->AddFontFromFileTTF(codePath.c_str(), 76.0f * dpi * fbScale) : nullptr;
    appSetMarkFonts(markUi ? markUi : uiFont, markCode ? markCode : codeRegular);
    appPrepareLogo();
    io.FontGlobalScale = 1.0f / fbScale;

    ImGui_ImplGlfw_InitForOpenGL(g_window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    appDrawStartupSplash();
    ImGui::Render();
    int splashW = 0, splashH = 0;
    glfwGetFramebufferSize(g_window, &splashW, &splashH);
    glViewport(0, 0, splashW, splashH);
    glClearColor(.06f, .06f, .07f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(g_window);
    glfwPollEvents();
    appInit();
    if (argc > 1 && argv[1] && *argv[1]) appOpenProjectPath(argv[1]);

    auto nextFrame = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(g_window)) {
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        if (now < nextFrame) {
            glfwWaitEventsTimeout(std::chrono::duration<double>(nextFrame - now).count());
            continue;
        }
        nextFrame = now + std::chrono::milliseconds(appNeedsResponsiveFrames() ? 16 : 50);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const bool visible = !glfwGetWindowAttrib(g_window, GLFW_ICONIFIED);
        appFrame(io.DeltaTime, visible);
        if (appQuitRequested()) { ImGui::EndFrame(); break; }
        if (!visible) { ImGui::EndFrame(); continue; }
        ImGui::Render();

        int fw = 0, fh = 0;
        glfwGetFramebufferSize(g_window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.06f, 0.06f, 0.07f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(g_window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
