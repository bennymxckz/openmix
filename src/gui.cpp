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
bool g_ownDefaults = false;
bool g_linkDevices = true;
// -1 is the mixer; anything else is that channel's own page.
int g_page = -1;
char g_presetName[40] = {};
char g_profileName[40] = {};
// The preset each channel is showing, so the combo can say so rather than
// asking every time. Empty means the curve has been edited since.
std::vector<std::string> g_busPreset;

// Defined further down, but used by the engine restart and shutdown paths
// above them.
void restoreDefaults();
void claimDefaults();
void rememberDefaults();
std::string g_channelError;
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
        ::AppendMenuW(menu, MF_STRING | (b.muted ? MF_CHECKED : 0),
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
            // Wide enough for the four default channels without scrolling:
            // four 138 px strips, their gaps, and the window padding.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = static_cast<LONG>(624 * g_scale);
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
                    b.muted = !b.muted;
                    break;
                }
                return 0;
            }
            if (LOWORD(wp) == ID_TRAY_QUIT) { ::PostQuitMessage(0); return 0; }
            break;

        case WM_HOTKEY:
            if (wp == ID_HOTKEY_MIC) {
                // One level per channel now, so this is simply the mic mute.
                for (auto& b : g_engine.buses()) {
                    if (!b.isCapture) continue;
                    b.muted = !b.muted;
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
        g_config.setFloat(k + "monitor", b.monitorGain);
        g_config.set(k + "device", b.outputDevice);
        g_config.set(k + "preset", &b - g_engine.buses().data() < static_cast<ptrdiff_t>(g_busPreset.size())
                                   ? g_busPreset[&b - g_engine.buses().data()]
                                   : std::string());

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
        const std::string m = k + "mix.";
        g_config.setBool(m + "mono", b.mix.mono);
        g_config.setFloat(m + "balance", b.mix.balance);
        g_config.setFloat(m + "delay", b.mix.delayMs);
        g_config.setBool(m + "limiter", b.mix.limiter);
        g_config.setBool(m + "duck", b.mix.duck);
        g_config.setFloat(m + "duckdb", b.mix.duckDb);
        g_config.setFloat(m + "duckrel", b.mix.duckReleaseMs);

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
        b.gain        = g_config.getFloat(k + "gain", 1.0f);
        b.muted       = g_config.getBool(k + "mute", false);
        b.monitorGain = g_config.getFloat(k + "monitor", 0.0f);
        b.outputDevice = g_config.get(k + "device");
        const size_t bi = static_cast<size_t>(&b - g_engine.buses().data());
        if (g_busPreset.size() <= bi) g_busPreset.resize(bi + 1);
        g_busPreset[bi] = g_config.get(k + "preset");

        const std::string m = k + "mix.";
        b.mix.mono          = g_config.getBool(m + "mono", false);
        b.mix.balance       = g_config.getFloat(m + "balance", 0.0f);
        b.mix.delayMs       = g_config.getFloat(m + "delay", 0.0f);
        b.mix.limiter       = g_config.getBool(m + "limiter", false);
        b.mix.duck          = g_config.getBool(m + "duck", false);
        b.mix.duckDb        = g_config.getFloat(m + "duckdb", -12.0f);
        b.mix.duckReleaseMs = g_config.getFloat(m + "duckrel", 400.0f);

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
        micMuted = b.muted;
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

// Shorten a Windows device name for a narrow strip. "Speakers (HyperX Cloud
// Alpha S Game)" is mostly packaging; the part inside the brackets is the part
// that identifies it.
std::string shortDeviceName(const std::string& full) {
    const size_t open = full.find(" (");
    if (open == std::string::npos) return full;
    const size_t close = full.find_last_of(')');
    if (close == std::string::npos || close <= open + 2) return full;
    return full.substr(open + 2, close - open - 2);
}

void routeChannel(size_t index, const std::string& device) {
    std::string err;
    auto& buses = g_engine.buses();
    if (g_linkDevices) {
        // Linked: one choice moves every playback channel, which is what the
        // link is for. The microphone has no output of its own.
        for (size_t i = 0; i < buses.size(); ++i) {
            if (buses[i].isCapture) continue;
            buses[i].outputDevice = device;
        }
        if (!g_engine.setOutputDevice(device, err)) g_notice = err;
    } else if (index < buses.size()) {
        if (!g_engine.setChannelDevice(index, device, err)) g_notice = err;
    }
    if (err.empty()) g_notice.clear();
    saveSettings();
}

void restartEngine() {
    saveSettings();
    g_engine.stop();
    EngineConfig cfg;
    cfg.playbackBuses = g_channels;
    cfg.outMatch = g_config.get("output");
    cfg.micMatch = g_config.get("mic");
    g_startError.clear();
    if (g_engine.start(cfg, g_startError)) {
        applySettings();
        // The endpoints were republished, so the defaults point at devices
        // that no longer exist until they are claimed again.
        if (g_ownDefaults) {
            g_outDevices = listRenderDevices();
            g_micDevices = listCaptureDevices();
            claimDefaults();
        }
    }
}

// Levels are a percentage of full scale, linear in amplitude.
float percentFromGain(float g) { return std::clamp(g, 0.0f, 1.0f) * 100.0f; }
float gainFromPercent(float p) { return std::clamp(p, 0.0f, 100.0f) / 100.0f; }

void openWindowsAppVolume();
void restoreDefaults();
void claimDefaults();

// ---- Windows default devices -------------------------------------------

std::string narrowW(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), char{});
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widenS(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), wchar_t{});
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

