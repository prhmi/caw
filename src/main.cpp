#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <shellapi.h>
#include <string>
#include <cstring>
#include <chrono>

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_sc = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void createRTV() {
    ID3D11Texture2D* back = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static void releaseRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU: software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    createRTV();
    return true;
}

static void destroyDevice() {
    releaseRTV();
    if (g_sc) { g_sc->Release(); g_sc = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_resizeW = (UINT)LOWORD(lp);
        g_resizeH = (UINT)HIWORD(lp);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // disable ALT application menu
        break;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        char path[MAX_PATH] = {};
        if (DragQueryFileA(drop, 0, path, MAX_PATH) > 0) {
            const size_t len = std::strlen(path);
            const bool project = len > 4 && (path[len - 4] == '.') &&
                (path[len - 3] == 'c' || path[len - 3] == 'C') &&
                (path[len - 2] == 'a' || path[len - 2] == 'A') &&
                (path[len - 1] == 'w' || path[len - 1] == 'W');
            if (project) appOpenProjectPath(path);
            else {
                POINT pt;
                DragQueryPoint(drop, &pt);
                appQueueFileDrop(path, pt.x, pt.y);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        appRequestClose();   // asks "save changes?" in the program when needed, then the main loop ends
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR commandLine, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    wc.hIconSm = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "CawWindow";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "CAW - Code Audio Workstation (build 19)", WS_OVERLAPPEDWINDOW, 100, 100,
                              (int)(1400 * dpi), (int)(800 * dpi), nullptr, nullptr, hInst, nullptr);

    if (!createDevice(hwnd)) {
        destroyDevice();
        UnregisterClassA(wc.lpszClassName, hInst);
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    appSetupStyle(dpi);
    ImFont* uiFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpi);
    if (uiFont) io.FontDefault = uiFont;
    else { uiFont = io.Fonts->AddFontDefault(); io.FontDefault = uiFont; }
    ImFont* codeRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 17.0f * dpi);
    ImFont* codeBold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 17.0f * dpi);
    ImFont* codeItalic = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolai.ttf", 17.0f * dpi);
    appSetCodeFonts(codeRegular ? codeRegular : uiFont, codeBold, codeItalic);
    ImFont* markUi = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 76.0f * dpi);
    ImFont* markCode = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 76.0f * dpi);
    appSetMarkFonts(markUi ? markUi : uiFont, markCode ? markCode : codeRegular);
    appPrepareLogo();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    appDrawStartupSplash();
    ImGui::Render();
    const float splashClear[4] = {0.06f, 0.06f, 0.07f, 1.f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, splashClear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_sc->Present(1, 0);
    appInit();
    if (commandLine && *commandLine) {
        std::string initialPath = commandLine;
        if (initialPath.size() >= 2 && initialPath.front() == '"' && initialPath.back() == '"')
            initialPath = initialPath.substr(1, initialPath.size() - 2);
        appOpenProjectPath(initialPath.c_str());
    }

    bool done = false;
    auto nextFrame = std::chrono::steady_clock::now();
    auto lastFrame = nextFrame - std::chrono::milliseconds(16);
    auto activeUntil = nextFrame + std::chrono::seconds(2);
    while (!done) {
        MSG msg;
        bool inputWake = false;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if ((msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
                (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
                msg.message == WM_SIZE || msg.message == WM_SETFOCUS || msg.message == WM_PAINT)
                inputWake = true;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        const auto now = std::chrono::steady_clock::now();
        if (inputWake) {
            activeUntil = now + std::chrono::seconds(2);
            // Wake promptly for input, but never exceed the 60 fps cap.
            const auto earliest = lastFrame + std::chrono::milliseconds(16);
            const auto requested = now > earliest ? now : earliest;
            if (requested < nextFrame) nextFrame = requested;
        }
        if (now < nextFrame) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrame - now).count();
            MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(remaining + 1), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        // A still, engine-off window idles at 5 fps. Input wakes it promptly
        // and keeps it responsive; playback/timeline/export stay at 60 fps.
        lastFrame = now;
        // still + no engine: 5 fps; engine started but idle: 30 fps; playing / input in the last 2 s: 60 fps
        nextFrame = now + std::chrono::milliseconds((appNeedsResponsiveFrames() || now < activeUntil) ? 16 : (appEnginesIdleRunning() ? 33 : 200));

        if (g_resizeW != 0 && g_resizeH != 0) {
            releaseRTV();
            g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            createRTV();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const bool visible = !IsIconic(hwnd);
        appFrame(io.DeltaTime, visible);
        if (appQuitRequested()) { ImGui::EndFrame(); break; }
        if (!visible) { ImGui::EndFrame(); continue; }  // service engine/timeline, skip hidden UI and GPU work
        ImGui::Render();

        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInst);
    return 0;
}
