// openmix - open-source per-application audio bus mixer for Windows.
//
// Two modes:
//
//   --endpoints (default)  Export virtual USB Audio Class devices over USB/IP.
//                          Windows publishes them as real playback devices
//                          ("openmix Game", "openmix Chat", ...) that apps can
//                          select, and audio arrives as isochronous packets.
//
//   --loopback             Tap running applications with the Windows
//                          process-loopback API instead. No devices are
//                          created; useful for metering without any setup.
//
// Both paths mix into one monitor output on your real device.

#include "audio.h"
#include "usbip_server.h"

#include <conio.h>
#include <timeapi.h>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>
#include <algorithm>

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    std::transform(w.begin(), w.end(), w.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return w;
}

std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) {
            if (start < s.size()) out.push_back(s.substr(start));
            break;
        }
        if (comma > start) out.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

void usage() {
    std::puts(
        "openmix - open-source per-app audio bus mixer\n"
        "\n"
        "  openmix [--endpoints|--loopback] [--bus NAME[=procs]] [-v]\n"
        "\n"
        "  --endpoints     export virtual USB audio devices (default)\n"
        "  --loopback      tap running apps instead of creating devices\n"
        "  --bus NAME      add a bus (endpoint mode)\n"
        "  --bus NAME=a.exe,b.exe   bus fed by those apps (loopback mode)\n"
        "  --bus NAME=*    catch-all bus (loopback mode)\n"
        "  --out NAME      send the monitor mix to this output device\n"
        "  --mic NAME      source the virtual microphone from this input\n"
        "  --no-mic        do not create the openmix Mic device\n"
        "  --list-devices  show available output devices and exit\n"
        "  --fix-names     rename the endpoints to \"Openmix - X\" (needs admin,\n"
        "  --selftest [CH] play a tone through a channel and check it comes\n"
        "                  back intact (default Game); openmix must be running\n"
        "                  run once while openmix is running)\n"
        "  --port N        USB/IP listen port (default 3240)\n"
        "  -v              log attach/detach and control traffic\n"
        "\n"
        "  Default buses: Game, Chat, Media\n"
        "\n"
        "Keys: 1-9 select bus | +/- gain | m mute | q quit\n");
}

// Run a command with no console window and wait for it. Returns its exit code,
// or -1 if the process could not be started.
int runQuiet(const std::string& cmd) {
    std::string mutableCmd = cmd;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    ::WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

std::string findUsbip() {
    const char* candidates[] = {
        "C:\\Program Files\\USBip\\usbip.exe",
        "C:\\Program Files (x86)\\USBip\\usbip.exe",
    };
    for (const char* c : candidates) {
        if (::GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return {};
}

// Windows names USB audio endpoints "<terminal type> (<product>)" and a device
// cannot override it, so our channels all show up called "Speakers". Setting
// the endpoint's own friendly name is the only thing that sticks, and it needs
// administrator rights -- but only once, because the name persists.
int fixNames() {
    auto widen = [](const std::string& in) {
        if (in.empty()) return std::wstring{};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), wchar_t{});
        ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), w.data(), n);
        return w;
    };

    std::vector<RenderDevice> all = listRenderDevices();
    const std::vector<RenderDevice> caps = listCaptureDevices();
    all.insert(all.end(), caps.begin(), caps.end());

    int fixed = 0, failed = 0, already = 0;
    for (const auto& d : all) {
        const size_t at = d.name.find("Openmix - ");
        if (at == std::string::npos) continue;

        std::string want = d.name.substr(at);
        while (!want.empty() && (want.back() == ')' || want.back() == ' ')) want.pop_back();
        // "Openmix - Mic (Openmix - Mic)" collapses to the first occurrence.
        const size_t dup = want.find(" (");
        if (dup != std::string::npos) want = want.substr(0, dup);

        if (d.name == want) { ++already; continue; }

        if (renameEndpoint(d.id, widen(want))) {
            std::printf("  %-34s -> %s\n", d.name.c_str(), want.c_str());
            ++fixed;
        } else {
            std::printf("  %-34s FAILED (run as administrator)\n", d.name.c_str());
            ++failed;
        }
    }

    if (!fixed && !failed && !already) {
        std::printf("No openmix endpoints found. Start openmix first, then run this.\n");
        return 1;
    }
    std::printf("\n%d renamed, %d already correct, %d failed\n", fixed, already, failed);
    return failed ? 1 : 0;
}

