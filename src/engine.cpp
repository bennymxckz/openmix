#include "engine.h"

#include <windows.h>
#include <string>

namespace {

// Run a command with no console window and wait for it.
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

}  // namespace

Engine::~Engine() { stop(); }

std::string Engine::usbipPath() {
    const char* candidates[] = {
        "C:\\Program Files\\USBip\\usbip.exe",
        "C:\\Program Files (x86)\\USBip\\usbip.exe",
    };
    for (const char* c : candidates) {
        if (::GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return {};
}

bool Engine::start(const EngineConfig& cfg, std::string& err) {
    if (running_) return true;
    cfg_ = cfg;

    buses_.clear();
    endpoints_.clear();
    micDeviceName_.clear();
    micError_.clear();

    buses_.reserve(cfg_.playbackBuses.size() + 1);
    for (const auto& name : cfg_.playbackBuses) {
        Bus b;
        b.name = name;
        buses_.push_back(std::move(b));
    }
    if (cfg_.enableMic) {
        Bus m;
        m.name = cfg_.micBusName;
        m.isCapture = true;
        buses_.push_back(std::move(m));
    }
    for (auto& b : buses_) b.ring.reset(kSampleRate / 2, kChannels);

    if (!out_.start(&buses_, cfg_.outMatch, err)) return false;

    for (size_t i = 0; i < buses_.size(); ++i) {
        auto ep = std::make_unique<VirtualEndpoint>();
        const bool cap = buses_[i].isCapture;
        const std::string devName = "openmix " + buses_[i].name;
        ep->device = std::make_unique<usbaudio::Device>(
            devName, usbaudio::Device::stableProductId(devName),
            cap ? usbaudio::Direction::Capture : usbaudio::Direction::Playback);
        if (cap) {
            ep->source = &buses_[i].ring;
        } else {
            ep->sink = &buses_[i].ring;
        }
        ep->busid = "1-" + std::to_string(i + 1);
        endpoints_.push_back(std::move(ep));
    }

    if (!usbip_.start(&endpoints_, cfg_.port, err)) {
        out_.stop();
        return false;
    }
    if (cfg_.autoAttach) attachAll();

    if (cfg_.enableMic) {
        for (auto& b : buses_) {
            if (!b.isCapture) continue;
            std::string micErr;
            if (mic_.start(&b.ring, cfg_.micMatch, micErr)) {
                micDeviceName_ = mic_.deviceName();
            } else {
                micError_ = micErr;
            }
            break;
        }
    }

    lastFrames_.assign(endpoints_.size(), 0);
    rates_.assign(endpoints_.size(), 0.0);
    LARGE_INTEGER f{}, n{};
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&n);
    qpf_ = f.QuadPart ? f.QuadPart : 1;
    lastSample_ = n.QuadPart;

    running_ = true;
    return true;
}

void Engine::attachAll() {
    const std::string exe = usbipPath();
    if (exe.empty()) return;
    for (const auto& ep : endpoints_) {
        runQuiet("\"" + exe + "\" attach -r 127.0.0.1 -b " + ep->busid);
    }
}

void Engine::stop() {
    if (!running_) return;
    mic_.stop();
    usbip_.stop();     // dropping the connections unplugs the devices
    out_.stop();
    endpoints_.clear();
    buses_.clear();
    running_ = false;
}

void Engine::sampleRates() {
    if (!running_ || endpoints_.empty()) return;
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    const double secs = static_cast<double>(now.QuadPart - lastSample_) / static_cast<double>(qpf_);
    if (secs <= 0.0) return;
    lastSample_ = now.QuadPart;
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        const unsigned long long f = endpoints_[i]->framesIn.load();
        rates_[i] = static_cast<double>(f - lastFrames_[i]) / secs;
        lastFrames_[i] = f;
    }
}

double Engine::rate(size_t busIndex) const {
    return busIndex < rates_.size() ? rates_[busIndex] : 0.0;
}
