#include "engine.h"

#include <windows.h>
#include <algorithm>
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
    for (auto& b : buses_) {
        b.ring.reset(kSampleRate / 2, kChannels);
        b.stream.reset(kSampleRate / 4, kChannels);   // stream send, or mic monitor
        b.strip.prepare(kSampleRate);
        b.micChain.prepare(kSampleRate);
        b.mixChain.prepare(kSampleRate);
    }

    if (!rebuildOutputs(err)) return false;

    for (size_t i = 0; i < buses_.size(); ++i) {
        auto ep = std::make_unique<VirtualEndpoint>();
        const bool cap = buses_[i].isCapture;
        ep->device = std::make_unique<usbaudio::Device>(
            "Openmix - " + buses_[i].name,   // shown to the user
            buses_[i].name,                  // identity, stable across renames
            cap ? usbaudio::Direction::Capture : usbaudio::Direction::Duplex);
        if (cap) {
            ep->source = &buses_[i].ring;
            // The stream fader is how loud applications hear the microphone;
            // the headphone fader is how loud you hear yourself.
            // One fader per channel: the level you set is the level everyone
            // gets, here and on the stream.
            ep->streamGain  = &buses_[i].gain;
            ep->streamMuted = &buses_[i].muted;
        } else {
            // Playback channels are duplex: applications render in, OBS
            // records the same audio back out at its own level.
            ep->sink        = &buses_[i].ring;
            ep->streamTap   = &buses_[i].stream;
            ep->source      = &buses_[i].stream;
            ep->streamGain  = &buses_[i].gain;
            ep->streamMuted = &buses_[i].muted;
            ep->eq          = &buses_[i].eq;
            ep->strip       = &buses_[i].strip;
            ep->mix         = &buses_[i].mix;
            ep->mixChain    = &buses_[i].mixChain;
            ep->activity    = &micActivity_;
        }
        ep->busid = "1-" + std::to_string(i + 1);
        endpoints_.push_back(std::move(ep));
    }

    if (!usbip_.start(&endpoints_, cfg_.port, err)) {
        for (auto& o : outputs_) o->stop();
        outputs_.clear();
        return false;
    }
    if (cfg_.autoAttach) {
        attachAll();
        renameEndpoints();
    }

    if (cfg_.enableMic) {
        for (auto& b : buses_) {
            if (!b.isCapture) continue;
            std::string micErr;
            mic_.setEq(&b.eq, &b.strip);
            mic_.setDynamics(&b.mic, &b.micChain);
            mic_.setActivity(&micActivity_);
            mic_.setMonitor(&b.stream);
            // Self-monitoring starts silent -- monitorGain, not gain: hearing
            // your own voice unasked is startling and on speakers it feeds
            // back. The level applications hear stays where the user put it.
            mic_.setActivity(&micActivity_);
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

bool Engine::devicesMissing() const {
    if (!running_) return false;
    for (const auto& ep : endpoints_) {
        if (!ep->attached.load()) return true;
    }
    return false;
}

void Engine::attachAll() {
    const std::string exe = usbipPath();
    if (exe.empty()) {
        // Without the transport there is nothing to attach to, and the
        // channels would silently never appear.
        usbipMissing_ = true;
        return;
    }
    usbipMissing_ = false;
    for (const auto& ep : endpoints_) {
        runQuiet("\"" + exe + "\" attach -r 127.0.0.1 -b " + ep->busid);
    }
}

void Engine::renameEndpoints() {
    // Windows needs a moment to enumerate the freshly attached devices and
    // build their endpoints before the property store exists.
    ::Sleep(1200);

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

    renamedOk_ = true;
    for (const auto& ep : endpoints_) {
        const std::string want = ep->device->productName();   // "Openmix - Game"
        for (const auto& d : all) {
            // Windows wraps our product string, e.g. "Speakers (Openmix - Game)".
            if (d.name == want) break;                        // already correct
            if (d.name.find(want) == std::string::npos) continue;
            if (!renameEndpoint(d.id, widen(want))) renamedOk_ = false;
            break;
        }
    }
}

namespace {
const std::string kEmpty;
}  // namespace

const std::string& Engine::monitorDevice() const {
    return outputs_.empty() ? kEmpty : outputs_[0]->deviceName();
}

double Engine::monitorBufferMs() const {
    return outputs_.empty() ? 0.0 : outputs_[0]->bufferMs();
}

// One output per distinct device. Channels routed nowhere in particular ride
// on the primary, which is index 0.
bool Engine::rebuildOutputs(std::string& err) {
    for (auto& o : outputs_) o->stop();
    outputs_.clear();

    auto primary = std::make_unique<MonitorOutput>();
    if (!primary->start(&buses_, cfg_.outMatch, true, err)) return false;
    const std::string primaryName = primary->deviceName();
    outputs_.push_back(std::move(primary));

    // Anything a channel asks for that the primary is not already playing.
    std::vector<std::string> extra;
    for (const auto& b : buses_) {
        if (b.outputDevice.empty() || b.outputDevice == primaryName) continue;
        if (std::find(extra.begin(), extra.end(), b.outputDevice) == extra.end()) {
            extra.push_back(b.outputDevice);
        }
    }
    for (const auto& name : extra) {
        auto o = std::make_unique<MonitorOutput>();
        std::string ignored;
        // A channel pointed at a device that has since gone away should not
        // stop the rest of the mixer from starting.
        if (o->start(&buses_, name, false, ignored)) outputs_.push_back(std::move(o));
    }
    return true;
}

bool Engine::setChannelDevice(size_t busIndex, const std::string& deviceName,
                              std::string& err) {
    if (!running_ || busIndex >= buses_.size()) return false;
    buses_[busIndex].outputDevice = deviceName;
    return rebuildOutputs(err);
}

bool Engine::setOutputDevice(const std::string& match, std::string& err) {
    if (!running_) return false;
    cfg_.outMatch = match;
    if (!rebuildOutputs(err)) {
        // Fall back to whatever Windows will give us rather than going silent.
        cfg_.outMatch.clear();
        std::string ignored;
        rebuildOutputs(ignored);
        return false;
    }
    return true;
}

bool Engine::setMicDevice(const std::string& match, std::string& err) {
    if (!running_) return false;
    FloatRing* ring = nullptr;
    for (auto& b : buses_) {
        if (b.isCapture) {
            ring = &b.ring;
            mic_.setEq(&b.eq, &b.strip);
            mic_.setDynamics(&b.mic, &b.micChain);
            mic_.setMonitor(&b.stream);
            break;
        }
    }
    if (!ring) {
        err = "no microphone bus";
        return false;
    }
    mic_.stop();
    cfg_.micMatch = match;
    micDeviceName_.clear();
    micError_.clear();
    if (!mic_.start(ring, cfg_.micMatch, err)) {
        micError_ = err;
        return false;
    }
    micDeviceName_ = mic_.deviceName();
    return true;
}

void Engine::stop() {
    if (!running_) return;
    mic_.stop();
    usbip_.stop();     // dropping the connections unplugs the devices
    for (auto& o : outputs_) o->stop();
    outputs_.clear();
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