bool isOpenmixId(const std::wstring& id) {
    for (const auto& d : g_outDevices) if (d.id == id) return d.isOpenmix;
    for (const auto& d : g_micDevices) if (d.id == id) return d.isOpenmix;
    return false;
}

std::string defaultKey(bool capture, int role) {
    return std::string("prevDefault.") + (capture ? "in." : "out.") + std::to_string(role);
}

// Remember what the defaults were before openmix took them, so they can be
// handed back. An openmix device is never recorded as the previous one -- that
// would make "restore" a no-op after the first restart.
void rememberDefaults() {
    for (int cap = 0; cap < 2; ++cap) {
        for (int role = 0; role < 3; ++role) {
            const std::wstring id = defaultEndpointId(cap != 0, role);
            if (id.empty() || isOpenmixId(id)) continue;
            g_config.set(defaultKey(cap != 0, role), narrowW(id));
        }
    }
}

void restoreDefaults() {
    for (int cap = 0; cap < 2; ++cap) {
        for (int role = 0; role < 3; ++role) {
            const std::wstring id = widenS(g_config.get(defaultKey(cap != 0, role)));
            if (!id.empty()) setDefaultEndpoint(id, role);
        }
    }
}

// Everything general goes to the first channel, chat applications to the
// second via the separate Communications role, recording to the microphone.
// That is how Discord lands on Chat without touching Discord's settings.
void claimDefaults() {
    auto find = [](const std::vector<RenderDevice>& list, const std::string& want) {
        for (const auto& d : list) {
            if (d.name.find("Openmix - " + want) != std::string::npos) return d.id;
        }
        return std::wstring{};
    };

    const std::string general = g_channels.empty() ? std::string("Game") : g_channels[0];
    const std::string chat = g_channels.size() > 1 ? g_channels[1] : general;

    const std::wstring gen = find(g_outDevices, general);
    const std::wstring cht = find(g_outDevices, chat);
    const std::wstring mic = find(g_micDevices, "Mic");

    if (!gen.empty()) {
        setDefaultEndpoint(gen, 0);   // Console
        setDefaultEndpoint(gen, 1);   // Multimedia
    }
    if (!cht.empty()) setDefaultEndpoint(cht, 2);   // Communications
    if (!mic.empty()) {
        for (int role = 0; role < 3; ++role) setDefaultEndpoint(mic, role);
    }
}

