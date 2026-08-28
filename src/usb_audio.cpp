#include "usb_audio.h"

#include <algorithm>
#include <cctype>
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

constexpr uint8_t DT_DEVICE       = 0x01;
constexpr uint8_t DT_CONFIG       = 0x02;
constexpr uint8_t DT_STRING       = 0x03;
constexpr uint8_t DT_INTERFACE    = 0x04;
constexpr uint8_t DT_ENDPOINT     = 0x05;
constexpr uint8_t DT_CS_INTERFACE = 0x24;
constexpr uint8_t DT_CS_ENDPOINT  = 0x25;

constexpr uint8_t CLASS_AUDIO      = 0x01;
constexpr uint8_t SUBCLASS_CONTROL = 0x01;
constexpr uint8_t SUBCLASS_STREAM  = 0x02;

constexpr uint8_t FU_MUTE   = 0x01;
constexpr uint8_t FU_VOLUME = 0x02;

constexpr uint8_t STR_MANUFACTURER = 1;
constexpr uint8_t STR_PRODUCT      = 2;
constexpr uint8_t STR_SERIAL       = 3;

// UAC volume is signed 1/256 dB. Silence is 0x8000; we clamp to -96 dB.
constexpr int16_t kVolMin = -96 * 256;
constexpr int16_t kVolMax = 0;
constexpr int16_t kVolRes = 256;

// Emit an audio-control terminal/unit chain. `usbIsSource` is true when the
// USB stream feeds the chain (playback) and false when the chain feeds the USB
// stream (capture).
void emitChain(std::vector<uint8_t>& ac, uint8_t itId, uint8_t fuId, uint8_t otId,
               bool usbIsSource) {
    // Input terminal
    u8(ac, 12); u8(ac, DT_CS_INTERFACE); u8(ac, 0x02);
    u8(ac, itId);
    le16(ac, usbIsSource ? 0x0101 : 0x0201);   // USB Streaming : Microphone
    u8(ac, 0);
    u8(ac, kChannels);
    le16(ac, 0x0003);         // front left + right
    u8(ac, 0);
    u8(ac, 0);

    // Feature unit: master mute + volume
    u8(ac, 10); u8(ac, DT_CS_INTERFACE); u8(ac, 0x06);
    u8(ac, fuId);
    u8(ac, itId);
    u8(ac, 1);                // bControlSize
    u8(ac, 0x03);             // master: mute | volume
    u8(ac, 0x00);
    u8(ac, 0x00);
    u8(ac, 0);

    // Output terminal
    u8(ac, 9); u8(ac, DT_CS_INTERFACE); u8(ac, 0x03);
    u8(ac, otId);
    le16(ac, usbIsSource ? 0x0301 : 0x0101);   // Speaker : USB Streaming
    u8(ac, 0);
    u8(ac, fuId);
    u8(ac, 0);
}

// Emit a streaming interface: a zero-bandwidth alt 0 so the host can idle the
// stream, and an alt 1 carrying one isochronous endpoint.
void emitStreaming(std::vector<uint8_t>& body, uint8_t ifNum, uint8_t terminalLink,
                   uint8_t endpointAddr, bool isInput) {
    u8(body, 9); u8(body, DT_INTERFACE);
    u8(body, ifNum); u8(body, 0); u8(body, 0);
    u8(body, CLASS_AUDIO); u8(body, SUBCLASS_STREAM); u8(body, 0); u8(body, 0);

    u8(body, 9); u8(body, DT_INTERFACE);
    u8(body, ifNum); u8(body, 1); u8(body, 1);
    u8(body, CLASS_AUDIO); u8(body, SUBCLASS_STREAM); u8(body, 0); u8(body, 0);

    // AS general
    u8(body, 7); u8(body, DT_CS_INTERFACE); u8(body, 0x01);
    u8(body, terminalLink);
    u8(body, 1);              // bDelay
    le16(body, 0x0001);       // PCM

    // Type I format: 48 kHz, 16-bit stereo, single discrete rate
    u8(body, 11); u8(body, DT_CS_INTERFACE); u8(body, 0x02);
    u8(body, 1);
    u8(body, kChannels);
    u8(body, kBytesPerSam);
    u8(body, kBytesPerSam * 8);
    u8(body, 1);
    le24(body, kSampleRate);

    // Audio endpoint descriptors are 9 bytes, not the usual 7. Playback is
    // adaptive (the host follows our consumption); capture is asynchronous
    // (we own the clock and the host takes what we produce).
    u8(body, 9); u8(body, DT_ENDPOINT);
    u8(body, endpointAddr);
    u8(body, isInput ? 0x05 : 0x09);
    le16(body, kPacketBytes);
    u8(body, 1);              // bInterval: every frame
    u8(body, 0);              // bRefresh
    u8(body, 0);              // bSynchAddress

    // Class-specific endpoint
    u8(body, 7); u8(body, DT_CS_ENDPOINT); u8(body, 0x01);
    u8(body, 0x00);           // no sampling-frequency control
    u8(body, 0);
    le16(body, 0);
}

}  // namespace

