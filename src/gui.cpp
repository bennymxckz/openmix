// openmix: a mixer window that lives in the tray.
//
// Dear ImGui over Win32 + D3D11. Chosen because it is MIT (so the project
// stays MIT), needs no installed runtime, and redraws meters cheaply.

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "engine.h"
#include "config.h"
#include "theme.h"
#include "widgets.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr UINT WM_OPENMIX_TRAY = WM_APP + 1;
constexpr UINT ID_TRAY_SHOW = 1001;
constexpr UINT ID_TRAY_QUIT = 1002;
constexpr UINT ID_TRAY_MICMUTE = 1003;
// Muting the microphone is the one thing worth reaching for without
// finding the window first.
constexpr int ID_HOTKEY_MIC = 1;

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool                    g_occluded = false;
bool                    g_inTray = false;
HWND                    g_hwnd = nullptr;
NOTIFYICONDATAW         g_tray{};

ImFont* g_fontBody = nullptr;
ImFont* g_fontHead = nullptr;
ImFont* g_fontSmall = nullptr;
float   g_scale = 1.0f;

Engine g_engine;
Config g_config;
std::string g_startError;
std::string g_notice;
std::vector<RenderDevice> g_outDevices;
std::vector<RenderDevice> g_micDevices;
std::vector<mix::MeterState> g_meters;
// What is playing on each channel, refreshed on a timer rather than every
// frame: enumerating sessions is a COM round trip per device.
std::vector<std::vector<std::string>> g_channelApps;
std::vector<std::string> g_channels{"Game", "Chat", "Media"};
char g_newChannel[32] = {};
bool g_autostart = false;
bool g_showSettings = false;
bool g_showWelcome = false;
bool g_micHotkey = false;
bool g_hotkeyFailed = false;

// ---- device plumbing ---------------------------------------------------

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

// ---- tray --------------------------------------------------------------

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

void removeTrayIcon() { ::Shell_NotifyIconW(NIM_DELETE, &g_tray); }

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

    for (const auto& b : g_engine.buses()) {
        if (!b.isCapture) continue;
        ::AppendMenuW(menu, MF_STRING | (b.streamMuted ? MF_CHECKED : 0),
                      ID_TRAY_MICMUTE, L"Mute microphone");
        break;
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit openmix");
    ::SetForegroundWindow(hwnd);   // so the menu dismisses on an outside click
    ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);
}

void saveWindowPlacement();

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) { hideToTray(hwnd); return 0; }
            if (g_device) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp),
                                           DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
            }
            return 0;

        case WM_GETMINMAXINFO: {
            // Below this the strips overlap and the window stops being useful.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = static_cast<LONG>(560 * g_scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(470 * g_scale);
            return 0;
        }

        case WM_EXITSIZEMOVE:
            // Saved when the move or resize finishes rather than only at exit,
            // so a crash or a force-quit does not lose it.
            saveWindowPlacement();
            return 0;

        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_CLOSE:
            hideToTray(hwnd);   // quitting is explicit, from the tray menu
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
            if (LOWORD(wp) == ID_TRAY_MICMUTE) {
                for (auto& b : g_engine.buses()) {
                    if (!b.isCapture) continue;
                    b.streamMuted = !b.streamMuted;
                    break;
                }
                return 0;
            }
            if (LOWORD(wp) == ID_TRAY_QUIT) { ::PostQuitMessage(0); return 0; }
            break;

        case WM_HOTKEY:
            if (wp == ID_HOTKEY_MIC) {
                // Mute the stream side: what applications hear. The headphone
                // side is your own monitoring and is a separate thing.
                for (auto& b : g_engine.buses()) {
                    if (!b.isCapture) continue;
                    b.streamMuted = !b.streamMuted;
                    break;
                }
                return 0;
            }
            break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- settings ----------------------------------------------------------

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

void saveSettings() {
    g_config.set("output", g_engine.monitorDevice());
    g_config.set("mic", g_engine.micDevice());
    for (const auto& b : g_engine.buses()) {
        const std::string k = "bus." + b.name + ".";
        g_config.setFloat(k + "gain", b.gain);
        g_config.setBool(k + "mute", b.muted);
        g_config.setFloat(k + "streamGain", b.streamGain);
        g_config.setBool(k + "streamMute", b.streamMuted);

        const std::string e = k + "eq.";
        g_config.setBool(e + "on", b.eq.enabled);
        const dsp::Band* bands[4] = { &b.eq.hp, &b.eq.low, &b.eq.mid, &b.eq.high };
        const char* names[4] = { "hp", "low", "mid", "high" };
        for (int i = 0; i < 4; ++i) {
            g_config.setBool(e + names[i] + ".on", bands[i]->on);
            g_config.setFloat(e + names[i] + ".f", bands[i]->freq);
            g_config.setFloat(e + names[i] + ".g", bands[i]->gainDb);
            g_config.setFloat(e + names[i] + ".q", bands[i]->q);
        }
        if (b.isCapture) {
            g_config.setBool(k + "gate.on", b.mic.gate.enabled);
            g_config.setFloat(k + "gate.thresh", b.mic.gate.thresholdDb);
            g_config.setFloat(k + "gate.hold", b.mic.gate.holdMs);
            g_config.setFloat(k + "gate.release", b.mic.gate.releaseMs);
            g_config.setBool(k + "comp.on", b.mic.comp.enabled);
            g_config.setFloat(k + "comp.thresh", b.mic.comp.thresholdDb);
            g_config.setFloat(k + "comp.ratio", b.mic.comp.ratio);
            g_config.setFloat(k + "comp.attack", b.mic.comp.attackMs);
            g_config.setFloat(k + "comp.release", b.mic.comp.releaseMs);
            g_config.setFloat(k + "comp.makeup", b.mic.comp.makeupDb);
        }
    }
    g_config.save();
}

