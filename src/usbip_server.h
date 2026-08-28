#pragma once
// A USB/IP server that exports virtual USB Audio Class devices.
//
// usbip-win2's VHCI attaches to this over TCP and Windows' in-box usbaudio.sys
// then binds each one, publishing a normal render endpoint. Audio arrives as
// isochronous OUT packets, so there is no loopback tap and no separate capture
// clock to drift against.

#include <winsock2.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ring.h"
#include "dsp.h"
#include "usb_audio.h"

// One exported device, paired with the bus it feeds.
struct VirtualEndpoint {
    std::unique_ptr<usbaudio::Device> device;
    FloatRing* sink = nullptr;       // playback: where host audio lands
    FloatRing* source = nullptr;     // capture: where we take audio from
    // On a duplex device the same audio is copied here on arrival so the
    // capture side can serve it to OBS at its own level.
    FloatRing* streamTap = nullptr;
    const float* streamGain = nullptr;
    const bool* streamMuted = nullptr;
    // Applied to arriving audio before it is split, so monitor and stream
    // hear the same processed channel.
    dsp::EqParams* eq = nullptr;
    dsp::ChannelStrip* strip = nullptr;
    std::string busid;               // e.g. "1-1"
    std::atomic<bool> attached{false};
    std::atomic<unsigned long long> framesIn{0};
};

class UsbipServer {
public:
    ~UsbipServer();

    bool start(std::vector<std::unique_ptr<VirtualEndpoint>>* endpoints,
               uint16_t port, std::string& err);
    void stop();

    uint16_t port() const { return port_; }
    bool verbose = false;

private:
    void acceptLoop();
    void serveConnection(SOCKET s);
    bool handleStreaming(SOCKET s, VirtualEndpoint& ep);

    static unsigned long __stdcall acceptThunk(void* self);

    std::vector<std::unique_ptr<VirtualEndpoint>>* endpoints_ = nullptr;
    SOCKET listen_ = INVALID_SOCKET;
    void* thread_ = nullptr;
    std::atomic<bool> stopping_{false};
    uint16_t port_ = 3240;

    // Each imported device holds its connection open for as long as it is
    // plugged in, so every connection needs its own thread.
    std::mutex clientsMutex_;
    std::vector<SOCKET> clientSockets_;
    std::vector<std::thread> clientThreads_;
};