Device::Device(std::string productName, std::string key, Direction dir)
    : productName_(std::move(productName)), key_(std::move(key)),
      productId_(stableProductId(key_)), dir_(dir) {

    const bool duplex  = (dir_ == Direction::Duplex);
    const bool capture = (dir_ == Direction::Capture);

    // ---- device descriptor ----
    devDesc_.reserve(18);
    u8(devDesc_, 18);
    u8(devDesc_, DT_DEVICE);
    le16(devDesc_, 0x0110);   // USB 1.1, full speed
    u8(devDesc_, 0);          // class defined per-interface
    u8(devDesc_, 0);
    u8(devDesc_, 0);
    u8(devDesc_, 64);         // bMaxPacketSize0
    le16(devDesc_, 0x1D6B);   // Linux Foundation, as USB/IP devices conventionally use
    le16(devDesc_, productId_);
    le16(devDesc_, 0x0100);
    u8(devDesc_, STR_MANUFACTURER);
    u8(devDesc_, STR_PRODUCT);
    u8(devDesc_, STR_SERIAL);
    u8(devDesc_, 1);          // bNumConfigurations

    // ---- audio control interface ----
    const uint8_t streamingCount = duplex ? 2 : 1;

    std::vector<uint8_t> ac;
    u8(ac, static_cast<uint8_t>(8 + streamingCount));
    u8(ac, DT_CS_INTERFACE); u8(ac, 0x01);
    le16(ac, 0x0100);         // bcdADC
    le16(ac, 0);              // wTotalLength, patched below
    u8(ac, streamingCount);   // bInCollection
    u8(ac, 1);                // baInterfaceNr[0]
    if (duplex) u8(ac, 2);    // baInterfaceNr[1]

    if (duplex) {
        emitChain(ac, ID_PLAY_IT, ID_PLAY_FU, ID_PLAY_OT, true);
        emitChain(ac, ID_CAP_IT, ID_CAP_FU, ID_CAP_OT, false);
    } else {
        emitChain(ac, ID_PLAY_IT, ID_PLAY_FU, ID_PLAY_OT, !capture);
    }

    const uint16_t acTotal = static_cast<uint16_t>(ac.size());
    ac[5] = static_cast<uint8_t>(acTotal & 0xFF);
    ac[6] = static_cast<uint8_t>(acTotal >> 8);

    // ---- full configuration ----
    std::vector<uint8_t> body;

    u8(body, 9); u8(body, DT_INTERFACE);
    u8(body, 0); u8(body, 0); u8(body, 0);
    u8(body, CLASS_AUDIO); u8(body, SUBCLASS_CONTROL); u8(body, 0); u8(body, 0);
    body.insert(body.end(), ac.begin(), ac.end());

    if (duplex) {
        emitStreaming(body, 1, ID_PLAY_IT, EP_OUT, false);
        emitStreaming(body, 2, ID_CAP_OT, EP_IN, true);
    } else if (capture) {
        emitStreaming(body, 1, ID_PLAY_OT, EP_IN, true);
    } else {
        emitStreaming(body, 1, ID_PLAY_IT, EP_OUT, false);
    }

    const uint16_t total = static_cast<uint16_t>(9 + body.size());
    cfgDesc_.reserve(total);
    u8(cfgDesc_, 9); u8(cfgDesc_, DT_CONFIG);
    le16(cfgDesc_, total);
    u8(cfgDesc_, static_cast<uint8_t>(1 + streamingCount));   // bNumInterfaces
    u8(cfgDesc_, 1);          // bConfigurationValue
    u8(cfgDesc_, 0);
    u8(cfgDesc_, 0xC0);       // self-powered
    u8(cfgDesc_, 25);         // 50 mA
    cfgDesc_.insert(cfgDesc_.end(), body.begin(), body.end());
}