void applySettings() {
    for (auto& b : g_engine.buses()) {
        const std::string k = "bus." + b.name + ".";
        // Microphone monitoring defaults to silent: hearing your own voice
        // unasked is startling, and on speakers it feeds back.
        b.gain        = g_config.getFloat(k + "gain", b.isCapture ? 0.0f : 1.0f);
        b.muted       = g_config.getBool(k + "mute", false);
        b.streamGain  = g_config.getFloat(k + "streamGain", 1.0f);
        b.streamMuted = g_config.getBool(k + "streamMute", false);

        const std::string e = k + "eq.";
        b.eq.enabled = g_config.getBool(e + "on", false);
        dsp::Band* bands[4] = { &b.eq.hp, &b.eq.low, &b.eq.mid, &b.eq.high };
        const char* names[4] = { "hp", "low", "mid", "high" };
        for (int i = 0; i < 4; ++i) {
            bands[i]->on     = g_config.getBool(e + names[i] + ".on", bands[i]->on);
            bands[i]->freq   = g_config.getFloat(e + names[i] + ".f", bands[i]->freq);
            bands[i]->gainDb = g_config.getFloat(e + names[i] + ".g", bands[i]->gainDb);
            bands[i]->q      = g_config.getFloat(e + names[i] + ".q", bands[i]->q);
        }
        if (b.isCapture) {
            b.mic.gate.enabled     = g_config.getBool(k + "gate.on", false);
            b.mic.gate.thresholdDb = g_config.getFloat(k + "gate.thresh", b.mic.gate.thresholdDb);
            b.mic.gate.holdMs      = g_config.getFloat(k + "gate.hold", b.mic.gate.holdMs);
            b.mic.gate.releaseMs   = g_config.getFloat(k + "gate.release", b.mic.gate.releaseMs);
            b.mic.comp.enabled     = g_config.getBool(k + "comp.on", false);
            b.mic.comp.thresholdDb = g_config.getFloat(k + "comp.thresh", b.mic.comp.thresholdDb);
            b.mic.comp.ratio       = g_config.getFloat(k + "comp.ratio", b.mic.comp.ratio);
            b.mic.comp.attackMs    = g_config.getFloat(k + "comp.attack", b.mic.comp.attackMs);
            b.mic.comp.releaseMs   = g_config.getFloat(k + "comp.release", b.mic.comp.releaseMs);
            b.mic.comp.makeupDb    = g_config.getFloat(k + "comp.makeup", b.mic.comp.makeupDb);
        }
    }
}

// Match each channel to its Windows endpoint and ask what is playing on it.
void refreshChannelApps() {
    const auto& buses = g_engine.buses();
    g_channelApps.assign(buses.size(), {});
    if (!g_engine.running()) return;

    const auto devices = listRenderDevices();
    for (size_t i = 0; i < buses.size(); ++i) {
        if (buses[i].isCapture) continue;   // a microphone has no players
        const std::string want = "Openmix - " + buses[i].name;
        for (const auto& d : devices) {
            if (d.name.find(want) == std::string::npos) continue;
            g_channelApps[i] = appsOnDevice(d.id);
            break;
        }
    }
}

