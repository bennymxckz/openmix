#include "usbip_server.h"
#include "usbip_proto.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

using namespace usbip;

namespace {

bool readExact(SOCKET s, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const int r = ::recv(s, reinterpret_cast<char*>(p + got), static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool writeAll(SOCKET s, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < n) {
        const int r = ::send(s, reinterpret_cast<const char*>(p + sent),
                             static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool writeVec(SOCKET s, const std::vector<uint8_t>& v) {
    return v.empty() ? true : writeAll(s, v.data(), v.size());
}

void fillExported(const VirtualEndpoint& ep, ExportedDevice& d) {
    std::snprintf(d.path, sizeof(d.path), "/openmix/%s", ep.busid.c_str());
    std::snprintf(d.busid, sizeof(d.busid), "%s", ep.busid.c_str());
    d.busnum = 1;
    // "1-3" -> devnum 3, so each export gets a distinct address.
    const char* dash = std::strchr(ep.busid.c_str(), '-');
    d.devnum = dash ? static_cast<uint32_t>(std::atoi(dash + 1)) : 1;
    d.speed = SPEED_FULL;
    d.idVendor = 0x1D6B;
    d.idProduct = ep.device->productId();
    d.bcdDevice = 0x0100;
    d.bDeviceClass = 0;
    d.bDeviceSubClass = 0;
    d.bDeviceProtocol = 0;
    d.bConfigurationValue = 1;
    d.bNumConfigurations = 1;
    d.bNumInterfaces = ep.device->isDuplex() ? 3 : 2;
}

// The two interfaces we expose, in the order the config descriptor lists them.
// Identical for playback and capture: the direction lives in the endpoint.
void writeInterfaces(std::vector<uint8_t>& v, bool duplex) {
    // AudioControl
    v.push_back(0x01); v.push_back(0x01); v.push_back(0x00); v.push_back(0x00);
    // AudioStreaming, one per direction
    v.push_back(0x01); v.push_back(0x02); v.push_back(0x00); v.push_back(0x00);
    if (duplex) {
        v.push_back(0x01); v.push_back(0x02); v.push_back(0x00); v.push_back(0x00);
    }
}

}  // namespace

UsbipServer::~UsbipServer() { stop(); }

unsigned long __stdcall UsbipServer::acceptThunk(void* self) {
    static_cast<UsbipServer*>(self)->acceptLoop();
    return 0;
}

bool UsbipServer::start(std::vector<std::unique_ptr<VirtualEndpoint>>* endpoints,
                        uint16_t port, std::string& err) {
    endpoints_ = endpoints;
    port_ = port;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup failed";
        return false;
    }

    listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) {
        err = "socket() failed";
        return false;
    }

    BOOL yes = TRUE;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port_);
    ::InetPtonW(AF_INET, L"127.0.0.1", &addr.sin_addr);

    if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        err = "bind to 127.0.0.1:" + std::to_string(port_) +
              " failed (" + std::to_string(::WSAGetLastError()) + ")";
        ::closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    if (::listen(listen_, 8) == SOCKET_ERROR) {
        err = "listen() failed";
        ::closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }

    thread_ = ::CreateThread(nullptr, 0, &UsbipServer::acceptThunk, this, 0, nullptr);
    if (!thread_) {
        err = "CreateThread failed";
        return false;
    }
    return true;
}

void UsbipServer::stop() {
    stopping_.store(true);
    if (listen_ != INVALID_SOCKET) {
        ::closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    // Client threads are parked in recv(); closing their sockets unblocks them.
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (SOCKET s : clientSockets_) ::closesocket(s);
        clientSockets_.clear();
    }
    if (thread_) {
        ::WaitForSingleObject(thread_, 2000);
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }
    for (auto& t : clientThreads_) {
        if (t.joinable()) t.join();
    }
    clientThreads_.clear();
}

void UsbipServer::acceptLoop() {
    while (!stopping_.load()) {
        sockaddr_in peer{};
        int len = sizeof(peer);
        SOCKET s = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (s == INVALID_SOCKET) break;

        BOOL nodelay = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // An imported device keeps its connection open for its whole lifetime,
        // so serving inline would stop us ever accepting the next device.
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clientSockets_.push_back(s);
            clientThreads_.emplace_back([this, s]() {
                serveConnection(s);
                {
                    std::lock_guard<std::mutex> l(clientsMutex_);
                    auto it = std::find(clientSockets_.begin(), clientSockets_.end(), s);
                    if (it != clientSockets_.end()) {
                        ::closesocket(s);
                        clientSockets_.erase(it);
                    }
                }
            });
        }
    }
}

