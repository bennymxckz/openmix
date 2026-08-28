// openmix GUI: a mixer window that lives in the tray.
//
// Dear ImGui over Win32 + D3D11. Chosen because it is MIT (so the project
// stays MIT), needs no installed runtime, and redraws meters cheaply.

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <timeapi.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "engine.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr UINT WM_OPENMIX_TRAY = WM_APP + 1;
constexpr UINT ID_TRAY_SHOW = 1001;
constexpr UINT ID_TRAY_QUIT = 1002;

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool                    g_occluded = false;
bool                    g_inTray = false;
bool                    g_quitting = false;
HWND                    g_hwnd = nullptr;
NOTIFYICONDATAW         g_tray{};

Engine g_engine;
std::string g_startError;
std::vector<RenderDevice> g_outDevices;
std::vector<RenderDevice> g_micDevices;
std::string g_deviceError;

void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void cleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = {};
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}

void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

void addTrayIcon(HWND hwnd) {
    g_tray = {};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_OPENMIX_TRAY;
    g_tray.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::wcscpy_s(g_tray.szTip, L"openmix");
    ::Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void removeTrayIcon() {
    ::Shell_NotifyIconW(NIM_DELETE, &g_tray);
}

void hideToTray(HWND hwnd) {
    ::ShowWindow(hwnd, SW_HIDE);
    g_inTray = true;
}

void restoreFromTray(HWND hwnd) {
    ::ShowWindow(hwnd, SW_SHOW);
    ::SetForegroundWindow(hwnd);
    g_inTray = false;
}

void showTrayMenu(HWND hwnd) {
    POINT pt{};
    ::GetCursorPos(&pt);
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, g_inTray ? L"Show mixer" : L"Hide mixer");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit openmix");
    // Required so the menu dismisses when the user clicks elsewhere.
    ::SetForegroundWindow(hwnd);
    ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                // Minimising parks it in the tray rather than the taskbar.
                hideToTray(hwnd);
                return 0;
            }
            if (g_device) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp),
                                           DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
            }
            return 0;

        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_CLOSE:
            // Closing hides; quitting is explicit, from the tray menu.
            hideToTray(hwnd);
            return 0;

        case WM_OPENMIX_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                if (g_inTray) restoreFromTray(hwnd); else hideToTray(hwnd);
            } else if (LOWORD(lp) == WM_RBUTTONUP) {
                showTrayMenu(hwnd);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == ID_TRAY_SHOW) {
                if (g_inTray) restoreFromTray(hwnd); else hideToTray(hwnd);
                return 0;
            }
            if (LOWORD(wp) == ID_TRAY_QUIT) {
                g_quitting = true;
                ::PostQuitMessage(0);
                return 0;
            }
            break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

float dbFromGain(float g) {
    return 20.0f * std::log10(g > 0.0001f ? g : 0.0001f);
}

float gainFromDb(float db) {
    return db <= -60.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

// A level meter that reads like hardware: green up to -6 dB, amber to -1,
// red at the top, so clipping is visible without reading numbers.
void drawMeter(float peak, float width) {
    const ImVec2 size(width, 10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(28, 30, 34, 255), 2.0f);

    const float level = peak > 1.0f ? 1.0f : peak;
    if (level > 0.0f) {
        const float w = size.x * level;
        ImU32 col = IM_COL32(80, 200, 120, 255);
        if (level > 0.89f)      col = IM_COL32(220, 80, 70, 255);
        else if (level > 0.5f)  col = IM_COL32(225, 170, 60, 255);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + size.y), col, 2.0f);
    }
    ImGui::Dummy(size);
}

