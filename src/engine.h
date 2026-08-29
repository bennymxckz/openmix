#pragma once
// The audio engine, independent of any user interface. Owns the buses, the
// virtual USB devices, the USB/IP server, the monitor output and the
// microphone capture, so both the console and GUI front ends drive the same
// object rather than duplicating setup.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "usbip_server.h"

struct EngineConfig {
    std::vector<std::string> playbackBuses{"Game", "Chat", "Media"};
    bool enableMic = true;
    std::string micBusName = "Mic";
    std::string outMatch;    // substring of the monitor output device name
    std::string micMatch;    // substring of the microphone source device name
    uint16_t port = 3240;
    bool autoAttach = true;
};

class Engine {
public:
    ~Engine();

    bool start(const EngineConfig& cfg, std::string& err);
    void stop();
    bool running() const { return running_; }
    // How open the microphone is right now, 0..1. What the ducking
    // reads, so it is worth showing next to the controls for it.
    float micActivity() const { return micActivity_.load(std::memory_order_relaxed); }

    std::vector<Bus>& buses() { return buses_; }
    const std::vector<std::unique_ptr<VirtualEndpoint>>& endpoints() const { return endpoints_; }

    const std::string& monitorDevice() const;
    const std::string& micDevice() const { return micDeviceName_; }
    const std::string& micError() const { return micError_; }
    // False when Windows refused the endpoint rename, which needs admin.
    bool namesApplied() const { return renamedOk_; }
    // True when the usbip transport is missing, which is the one prerequisite
    // openmix cannot supply for itself.
    bool usbipMissing() const { return usbipMissing_; }
    // True when devices were attached but Windows never enumerated them.
    bool devicesMissing() const;
    double monitorBufferMs() const;
    // Route one channel to its own device, or to the primary when empty.
    bool setChannelDevice(size_t busIndex, const std::string& deviceName, std::string& err);
    // Reopen the monitor outputs to match whatever the buses now say. For
    // callers that have set several channels at once and want one restart
    // rather than one per channel.
    bool rerouteAll(std::string& err);
    const EngineConfig& config() const { return cfg_; }

    // Swap the monitor output or microphone source while running. Only that
    // one stream restarts -- the USB devices stay attached, so applications
    // pointed at them never notice.
    bool setOutputDevice(const std::string& match, std::string& err);
    bool setMicDevice(const std::string& match, std::string& err);

    // Per-bus packet rate in frames/sec, sampled against the performance
    // counter. Call about once a second from the UI thread.
    void sampleRates();
    double rate(size_t busIndex) const;

    // Names as Windows shows them, for display and troubleshooting.
    static std::string usbipPath();

private:
    void attachAll();
    bool rebuildOutputs(std::string& err);
    void renameEndpoints();

    EngineConfig cfg_;
    std::vector<Bus> buses_;
    std::vector<std::unique_ptr<VirtualEndpoint>> endpoints_;
    UsbipServer usbip_;
    // One per distinct output device in use; index 0 is the primary, which
    // carries every channel that has not been routed elsewhere.
    std::vector<std::unique_ptr<MonitorOutput>> outputs_;
    MicCapture mic_;
    // How open the microphone is, 0..1. Written by the capture thread and
    // read by every channel that ducks under it.
    std::atomic<float> micActivity_{0.0f};
    std::string micDeviceName_;
    bool renamedOk_ = true;
    bool usbipMissing_ = false;
    std::string micError_;
    bool running_ = false;

    std::vector<unsigned long long> lastFrames_;
    std::vector<double> rates_;
    long long lastSample_ = 0;
    long long qpf_ = 1;
};