uint16_t Device::stableProductId(const std::string& key) {
    uint32_t h = 2166136261u;                 // FNV-1a
    for (unsigned char c : key) {
        h ^= c;
        h *= 16777619u;
    }
    return static_cast<uint16_t>(0x1000 | (h & 0x0FFF));
}

float Device::linearGain(bool captureSide) const {
    const int i = captureSide ? 1 : 0;
    if (mute_[i]) return 0.0f;
    if (volume_[i] <= kVolMin) return 0.0f;
    return std::pow(10.0f, (static_cast<float>(volume_[i]) / 256.0f) / 20.0f);
}

std::vector<uint8_t> Device::stringDescriptor(uint8_t index) const {
    std::vector<uint8_t> d;
    if (index == 0) {
        d = {4, DT_STRING, 0x09, 0x04};   // US English
        return d;
    }
    std::string s;
    switch (index) {
        case STR_MANUFACTURER: s = "openmix"; break;
        case STR_PRODUCT:      s = productName_; break;
        case STR_SERIAL: {
            // Identity, so it comes from the key -- never the display name,
            // which the user may want changed.
            s = "openmix-";
            for (unsigned char c : key_) s += static_cast<char>(::tolower(c));
            break;
        }
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
                    return false;   // DEVICE_QUALIFIER etc: STALL is correct at full speed
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
            case REQ_SET_INTERFACE: {
                const uint8_t ifNum = static_cast<uint8_t>(s.wIndex & 0xFF);
                if (ifNum >= 1 && ifNum <= 2) {
                    altSetting_[ifNum - 1] = static_cast<uint8_t>(s.wValue & 0xFF);
                }
                return true;
            }
            case REQ_GET_INTERFACE: {
                const uint8_t ifNum = static_cast<uint8_t>(s.wIndex & 0xFF);
                in.push_back((ifNum >= 1 && ifNum <= 2) ? altSetting_[ifNum - 1] : 0);
                return true;
            }
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

    // ---- audio class requests on a feature unit ----
    if (s.type() == 1) {
        const uint8_t unit     = static_cast<uint8_t>(s.wIndex >> 8);
        const uint8_t selector = static_cast<uint8_t>(s.wValue >> 8);

        int i;
        if (unit == ID_PLAY_FU)      i = 0;
        else if (unit == ID_CAP_FU)  i = 1;
        else                         return false;

        if (s.deviceToHost()) {
            if (selector == FU_MUTE) {
                if (s.bRequest != UAC_GET_CUR) return false;
                in.push_back(mute_[i] ? 1 : 0);
                return true;
            }
            if (selector == FU_VOLUME) {
                int16_t v = 0;
                switch (s.bRequest) {
                    case UAC_GET_CUR: v = volume_[i]; break;
                    case UAC_GET_MIN: v = kVolMin;    break;
                    case UAC_GET_MAX: v = kVolMax;    break;
                    case UAC_GET_RES: v = kVolRes;    break;
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
            mute_[i] = outData[0] != 0;
            return true;
        }
        if (selector == FU_VOLUME && outLen >= 2) {
            volume_[i] = static_cast<int16_t>(outData[0] | (outData[1] << 8));
            return true;
        }
        return false;
    }

    return false;
}

}  // namespace usbaudio