void drawUi() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("openmix", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (!g_engine.running()) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "Engine stopped");
        if (!g_startError.empty()) ImGui::TextWrapped("%s", g_startError.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Retry")) {
            EngineConfig cfg;
            g_startError.clear();
            g_engine.start(cfg, g_startError);
        }
        ImGui::End();
        return;
    }

    // Device pickers. openmix's own endpoints are filtered out of both lists:
    // monitoring into our own output would feed the engine back into itself,
    // and capturing our own microphone would loop it.
    ImGui::PushItemWidth(340.0f);

    ImGui::TextDisabled("Headphones");
    ImGui::SameLine(110.0f);
    if (ImGui::BeginCombo("##out", g_engine.monitorDevice().c_str())) {
        for (const auto& d : g_outDevices) {
            if (d.isOpenmix) continue;
            const bool sel = (d.name == g_engine.monitorDevice());
            if (ImGui::Selectable(d.name.c_str(), sel)) {
                std::string err;
                if (!g_engine.setOutputDevice(d.name, err)) g_deviceError = err;
                else g_deviceError.clear();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f ms", g_engine.monitorBufferMs());

    ImGui::TextDisabled("Microphone");
    ImGui::SameLine(110.0f);
    const std::string micLabel =
        g_engine.micDevice().empty() ? std::string("(none)") : g_engine.micDevice();
    if (ImGui::BeginCombo("##mic", micLabel.c_str())) {
        for (const auto& d : g_micDevices) {
            if (d.isOpenmix) continue;
            const bool sel = (d.name == g_engine.micDevice());
            if (ImGui::Selectable(d.name.c_str(), sel)) {
                std::string err;
                if (!g_engine.setMicDevice(d.name, err)) g_deviceError = err;
                else g_deviceError.clear();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) {
        g_outDevices = listRenderDevices();
        g_micDevices = listCaptureDevices();
    }

    ImGui::PopItemWidth();

    if (!g_deviceError.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.4f, 1.0f), "%s", g_deviceError.c_str());
    } else if (g_engine.micDevice().empty() && !g_engine.micError().empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "%s", g_engine.micError().c_str());
    }

    if (!g_engine.namesApplied()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f),
                           "Windows refused the device rename - run openmix as "
                           "administrator once to apply the names.");
    }

    ImGui::Separator();
    ImGui::Spacing();

    auto& buses = g_engine.buses();
    const auto& eps = g_engine.endpoints();

    if (ImGui::BeginTable("buses", 5,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < buses.size(); ++i) {
            Bus& b = buses[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", b.name.c_str());
            if (b.isCapture) {
                ImGui::SameLine();
                ImGui::TextDisabled("in");
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            drawMeter(b.ring.takePeak(), 140.0f);

            ImGui::TableSetColumnIndex(2);
            float db = dbFromGain(b.gain);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("##vol", &db, -60.0f, 12.0f, "%.1f dB")) {
                b.gain = gainFromDb(db);
            }

            ImGui::TableSetColumnIndex(3);
            bool muted = b.muted;
            if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.25f, 0.22f, 1.0f));
            if (ImGui::Button(muted ? "Muted" : "Mute", ImVec2(50, 0))) b.muted = !muted;
            if (muted) ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(4);
            ImGui::AlignTextToFramePadding();
            const bool attached = i < eps.size() && eps[i]->attached.load();
            const double r = g_engine.rate(i);
            const double backlogMs = 1000.0 * static_cast<double>(b.ring.readable()) /
                                     static_cast<double>(kChannels * kSampleRate);
            if (!attached) {
                ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.4f, 1.0f), "not connected");
            } else if (r > 1000.0) {
                // Backlog is latency. Flag it when it climbs past a sane budget.
                if (backlogMs > 60.0) {
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f),
                                       "%.1f kHz  %.0f ms", r / 1000.0, backlogMs);
                } else {
                    ImGui::TextDisabled("%.1f kHz  %.0f ms", r / 1000.0, backlogMs);
                }
            } else {
                ImGui::TextDisabled("idle");
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Point apps at \"openmix ...\" in Windows sound settings.");
    ImGui::TextDisabled("Closing this window keeps openmix running in the tray.");

    ImGui::End();
}

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.CellPadding = ImVec2(6, 5);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(8, 7);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.16f, 0.17f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]= ImVec4(0.22f, 0.23f, 0.26f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.35f, 0.65f, 0.72f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.78f, 0.85f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.20f, 0.21f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.29f, 0.32f, 1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.11f, 0.12f, 0.13f, 1.00f);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::timeBeginPeriod(1);   // the USB pacer sleeps in ~1 ms steps

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"openmixWindow";
    ::RegisterClassExW(&wc);

    g_hwnd = ::CreateWindowW(wc.lpszClassName, L"openmix", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 720, 420,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    if (!createDevice(g_hwnd)) {
        cleanupDevice();
        ::UnregisterClassW(wc.lpszClassName, hInst);
        ::MessageBoxW(nullptr, L"Could not create a Direct3D device.", L"openmix", MB_ICONERROR);
        return 1;
    }

    // Dark title bar to match the window contents.
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(g_hwnd);
    addTrayIcon(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // no imgui.ini beside the exe
    applyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_outDevices = listRenderDevices();
    g_micDevices = listCaptureDevices();

    EngineConfig cfg;
    if (!g_engine.start(cfg, g_startError)) {
        // Keep the window up so the user can read why and retry.
    }

    DWORD lastRateSample = ::GetTickCount();
    bool done = false;

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Parked in the tray: stop rendering entirely, but keep audio running.
        if (g_inTray) {
            ::Sleep(120);
            const DWORD now = ::GetTickCount();
            if (now - lastRateSample >= 1000) {
                g_engine.sampleRates();
                lastRateSample = now;
            }
            continue;
        }

        if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(16);
            continue;
        }
        g_occluded = false;

        const DWORD now = ::GetTickCount();
        if (now - lastRateSample >= 1000) {
            g_engine.sampleRates();
            lastRateSample = now;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawUi();
        ImGui::Render();

        const float clear[4] = { 0.09f, 0.10f, 0.11f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_swapChain->Present(1, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    g_engine.stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    removeTrayIcon();
    cleanupDevice();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(wc.lpszClassName, hInst);

    ::timeEndPeriod(1);
    ::CoUninitialize();
    return 0;
}
