#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <memory>
#include "ring.h"
#include "dsp.h"
#include "dynamics.h"

// Canonical internal format. Everything inside the engine is float32 interleaved.
constexpr unsigned kSampleRate = 48000;
constexpr unsigned kChannels   = 2;

std::string hrToString(HRESULT hr);

// ---- process discovery -------------------------------------------------

struct ProcEntry {
    DWORD pid = 0;
    DWORD ppid = 0;
    std::wstring name;   // lowercase image name, e.g. L"discord.exe"
};

std::vector<ProcEntry> enumProcesses();

// Root PIDs of every process tree whose image name matches `name`.
// Electron apps spawn same-named children; capturing the root with
// INCLUDE_TARGET_PROCESS_TREE picks up the audio-rendering child.
std::vector<DWORD> findRootPids(const std::vector<ProcEntry>& all, const std::wstring& name);

// PIDs that currently hold an audio render session on the default endpoint.
std::vector<DWORD> enumAudioSessionPids();

// ---- per-process loopback capture --------------------------------------

class ProcessLoopbackCapture {
public:
    ~ProcessLoopbackCapture();
    bool start(DWORD pid, bool includeTree, FloatRing* sink, std::string& err);
    void stop();
    DWORD pid() const { return pid_; }
    unsigned long long framesCaptured() const;

private:
    void run();
    static DWORD WINAPI thunk(LPVOID self);

    DWORD pid_ = 0;
    bool includeTree_ = true;
    FloatRing* sink_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE stopEvt_ = nullptr;
    std::atomic<unsigned long long> frames_{0};
    std::string startErr_;
    HANDLE readyEvt_ = nullptr;
    bool startOk_ = false;
};

// ---- monitor output ----------------------------------------------------

struct Bus {
    std::string name;
    // Capture buses feed a virtual microphone rather than the monitor mix, so
    // the mixer must not consume their ring.
    bool isCapture = false;

    // What you hear in your headphones.
    float gain = 1.0f;
    bool muted = false;

    // What OBS records, independently. Separate rings because each is a
    // single-producer/single-consumer queue with a different consumer: the
    // monitor thread drains `ring`, the USB capture endpoint drains `stream`.
    float streamGain = 1.0f;
    bool streamMuted = false;
    FloatRing stream;

    // EQ is a property of the channel, so it applies before the split and
    // both the monitor mix and the stream hear the same thing.
    dsp::EqParams eq;
    dsp::ChannelStrip strip;

    // Gate and compressor, used on the microphone channel.
    dsp::MicParams mic;
    dsp::MicChain micChain;

    FloatRing ring;
    std::vector<std::unique_ptr<ProcessLoopbackCapture>> captures;
    std::vector<std::wstring> matchNames;  // lowercase image names
    bool isRest = false;
};

// An active Windows render endpoint we could send the monitor mix to.
struct RenderDevice {
    std::wstring id;
    std::string name;
    bool isOpenmix = false;
};
std::vector<RenderDevice> listRenderDevices();
std::vector<RenderDevice> listCaptureDevices();

// Windows composes USB audio endpoint names as "<terminal type> (<product>)"
// and a device cannot override that, so every openmix channel shows up called
// "Speakers". Setting the endpoint's own friendly name is what the Rename
// button in Sound Control Panel does, and it is the only thing that sticks.
// Returns false when the property store refuses the write.
bool renameEndpoint(const std::wstring& deviceId, const std::wstring& newName);

// Names of the applications currently playing to one endpoint, so a channel
// can show what is actually on it rather than what you hope is.
std::vector<std::string> appsOnDevice(const std::wstring& deviceId);

// Round-trip audio integrity check: play a tone into a channel's playback side
// and record it from the same channel's capture side, then look for the gaps a
// broken clock produces. Requires openmix to be running. Returns 0 on pass.
int runSelfTest(const std::string& channel, int seconds);

// Offline filter checks: no devices, no audio hardware. Returns 0 on pass.
int runDspTest();

int runDynamicsTest();

// Pulls audio from a real input device into a ring, which the USB capture
// endpoint then serves to whichever application selected "openmix Mic".
class MicCapture {
public:
    ~MicCapture();
    // Optional processing applied on the way in, so everything downstream --
    // the virtual microphone and any monitoring -- hears the same thing.
    void setEq(dsp::EqParams* eq, dsp::ChannelStrip* strip);
    void setDynamics(dsp::MicParams* mic, dsp::MicChain* chain);
    // Optional second destination for the processed signal, so you can hear
    // your own voice as applications will hear it.
    void setMonitor(FloatRing* monitor);
    bool start(FloatRing* sink, const std::string& deviceMatch, std::string& err);
    void stop();
    const std::string& deviceName() const { return deviceName_; }

private:
    void run();
    // One attempt at opening the input and capturing; run() keeps retrying so
    // a device that disappears can come back.
    bool captureOnce();
    static DWORD WINAPI thunk(LPVOID self);

    FloatRing* sink_ = nullptr;
    dsp::EqParams* eq_ = nullptr;
    dsp::ChannelStrip* strip_ = nullptr;
    dsp::MicParams* mic_ = nullptr;
    dsp::MicChain* micChain_ = nullptr;
    FloatRing* monitor_ = nullptr;
    std::string deviceMatch_;
    std::string deviceName_;
    std::string startErr_;
    HANDLE thread_ = nullptr;
    HANDLE stopEvt_ = nullptr;
    HANDLE readyEvt_ = nullptr;
    bool ok_ = false;
};

class MonitorOutput {
public:
    // deviceMatch is a case-insensitive substring of the output device's name;
    // empty means "use the system default". openmix's own virtual endpoints are
    // never selected, so the monitor mix cannot loop back into the engine.
    bool start(std::vector<Bus>* buses, const std::string& deviceMatch, std::string& err);
    void stop();
    double bufferMs() const { return bufferMs_; }
    const std::string& deviceName() const { return deviceName_; }
    bool fellBack() const { return fellBack_; }

private:
    void run();
    // One attempt at opening the device and streaming. Returns when the
    // device fails or a stop is requested; run() keeps trying.
    bool streamOnce();
    static DWORD WINAPI thunk(LPVOID self);

    std::vector<Bus>* buses_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE stopEvt_ = nullptr;
    HANDLE readyEvt_ = nullptr;
    double bufferMs_ = 0.0;
    std::string deviceMatch_;
    std::string deviceName_;
    std::string startErr_;
    bool fellBack_ = false;
};
