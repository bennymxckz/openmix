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
#include <algorithm>
#include <cctype>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "engine.h"
#include "config.h"

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
Config g_config;
std::vector<std::string> g_channels{"Game", "Chat", "Media"};
char g_newChannel[32] = {};

std::string joinChannels(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ",";
        out += v[i];
    }
    return out;
}

std::vector<std::string> splitChannels(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const std::string part =
            s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) out.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

bool g_autostart = false;

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
    g_tray.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!g_tray.hIcon) g_tray.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
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

// Held peak and clip state per channel. A meter without hold shows a
// transient for one frame, which is the same as not showing it.
struct MeterState {
    float level = 0.0f;      // smoothed bar, falls back at a readable rate
    float hold = 0.0f;       // peak marker
    float holdAge = 0.0f;    // seconds since the marker was set
    float clipAge = 1e9f;    // seconds since the last full-scale sample
};
std::vector<MeterState> g_meters;

// Amplitude is linear but hearing is not: on a linear meter everything
// interesting is crushed into the top fifth of the bar. Map to dB instead.
float meterPosition(float amplitude) {
    if (amplitude <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(amplitude);
    constexpr float floorDb = -54.0f;
    if (db <= floorDb) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return 1.0f - (db / floorDb);
}

// A level meter that reads like hardware: green through most of the range,
// amber approaching full scale, red at the top, with a peak marker that hangs
// so you can see what you just missed.
void drawMeter(MeterState& m, float peak, float dt, float width) {
    const ImVec2 size(width, 11.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    if (peak >= 0.999f) m.clipAge = 0.0f; else m.clipAge += dt;

    const float pos = meterPosition(peak);
    // Rise instantly, fall at about 40 dB per second: fast enough to follow
    // speech, slow enough to read.
    m.level = pos > m.level ? pos : (std::max)(pos, m.level - dt * 0.75f);

    if (pos >= m.hold) {
        m.hold = pos;
        m.holdAge = 0.0f;
    } else {
        m.holdAge += dt;
        if (m.holdAge > 1.2f) m.hold = (std::max)(pos, m.hold - dt * 0.5f);
    }

    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(26, 28, 32, 255), 2.0f);

    if (m.level > 0.0f) {
        const float w = size.x * m.level;
        ImU32 col = IM_COL32(78, 196, 118, 255);
        if (m.level > 0.94f)      col = IM_COL32(220, 80, 70, 255);
        else if (m.level > 0.82f) col = IM_COL32(225, 170, 60, 255);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + size.y), col, 2.0f);
    }

    if (m.hold > 0.01f) {
        const float x = p.x + size.x * m.hold;
        dl->AddRectFilled(ImVec2(x - 1.0f, p.y), ImVec2(x + 1.0f, p.y + size.y),
                          IM_COL32(235, 240, 245, 200));
    }

    // The clip marker latches for a couple of seconds; a one-frame flash of
    // red is exactly the thing you look away and miss.
    if (m.clipAge < 2.0f) {
        dl->AddRectFilled(ImVec2(p.x + size.x - 3.0f, p.y),
                          ImVec2(p.x + size.x, p.y + size.y),
                          IM_COL32(255, 70, 60, 255));
    }

    ImGui::Dummy(size);
}