bool busClaimsPid(const Bus& bus, DWORD pid, const std::vector<ProcEntry>& procs) {
    DWORD cur = pid;
    for (int depth = 0; depth < 12 && cur != 0; ++depth) {
        auto it = std::find_if(procs.begin(), procs.end(),
                               [&](const ProcEntry& p) { return p.pid == cur; });
        if (it == procs.end()) return false;
        for (const auto& n : bus.matchNames) {
            if (it->name == n) return true;
        }
        cur = it->ppid;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<Bus> buses;
    bool verbose = false;
    bool loopbackMode = false;
    bool listDevices = false;
    bool fixNames_ = false;
    bool selfTest = false;
    std::string selfTestChannel = "Game";
    bool noMic = false;
    std::string outMatch;
    std::string micMatch;
    uint16_t port = 3240;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help")  { usage(); return 0; }
        if (a == "-v" || a == "--verbose") { verbose = true; continue; }
        if (a == "--loopback")  { loopbackMode = true; continue; }
        if (a == "--endpoints") { loopbackMode = false; continue; }
        if (a == "--list-devices") { listDevices = true; continue; }
        if (a == "--fix-names") { fixNames_ = true; continue; }
        if (a == "--selftest") {
            selfTest = true;
            if (i + 1 < argc && argv[i + 1][0] != 0x2D) selfTestChannel = argv[++i];
            continue;
        }
        if (a == "--out" && i + 1 < argc) { outMatch = argv[++i]; continue; }
        if (a == "--mic" && i + 1 < argc) { micMatch = argv[++i]; continue; }
        if (a == "--no-mic") { noMic = true; continue; }
        if (a == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
            continue;
        }
        if (a == "--bus" && i + 1 < argc) {
            const std::string spec = argv[++i];
            Bus b;
            const size_t eq = spec.find('=');
            if (eq == std::string::npos) {
                b.name = spec;
            } else {
                b.name = spec.substr(0, eq);
                const std::string rhs = spec.substr(eq + 1);
                if (rhs == "*") {
                    b.isRest = true;
                } else {
                    for (const auto& n : splitCommas(rhs)) b.matchNames.push_back(widen(n));
                }
            }
            buses.push_back(std::move(b));
            continue;
        }
        std::printf("unknown argument '%s'\n\n", a.c_str());
        usage();
        return 1;
    }

    if (buses.empty()) {
        if (loopbackMode) {
            Bus chat;  chat.name  = "Chat";  chat.matchNames.push_back(L"discord.exe");
            Bus media; media.name = "Media"; media.matchNames.push_back(L"spotify.exe");
            Bus game;  game.name  = "Game";  game.isRest = true;
            buses.push_back(std::move(chat));
            buses.push_back(std::move(media));
            buses.push_back(std::move(game));
        } else {
            for (const char* n : {"Game", "Chat", "Media"}) {
                Bus b;
                b.name = n;
                buses.push_back(std::move(b));
            }
            if (!noMic) {
                Bus m;
                m.name = "Mic";
                m.isCapture = true;
                buses.push_back(std::move(m));
            }
        }
    }

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::timeBeginPeriod(1);   // the USB pacer sleeps in ~1 ms steps

    if (selfTest) {
        const int rc = runSelfTest(selfTestChannel, 5);
        ::CoUninitialize();
        return rc;
    }

    if (fixNames_) {
        const int rc = fixNames();
        ::CoUninitialize();
        return rc;
    }

    if (listDevices) {
        std::printf("Output devices:\n");
        for (const auto& d : listRenderDevices()) {
            std::printf("  %-46s %s\n", d.name.c_str(),
                        d.isOpenmix ? "(openmix - not selectable)" : "");
        }
        ::CoUninitialize();
        return 0;
    }

    for (auto& b : buses) {
        b.ring.reset(kSampleRate / 2, kChannels);
        if (!b.isCapture) b.stream.reset(kSampleRate / 4, kChannels);
    }

    MonitorOutput out;
    std::string err;
    if (!out.start(&buses, outMatch, err)) {
        std::printf("monitor output failed: %s\n", err.c_str());
        return 1;
    }

    std::vector<std::unique_ptr<VirtualEndpoint>> endpoints;
    UsbipServer usbip;

    if (!loopbackMode) {
        for (size_t i = 0; i < buses.size(); ++i) {
            auto ep = std::make_unique<VirtualEndpoint>();
            const bool cap = buses[i].isCapture;
            ep->device = std::make_unique<usbaudio::Device>(
                "Openmix - " + buses[i].name, buses[i].name,
                cap ? usbaudio::Direction::Capture : usbaudio::Direction::Duplex);
            if (cap) {
                ep->source = &buses[i].ring;
            } else {
                ep->sink        = &buses[i].ring;
                ep->streamTap   = &buses[i].stream;
                ep->source      = &buses[i].stream;
                ep->streamGain  = &buses[i].streamGain;
                ep->streamMuted = &buses[i].streamMuted;
            }
            ep->busid = "1-" + std::to_string(i + 1);
            endpoints.push_back(std::move(ep));
        }
        usbip.verbose = verbose;
        if (!usbip.start(&endpoints, port, err)) {
            std::printf("usbip server failed: %s\n", err.c_str());
            return 1;
        }
    }

    std::printf("openmix  -  %u Hz, %u ch, monitor buffer %.1f ms\n",
                kSampleRate, kChannels, out.bufferMs());
    std::printf("monitor -> %s%s\n", out.deviceName().c_str(),
                out.fellBack() ? "  (default was an openmix device; picked a real one)" : "");

    if (loopbackMode) {
        std::printf("mode: loopback (no devices created)\nbuses: ");
        for (size_t i = 0; i < buses.size(); ++i) {
            std::printf("%s%s%s", i ? ", " : "", buses[i].name.c_str(),
                        buses[i].isRest ? " (catch-all)" : "");
        }
        std::printf("\n");
    } else {
        std::printf("mode: USB/IP endpoints, listening on 127.0.0.1:%u\n", port);

        const std::string usbipExe = findUsbip();
        if (usbipExe.empty()) {
            std::printf("\n  usbip.exe not found - install usbip-win2, then attach manually:\n");
            for (const auto& ep : endpoints) {
                std::printf("    usbip attach -r 127.0.0.1 -b %s   -> \"%s\"\n",
                            ep->busid.c_str(), ep->device->productName().c_str());
            }
        } else {
            std::printf("\nplugging in devices:\n");
            for (const auto& ep : endpoints) {
                const std::string cmd = "\"" + usbipExe + "\" attach -r 127.0.0.1 -b " + ep->busid;
                const int rc = runQuiet(cmd);
                std::printf("  %-16s %s\n", ep->device->productName().c_str(),
                            rc == 0 ? "ok" : "FAILED (already attached? run 'usbip port')");
            }
            std::printf("\nThey appear in Sound settings as \"Speakers (openmix ...)\".\n"
                        "Point each app at one, and set your real headphones as the\n"
                        "default device so the monitor mix lands there.\n"
                        "Devices unplug automatically when openmix exits.\n");
        }
    }

    MicCapture mic;
    if (!loopbackMode && !noMic) {
        for (auto& b : buses) {
            if (!b.isCapture) continue;
            std::string micErr;
            if (mic.start(&b.ring, micMatch, micErr)) {
                std::printf("mic <- %s\n", mic.deviceName().c_str());
            } else {
                std::printf("mic capture unavailable: %s\n", micErr.c_str());
            }
            break;
        }
    }

    std::printf("\nKeys: 1-9 select | +/- gain | m mute | q quit\n\n");

    size_t selected = 0;
    int ticks = 0;
    std::vector<unsigned long long> lastFrames(endpoints.size(), 0);
    std::vector<double> rates(endpoints.size(), 0.0);
    LARGE_INTEGER qpf{}, lastSample{};
    ::QueryPerformanceFrequency(&qpf);
    ::QueryPerformanceCounter(&lastSample);
    bool running = true;

    while (running) {
        if (loopbackMode && ticks % 10 == 0) {
            const auto procs = enumProcesses();
            const auto sessionPids = enumAudioSessionPids();

            for (auto& bus : buses) {
                std::vector<DWORD> wanted;
                if (bus.isRest) {
                    for (DWORD pid : sessionPids) {
                        if (pid == ::GetCurrentProcessId()) continue;
                        bool takenByName = false;
                        for (const auto& other : buses) {
                            if (other.matchNames.empty()) continue;
                            if (busClaimsPid(other, pid, procs)) { takenByName = true; break; }
                        }
                        if (!takenByName) wanted.push_back(pid);
                    }
                } else {
                    for (const auto& name : bus.matchNames) {
                        for (DWORD pid : findRootPids(procs, name)) wanted.push_back(pid);
                    }
                }

                bus.captures.erase(
                    std::remove_if(bus.captures.begin(), bus.captures.end(),
                                   [&](const std::unique_ptr<ProcessLoopbackCapture>& c) {
                                       return std::find(wanted.begin(), wanted.end(), c->pid())
                                              == wanted.end();
                                   }),
                    bus.captures.end());

                for (DWORD pid : wanted) {
                    const bool already = std::any_of(
                        bus.captures.begin(), bus.captures.end(),
                        [&](const std::unique_ptr<ProcessLoopbackCapture>& c) {
                            return c->pid() == pid;
                        });
                    if (already) continue;

                    auto cap = std::make_unique<ProcessLoopbackCapture>();
                    std::string capErr;
                    if (cap->start(pid, true, &bus.ring, capErr)) {
                        if (verbose) {
                            std::printf("\n  + %s <- pid %lu\n", bus.name.c_str(),
                                        static_cast<unsigned long>(pid));
                        }
                        bus.captures.push_back(std::move(cap));
                    } else if (verbose) {
                        std::printf("\n  ! %s pid %lu failed: %s\n", bus.name.c_str(),
                                    static_cast<unsigned long>(pid), capErr.c_str());
                    }
                }
            }
        }

        if (!loopbackMode && ticks % 10 == 0 && ticks > 0) {
            // Measure against the performance counter, not the tick
            // count: the loop sleeps a little over 100 ms per tick, so
            // a tick-based window reports ~1% high and looks like drift.
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            const double secs = static_cast<double>(now.QuadPart - lastSample.QuadPart) /
                                static_cast<double>(qpf.QuadPart);
            lastSample = now;
            for (size_t i = 0; i < endpoints.size(); ++i) {
                const unsigned long long f = endpoints[i]->framesIn.load();
                rates[i] = secs > 0.0 ? static_cast<double>(f - lastFrames[i]) / secs : 0.0;
                lastFrames[i] = f;
            }
        }

        std::printf("\r");
        for (size_t i = 0; i < buses.size(); ++i) {
            auto& b = buses[i];
            const float peak = b.ring.takePeak();
            const int bars = static_cast<int>(std::min(1.0f, peak) * 10.0f);
            char meter[11];
            for (int k = 0; k < 10; ++k) meter[k] = (k < bars) ? '=' : '.';
            meter[10] = '\0';

            const char* state = "";
            if (!loopbackMode && i < endpoints.size()) {
                state = endpoints[i]->attached.load() ? "" : "*";
            }

            char rateBuf[20] = "";
            if (!loopbackMode && i < endpoints.size() && rates[i] > 0.0) {
                std::snprintf(rateBuf, sizeof(rateBuf), " %.1fk", rates[i] / 1000.0);
            }

            std::printf("%c%s%s%c %s %+.0fdB%s%s  ",
                        (i == selected) ? '[' : ' ',
                        b.name.c_str(), state,
                        (i == selected) ? ']' : ' ',
                        meter,
                        20.0 * std::log10(b.gain > 0.0001f ? b.gain : 0.0001f),
                        b.muted ? " MUTE" : "", rateBuf);
        }
        std::fflush(stdout);

        while (::_kbhit()) {
            const int c = ::_getch();
            if (c == 'q' || c == 'Q') { running = false; break; }
            if (c >= '1' && c <= '9') {
                const size_t idx = static_cast<size_t>(c - '1');
                if (idx < buses.size()) selected = idx;
            }
            if (c == '+' || c == '=') buses[selected].gain = std::min(4.0f, buses[selected].gain * 1.122f);
            if (c == '-' || c == '_') buses[selected].gain = std::max(0.0f, buses[selected].gain / 1.122f);
            if (c == 'm' || c == 'M') buses[selected].muted = !buses[selected].muted;
        }

        ::Sleep(100);
        ++ticks;
    }

    std::printf("\nstopping...\n");
    ::timeEndPeriod(1);
    mic.stop();
    usbip.stop();
    for (auto& b : buses) b.captures.clear();
    out.stop();
    ::CoUninitialize();
    return 0;
}