// Where the user put the window is a setting like any other.
void saveWindowPlacement() {
    if (!g_hwnd) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!::GetWindowPlacement(g_hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    g_config.set("window", std::to_string(r.left) + "," + std::to_string(r.top) + "," +
                           std::to_string(r.right - r.left) + "," +
                           std::to_string(r.bottom - r.top));
    g_config.save();
}

void restoreWindowPlacement() {
    const std::string v = g_config.get("window");
    int x = 0, y = 0, w = 0, h = 0;
    if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return;
    if (w < 400 || h < 300) return;

    // A monitor that has since been unplugged would put the window somewhere
    // unreachable, so only restore a position that is still on a screen.
    const RECT want{ x, y, x + w, y + h };
    if (!::MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) return;
    ::SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// The tray icon is all you can see while hidden, so its tooltip carries the
// state that matters.
void updateTrayTip() {
    bool micMuted = false;
    bool haveMic = false;
    for (const auto& b : g_engine.buses()) {
        if (!b.isCapture) continue;
        haveMic = true;
        micMuted = b.streamMuted;
        break;
    }
    const wchar_t* text = !g_engine.running() ? L"openmix - not running"
                        : (haveMic && micMuted) ? L"openmix - microphone muted"
                                                : L"openmix";
    if (::wcscmp(g_tray.szTip, text) == 0) return;
    ::wcscpy_s(g_tray.szTip, text);
    g_tray.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void setMicHotkey(bool on) {
    ::UnregisterHotKey(g_hwnd, ID_HOTKEY_MIC);
    g_hotkeyFailed = false;
    if (on) {
        // Ctrl+Alt+M: unlikely to collide, and nothing else in openmix needs
        // a global key.
        if (!::RegisterHotKey(g_hwnd, ID_HOTKEY_MIC, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M')) {
            g_hotkeyFailed = true;   // something else already owns it
            on = false;
        }
    }
    g_micHotkey = on;
    g_config.setBool("micHotkey", on);
    g_config.save();
}

void restartEngine() {
    saveWindowPlacement();
    saveSettings();
    g_engine.stop();
    EngineConfig cfg;
    cfg.playbackBuses = g_channels;
    cfg.outMatch = g_config.get("output");
    cfg.micMatch = g_config.get("mic");
    g_startError.clear();
    if (g_engine.start(cfg, g_startError)) applySettings();
}

float dbFromGain(float g) { return 20.0f * std::log10(g > 0.0005f ? g : 0.0005f); }
float gainFromDb(float db) { return db <= -59.5f ? 0.0f : std::pow(10.0f, db / 20.0f); }

void openWindowsAppVolume();

void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::PushFont(g_fontSmall);
        ImGui::SetTooltip("%s", text);
        ImGui::PopFont();
    }
}

// ---- effects panel -----------------------------------------------------

bool drawBand(const char* label, dsp::Band& b, bool hasGain) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    changed |= ImGui::Checkbox("##on", &b.on);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);

    ImGui::BeginDisabled(!b.on);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderFloat("##f", &b.freq, 20.0f, 20000.0f, "%.0f Hz",
                                  ImGuiSliderFlags_Logarithmic);
    ImGui::TableSetColumnIndex(2);
    if (hasGain) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("##g", &b.gainDb, -18.0f, 18.0f, "%+.1f dB");
    } else {
        mix::textDim("--");
    }
    ImGui::TableSetColumnIndex(3);
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderFloat("##q", &b.q, 0.2f, 8.0f, "Q %.2f");
    ImGui::EndDisabled();

    ImGui::PopID();
    return changed;
}

// The chain's actual response, drawn from its coefficients. Log frequency,
// because that is how the bands are spaced and how hearing works.
void drawEqCurve(const dsp::EqParams& p, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), theme::kMeterBg, 5.0f);

    constexpr double kMinHz = 20.0, kMaxHz = 20000.0;
    constexpr float kRangeDb = 18.0f;
    auto yFor = [&](double db) {
        const float t = static_cast<float>(db) / kRangeDb;   // -1..1
        return o.y + size.y * 0.5f - t * size.y * 0.5f;
    };

    // Octave grid, labelled where there is room.
    const double marks[] = { 100.0, 1000.0, 10000.0 };
    for (double f : marks) {
        const float x = o.x + size.x * static_cast<float>(
            (std::log10(f) - std::log10(kMinHz)) / (std::log10(kMaxHz) - std::log10(kMinHz)));
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + size.y), theme::fade(theme::kLine, 0.8f));
    }
    for (double db : { -12.0, -6.0, 6.0, 12.0 }) {
        const float y = yFor(db);
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + size.x, y), theme::fade(theme::kLine, 0.6f));
    }
    dl->AddLine(ImVec2(o.x, yFor(0.0)), ImVec2(o.x + size.x, yFor(0.0)),
                theme::fade(theme::kTextFaint, 0.9f));

    // The curve, plus a translucent fill down to the 0 dB line so a boost and
    // a cut are distinguishable at a glance.
    constexpr int kPoints = 160;
    ImVec2 pts[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const double t = static_cast<double>(i) / (kPoints - 1);
        const double f = std::pow(10.0, std::log10(kMinHz) +
                                        t * (std::log10(kMaxHz) - std::log10(kMinHz)));
        const double db = dsp::ChannelStrip::responseDb(p, f, kSampleRate);
        pts[i] = ImVec2(o.x + size.x * static_cast<float>(t),
                        std::clamp(yFor(db), o.y + 1.0f, o.y + size.y - 1.0f));
    }
    const float zeroY = yFor(0.0);
    for (int i = 0; i + 1 < kPoints; ++i) {
        dl->AddQuadFilled(ImVec2(pts[i].x, zeroY), ImVec2(pts[i + 1].x, zeroY),
                          pts[i + 1], pts[i], theme::fade(theme::kAccent, 0.16f));
    }
    dl->AddPolyline(pts, kPoints, theme::kAccent, 0, 2.0f);

    ImGui::Dummy(size);
}