// Settings are written on change rather than only at exit, so a crash or a
// forced quit does not lose them.
void saveSettings() {
    g_config.set("output", g_engine.monitorDevice());
    g_config.set("mic", g_engine.micDevice());
    for (const auto& b : g_engine.buses()) {
        g_config.setFloat("bus." + b.name + ".gain", b.gain);
        g_config.setBool("bus." + b.name + ".mute", b.muted);
        g_config.setFloat("bus." + b.name + ".streamGain", b.streamGain);
        g_config.setBool("bus." + b.name + ".streamMute", b.streamMuted);

        const std::string e = "bus." + b.name + ".eq.";
        g_config.setBool(e + "on", b.eq.enabled);
        const dsp::Band* bands[4] = { &b.eq.hp, &b.eq.low, &b.eq.mid, &b.eq.high };
        const char* names[4] = { "hp", "low", "mid", "high" };
        if (b.isCapture) {
            const std::string m = "bus." + b.name + ".";
            g_config.setBool(m + "gate.on", b.mic.gate.enabled);
            g_config.setFloat(m + "gate.thresh", b.mic.gate.thresholdDb);
            g_config.setFloat(m + "gate.hold", b.mic.gate.holdMs);
            g_config.setFloat(m + "gate.release", b.mic.gate.releaseMs);
            g_config.setBool(m + "comp.on", b.mic.comp.enabled);
            g_config.setFloat(m + "comp.thresh", b.mic.comp.thresholdDb);
            g_config.setFloat(m + "comp.ratio", b.mic.comp.ratio);
            g_config.setFloat(m + "comp.attack", b.mic.comp.attackMs);
            g_config.setFloat(m + "comp.release", b.mic.comp.releaseMs);
            g_config.setFloat(m + "comp.makeup", b.mic.comp.makeupDb);
        }
        for (int i = 0; i < 4; ++i) {
            g_config.setBool(e + names[i] + ".on", bands[i]->on);
            g_config.setFloat(e + names[i] + ".f", bands[i]->freq);
            g_config.setFloat(e + names[i] + ".g", bands[i]->gainDb);
            g_config.setFloat(e + names[i] + ".q", bands[i]->q);
        }
    }
    g_config.save();
}

void applySettings() {
    for (auto& b : g_engine.buses()) {
        b.gain        = g_config.getFloat("bus." + b.name + ".gain", 1.0f);
        b.muted       = g_config.getBool("bus." + b.name + ".mute", false);
        b.streamGain  = g_config.getFloat("bus." + b.name + ".streamGain", 1.0f);
        b.streamMuted = g_config.getBool("bus." + b.name + ".streamMute", false);

        const std::string e = "bus." + b.name + ".eq.";
        b.eq.enabled = g_config.getBool(e + "on", false);
        dsp::Band* bands[4] = { &b.eq.hp, &b.eq.low, &b.eq.mid, &b.eq.high };
        const char* names[4] = { "hp", "low", "mid", "high" };
        if (b.isCapture) {
            const std::string m = "bus." + b.name + ".";
            b.mic.gate.enabled     = g_config.getBool(m + "gate.on", false);
            b.mic.gate.thresholdDb = g_config.getFloat(m + "gate.thresh", b.mic.gate.thresholdDb);
            b.mic.gate.holdMs      = g_config.getFloat(m + "gate.hold", b.mic.gate.holdMs);
            b.mic.gate.releaseMs   = g_config.getFloat(m + "gate.release", b.mic.gate.releaseMs);
            b.mic.comp.enabled     = g_config.getBool(m + "comp.on", false);
            b.mic.comp.thresholdDb = g_config.getFloat(m + "comp.thresh", b.mic.comp.thresholdDb);
            b.mic.comp.ratio       = g_config.getFloat(m + "comp.ratio", b.mic.comp.ratio);
            b.mic.comp.attackMs    = g_config.getFloat(m + "comp.attack", b.mic.comp.attackMs);
            b.mic.comp.releaseMs   = g_config.getFloat(m + "comp.release", b.mic.comp.releaseMs);
            b.mic.comp.makeupDb    = g_config.getFloat(m + "comp.makeup", b.mic.comp.makeupDb);
        }
        for (int i = 0; i < 4; ++i) {
            bands[i]->on     = g_config.getBool(e + names[i] + ".on", bands[i]->on);
            bands[i]->freq   = g_config.getFloat(e + names[i] + ".f", bands[i]->freq);
            bands[i]->gainDb = g_config.getFloat(e + names[i] + ".g", bands[i]->gainDb);
            bands[i]->q      = g_config.getFloat(e + names[i] + ".q", bands[i]->q);
        }
    }
}