void UsbipServer::serveConnection(SOCKET s) {
    uint8_t op[8];
    if (!readExact(s, op, sizeof(op))) return;

    const uint16_t version = rd16(op + 0);
    const uint16_t code    = rd16(op + 2);
    (void)version;

    if (code == OP_REQ_DEVLIST) {
        std::vector<uint8_t> out;
        put16(out, kVersion);
        put16(out, OP_REP_DEVLIST);
        put32(out, 0);                                   // status OK
        put32(out, static_cast<uint32_t>(endpoints_->size()));
        for (const auto& ep : *endpoints_) {
            ExportedDevice d{};
            fillExported(*ep, d);
            writeExportedDevice(out, d);
            writeInterfaces(out, ep->device->isDuplex());
        }
        writeVec(s, out);
        if (verbose) std::printf("\n  usbip: devlist -> %zu device(s)\n", endpoints_->size());
        return;
    }

    if (code != OP_REQ_IMPORT) return;

    char busid[32] = {};
    if (!readExact(s, busid, sizeof(busid))) return;

    VirtualEndpoint* found = nullptr;
    for (const auto& ep : *endpoints_) {
        if (ep->busid == busid) { found = ep.get(); break; }
    }

    std::vector<uint8_t> reply;
    put16(reply, kVersion);
    put16(reply, OP_REP_IMPORT);
    if (!found) {
        put32(reply, 1);                                 // status: error
        writeVec(s, reply);
        return;
    }
    put32(reply, 0);
    ExportedDevice d{};
    fillExported(*found, d);
    writeExportedDevice(reply, d);
    if (!writeVec(s, reply)) return;

    if (verbose) std::printf("\n  usbip: imported %s (%s)\n",
                             found->busid.c_str(), found->device->productName().c_str());
    found->attached.store(true);
    handleStreaming(s, *found);
    found->attached.store(false);
    if (verbose) std::printf("\n  usbip: detached %s\n", found->busid.c_str());
}

