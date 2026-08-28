#pragma once
// A USB Audio Class 1.0 playback device, described well enough that Windows'
// in-box usbaudio.sys will bind to it and publish a render endpoint.
//
// UAC1 is deliberate: it is supported out of the box on every Windows version
// we care about, needs no vendor driver, and the endpoint's friendly name is
// taken from the USB iProduct string -- which is how "openmix Game" ends up in
// the sound settings.

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace usbaudio {

// One isochronous packet per USB frame at full speed: 1 ms of audio.
constexpr uint32_t kSampleRate  = 48000;
constexpr uint8_t  kChannels    = 2;
constexpr uint8_t  kBytesPerSam = 2;                       // 16-bit
constexpr uint16_t kPacketBytes = static_cast<uint16_t>(
    (kSampleRate / 1000) * kChannels * kBytesPerSam);      // 192

// Standard USB request codes we answer.
enum : uint8_t {
    REQ_GET_STATUS        = 0x00,
    REQ_CLEAR_FEATURE     = 0x01,
    REQ_SET_FEATURE       = 0x03,
    REQ_SET_ADDRESS       = 0x05,
    REQ_GET_DESCRIPTOR    = 0x06,
    REQ_SET_DESCRIPTOR    = 0x07,
    REQ_GET_CONFIGURATION = 0x08,
    REQ_SET_CONFIGURATION = 0x09,
    REQ_GET_INTERFACE     = 0x0A,
    REQ_SET_INTERFACE     = 0x0B,
};

// UAC1 class request codes.
enum : uint8_t {
    UAC_SET_CUR = 0x01,
    UAC_GET_CUR = 0x81,
    UAC_GET_MIN = 0x82,
    UAC_GET_MAX = 0x83,
    UAC_GET_RES = 0x84,
};

// Playback devices sink audio from the host; capture devices source it.
// Duplex devices carry both: applications render into the playback side, and
// OBS records the processed result from the capture side. That is what makes
// independent monitor and stream levels possible.
enum class Direction { Playback, Capture, Duplex };

// Terminal and unit IDs, and the endpoint addresses that carry audio.
constexpr uint8_t ID_PLAY_IT = 1;
constexpr uint8_t ID_PLAY_FU = 2;
constexpr uint8_t ID_PLAY_OT = 3;
constexpr uint8_t ID_CAP_IT  = 4;
constexpr uint8_t ID_CAP_FU  = 5;
constexpr uint8_t ID_CAP_OT  = 6;

constexpr uint8_t EP_OUT = 0x01;
constexpr uint8_t EP_IN  = 0x82;

struct SetupPacket {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;

    bool deviceToHost() const { return (bmRequestType & 0x80) != 0; }
    uint8_t type()      const { return (bmRequestType >> 5) & 0x03; }  // 0 std, 1 class
    uint8_t recipient() const { return bmRequestType & 0x1F; }
};

// A single virtual audio device: descriptors plus the mutable state Windows
// manipulates over the control endpoint.
class Device {
public:
    // `key` is the identity (e.g. "Game") and `productName` is what the user
    // sees. They are separate on purpose: identity must survive a rename, or
    // every cosmetic change makes Windows register brand-new hardware and
    // leaves the old entries behind forever.
    Device(std::string productName, std::string key,
           Direction dir = Direction::Playback);

    static uint16_t stableProductId(const std::string& key);

    Direction direction() const { return dir_; }
    bool isCapture() const { return dir_ == Direction::Capture; }
    bool isDuplex() const { return dir_ == Direction::Duplex; }

    const std::string& productName() const { return productName_; }
    uint16_t productId() const { return productId_; }

    // True once the host has selected a streaming alt-setting, i.e. audio is
    // actually flowing on that interface.
    bool streaming() const { return altSetting_[0] == 1 || altSetting_[1] == 1; }

    // Host-side volume/mute applied through the UAC feature unit. Volume is
    // reported in UAC 1/256 dB units; linearGain() converts to a multiplier.
    int16_t volumeRaw(bool captureSide = false) const { return volume_[captureSide ? 1 : 0]; }
    bool muted(bool captureSide = false) const { return mute_[captureSide ? 1 : 0]; }
    float linearGain(bool captureSide = false) const;

    // Answer a control transfer. Returns false to STALL the endpoint.
    // For device-to-host requests the reply is appended to `out`.
    bool handleControl(const SetupPacket& s, const uint8_t* outData, size_t outLen,
                       std::vector<uint8_t>& in);

    const std::vector<uint8_t>& deviceDescriptor() const { return devDesc_; }
    const std::vector<uint8_t>& configDescriptor() const { return cfgDesc_; }

private:
    std::vector<uint8_t> stringDescriptor(uint8_t index) const;

    std::string productName_;
    std::string key_;
    uint16_t productId_;
    Direction dir_ = Direction::Playback;
    std::vector<uint8_t> devDesc_;
    std::vector<uint8_t> cfgDesc_;

    uint8_t configuration_ = 0;
    uint8_t altSetting_[2] = {0, 0};   // one per streaming interface
    int16_t volume_[2] = {0, 0};       // 0 dB; [0] playback, [1] capture
    bool mute_[2] = {false, false};
};

}  // namespace usbaudio