void drawEffects(Bus& b) {
    bool changed = false;

    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted("Equaliser");
    ImGui::PopFont();
    ImGui::SameLine();
    changed |= ImGui::Checkbox("##eqon", &b.eq.enabled);
    ImGui::SameLine();
    if (ImGui::SmallButton("Flat")) {
        const bool was = b.eq.enabled;
        b.eq = dsp::EqParams{};
        b.eq.enabled = was;
        changed = true;
    }
    tip("Reset every band to no change");

    ImGui::Spacing();
    drawEqCurve(b.eq, ImVec2(-FLT_MIN, 96.0f * g_scale));
    ImGui::Spacing();

    ImGui::BeginDisabled(!b.eq.enabled);
    if (ImGui::BeginTable("eq", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("band", ImGuiTableColumnFlags_WidthFixed, 110.0f * g_scale);
        ImGui::TableSetupColumn("freq");
        ImGui::TableSetupColumn("gain");
        ImGui::TableSetupColumn("q");
        changed |= drawBand("High-pass", b.eq.hp, false);
        changed |= drawBand("Low", b.eq.low, true);
        changed |= drawBand("Mid", b.eq.mid, true);
        changed |= drawBand("High", b.eq.high, true);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (b.isCapture) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(g_fontHead);
        ImGui::TextUnformatted("Noise gate");
        ImGui::PopFont();
        ImGui::SameLine();
        changed |= ImGui::Checkbox("##gateon", &b.mic.gate.enabled);
        ImGui::SameLine();
        mix::textDim("%+.0f dB", b.micChain.gateReductionDb());
        tip("How much the gate is turning the microphone down right now");

        ImGui::BeginDisabled(!b.mic.gate.enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Opens above", &b.mic.gate.thresholdDb,
                                      -80.0f, 0.0f, "%.0f dB");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Stays open for", &b.mic.gate.holdMs,
                                      0.0f, 500.0f, "%.0f ms");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Closes over", &b.mic.gate.releaseMs,
                                      10.0f, 1000.0f, "%.0f ms");
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::PushFont(g_fontHead);
        ImGui::TextUnformatted("Compressor");
        ImGui::PopFont();
        ImGui::SameLine();
        changed |= ImGui::Checkbox("##compon", &b.mic.comp.enabled);
        ImGui::SameLine();
        mix::textDim("%+.1f dB", b.micChain.compReductionDb());
        tip("How much the compressor is turning the microphone down right now");

        ImGui::BeginDisabled(!b.mic.comp.enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Squeezes above", &b.mic.comp.thresholdDb,
                                      -48.0f, 0.0f, "%.0f dB");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("By", &b.mic.comp.ratio, 1.0f, 12.0f, "%.1f : 1");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Reacts in", &b.mic.comp.attackMs,
                                      0.5f, 50.0f, "%.1f ms");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Recovers in", &b.mic.comp.releaseMs,
                                      20.0f, 800.0f, "%.0f ms");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("Then lift by", &b.mic.comp.makeupDb,
                                      0.0f, 24.0f, "%+.1f dB");
        ImGui::EndDisabled();
    }

    if (changed) saveSettings();
}

// ---- channel strip -----------------------------------------------------