// One EQ band: enable, frequency, gain and Q. Frequency is logarithmic
// because hearing is, so a linear slider would waste most of its travel above
// 10 kHz.
bool drawBand(const char* label, dsp::Band& b, bool hasGain) {
    bool changed = false;
    ImGui::PushID(label);
    changed |= ImGui::Checkbox("##on", &b.on);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);

    ImGui::BeginDisabled(!b.on);
    ImGui::SetNextItemWidth(150.0f);
    changed |= ImGui::SliderFloat("##f", &b.freq, 20.0f, 20000.0f, "%.0f Hz",
                                  ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    if (hasGain) {
        ImGui::SetNextItemWidth(130.0f);
        changed |= ImGui::SliderFloat("##g", &b.gainDb, -18.0f, 18.0f, "%+.1f dB");
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(110.0f);
    changed |= ImGui::SliderFloat("##q", &b.q, 0.2f, 8.0f, "Q %.2f");
    ImGui::EndDisabled();

    ImGui::PopID();
    return changed;
}

void drawEqPopup(Bus& b) {
    if (!ImGui::BeginPopup("eq")) return;

    ImGui::Text("%s equaliser", b.name.c_str());
    ImGui::Separator();

    bool changed = ImGui::Checkbox("Enabled", &b.eq.enabled);
    ImGui::SameLine();
    if (ImGui::SmallButton("Flat")) {
        const bool wasOn = b.eq.enabled;
        b.eq = dsp::EqParams{};
        b.eq.enabled = wasOn;
        changed = true;
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(!b.eq.enabled);
    changed |= drawBand("High-pass", b.eq.hp, false);
    changed |= drawBand("Low", b.eq.low, true);
    changed |= drawBand("Mid", b.eq.mid, true);
    changed |= drawBand("High", b.eq.high, true);
    ImGui::EndDisabled();

    if (b.isCapture) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Microphone dynamics");
        ImGui::Spacing();

        changed |= ImGui::Checkbox("Noise gate", &b.mic.gate.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f dB", b.micChain.gateReductionDb());
        ImGui::BeginDisabled(!b.mic.gate.enabled);
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Threshold##g", &b.mic.gate.thresholdDb,
                                      -80.0f, 0.0f, "%.0f dB");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Hold##g", &b.mic.gate.holdMs, 0.0f, 500.0f, "%.0f ms");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Release##g", &b.mic.gate.releaseMs,
                                      10.0f, 1000.0f, "%.0f ms");
        ImGui::EndDisabled();

        ImGui::Spacing();
        changed |= ImGui::Checkbox("Compressor", &b.mic.comp.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f dB", b.micChain.compReductionDb());
        ImGui::BeginDisabled(!b.mic.comp.enabled);
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Threshold##c", &b.mic.comp.thresholdDb,
                                      -48.0f, 0.0f, "%.0f dB");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Ratio##c", &b.mic.comp.ratio, 1.0f, 12.0f, "%.1f : 1");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Attack##c", &b.mic.comp.attackMs, 0.5f, 50.0f, "%.1f ms");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Release##c", &b.mic.comp.releaseMs,
                                      20.0f, 800.0f, "%.0f ms");
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("Makeup##c", &b.mic.comp.makeupDb, 0.0f, 24.0f, "%+.1f dB");
        ImGui::EndDisabled();
    }

    if (changed) saveSettings();
    ImGui::EndPopup();
}

// Channels are USB devices, so adding or removing one means republishing the
// device set. Restarting the engine is the honest way to do that; it takes
// about a second and applications reconnect on their own.
void restartEngine() {
    saveSettings();
    g_engine.stop();

    EngineConfig cfg;
    cfg.playbackBuses = g_channels;
    cfg.outMatch = g_config.get("output");
    cfg.micMatch = g_config.get("mic");
    g_startError.clear();
    if (g_engine.start(cfg, g_startError)) applySettings();
}

