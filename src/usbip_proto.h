#pragma once
// USB/IP wire protocol. Every multi-byte field on the wire is big-endian.
//
// Reference: Linux tools/usb/usbip/USBIP_PROTOCOL.txt

#include <winsock2.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace usbip {

constexpr uint16_t kVersion = 0x0111;

// op codes for the connection-setup phase
constexpr uint16_t OP_REQ_DEVLIST = 0x8005;
constexpr uint16_t OP_REP_DEVLIST = 0x0005;
constexpr uint16_t OP_REQ_IMPORT  = 0x8003;
constexpr uint16_t OP_REP_IMPORT  = 0x0003;

// commands for the streaming phase
constexpr uint32_t CMD_SUBMIT  = 0x00000001;
constexpr uint32_t RET_SUBMIT  = 0x00000003;
constexpr uint32_t CMD_UNLINK  = 0x00000002;
constexpr uint32_t RET_UNLINK  = 0x00000004;

constexpr uint32_t DIR_OUT = 0;
constexpr uint32_t DIR_IN  = 1;

constexpr uint32_t SPEED_FULL = 2;
constexpr uint32_t SPEED_HIGH = 3;

// ---- byte order helpers -------------------------------------------------

inline uint16_t be16(uint16_t v) { return htons(v); }
inline uint32_t be32(uint32_t v) { return htonl(v); }
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}

inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
inline void putBytes(std::vector<uint8_t>& v, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}
inline void putPad(std::vector<uint8_t>& v, size_t n) { v.insert(v.end(), n, 0); }

// ---- streaming-phase header --------------------------------------------
//
// The 48-byte header is basic(20) + command-specific(28). Kept as a plain
// struct in host order; the wire form is parsed and emitted explicitly.

struct Header {
    uint32_t command = 0;
    uint32_t seqnum = 0;
    uint32_t devid = 0;
    uint32_t direction = 0;
    uint32_t ep = 0;

    // CMD_SUBMIT
    uint32_t transferFlags = 0;
    int32_t  transferBufferLength = 0;
    int32_t  startFrame = 0;
    int32_t  numberOfPackets = 0;
    int32_t  interval = 0;
    uint8_t  setup[8] = {};

    // RET_SUBMIT
    int32_t  status = 0;
    int32_t  actualLength = 0;
    int32_t  errorCount = 0;

    // CMD_UNLINK
    uint32_t unlinkSeqnum = 0;

    bool isIso() const { return numberOfPackets > 0 && numberOfPackets != -1; }
};

constexpr size_t kHeaderSize = 48;

inline void parseHeader(const uint8_t* p, Header& h) {
    h.command   = rd32(p + 0);
    h.seqnum    = rd32(p + 4);
    h.devid     = rd32(p + 8);
    h.direction = rd32(p + 12);
    h.ep        = rd32(p + 16);

    if (h.command == CMD_SUBMIT) {
        h.transferFlags        = rd32(p + 20);
        h.transferBufferLength = static_cast<int32_t>(rd32(p + 24));
        h.startFrame           = static_cast<int32_t>(rd32(p + 28));
        h.numberOfPackets      = static_cast<int32_t>(rd32(p + 32));
        h.interval             = static_cast<int32_t>(rd32(p + 36));
        std::memcpy(h.setup, p + 40, 8);
    } else if (h.command == CMD_UNLINK) {
        h.unlinkSeqnum = rd32(p + 20);
    }
}

inline void writeRetSubmit(std::vector<uint8_t>& v, const Header& h) {
    put32(v, RET_SUBMIT);
    put32(v, h.seqnum);
    put32(v, h.devid);
    put32(v, h.direction);
    put32(v, h.ep);
    put32(v, static_cast<uint32_t>(h.status));
    put32(v, static_cast<uint32_t>(h.actualLength));
    put32(v, static_cast<uint32_t>(h.startFrame));
    put32(v, static_cast<uint32_t>(h.numberOfPackets));
    put32(v, static_cast<uint32_t>(h.errorCount));
    putPad(v, 8);
}

inline void writeRetUnlink(std::vector<uint8_t>& v, const Header& h, int32_t status) {
    put32(v, RET_UNLINK);
    put32(v, h.seqnum);
    put32(v, h.devid);
    put32(v, h.direction);
    put32(v, h.ep);
    put32(v, static_cast<uint32_t>(status));
    putPad(v, 24);
}

// ---- isochronous packet descriptor --------------------------------------

struct IsoDesc {
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t actualLength = 0;
    uint32_t status = 0;
};
constexpr size_t kIsoDescSize = 16;

inline void parseIso(const uint8_t* p, IsoDesc& d) {
    d.offset       = rd32(p + 0);
    d.length       = rd32(p + 4);
    d.actualLength = rd32(p + 8);
    d.status       = rd32(p + 12);
}
inline void writeIso(std::vector<uint8_t>& v, const IsoDesc& d) {
    put32(v, d.offset);
    put32(v, d.length);
    put32(v, d.actualLength);
    put32(v, d.status);
}

// ---- exported device description (setup phase) --------------------------

struct ExportedDevice {
    char path[256] = {};
    char busid[32] = {};
    uint32_t busnum = 1;
    uint32_t devnum = 1;
    uint32_t speed = SPEED_FULL;
    uint16_t idVendor = 0;
    uint16_t idProduct = 0;
    uint16_t bcdDevice = 0x0100;
    uint8_t  bDeviceClass = 0;
    uint8_t  bDeviceSubClass = 0;
    uint8_t  bDeviceProtocol = 0;
    uint8_t  bConfigurationValue = 1;
    uint8_t  bNumConfigurations = 1;
    uint8_t  bNumInterfaces = 2;
};

inline void writeExportedDevice(std::vector<uint8_t>& v, const ExportedDevice& d) {
    putBytes(v, d.path, sizeof(d.path));
    putBytes(v, d.busid, sizeof(d.busid));
    put32(v, d.busnum);
    put32(v, d.devnum);
    put32(v, d.speed);
    put16(v, d.idVendor);
    put16(v, d.idProduct);
    put16(v, d.bcdDevice);
    v.push_back(d.bDeviceClass);
    v.push_back(d.bDeviceSubClass);
    v.push_back(d.bDeviceProtocol);
    v.push_back(d.bConfigurationValue);
    v.push_back(d.bNumConfigurations);
    v.push_back(d.bNumInterfaces);
}

}  // namespace usbip