void setOwnDefaults(bool on) {
    // The device lists have to be current or the openmix endpoints will not be
    // found; they only appear once the engine has attached them.
    g_outDevices = listRenderDevices();
    g_micDevices = listCaptureDevices();

    if (on) {
        rememberDefaults();
        claimDefaults();
    } else {
        restoreDefaults();
    }
    g_ownDefaults = on;
    g_config.setBool("ownDefaults", on);
    g_config.save();
}


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
    // Separate from `changed`: switching the equaliser on does not make the
    // curve stop being the preset it came from, but moving a band does.
    bool curveEdited = false;

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
        changed = curveEdited = true;
    }
    tip("Reset every band to no change");

    ImGui::Spacing();
    drawEqCurve(b.eq, ImVec2(ImGui::GetContentRegionAvail().x,
                             (g_page >= 0 ? 200.0f : 96.0f) * g_scale));
    ImGui::Spacing();

    ImGui::BeginDisabled(!b.eq.enabled);
    if (ImGui::BeginTable("eq", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("band", ImGuiTableColumnFlags_WidthFixed, 110.0f * g_scale);
        ImGui::TableSetupColumn("freq");
        ImGui::TableSetupColumn("gain");
        ImGui::TableSetupColumn("q");
        curveEdited |= drawBand("High-pass", b.eq.hp, false);
        curveEdited |= drawBand("Low", b.eq.low, true);
        curveEdited |= drawBand("Mid", b.eq.mid, true);
        curveEdited |= drawBand("High", b.eq.high, true);
        changed |= curveEdited;
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (curveEdited && g_page >= 0 &&
        static_cast<size_t>(g_page) < g_busPreset.size()) {
        g_busPreset[g_page].clear();
    }

    if (b.isCapture) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(g_fontHead);
        ImGui::TextUnformatted("Hear yourself");
        ImGui::PopFont();
        ImGui::PushFont(g_fontSmall);
        mix::textDim("Separate from how loud applications hear you.");
        ImGui::PopFont();
        float mon = percentFromGain(b.monitorGain);
        ImGui::SetNextItemWidth(-150.0f * g_scale);
        if (ImGui::SliderFloat("In your headphones", &mon, 0.0f, 100.0f, "%.0f%%")) {
            b.monitorGain = gainFromPercent(mon);
            changed = true;
        }

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
        const float lbl = -150.0f * g_scale;   // room for the label
        ImGui::SetNextItemWidth(lbl);
        changed |= ImGui::SliderFloat("Opens above", &b.mic.gate.thresholdDb,
                                      -80.0f, 0.0f, "%.0f dB");
        ImGui::SetNextItemWidth(lbl);
        changed |= ImGui::SliderFloat("Stays open for", &b.mic.gate.holdMs,
                                      0.0f, 500.0f, "%.0f ms");
        ImGui::SetNextItemWidth(lbl);
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
        ImGui::SetNextItemWidth(lbl);
        changed |= ImGui::SliderFloat("Squeezes above", &b.mic.comp.thresholdDb,
                                      -48.0f, 0.0f, "%.0f dB");
        ImGui::SetNextItemWidth(lbl);
        changed |= ImGui::SliderFloat("By", &b.mic.comp.ratio, 1.0f, 12.0f, "%.1f : 1");
        ImGui::SetNextItemWidth(lbl);
        changed |= ImGui::SliderFloat("Reacts in", &b.mic.comp.attackMs,
                                      0.5f, 50.0f, "%.1f ms");
        ImGui::SetNextItemWidth(lbl);
        changed |= ImGui::SliderFloat("Recovers in", &b.mic.comp.releaseMs,
                                      20.0f, 800.0f, "%.0f ms");
        ImGui::SetNextItemWidth(lbl);
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
    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));

    // Where this channel is heard. The microphone has no output of its own.
    if (!b.isCapture) {
        const std::string current = b.outputDevice.empty() ? g_engine.monitorDevice()
                                                           : b.outputDevice;
        ImGui::SetCursorPosX(8.0f * g_scale);
        ImGui::SetNextItemWidth(stripW - 16.0f * g_scale);
        ImGui::PushFont(g_fontSmall);
        if (ImGui::BeginCombo("##dev", shortDeviceName(current).c_str(),
                              ImGuiComboFlags_HeightSmall)) {
            for (const auto& d : g_outDevices) {
                if (d.isOpenmix) continue;   // routing a channel into itself
                if (ImGui::Selectable(d.name.c_str(), d.name == current)) {
                    routeChannel(index, d.name);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopFont();
        tip(g_linkDevices ? "Output device. Channels are linked, so this moves them all."
                          : "Output device for this channel only.");
    } else {
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
    }

    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));

    // One level per channel: what you set is what everyone gets, here and on
    // the stream. Two faders per channel read as twice the decision.
    // Everything above and below the fader: name, apps, device row, the
    // percentage, and the button row. The fader takes what is left.
    const float faderH = height - 218.0f * g_scale;
    const float meterW = 13.0f * g_scale;
    const float faderW = 46.0f * g_scale;

    float percent = percentFromGain(b.gain);

    ImGui::PushFont(g_fontHead);
    {
        char pct[12];
        std::snprintf(pct, sizeof(pct), "%.0f%%", percent);
        const float w = ImGui::CalcTextSize(pct).x;
        ImGui::SetCursorPosX((stripW - w) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(b.muted ? theme::kMuted : accent));
        ImGui::TextUnformatted(pct);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));

    const float groupW = meterW + 8.0f * g_scale + faderW;
    ImGui::SetCursorPosX((stripW - groupW) * 0.5f);
    if (g_meters.size() <= index) g_meters.resize(index + 1);
    mix::verticalMeter(g_meters[index], b.ring.takePeak(),
                       ImGui::GetIO().DeltaTime, ImVec2(meterW, faderH));

    bool released = false;
    ImGui::SameLine(0.0f, 8.0f * g_scale);
    if (mix::verticalFader("lvl", &percent, ImVec2(faderW, faderH), accent, &released)) {
        b.gain = gainFromPercent(percent);
    }
    if (released) saveSettings();
    tip(b.isCapture ? "How loud applications hear you" : "How loud this channel is");

    ImGui::Dummy(ImVec2(0, 6.0f * g_scale));

    ImGui::SetCursorPosX(12.0f * g_scale);
    // Lit whenever the channel is doing something to its sound, so a
    // forgotten duck or limiter is visible from the mixer.
    const bool dspOn = b.eq.enabled || b.mic.gate.enabled || b.mic.comp.enabled ||
                       b.mix.duck || b.mix.limiter || b.mix.mono ||
                       b.mix.balance != 0.0f || b.mix.delayMs >= 0.5f;
    if (mix::pillButton(b.muted ? "MUTED" : "MUTE", b.muted,
                        ImVec2(48.0f * g_scale, 24.0f * g_scale), theme::kMuted)) {
        b.muted = !b.muted;
        saveSettings();
    }
    tip("Silence this channel everywhere");

    ImGui::SameLine(0.0f, 4.0f * g_scale);
    if (mix::pillButton("S", b.soloed,
                        ImVec2(26.0f * g_scale, 24.0f * g_scale), theme::kAccent)) {
        b.soloed = !b.soloed;
    }
    tip("Hear only the soloed channels. Does not affect the stream.");

    if (b.isCapture && b.muted) {
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
        g_page = static_cast<int>(index);
    }
    tip(b.isCapture ? "Equaliser, noise gate and compressor" : "Equaliser");

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