void drawChannelEditor() {
    if (!ImGui::CollapsingHeader("Channels")) return;

    ImGui::TextDisabled("Each channel is a device applications can select.");
    ImGui::Spacing();

    int removeAt = -1;
    for (size_t i = 0; i < g_channels.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Openmix - %s", g_channels[i].c_str());
        ImGui::SameLine(220.0f);
        // One playback channel has to remain, or there is nothing to mix.
        ImGui::BeginDisabled(g_channels.size() <= 1);
        if (ImGui::SmallButton("Remove")) removeAt = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(200.0f);
    const bool entered = ImGui::InputTextWithHint("##newch", "New channel name",
                                                  g_newChannel, sizeof(g_newChannel),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool clicked = ImGui::Button("Add");

    if ((entered || clicked) && g_newChannel[0] != 0) {
        std::string name = g_newChannel;
        // The name becomes a USB serial and a Windows device name, so keep it
        // to something both will accept.
        name.erase(std::remove_if(name.begin(), name.end(),
                                  [](unsigned char c) { return !std::isalnum(c) && c != ' '; }),
                   name.end());
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();

        const bool dup = std::find(g_channels.begin(), g_channels.end(), name) != g_channels.end();
        if (!name.empty() && !dup && g_channels.size() < 8) {
            g_channels.push_back(name);
            g_newChannel[0] = 0;
            g_config.set("channels", joinChannels(g_channels));
            g_config.save();
            restartEngine();
        }
    }

    if (removeAt >= 0) {
        g_channels.erase(g_channels.begin() + removeAt);
        g_config.set("channels", joinChannels(g_channels));
        g_config.save();
        restartEngine();
    }
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
            restartEngine();
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
                else { g_deviceError.clear(); saveSettings(); }
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
                else { g_deviceError.clear(); saveSettings(); }
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

    if (g_engine.usbipMissing()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.42f, 1.0f));
        ImGui::TextWrapped(
            "usbip-win2 is not installed, so no devices can be created. "
            "openmix publishes its channels as virtual USB audio devices and "
            "needs that driver to attach them.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Install release v.0.9.7.7 from "
                            "github.com/vadimgrn/usbip-win2, then restart openmix.");
        ImGui::Spacing();
    } else if (g_engine.devicesMissing()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f),
                           "Some channels did not attach. Check that usbip-win2 is working, "
                           "or use 'usbip port' to see what is connected.");
        ImGui::Spacing();
    }

    if (!g_engine.namesApplied()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f),
                           "Channels are showing as \"Speakers (Openmix - ...)\".");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fix names")) {
            // Renaming an endpoint is an administrator write, so this has to
            // elevate. It is needed once: the name persists.
            wchar_t exe[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cli(exe);
            const size_t slash = cli.find_last_of(L'\\');
            if (slash != std::wstring::npos) cli = cli.substr(0, slash + 1);
            cli += L"openmix-cli.exe";
            ::ShellExecuteW(nullptr, L"runas", cli.c_str(), L"--fix-names",
                            nullptr, SW_HIDE);
        }
    }

    ImGui::Separator();
    ImGui::Spacing();

    auto& buses = g_engine.buses();
    const auto& eps = g_engine.endpoints();

    if (ImGui::BeginTable("buses", 6,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Headphones", ImGuiTableColumnFlags_WidthFixed, 175.0f);
        ImGui::TableSetupColumn("Stream / to apps", ImGuiTableColumnFlags_WidthFixed, 175.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 76.0f);
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
            if (g_meters.size() < buses.size()) g_meters.resize(buses.size());
            drawMeter(g_meters[i], b.ring.takePeak(), ImGui::GetIO().DeltaTime, 110.0f);

            // Headphone level: what you hear.
            ImGui::TableSetColumnIndex(2);
            float db = dbFromGain(b.gain);
            ImGui::SetNextItemWidth(165.0f);
            if (ImGui::SliderFloat("##vol", &db, -60.0f, 12.0f, "%.1f dB")) {
                b.gain = gainFromDb(db);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();

            // Stream level: what OBS records, independently.
            // For a playback channel this is what the stream hears; for the
            // microphone it is what applications hear. Same control, and in
            // both cases it is independent of the headphone fader.
            ImGui::TableSetColumnIndex(3);
            {
                float sdb = dbFromGain(b.streamGain);
                ImGui::SetNextItemWidth(165.0f);
                if (ImGui::SliderFloat("##stream", &sdb, -60.0f, 12.0f, "%.1f dB")) {
                    b.streamGain = gainFromDb(sdb);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
                if (ImGui::BeginPopupContextItem("##streamctx")) {
                    if (ImGui::MenuItem(b.streamMuted ? "Unmute" : "Mute")) {
                        b.streamMuted = !b.streamMuted;
                        saveSettings();
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::TableSetColumnIndex(4);
            bool muted = b.muted;
            if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.25f, 0.22f, 1.0f));
            if (ImGui::Button(muted ? "M" : "m", ImVec2(36, 0))) {
                b.muted = !muted;
                saveSettings();
            }
            ImGui::SameLine(0.0f, 4.0f);
            const bool dspOn = b.eq.enabled || b.mic.gate.enabled || b.mic.comp.enabled;
            if (dspOn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.50f, 1.0f));
            const bool anyDsp = b.eq.enabled || b.mic.gate.enabled || b.mic.comp.enabled;
            if (ImGui::Button(b.isCapture ? "FX" : "EQ", ImVec2(30, 0))) ImGui::OpenPopup("eq");
            (void)anyDsp;
            if (dspOn) ImGui::PopStyleColor();
            drawEqPopup(b);
            if (muted) ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(5);
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

    drawChannelEditor();
    ImGui::Spacing();

    if (ImGui::Checkbox("Start with Windows", &g_autostart)) {
        if (!setAutostart(g_autostart)) {
            g_autostart = autostartEnabled();   // registry refused; show the truth
            g_deviceError = "Could not update the startup entry.";
        }
    }

    ImGui::TextDisabled("Point apps at the Openmix devices in Windows sound settings.");
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
    // A second instance would publish its own full set of devices and fight
    // the first for them. Show the window that is already running instead.
    HANDLE only = ::CreateMutexW(nullptr, TRUE, L"openmix.single-instance");
    if (only && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(L"openmixWindow", nullptr)) {
            ::ShowWindow(existing, SW_SHOW);
            ::SetForegroundWindow(existing);
        }
        ::CloseHandle(only);
        return 0;
    }

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::timeBeginPeriod(1);   // the USB pacer sleeps in ~1 ms steps

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = ::LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"openmixWindow";
    ::RegisterClassExW(&wc);

    g_hwnd = ::CreateWindowW(wc.lpszClassName, L"openmix", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 440,
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

    // Launched by the autostart entry: go straight to the tray rather than
    // throwing a window in the user's face at every sign-in.
    const bool startHidden = ::wcsstr(::GetCommandLineW(), L"--tray") != nullptr;
    if (startHidden) {
        g_inTray = true;
    } else {
        ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        ::UpdateWindow(g_hwnd);
    }
    addTrayIcon(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // no imgui.ini beside the exe
    applyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_outDevices = listRenderDevices();
    g_micDevices = listCaptureDevices();

    g_config.load();
    g_autostart = autostartEnabled();
    {
        const auto saved = splitChannels(g_config.get("channels"));
        if (!saved.empty()) g_channels = saved;
    }

    EngineConfig cfg;
    cfg.playbackBuses = g_channels;
    cfg.outMatch = g_config.get("output");
    cfg.micMatch = g_config.get("mic");
    if (g_engine.start(cfg, g_startError)) {
        applySettings();
    }
    // Otherwise the window stays up so the user can read why and retry.

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

    saveSettings();
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
    if (only) ::CloseHandle(only);
    return 0;
}