void drawStrip(size_t index, Bus& b, bool attached, float rate, float height) {
    const ImU32 accent = theme::channelColor(index);
    ImGui::PushID(static_cast<int>(index));

    const float stripW = 138.0f * g_scale;
    ImGui::BeginChild("strip", ImVec2(stripW, height), false,
                      ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 avail = ImGui::GetWindowSize();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                      theme::kPanel, 8.0f);
    // A colour bar across the top is how you find a strip without reading it.
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + 3.0f),
                      attached ? accent : theme::kTextFaint, 8.0f);

    ImGui::Dummy(ImVec2(0, 8.0f * g_scale));
    ImGui::Indent(10.0f * g_scale);

    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted(b.name.c_str());
    ImGui::PopFont();

    ImGui::PushFont(g_fontSmall);
    if (!attached) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kMuted));
        ImGui::TextUnformatted("not connected");
        ImGui::PopStyleColor();
    } else if (b.isCapture) {
        mix::textDim("microphone");
    } else if (index < g_channelApps.size() && !g_channelApps[index].empty()) {
        // Naming what is actually playing beats "receiving": it is how you
        // confirm Discord really is on Chat.
        const auto& apps = g_channelApps[index];
        std::string line = apps[0];
        if (apps.size() > 1) line += " +" + std::to_string(apps.size() - 1);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextDim));
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        if (apps.size() > 1 && ImGui::IsItemHovered()) {
            std::string all;
            for (const auto& a : apps) all += (all.empty() ? "" : "\n") + a;
            ImGui::SetTooltip("%s", all.c_str());
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextFaint));
        ImGui::TextUnformatted(rate > 1000.0f ? "no apps" : "idle");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::PushFont(g_fontSmall);
            ImGui::SetTooltip("Point an application at \"Openmix - %s\" in\n"
                              "Windows sound settings to route it here.", b.name.c_str());
            ImGui::PopFont();
        }
    }
    ImGui::PopFont();

    ImGui::Unindent(10.0f * g_scale);
    ImGui::Dummy(ImVec2(0, 6.0f * g_scale));

    // Meter and the two faders, side by side. Left is what you hear, right is
    // what everyone else hears; that holds on every strip including the mic.
    const float faderH = height - 172.0f * g_scale;
    const float meterW = 12.0f * g_scale;
    const float faderW = 38.0f * g_scale;
    const float faderGap = 8.0f * g_scale;

    ImGui::SetCursorPosX(12.0f * g_scale);
    if (g_meters.size() <= index) g_meters.resize(index + 1);
    mix::verticalMeter(g_meters[index], b.ring.takePeak(),
                       ImGui::GetIO().DeltaTime, ImVec2(meterW, faderH));

    bool released = false;
    float db = dbFromGain(b.gain);
    ImGui::SameLine(0.0f, 6.0f * g_scale);
    if (mix::verticalFader("mon", &db, ImVec2(faderW, faderH), accent, &released)) {
        b.gain = gainFromDb(db);
    }
    if (released) saveSettings();
    tip(b.isCapture ? "How loud you hear yourself" : "How loud you hear this");

    float sdb = dbFromGain(b.streamGain);
    ImGui::SameLine(0.0f, faderGap);
    if (mix::verticalFader("str", &sdb, ImVec2(faderW, faderH),
                           theme::mix(accent, theme::kText, 0.45f), &released)) {
        b.streamGain = gainFromDb(sdb);
    }
    if (released) saveSettings();
    tip(b.isCapture ? "How loud applications hear you"
                    : "How loud the stream hears this");

    // Each readout is centred under the fader it belongs to.
    const float monX = 12.0f * g_scale + meterW + 6.0f * g_scale;
    const float strX = monX + faderW + faderGap;
    auto centred = [&](float columnX, const char* text) {
        const float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(columnX + (faderW - w) * 0.5f);
        ImGui::TextUnformatted(text);
    };

    char buf[16];
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextDim));
    std::snprintf(buf, sizeof(buf), "%.1f", db);
    centred(monX, buf);
    ImGui::SameLine(0.0f, 0.0f);
    std::snprintf(buf, sizeof(buf), "%.1f", sdb);
    centred(strX, buf);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextFaint));
    centred(monX, "you");
    ImGui::SameLine(0.0f, 0.0f);
    centred(strX, b.isCapture ? "apps" : "stream");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 5.0f * g_scale));

    ImGui::SetCursorPosX(12.0f * g_scale);
    const bool dspOn = b.eq.enabled || b.mic.gate.enabled || b.mic.comp.enabled;
    if (mix::pillButton(b.muted ? "MUTED" : "MUTE", b.muted,
                        ImVec2(48.0f * g_scale, 24.0f * g_scale), theme::kMuted)) {
        b.muted = !b.muted;
        saveSettings();
    }
    tip("Silence this in your headphones only");

    ImGui::SameLine(0.0f, 4.0f * g_scale);
    if (mix::pillButton("S", b.soloed,
                        ImVec2(26.0f * g_scale, 24.0f * g_scale), theme::kAccent)) {
        b.soloed = !b.soloed;
    }
    tip("Hear only the soloed channels. Does not affect the stream.");

    if (b.isCapture && b.streamMuted) {
        // A muted microphone that looks fine is how people talk to nobody for
        // ten minutes, so it is called out rather than implied by a fader.
        ImDrawList* d2 = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        d2->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), theme::kMuted, 8.0f, 0, 2.0f);
    }

    ImGui::SameLine(0.0f, 4.0f * g_scale);
    if (mix::pillButton(b.isCapture ? "FX" : "EQ", dspOn,
                        ImVec2(30.0f * g_scale, 24.0f * g_scale), theme::kAccent)) {
        ImGui::OpenPopup("fx");
    }
    tip(b.isCapture ? "Equaliser, noise gate and compressor" : "Equaliser");

    ImGui::SetNextWindowSize(ImVec2(430.0f * g_scale, 0));
    if (ImGui::BeginPopup("fx")) {
        drawEffects(b);
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopID();
}

// ---- panels ------------------------------------------------------------

void drawDevicePicker(const char* label, const char* id,
                      const std::vector<RenderDevice>& list,
                      const std::string& current, bool isOutput, float width) {
    ImGui::BeginGroup();
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::SetNextItemWidth(width);
    const std::string shown = current.empty() ? std::string("(none)") : current;
    if (ImGui::BeginCombo(id, shown.c_str())) {
        for (const auto& d : list) {
            // openmix's own devices are never offered: monitoring into our own
            // output would feed the engine back into itself, and capturing our
            // own microphone would loop it.
            if (d.isOpenmix) continue;
            const bool sel = (d.name == current);
            if (ImGui::Selectable(d.name.c_str(), sel)) {
                std::string err;
                const bool ok = isOutput ? g_engine.setOutputDevice(d.name, err)
                                         : g_engine.setMicDevice(d.name, err);
                if (ok) { g_notice.clear(); saveSettings(); } else { g_notice = err; }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndGroup();
}

// A titled card, so settings reads as the same product as the mixer rather
// than as a form bolted to the side of it.
void beginCard(const char* title, const char* subtitle = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec(theme::kPanel));
    ImGui::BeginChild(title, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle) {
        ImGui::PushFont(g_fontSmall);
        mix::textDim("%s", subtitle);
        ImGui::PopFont();
    }
    ImGui::Spacing();
}

void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10.0f * g_scale));
}

