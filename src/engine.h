#pragma once
// The audio engine, independent of any user interface. Owns the buses, the
// virtual USB devices, the USB/IP server, the monitor output and the
// microphone capture, so both the console and GUI front ends drive the same
// object rather than duplicating setup.

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

    std::vector<Bus>& buses() { return buses_; }
    const std::vector<std::unique_ptr<VirtualEndpoint>>& endpoints() const { return endpoints_; }

    const std::string& monitorDevice() const { return out_.deviceName(); }
    const std::string& micDevice() const { return micDeviceName_; }
    const std::string& micError() const { return micError_; }
    // False when Windows refused the endpoint rename, which needs admin.
    bool namesApplied() const { return renamedOk_; }
    double monitorBufferMs() const { return out_.bufferMs(); }
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
    void renameEndpoints();

    EngineConfig cfg_;
    std::vector<Bus> buses_;
    std::vector<std::unique_ptr<VirtualEndpoint>> endpoints_;
    UsbipServer usbip_;
    MonitorOutput out_;
    MicCapture mic_;
    std::string micDeviceName_;
    bool renamedOk_ = true;
    std::string micError_;
    bool running_ = false;

    std::vector<unsigned long long> lastFrames_;
    std::vector<double> rates_;
    long long lastSample_ = 0;
    long long qpf_ = 1;
};
