#include "usb_audio.h"

#include <cmath>
#include <cstring>

namespace usbaudio {
namespace {

void u8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void le24(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
}

// Descriptor types
constexpr uint8_t DT_DEVICE       = 0x01;
constexpr uint8_t DT_CONFIG       = 0x02;
constexpr uint8_t DT_STRING       = 0x03;
constexpr uint8_t DT_INTERFACE    = 0x04;
constexpr uint8_t DT_ENDPOINT     = 0x05;
constexpr uint8_t DT_CS_INTERFACE = 0x24;
constexpr uint8_t DT_CS_ENDPOINT  = 0x25;

// Audio class
constexpr uint8_t CLASS_AUDIO      = 0x01;
constexpr uint8_t SUBCLASS_CONTROL = 0x01;
constexpr uint8_t SUBCLASS_STREAM  = 0x02;

// Feature unit control selectors
constexpr uint8_t FU_MUTE   = 0x01;
constexpr uint8_t FU_VOLUME = 0x02;

constexpr uint8_t ID_INPUT_TERMINAL  = 1;
constexpr uint8_t ID_FEATURE_UNIT    = 2;
constexpr uint8_t ID_OUTPUT_TERMINAL = 3;

// UAC volume is signed 1/256 dB. Silence is 0x8000; we clamp to -96 dB.
constexpr int16_t kVolMin = -96 * 256;
constexpr int16_t kVolMax = 0;
constexpr int16_t kVolRes = 256;

}  // namespace

Device::Device(std::string productName, uint16_t productId)
    : productName_(std::move(productName)), productId_(productId) {

    // ---- device descriptor ----
    devDesc_.reserve(18);
    u8(devDesc_, 18);
    u8(devDesc_, DT_DEVICE);
    le16(devDesc_, 0x0110);   // USB 1.1, full speed
    u8(devDesc_, 0);          // class defined per-interface
    u8(devDesc_, 0);
    u8(devDesc_, 0);
    u8(devDesc_, 64);         // bMaxPacketSize0
    le16(devDesc_, 0x1D6B);   // Linux Foundation VID, as USB/IP virtual devices conventionally use
    le16(devDesc_, productId_);
    le16(devDesc_, 0x0100);   // bcdDevice
    u8(devDesc_, 1);          // iManufacturer
    u8(devDesc_, 2);          // iProduct
    u8(devDesc_, 3);          // iSerialNumber
    u8(devDesc_, 1);          // bNumConfigurations

    // ---- audio control interface, class-specific block ----
    std::vector<uint8_t> ac;

    // AC header; wTotalLength patched below once the block is complete.
    u8(ac, 9); u8(ac, DT_CS_INTERFACE); u8(ac, 0x01);
    le16(ac, 0x0100);         // bcdADC
    le16(ac, 0);              // wTotalLength placeholder (offset 5..6)
    u8(ac, 1);                // bInCollection
    u8(ac, 1);                // baInterfaceNr[0] -> streaming interface

    // Input terminal: USB streaming in
    u8(ac, 12); u8(ac, DT_CS_INTERFACE); u8(ac, 0x02);
    u8(ac, ID_INPUT_TERMINAL);
    le16(ac, 0x0101);         // USB Streaming
    u8(ac, 0);                // bAssocTerminal
    u8(ac, kChannels);
    le16(ac, 0x0003);         // front left + front right
    u8(ac, 0);                // iChannelNames
    u8(ac, 0);                // iTerminal

    // Feature unit: master mute + volume
    u8(ac, 10); u8(ac, DT_CS_INTERFACE); u8(ac, 0x06);
    u8(ac, ID_FEATURE_UNIT);
    u8(ac, ID_INPUT_TERMINAL);
    u8(ac, 1);                // bControlSize
    u8(ac, 0x03);             // master: mute | volume
    u8(ac, 0x00);             // left
    u8(ac, 0x00);             // right
    u8(ac, 0);                // iFeature

    // Output terminal: speaker
    u8(ac, 9); u8(ac, DT_CS_INTERFACE); u8(ac, 0x03);
    u8(ac, ID_OUTPUT_TERMINAL);
    le16(ac, 0x0301);         // Speaker
    u8(ac, 0);                // bAssocTerminal
    u8(ac, ID_FEATURE_UNIT);
    u8(ac, 0);                // iTerminal

    const uint16_t acTotal = static_cast<uint16_t>(ac.size());
    ac[5] = static_cast<uint8_t>(acTotal & 0xFF);
    ac[6] = static_cast<uint8_t>(acTotal >> 8);

    // ---- full configuration ----
    std::vector<uint8_t> body;

    // Interface 0: AudioControl, no endpoints
    u8(body, 9); u8(body, DT_INTERFACE);
    u8(body, 0); u8(body, 0); u8(body, 0);
    u8(body, CLASS_AUDIO); u8(body, SUBCLASS_CONTROL); u8(body, 0); u8(body, 0);
    body.insert(body.end(), ac.begin(), ac.end());

    // Interface 1 alt 0: zero-bandwidth, so the host can idle the stream
    u8(body, 9); u8(body, DT_INTERFACE);
    u8(body, 1); u8(body, 0); u8(body, 0);
    u8(body, CLASS_AUDIO); u8(body, SUBCLASS_STREAM); u8(body, 0); u8(body, 0);

    // Interface 1 alt 1: one isochronous OUT endpoint
    u8(body, 9); u8(body, DT_INTERFACE);
    u8(body, 1); u8(body, 1); u8(body, 1);
    u8(body, CLASS_AUDIO); u8(body, SUBCLASS_STREAM); u8(body, 0); u8(body, 0);

    // AS general
    u8(body, 7); u8(body, DT_CS_INTERFACE); u8(body, 0x01);
    u8(body, ID_INPUT_TERMINAL);
    u8(body, 1);              // bDelay
    le16(body, 0x0001);       // PCM

    // Type I format: 48 kHz, 16-bit stereo, single discrete rate
    u8(body, 11); u8(body, DT_CS_INTERFACE); u8(body, 0x02);
    u8(body, 1);              // FORMAT_TYPE_I
    u8(body, kChannels);
    u8(body, kBytesPerSam);
    u8(body, kBytesPerSam * 8);
    u8(body, 1);              // one discrete sample rate
    le24(body, kSampleRate);

    // Isochronous OUT endpoint (audio endpoint descriptors are 9 bytes)
    u8(body, 9); u8(body, DT_ENDPOINT);
    u8(body, 0x01);           // OUT, endpoint 1
    u8(body, 0x09);           // isochronous, adaptive
    le16(body, kPacketBytes);
    u8(body, 1);              // bInterval: every frame
    u8(body, 0);              // bRefresh
    u8(body, 0);              // bSynchAddress

    // Class-specific endpoint
    u8(body, 7); u8(body, DT_CS_ENDPOINT); u8(body, 0x01);
    u8(body, 0x00);           // no sampling-frequency control
    u8(body, 0);              // bLockDelayUnits
    le16(body, 0);            // wLockDelay

    const uint16_t total = static_cast<uint16_t>(9 + body.size());
    cfgDesc_.reserve(total);
    u8(cfgDesc_, 9); u8(cfgDesc_, DT_CONFIG);
    le16(cfgDesc_, total);
    u8(cfgDesc_, 2);          // bNumInterfaces
    u8(cfgDesc_, 1);          // bConfigurationValue
    u8(cfgDesc_, 0);          // iConfiguration
    u8(cfgDesc_, 0xC0);       // self-powered
    u8(cfgDesc_, 25);         // 50 mA
    cfgDesc_.insert(cfgDesc_.end(), body.begin(), body.end());
}

float Device::linearGain() const {
    if (mute_) return 0.0f;
    if (volume_ <= kVolMin) return 0.0f;
    return std::pow(10.0f, (static_cast<float>(volume_) / 256.0f) / 20.0f);
}

std::vector<uint8_t> Device::stringDescriptor(uint8_t index) const {
    std::vector<uint8_t> d;
    if (index == 0) {
        d = {4, DT_STRING, 0x09, 0x04};   // US English
        return d;
    }
    std::string s;
    switch (index) {
        case 1: s = "openmix"; break;
        case 2: s = productName_; break;
        case 3: s = "OMX-" + std::to_string(productId_); break;
        default: return d;
    }
    d.push_back(static_cast<uint8_t>(2 + s.size() * 2));
    d.push_back(DT_STRING);
    for (char c : s) {
        d.push_back(static_cast<uint8_t>(c));
        d.push_back(0);
    }
    return d;
}

bool Device::handleControl(const SetupPacket& s, const uint8_t* outData, size_t outLen,
                           std::vector<uint8_t>& in) {
    // ---- standard requests ----
    if (s.type() == 0) {
        switch (s.bRequest) {
            case REQ_GET_DESCRIPTOR: {
                const uint8_t type = static_cast<uint8_t>(s.wValue >> 8);
                const uint8_t idx  = static_cast<uint8_t>(s.wValue & 0xFF);
                const std::vector<uint8_t>* src = nullptr;
                std::vector<uint8_t> tmp;
                if (type == DT_DEVICE) {
                    src = &devDesc_;
                } else if (type == DT_CONFIG) {
                    src = &cfgDesc_;
                } else if (type == DT_STRING) {
                    tmp = stringDescriptor(idx);
                    if (tmp.empty()) return false;
                    src = &tmp;
                } else {
                    return false;   // e.g. DEVICE_QUALIFIER: STALL is correct for full speed
                }
                const size_t n = (std::min)(static_cast<size_t>(s.wLength), src->size());
                in.assign(src->begin(), src->begin() + n);
                return true;
            }
            case REQ_SET_CONFIGURATION:
                configuration_ = static_cast<uint8_t>(s.wValue & 0xFF);
                return true;
            case REQ_GET_CONFIGURATION:
                in.push_back(configuration_);
                return true;
            case REQ_SET_INTERFACE:
                if ((s.wIndex & 0xFF) == 1) altSetting_ = static_cast<uint8_t>(s.wValue & 0xFF);
                return true;
            case REQ_GET_INTERFACE:
                in.push_back((s.wIndex & 0xFF) == 1 ? altSetting_ : 0);
                return true;
            case REQ_GET_STATUS:
                in.push_back(0);
                in.push_back(0);
                return true;
            case REQ_CLEAR_FEATURE:
            case REQ_SET_FEATURE:
                return true;
            default:
                return false;
        }
    }

    // ---- audio class requests on the feature unit ----
    if (s.type() == 1) {
        const uint8_t unit     = static_cast<uint8_t>(s.wIndex >> 8);
        const uint8_t selector = static_cast<uint8_t>(s.wValue >> 8);
        if (unit != ID_FEATURE_UNIT) return false;

        if (s.deviceToHost()) {
            if (selector == FU_MUTE) {
                if (s.bRequest != UAC_GET_CUR) return false;
                in.push_back(mute_ ? 1 : 0);
                return true;
            }
            if (selector == FU_VOLUME) {
                int16_t v = 0;
                switch (s.bRequest) {
                    case UAC_GET_CUR: v = volume_;  break;
                    case UAC_GET_MIN: v = kVolMin;  break;
                    case UAC_GET_MAX: v = kVolMax;  break;
                    case UAC_GET_RES: v = kVolRes;  break;
                    default: return false;
                }
                in.push_back(static_cast<uint8_t>(v & 0xFF));
                in.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                return true;
            }
            return false;
        }

        if (s.bRequest != UAC_SET_CUR) return false;
        if (selector == FU_MUTE && outLen >= 1) {
            mute_ = outData[0] != 0;
            return true;
        }
        if (selector == FU_VOLUME && outLen >= 2) {
            volume_ = static_cast<int16_t>(outData[0] | (outData[1] << 8));
            return true;
        }
        return false;
    }

    return false;
}

}  // namespace usbaudio