void drawSettings() {
    beginCard("Channels", "Each one becomes a device applications can select.");

    int removeAt = -1;
    for (size_t i = 0; i < g_channels.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(p.x, p.y + 3.0f), ImVec2(p.x + 5.0f, p.y + 21.0f),
                          theme::channelColor(i), 2.0f);
        ImGui::Dummy(ImVec2(12.0f * g_scale, 0));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Openmix - %s", g_channels[i].c_str());
        ImGui::SameLine(250.0f * g_scale);
        ImGui::BeginDisabled(g_channels.size() <= 1);
        if (ImGui::SmallButton("Remove")) removeAt = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // The microphone is a channel too, and leaving it out made settings
    // account for one fewer strip than the mixer shows.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(p.x, p.y + 3.0f), ImVec2(p.x + 5.0f, p.y + 21.0f),
                          theme::channelColor(g_channels.size()), 2.0f);
        ImGui::Dummy(ImVec2(12.0f * g_scale, 0));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Openmix - Mic");
        ImGui::SameLine(250.0f * g_scale);
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kTextFaint));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("always present");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(180.0f * g_scale);
    const bool entered = ImGui::InputTextWithHint("##newch", "Add a channel",
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
            restartEngine();
        }
    }
    if (removeAt >= 0) {
        g_channels.erase(g_channels.begin() + removeAt);
        g_config.set("channels", joinChannels(g_channels));
        restartEngine();
    }

    endCard();
    beginCard("General");

    bool hk = g_micHotkey;
    if (ImGui::Checkbox("Mute microphone with Ctrl+Alt+M", &hk)) setMicHotkey(hk);
    tip("Works while any application is focused");
    if (g_hotkeyFailed) {
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kMuted));
        ImGui::TextUnformatted("Another application already uses that shortcut.");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    if (ImGui::Button("Open Windows sound settings")) openWindowsAppVolume();
    tip("Where applications are pointed at a channel");
    ImGui::Spacing();

    if (ImGui::Checkbox("Start with Windows", &g_autostart)) {
        if (!setAutostart(g_autostart)) {
            g_autostart = autostartEnabled();
            g_notice = "Could not update the startup entry.";
        }
    }
    tip("Launches openmix straight to the tray when you sign in");

    if (!g_engine.namesApplied()) {
        ImGui::Spacing();
        ImGui::PushFont(g_fontSmall);
        mix::textDim("Channels show as \"Speakers (Openmix - ...)\" until renamed.");
        ImGui::PopFont();
        if (ImGui::Button("Fix device names")) {
            // Renaming an endpoint is an administrator write. It is needed
            // once: the name persists.
            wchar_t exe[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cli(exe);
            const size_t slash = cli.find_last_of(L'\\');
            if (slash != std::wstring::npos) cli = cli.substr(0, slash + 1);
            cli += L"openmix-cli.exe";
            ::ShellExecuteW(nullptr, L"runas", cli.c_str(), L"--fix-names", nullptr, SW_HIDE);
        }
        tip("Asks for administrator rights once; Windows remembers the names");
    }

    endCard();
    beginCard("Settings file");
    ImGui::PushFont(g_fontSmall);
    mix::textDim("%s", Config::path().c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::Button("Open folder")) {
        std::string dir = Config::path();
        const size_t slash = dir.find_last_of('\\');
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        ::ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    tip("Plain text; safe to edit while openmix is closed");
    endCard();
}

// Windows' per-application output setting is the one step openmix cannot do
// for itself, so at least take the user straight to it.
void openWindowsAppVolume() {
    ::ShellExecuteA(nullptr, "open", "ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
}

// Shown once, on the first run. Four strips and no explanation is not a
// starting point for someone who has just unzipped this.
void drawWelcome() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec(theme::kPanel));
    ImGui::BeginChild("welcome", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted("Welcome to openmix");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::TextWrapped(
        "Each channel below is now a device in Windows. Send an application to "
        "one and it gets its own fader here, and its own track in OBS.");
    ImGui::Spacing();

    ImGui::PushFont(g_fontSmall);
    mix::textDim("1.  Pick your headphones and microphone above.");
    mix::textDim("2.  Point applications at the channels in Windows sound settings.");
    mix::textDim("3.  In OBS, add each channel as an Audio Input Capture source.");
    ImGui::PopFont();
    ImGui::Spacing();

    if (ImGui::Button("Open Windows sound settings")) openWindowsAppVolume();
    ImGui::SameLine();
    if (ImGui::Button("Got it")) {
        g_showWelcome = false;
        g_config.setBool("welcomed", true);
        g_config.save();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10.0f * g_scale));
}