bool UsbipServer::handleStreaming(SOCKET s, VirtualEndpoint& ep) {
    std::vector<uint8_t> data;
    std::vector<IsoDesc> iso;
    std::vector<uint8_t> reply;
    std::vector<float> scratch;

    // Real hardware consumes one isochronous packet per USB frame, and that
    // pacing IS the audio clock the host synchronises to. Completing URBs the
    // instant they arrive tells usbaudio.sys the device drains infinitely
    // fast, so the application empties its render buffer in one burst and then
    // starves. Hold each completion until the audio it carries would actually
    // have been played out.
    LARGE_INTEGER qpf{};
    ::QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER base{};
    bool clockRunning = false;
    unsigned long long framesAccepted = 0;

    // Complete slightly early so the host always has URBs in flight.
    constexpr double kLeadSeconds = 0.004;
    constexpr double kResyncSeconds = 0.25;

    for (;;) {
        uint8_t raw[kHeaderSize];
        if (!readExact(s, raw, sizeof(raw))) return false;

        Header h{};
        parseHeader(raw, h);

        if (h.command == CMD_UNLINK) {
            reply.clear();
            writeRetUnlink(reply, h, -104 /* ECONNRESET: URB was cancelled */);
            if (!writeVec(s, reply)) return false;
            continue;
        }
        if (h.command != CMD_SUBMIT) return false;

        const bool isIso = h.isIso();
        const size_t bufLen = h.transferBufferLength > 0
                                  ? static_cast<size_t>(h.transferBufferLength) : 0;

        // OUT transfers carry their payload immediately after the header.
        data.clear();
        if (h.direction == DIR_OUT && bufLen) {
            data.resize(bufLen);
            if (!readExact(s, data.data(), bufLen)) return false;
        }

        // Isochronous transfers append one descriptor per packet.
        iso.clear();
        if (isIso) {
            iso.resize(static_cast<size_t>(h.numberOfPackets));
            std::vector<uint8_t> rawIso(iso.size() * kIsoDescSize);
            if (!readExact(s, rawIso.data(), rawIso.size())) return false;
            for (size_t i = 0; i < iso.size(); ++i) parseIso(&rawIso[i * kIsoDescSize], iso[i]);
        }

        Header r = h;
        r.status = 0;
        r.errorCount = 0;
        std::vector<uint8_t> in;

        if (h.ep == 0) {
            // Control transfer on the default endpoint.
            usbaudio::SetupPacket sp{};
            sp.bmRequestType = h.setup[0];
            sp.bRequest      = h.setup[1];
            sp.wValue        = static_cast<uint16_t>(h.setup[2] | (h.setup[3] << 8));
            sp.wIndex        = static_cast<uint16_t>(h.setup[4] | (h.setup[5] << 8));
            sp.wLength       = static_cast<uint16_t>(h.setup[6] | (h.setup[7] << 8));

            const bool ok = ep.device->handleControl(
                sp, data.empty() ? nullptr : data.data(), data.size(), in);
            if (!ok) {
                r.status = -32;          // EPIPE -> STALL
                r.actualLength = 0;
                in.clear();
            } else {
                r.actualLength = sp.deviceToHost()
                                     ? static_cast<int32_t>(in.size())
                                     : static_cast<int32_t>(data.size());
            }
        } else if (h.direction == DIR_OUT) {
            // Audio: 16-bit little-endian stereo at 48 kHz.
            size_t consumed = 0;
            if (isIso) {
                for (const auto& d : iso) {
                    if (d.offset + d.length > data.size()) continue;
                    consumed += d.length;
                }
            } else {
                consumed = data.size();
            }

            const size_t samples = data.size() / 2;
            if (samples) {
                if (scratch.size() < samples) scratch.resize(samples);
                const int16_t* pcm = reinterpret_cast<const int16_t*>(data.data());
                const float g = ep.device->linearGain();
                for (size_t i = 0; i < samples; ++i) {
                    scratch[i] = (static_cast<float>(pcm[i]) / 32768.0f) * g;
                }
                if (ep.sink) ep.sink->write(scratch.data(), samples);
                // Duplex: the same audio goes to the capture side, which
                // applies its own level, so monitor and stream are independent.
                if (ep.streamTap) ep.streamTap->write(scratch.data(), samples);

                const unsigned long long frames = samples / usbaudio::kChannels;
                ep.framesIn.fetch_add(frames, std::memory_order_relaxed);

                // Pace this completion to the device's virtual playout clock.
                framesAccepted += frames;
                LARGE_INTEGER now{};
                ::QueryPerformanceCounter(&now);
                if (!clockRunning) {
                    base = now;
                    clockRunning = true;
                }
                const double elapsed =
                    static_cast<double>(now.QuadPart - base.QuadPart) / static_cast<double>(qpf.QuadPart);
                const double due =
                    static_cast<double>(framesAccepted) / usbaudio::kSampleRate - kLeadSeconds;
                const double wait = due - elapsed;

                if (wait > 0.0) {
                    ::Sleep(static_cast<DWORD>(wait * 1000.0));
                } else if (wait < -kResyncSeconds) {
                    // The stream paused (app stopped, or alt-setting toggled).
                    // Restart the clock rather than sprinting to catch up.
                    base = now;
                    framesAccepted = 0;
                }
            }

            r.actualLength = static_cast<int32_t>(consumed ? consumed : data.size());
            for (auto& d : iso) {
                d.actualLength = d.length;
                d.status = 0;
            }
        } else if (ep.source) {
            // Isochronous IN: the host is asking for microphone audio. Always
            // return full-length packets -- a short packet is read as a glitch,
            // and the ring zero-fills any shortfall for us.
            const size_t bytes = bufLen;
            const size_t samples = bytes / 2;
            in.resize(bytes);
            if (samples) {
                if (scratch.size() < samples) scratch.resize(samples);

                // Bound the backlog before reading. The microphone's clock and
                // our pacing clock are independent, so without this any drift
                // accumulates in one direction until the ring is full and stays
                // pinned there -- heard as speech arriving hundreds of ms late.
                // Leave one target's worth behind to absorb jitter.
                constexpr size_t kTargetMs = 12;
                const size_t target =
                    (usbaudio::kSampleRate * usbaudio::kChannels * kTargetMs) / 1000;
                ep.source->trimTo(target + samples);

                ep.source->read(scratch.data(), samples);
                float g = ep.device->linearGain(true);
                if (ep.streamGain) g *= *ep.streamGain;
                if (ep.streamMuted && *ep.streamMuted) g = 0.0f;
                int16_t* pcm = reinterpret_cast<int16_t*>(in.data());
                for (size_t i = 0; i < samples; ++i) {
                    float v = scratch[i] * g;
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    pcm[i] = static_cast<int16_t>(v * 32767.0f);
                }
                ep.framesIn.fetch_add(samples / usbaudio::kChannels, std::memory_order_relaxed);
            }

            for (auto& d : iso) {
                d.actualLength = d.length;
                d.status = 0;
            }
            r.actualLength = static_cast<int32_t>(bytes);

            // Same virtual playout clock as the OUT path: deliver at real time
            // or the host will drain us as fast as the socket allows.
            if (samples) {
                const unsigned long long frames = samples / usbaudio::kChannels;
                framesAccepted += frames;
                LARGE_INTEGER now{};
                ::QueryPerformanceCounter(&now);
                if (!clockRunning) { base = now; clockRunning = true; }
                const double elapsed =
                    static_cast<double>(now.QuadPart - base.QuadPart) / static_cast<double>(qpf.QuadPart);
                const double due =
                    static_cast<double>(framesAccepted) / usbaudio::kSampleRate - kLeadSeconds;
                const double wait = due - elapsed;
                if (wait > 0.0) {
                    ::Sleep(static_cast<DWORD>(wait * 1000.0));
                } else if (wait < -kResyncSeconds) {
                    base = now;
                    framesAccepted = 0;
                }
            }
        } else {
            // No source attached; report a well-formed empty transfer.
            r.actualLength = 0;
        }

        reply.clear();
        r.numberOfPackets = isIso ? h.numberOfPackets : -1;
        writeRetSubmit(reply, r);
        if (h.direction == DIR_IN && !in.empty()) {
            reply.insert(reply.end(), in.begin(), in.end());
        }
        if (isIso) {
            for (const auto& d : iso) writeIso(reply, d);
        }
        if (!writeVec(s, reply)) return false;
    }
}