// ---------------------------------------------------------------------------
// Profiles: the whole mixer saved under a name.
//
// A streaming setup and a "just playing" setup differ in more than one
// control -- levels, ducking, which device each channel goes to -- and setting
// them back by hand every time is the sort of chore that ends with people not
// bothering. A profile is a copy of every bus.* key under profile.<name>.*,
// which is why the config is a flat map of strings rather than a struct.

std::vector<std::string> profileNames() {
    std::vector<std::string> out;
    for (const auto& key : g_config.keys()) {
        if (key.rfind("profile.", 0) != 0) continue;
        const size_t dot = key.find(".bus.", 8);
        if (dot == std::string::npos) continue;
        const std::string name = key.substr(8, dot - 8);
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void saveProfile(const std::string& name) {
    saveSettings();                       // capture what is on screen first
    const std::string dst = "profile." + name + ".";
    g_config.removePrefix(dst);           // replacing, not merging
    for (const auto& key : g_config.keys()) {
        if (key.rfind("bus.", 0) != 0) continue;
        g_config.set(dst + key, g_config.get(key));
    }
    g_config.save();
}

void loadProfile(const std::string& name) {
    const std::string src = "profile." + name + ".";
    // Copy first, then apply: writing into the map while walking it is how
    // you get a half-applied profile.
    std::vector<std::pair<std::string, std::string>> restore;
    for (const auto& key : g_config.keys()) {
        if (key.rfind(src, 0) != 0) continue;
        restore.emplace_back(key.substr(src.size()), g_config.get(key));
    }
    if (restore.empty()) return;

    // Channels the profile does not mention keep what they have; a profile
    // saved before a channel existed should not silently blank it.
    for (const auto& [k, v] : restore) g_config.set(k, v);
    applySettings();

    // Output devices live in the profile too, so the engine has to be told.
    std::string err;
    for (size_t i = 0; i < g_engine.buses().size(); ++i) {
        g_engine.setChannelDevice(i, g_engine.buses()[i].outputDevice, err);
    }
    g_config.save();
}

void deleteProfile(const std::string& name) {
    g_config.removePrefix("profile." + name + ".");
    g_config.save();
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

    if (!g_channelError.empty()) {
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kMuted));
        ImGui::TextUnformatted(g_channelError.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    if ((entered || clicked) && g_newChannel[0] != 0) {
        std::string name = g_newChannel;
        // The name becomes a USB serial and a Windows device name, so keep it
        // to something both will accept.
        name.erase(std::remove_if(name.begin(), name.end(),
                                  [](unsigned char c) { return !std::isalnum(c) && c != ' '; }),
                   name.end());
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();

        auto sameName = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };

        g_channelError.clear();
        if (name.empty()) {
            g_channelError = "Use letters and numbers.";
        } else if (sameName(name, "Mic")) {
            // A channel's USB identity comes from its name, so a second "Mic"
            // would claim the microphone's device.
            g_channelError = "\"Mic\" is taken by the microphone channel.";
        } else if (std::any_of(g_channels.begin(), g_channels.end(),
                               [&](const std::string& c) { return sameName(c, name); })) {
            g_channelError = "There is already a channel called that.";
        } else if (g_channels.size() >= 8) {
            g_channelError = "Eight channels is the limit.";
        } else {
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
    beginCard("Profiles",
              "The whole mixer under a name -- levels, devices, equalisers and all.");

    const auto profiles = profileNames();
    if (profiles.empty()) {
        ImGui::PushFont(g_fontSmall);
        mix::textDim("Set the mixer up the way you want it, name it, and it is one\n"
                     "click away from then on. A streaming layout and a casual one\n"
                     "rarely want the same levels.");
        ImGui::PopFont();
        ImGui::Spacing();
    }
    for (const auto& n : profiles) {
        ImGui::PushID(n.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(n.c_str());
        ImGui::SameLine(250.0f * g_scale);
        if (ImGui::SmallButton("Load")) loadProfile(n);
        tip("Put every channel back the way this profile has them");
        ImGui::SameLine();
        if (ImGui::SmallButton("Overwrite")) saveProfile(n);
        tip("Replace it with the mixer as it stands now");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) deleteProfile(n);
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(200.0f * g_scale);
    const bool profEnter = ImGui::InputTextWithHint("##newprof", "Save the mixer as...",
                                                    g_profileName, sizeof(g_profileName),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Save##prof") || profEnter) && g_profileName[0] != 0) {
        saveProfile(g_profileName);
        g_profileName[0] = 0;
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

    bool own = g_ownDefaults;
    if (ImGui::Checkbox("Make openmix the default audio device", &own)) setOwnDefaults(own);
    tip("Sends everything to the first channel, chat applications to the second,\n"
        "and recording to the microphone. Your previous devices are restored\n"
        "when this is turned off or openmix quits.");
    ImGui::PushFont(g_fontSmall);
    if (g_ownDefaults && g_channels.size() > 1) {
        mix::textDim("General audio to %s, chat applications to %s.",
                     g_channels[0].c_str(), g_channels[1].c_str());
    } else {
        mix::textDim("Windows keeps a separate default for chat applications; "
                     "openmix can use it.");
    }
    ImGui::PopFont();
    ImGui::Spacing();

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

// ---- equaliser presets --------------------------------------------------
//
// Stored in the same plain-text settings file as everything else, under
// preset.<name>.*, and shared across channels: a curve worth keeping for the
// microphone is often worth trying on a game.

// A few curves worth having before anyone has saved one. They are starting
// points, not settings: everything stays editable once loaded, and saving over
// one under a new name is the intended way to keep a tweak.
struct Preset {
    const char* name;
    dsp::EqParams eq;
};

const Preset kBuiltIn[] = {
    // Rolls off rumble, lifts presence, tames the boxy low mids. The usual
    // shape for a voice that has to cut through a game.
    {"Voice", {true, {90.0f, 0.0f, 0.707f, true},
                     {250.0f, -3.0f, 0.9f, true},
                     {2800.0f, 4.0f, 0.9f, true},
                     {8000.0f, 3.0f, 0.707f, true}}},
    // Footsteps and reloads live around 3-5 kHz; pulling the low end down
    // stops explosions masking them.
    {"Footsteps", {true, {120.0f, 0.0f, 0.707f, true},
                         {180.0f, -6.0f, 0.707f, true},
                         {4000.0f, 6.0f, 1.4f, true},
                         {9000.0f, 2.0f, 0.707f, true}}},
    {"Bass boost", {true, {20.0f, 0.0f, 0.707f, false},
                          {110.0f, 6.0f, 0.707f, true},
                          {1000.0f, 0.0f, 1.0f, false},
                          {10000.0f, 1.0f, 0.707f, true}}},
    // Small speakers and cheap headsets: less to fight in the low mids, more
    // at the top so detail survives.
    {"Clarity", {true, {60.0f, 0.0f, 0.707f, true},
                       {350.0f, -4.0f, 1.0f, true},
                       {1800.0f, 2.0f, 0.8f, true},
                       {7000.0f, 4.0f, 0.707f, true}}},
    {"Flat", {true, {80.0f, 0.0f, 0.707f, false},
                    {200.0f, 0.0f, 0.707f, false},
                    {1000.0f, 0.0f, 1.0f, false},
                    {6000.0f, 0.0f, 0.707f, false}}},
};

const dsp::EqParams* builtInPreset(const std::string& name) {
    for (const auto& p : kBuiltIn) {
        if (name == p.name) return &p.eq;
    }
    return nullptr;
}

std::vector<std::string> presetNames() {
    std::vector<std::string> out;
    for (const auto& key : g_config.keys()) {
        if (key.rfind("preset.", 0) != 0) continue;
        const size_t dot = key.find('.', 7);
        if (dot == std::string::npos) continue;
        const std::string name = key.substr(7, dot - 7);
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void savePreset(const std::string& name, const dsp::EqParams& eq) {
    const std::string k = "preset." + name + ".";
    const dsp::Band* bands[4] = { &eq.hp, &eq.low, &eq.mid, &eq.high };
    const char* names[4] = { "hp", "low", "mid", "high" };
    for (int i = 0; i < 4; ++i) {
        g_config.setBool(k + names[i] + ".on", bands[i]->on);
        g_config.setFloat(k + names[i] + ".f", bands[i]->freq);
        g_config.setFloat(k + names[i] + ".g", bands[i]->gainDb);
        g_config.setFloat(k + names[i] + ".q", bands[i]->q);
    }
    g_config.save();
}

void loadPreset(const std::string& name, dsp::EqParams& eq) {
    if (const dsp::EqParams* built = builtInPreset(name)) {
        eq = *built;
        eq.enabled = true;
        return;
    }
    const std::string k = "preset." + name + ".";
    dsp::Band* bands[4] = { &eq.hp, &eq.low, &eq.mid, &eq.high };
    const char* names[4] = { "hp", "low", "mid", "high" };
    for (int i = 0; i < 4; ++i) {
        bands[i]->on     = g_config.getBool(k + names[i] + ".on", bands[i]->on);
        bands[i]->freq   = g_config.getFloat(k + names[i] + ".f", bands[i]->freq);
        bands[i]->gainDb = g_config.getFloat(k + names[i] + ".g", bands[i]->gainDb);
        bands[i]->q      = g_config.getFloat(k + names[i] + ".q", bands[i]->q);
    }
    eq.enabled = true;   // loading a curve and hearing nothing is a puzzle
}

void deletePreset(const std::string& name) {
    g_config.removePrefix("preset." + name + ".");
    g_config.save();
}

// Everything a channel does to its own sound short of the equaliser. Playback
// only: the microphone has its own dynamics section, and delaying or ducking
// the source you are ducking against makes no sense.
bool drawMixSection(Bus& b) {
    bool changed = false;

    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted("Output");
    ImGui::PopFont();
    ImGui::Spacing();

    changed |= ImGui::Checkbox("Mono", &b.mix.mono);
    tip("Fold both sides together -- useful for a chat application that\n"
        "puts one person in each ear");

    ImGui::SameLine(0.0f, 24.0f * g_scale);
    changed |= ImGui::Checkbox("Limiter", &b.mix.limiter);
    tip("Brick wall just under full scale, so one loud moment cannot clip\n"
        "the recording");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-150.0f * g_scale);
    float bal = b.mix.balance * 100.0f;
    if (ImGui::SliderFloat("Balance", &bal, -100.0f, 100.0f,
                           std::fabs(bal) < 1.0f ? "Centre"
                                                 : (bal < 0 ? "%.0f left" : "%.0f right"))) {
        b.mix.balance = std::clamp(bal, -100.0f, 100.0f) / 100.0f;
        changed = true;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { b.mix.balance = 0.0f; changed = true; }
    tip("Right-click to centre");

    ImGui::SetNextItemWidth(-150.0f * g_scale);
    if (ImGui::SliderFloat("Delay", &b.mix.delayMs, 0.0f, 250.0f,
                           b.mix.delayMs < 0.5f ? "None" : "%.0f ms")) {
        changed = true;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { b.mix.delayMs = 0.0f; changed = true; }
    tip("Hold this channel back to line it up with video that arrives late.\n"
        "Right-click to clear");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted("Duck under the microphone");
    ImGui::PopFont();
    ImGui::SameLine();
    changed |= ImGui::Checkbox("##duckon", &b.mix.duck);
    ImGui::PushFont(g_fontSmall);
    mix::textDim("Pull this channel down while you are talking. Nothing Sonar does.");
    ImGui::PopFont();

    ImGui::BeginDisabled(!b.mix.duck);
    ImGui::SetNextItemWidth(-150.0f * g_scale);
    changed |= ImGui::SliderFloat("How far down", &b.mix.duckDb, -40.0f, -1.0f, "%.0f dB");
    ImGui::SetNextItemWidth(-150.0f * g_scale);
    changed |= ImGui::SliderFloat("Come back over", &b.mix.duckReleaseMs, 50.0f, 2000.0f, "%.0f ms");

    // The reduction actually being applied, so the amount can be set against
    // real speech instead of guessed at.
    const float red = b.mixChain.duckReductionDb();
    ImGui::PushFont(g_fontSmall);
    if (b.mix.duck && red < -0.2f) {
        ImGui::TextColored(theme::toVec(theme::kAccent), "-%.1f dB right now", -red);
    } else {
        mix::textDim("Not ducking");
    }
    ImGui::PopFont();
    ImGui::EndDisabled();

    return changed;
}

void drawChannelPage(size_t index, Bus& b) {
    const ImU32 accent = theme::channelColor(index);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + 5.0f, p.y + 26.0f), accent, 2.0f);
    ImGui::Dummy(ImVec2(14.0f * g_scale, 0));
    ImGui::SameLine();
    ImGui::PushFont(g_fontHead);
    ImGui::TextUnformatted(b.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(g_fontSmall);
    mix::textDim("Openmix - %s", b.name.c_str());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 6.0f * g_scale));

    // ---- level, mute and destination, so the page stands on its own ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec(theme::kPanel));
    ImGui::BeginChild("head", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

    if (mix::pillButton(b.muted ? "MUTED" : "MUTE", b.muted,
                        ImVec2(58.0f * g_scale, 26.0f * g_scale), theme::kMuted)) {
        b.muted = !b.muted;
        saveSettings();
    }
    tip("Silence this channel everywhere");

    ImGui::SameLine(0.0f, 6.0f * g_scale);
    if (mix::pillButton("S", b.soloed,
                        ImVec2(28.0f * g_scale, 26.0f * g_scale), theme::kAccent)) {
        b.soloed = !b.soloed;
    }
    tip("Hear only the soloed channels. Does not affect the stream.");

    ImGui::SameLine(0.0f, 12.0f * g_scale);
    float percent = percentFromGain(b.gain);
    ImGui::SetNextItemWidth(-260.0f * g_scale);
    if (ImGui::SliderFloat("##level", &percent, 0.0f, 100.0f, "%.0f%%")) {
        b.gain = gainFromPercent(percent);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
    tip(b.isCapture ? "How loud applications hear you" : "How loud this channel is");

    // Where this channel is heard. The microphone has no output of its own.
    ImGui::SameLine(0.0f, 12.0f * g_scale);
    if (!b.isCapture) {
        const std::string current = b.outputDevice.empty() ? g_engine.monitorDevice()
                                                           : b.outputDevice;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##pagedev", shortDeviceName(current).c_str())) {
            for (const auto& d : g_outDevices) {
                if (d.isOpenmix) continue;   // routing a channel into itself
                if (ImGui::Selectable(d.name.c_str(), d.name == current)) {
                    routeChannel(index, d.name);
                }
            }
            ImGui::EndCombo();
        }
        tip(g_linkDevices ? "Output device. Channels are linked, so this moves them all."
                          : "Output device for this channel only.");
    } else {
        ImGui::PushFont(g_fontSmall);
        ImGui::AlignTextToFramePadding();
        mix::textDim("%s", g_engine.micDevice().empty() ? "no microphone"
                                                        : g_engine.micDevice().c_str());
        ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0, 2.0f * g_scale));
    if (g_meters.size() <= index) g_meters.resize(index + 1);
    mix::horizontalMeter(g_meters[index], b.ring.takePeak(),
                         ImGui::GetIO().DeltaTime,
                         ImVec2(ImGui::GetContentRegionAvail().x, 8.0f * g_scale));

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10.0f * g_scale));

    // ---- presets ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec(theme::kPanel));
    ImGui::BeginChild("presets", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g_fontSmall);
    mix::textDim("PRESET");
    ImGui::PopFont();

    if (g_busPreset.size() <= index) g_busPreset.resize(index + 1);
    std::string& shown = g_busPreset[index];
    const auto saved = presetNames();

    ImGui::SetNextItemWidth(220.0f * g_scale);
    if (ImGui::BeginCombo("##preset", shown.empty() ? "Custom" : shown.c_str())) {
        for (const auto& p : kBuiltIn) {
            if (ImGui::Selectable(p.name, shown == p.name)) {
                loadPreset(p.name, b.eq);
                shown = p.name;
                saveSettings();
            }
        }
        if (!saved.empty()) {
            ImGui::Separator();
            for (const auto& n : saved) {
                if (ImGui::Selectable(n.c_str(), shown == n)) {
                    loadPreset(n, b.eq);
                    shown = n;
                    saveSettings();
                }
            }
        }
        ImGui::EndCombo();
    }
    tip("Built-in curves first, then your own. All of them stay editable.");

    // Deleting is only offered for a preset of the user's own that is actually
    // loaded, so there is never a list of trash icons to misclick.
    const bool ownPreset =
        !shown.empty() && !builtInPreset(shown) &&
        std::find(saved.begin(), saved.end(), shown) != saved.end();
    if (ownPreset) {
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            deletePreset(shown);
            shown.clear();
        }
        tip("Remove this preset. The curve stays where it is.");
    }

    ImGui::SameLine(0.0f, 16.0f * g_scale);
    ImGui::SetNextItemWidth(160.0f * g_scale);
    const bool enter = ImGui::InputTextWithHint("##pname", "Save as...",
                                                g_presetName, sizeof(g_presetName),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Save") || enter) && g_presetName[0] != 0) {
        savePreset(g_presetName, b.eq);
        shown = g_presetName;
        g_presetName[0] = 0;
    }
    tip("Presets are shared across channels, so a curve set up here can be\n"
        "dropped onto any of them");
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10.0f * g_scale));

    // ---- output shaping ----
    if (!b.isCapture) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec(theme::kPanel));
        ImGui::BeginChild("mix", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (drawMixSection(b)) saveSettings();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10.0f * g_scale));
    }

    // ---- processing ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec(theme::kPanel));
    ImGui::BeginChild("fx", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    drawEffects(b);
    ImGui::EndChild();
    ImGui::PopStyleColor();
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
    const float linkW = 62.0f * g_scale;
    const float settingsW = 70.0f * g_scale;
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const float gap = 16.0f * g_scale;

    // Share the space left over by the buttons equally, so a narrow window
    // shortens both device names rather than one.
    const float forPickers = ImGui::GetWindowWidth() - pad * 2.0f - rescanW - linkW
                             - settingsW - 12.0f * g_scale - gap * 2.0f;
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
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - pad - settingsW - 12.0f * g_scale
                         - rescanW - linkW);
    ImGui::PushFont(g_fontSmall);
    if (ImGui::Checkbox("Link", &g_linkDevices)) {
        g_config.setBool("linkDevices", g_linkDevices);
        if (g_linkDevices) routeChannel(0, g_engine.monitorDevice());
        g_config.save();
    }
    ImGui::PopFont();
    tip("Keep every channel on the same output device");
    ImGui::SameLine(0.0f, 8.0f * g_scale);

    if (ImGui::Button("Rescan", ImVec2(rescanW, 0))) {
        g_outDevices = listRenderDevices();
        g_micDevices = listCaptureDevices();
    }
    tip("Look for audio devices again");
    ImGui::SameLine(0.0f, 6.0f * g_scale);
    if (ImGui::Button(g_showSettings ? "Mixer" : "Settings", ImVec2(settingsW, 0))) {
        g_showSettings = !g_showSettings;
        if (g_showSettings) g_page = -1;
    }

    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6.0f * g_scale));

    // Mixer, then one tab per channel. The mixer is the overview; a channel's
    // own page is where its processing lives, at a size worth editing on.
    if (!g_showSettings && g_engine.running()) {
        auto tab = [&](const char* label, int page, ImU32 color) {
            const bool on = (g_page == page);
            // theme::fade returns a packed colour; PushStyleColor needs the
            // vector form to sit alongside the transparent default.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  on ? theme::toVec(theme::fade(color, 0.22f))
                                     : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  theme::toVec(theme::fade(color, 0.16f)));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  theme::toVec(theme::fade(color, 0.3f)));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(on ? color : theme::kTextDim));
            if (ImGui::Button(label)) g_page = page;
            ImGui::PopStyleColor(4);
        };

        tab("Mixer", -1, theme::kAccent);
        ImGui::SameLine(0.0f, 6.0f * g_scale);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec(theme::kLine));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("|");
        ImGui::PopStyleColor();

        const auto& buses = g_engine.buses();
        for (size_t i = 0; i < buses.size(); ++i) {
            ImGui::SameLine(0.0f, 6.0f * g_scale);
            tab(buses[i].name.c_str(), static_cast<int>(i), theme::channelColor(i));
        }
        ImGui::Dummy(ImVec2(0, 6.0f * g_scale));
    }

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
    } else if (g_engine.running() && g_page >= 0 &&
               static_cast<size_t>(g_page) < g_engine.buses().size()) {
        ImGui::BeginChild("page", ImVec2(0, 0), false);
        drawChannelPage(static_cast<size_t>(g_page), g_engine.buses()[g_page]);
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
    c[ImGuiCol_ScrollbarGrab]        = theme::toVec(theme::kLine);
    c[ImGuiCol_ScrollbarGrabHovered] = theme::toVec(theme::kPanelHi);
    c[ImGuiCol_ScrollbarGrabActive]  = theme::toVec(theme::kAccent);
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
        auto saved = splitChannels(g_config.get("channels"));
        // Drop anything that would collide with the microphone channel: an
        // older config could contain one, and two buses with the same name
        // would claim the same USB identity.
        saved.erase(std::remove_if(saved.begin(), saved.end(),
                                   [](const std::string& c) {
                                       return c.size() == 3 &&
                                              std::tolower(static_cast<unsigned char>(c[0])) == 'm' &&
                                              std::tolower(static_cast<unsigned char>(c[1])) == 'i' &&
                                              std::tolower(static_cast<unsigned char>(c[2])) == 'c';
                                   }),
                    saved.end());
        if (!saved.empty()) g_channels = saved;
    }

    EngineConfig cfg;
    cfg.playbackBuses = g_channels;
    cfg.outMatch = g_config.get("output");
    cfg.micMatch = g_config.get("mic");
    if (g_engine.start(cfg, g_startError)) applySettings();

    g_linkDevices = g_config.getBool("linkDevices", true);
    g_ownDefaults = g_config.getBool("ownDefaults", false);
    if (g_ownDefaults) {
        // The endpoints only exist once the engine has attached them.
        g_outDevices = listRenderDevices();
        g_micDevices = listCaptureDevices();
        // Record what the defaults are before taking them, or there would be
        // nothing to hand back on the way out. At this point they are still
        // real devices: openmix's have only just appeared.
        rememberDefaults();
        claimDefaults();
    }

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

    saveWindowPlacement();
    saveSettings();
    // Hand the defaults back before the devices disappear, or Windows is left
    // pointing at endpoints that are about to stop existing.
    if (g_ownDefaults) restoreDefaults();
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