void drawBanner(const char* text, ImU32 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 48.0f * g_scale;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::fade(color, 0.14f), 6.0f);
    dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), color, 6.0f);

    ImGui::Dummy(ImVec2(0, 7.0f * g_scale));
    ImGui::Indent(14.0f * g_scale);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(color));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 28.0f * g_scale);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(14.0f * g_scale);
    ImGui::Dummy(ImVec2(0, 6.0f * g_scale));
}

void drawUi() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("openmix", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    const float headerY = ImGui::GetCursorPosY();
    const float rescanW = 62.0f * g_scale;
    const float settingsW = 70.0f * g_scale;
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const float gap = 16.0f * g_scale;

    // Share the space left over by the buttons equally, so a narrow window
    // shortens both device names rather than one.
    const float forPickers =
        ImGui::GetWindowWidth() - pad * 2.0f - rescanW - settingsW - 6.0f * g_scale - gap * 2.0f;
    const float pickerW =
        (std::max)(130.0f * g_scale, (std::min)(forPickers * 0.5f, 260.0f * g_scale));

    ImGui::SetCursorPos(ImVec2(pad, headerY));
    drawDevicePicker("HEADPHONES", "##out", g_outDevices,
                     g_engine.monitorDevice(), true, pickerW);

    ImGui::SetCursorPos(ImVec2(pad + pickerW + gap, headerY));
    drawDevicePicker("MICROPHONE", "##mic", g_micDevices,
                     g_engine.micDevice(), false, pickerW);
    // Line the buttons up with the combo boxes rather than with their labels.
    ImGui::SetCursorPosY(headerY + ImGui::GetTextLineHeight() + 6.0f * g_scale);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - pad - settingsW - 6.0f * g_scale - rescanW);
    if (ImGui::Button("Rescan", ImVec2(rescanW, 0))) {
        g_outDevices = listRenderDevices();
        g_micDevices = listCaptureDevices();
    }
    tip("Look for audio devices again");
    ImGui::SameLine(0.0f, 6.0f * g_scale);
    if (ImGui::Button(g_showSettings ? "Mixer" : "Settings", ImVec2(settingsW, 0))) {
        g_showSettings = !g_showSettings;
    }

    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));

    // Problems worth interrupting for.
    if (g_engine.usbipMissing()) {
        drawBanner("usbip-win2 is not installed, so no channels can be created. "
                   "Install v.0.9.7.7 from github.com/vadimgrn/usbip-win2, then "
                   "restart openmix.", theme::kMeterHot);
    } else if (!g_engine.running()) {
        drawBanner(g_startError.empty() ? "The audio engine is not running."
                                        : g_startError.c_str(), theme::kMeterHot);
        if (ImGui::Button("Retry")) restartEngine();
    } else if (g_engine.devicesMissing()) {
        drawBanner("Some channels did not connect. Check that usbip-win2 is working.",
                   theme::kMeterWarn);
    }
    if (!g_notice.empty()) drawBanner(g_notice.c_str(), theme::kMeterWarn);

    if (g_showWelcome && g_engine.running()) drawWelcome();

    if (g_showSettings) {
        ImGui::BeginChild("settings", ImVec2(0, 0), false);
        drawSettings();
        ImGui::EndChild();
    } else if (g_engine.running()) {
        const float height = ImGui::GetContentRegionAvail().y - 4.0f * g_scale;
        auto& buses = g_engine.buses();
        const auto& eps = g_engine.endpoints();

        ImGui::BeginChild("strips", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (size_t i = 0; i < buses.size(); ++i) {
            if (i) ImGui::SameLine(0.0f, 8.0f * g_scale);
            const bool attached = i < eps.size() && eps[i]->attached.load();
            drawStrip(i, buses[i], attached, static_cast<float>(g_engine.rate(i)), height);
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 4.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding = ImVec2(16, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 0.0f;
    s.ScrollbarSize = 10.0f;
    s.ScaleAllSizes(g_scale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = theme::toVec(theme::kBg);
    c[ImGuiCol_ChildBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]          = theme::toVec(theme::kPanel);
    c[ImGuiCol_Text]             = theme::toVec(theme::kText);
    c[ImGuiCol_TextDisabled]     = theme::toVec(theme::kTextFaint);
    c[ImGuiCol_Border]           = theme::toVec(theme::kLine);
    c[ImGuiCol_FrameBg]          = theme::toVec(theme::kPanelHi);
    c[ImGuiCol_FrameBgHovered]   = theme::toVec(theme::mix(theme::kPanelHi, theme::kAccent, 0.18f));
    c[ImGuiCol_FrameBgActive]    = theme::toVec(theme::mix(theme::kPanelHi, theme::kAccent, 0.3f));
    c[ImGuiCol_Button]           = theme::toVec(theme::kPanelHi);
    c[ImGuiCol_ButtonHovered]    = theme::toVec(theme::mix(theme::kPanelHi, theme::kAccent, 0.25f));
    c[ImGuiCol_ButtonActive]     = theme::toVec(theme::mix(theme::kPanelHi, theme::kAccent, 0.4f));
    c[ImGuiCol_Header]           = theme::toVec(theme::kPanelHi);
    c[ImGuiCol_HeaderHovered]    = theme::toVec(theme::mix(theme::kPanelHi, theme::kAccent, 0.25f));
    c[ImGuiCol_HeaderActive]     = theme::toVec(theme::mix(theme::kPanelHi, theme::kAccent, 0.35f));
    c[ImGuiCol_SliderGrab]       = theme::toVec(theme::kAccent);
    c[ImGuiCol_SliderGrabActive] = theme::toVec(theme::mix(theme::kAccent, theme::kText, 0.3f));
    c[ImGuiCol_CheckMark]        = theme::toVec(theme::kAccent);
    c[ImGuiCol_Separator]        = theme::toVec(theme::kLine);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]    = theme::toVec(theme::kPanelHi);
    c[ImGuiCol_TableHeaderBg]    = theme::toVec(theme::kPanel);
    c[ImGuiCol_TableRowBgAlt]    = ImVec4(1, 1, 1, 0.02f);
}

// The stock ImGui font is a bitmap face made for debug overlays, and nothing
// else makes a window look more like a developer tool.
void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    const char* body = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* semi = "C:\\Windows\\Fonts\\seguisb.ttf";
    const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";

    g_fontBody = io.Fonts->AddFontFromFileTTF(body, 16.0f * g_scale, &cfg);
    if (!g_fontBody) {
        // A system without Segoe UI still gets a working window.
        g_fontBody = io.Fonts->AddFontDefault();
        g_fontHead = g_fontBody;
        g_fontSmall = g_fontBody;
        return;
    }
    g_fontHead = io.Fonts->AddFontFromFileTTF(semi, 16.0f * g_scale, &cfg);
    if (!g_fontHead) g_fontHead = io.Fonts->AddFontFromFileTTF(bold, 16.0f * g_scale, &cfg);
    if (!g_fontHead) g_fontHead = g_fontBody;
    g_fontSmall = io.Fonts->AddFontFromFileTTF(body, 12.5f * g_scale, &cfg);
    if (!g_fontSmall) g_fontSmall = g_fontBody;
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

    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::timeBeginPeriod(1);   // the USB pacer sleeps in ~1 ms steps

    g_scale = static_cast<float>(::GetDpiForSystem()) / 96.0f;
    if (g_scale < 1.0f) g_scale = 1.0f;

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
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             static_cast<int>(700 * g_scale),
                             static_cast<int>(540 * g_scale),
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    if (!createDevice(g_hwnd)) {
        cleanupDevice();
        ::UnregisterClassW(wc.lpszClassName, hInst);
        ::MessageBoxW(nullptr, L"Could not create a Direct3D device.", L"openmix", MB_ICONERROR);
        return 1;
    }

    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    // Launched by the autostart entry: go straight to the tray rather than
    // throwing a window in the user's face at every sign-in.
    g_config.load();
    restoreWindowPlacement();

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
    loadFonts();
    applyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_outDevices = listRenderDevices();
    g_micDevices = listCaptureDevices();

    g_autostart = autostartEnabled();
    g_showWelcome = !g_config.existed() || !g_config.getBool("welcomed", false);
    if (g_config.getBool("micHotkey", false)) setMicHotkey(true);
    {
        const auto saved = splitChannels(g_config.get("channels"));
        if (!saved.empty()) g_channels = saved;
    }

    EngineConfig cfg;
    cfg.playbackBuses = g_channels;
    cfg.outMatch = g_config.get("output");
    cfg.micMatch = g_config.get("mic");
    if (g_engine.start(cfg, g_startError)) applySettings();

    DWORD lastRateSample = ::GetTickCount();
    DWORD lastAppScan = 0;
    bool done = false;

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        const DWORD now = ::GetTickCount();
        if (now - lastRateSample >= 1000) {
            g_engine.sampleRates();
            lastRateSample = now;
        }
        // Enumerating sessions is a COM round trip per device, so it happens
        // far less often than the meters update, and never while hidden.
        if (!g_inTray && now - lastAppScan >= 2000) {
            refreshChannelApps();
            lastAppScan = now;
        }
        updateTrayTip();

        // Parked in the tray: stop rendering entirely, but keep audio running.
        if (g_inTray) {
            ::Sleep(120);
            continue;
        }

        if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(16);
            continue;
        }
        g_occluded = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(g_fontBody);
        drawUi();
        ImGui::PopFont();
        ImGui::Render();

        const ImVec4 bg = theme::toVec(theme::kBg);
        const float clear[4] = { bg.x, bg.y, bg.z, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present every second vblank. openmix is open for hours beside a
        // game, and a level meter is perfectly readable at 30 fps -- this
        // roughly halves the cost of having the window on screen.
        const HRESULT hr = g_swapChain->Present(2, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    saveSettings();
    g_engine.stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    ::UnregisterHotKey(g_hwnd, ID_HOTKEY_MIC);
    removeTrayIcon();
    cleanupDevice();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(wc.lpszClassName, hInst);

    ::timeEndPeriod(1);
    ::CoUninitialize();
    if (only) ::CloseHandle(only);
    return 0;
}
