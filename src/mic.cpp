// Real microphone capture. The audio lands in a ring that the USB capture
// endpoint serves to whichever application selected "openmix Mic".

#include "audio.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return s;
}

bool contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::string friendlyName(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
        name = narrow(pv.pwszVal);
    }
    ::PropVariantClear(&pv);
    return name;
}

}  // namespace

std::vector<RenderDevice> listCaptureDevices() {
    std::vector<RenderDevice> out;
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(devEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll))) return out;

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        RenderDevice rd;
        rd.id = id;
        rd.name = friendlyName(dev.p);
        rd.isOpenmix = contains(rd.name, "openmix");
        ::CoTaskMemFree(id);
        out.push_back(std::move(rd));
    }
    return out;
}

MicCapture::~MicCapture() { stop(); }

DWORD WINAPI MicCapture::thunk(LPVOID self) {
    static_cast<MicCapture*>(self)->run();
    return 0;
}

void MicCapture::setEq(dsp::EqParams* eq, dsp::ChannelStrip* strip) {
    eq_ = eq;
    strip_ = strip;
}

void MicCapture::setActivity(std::atomic<float>* activity) {
    activity_ = activity;
}

void MicCapture::setDenoise(dsp::DenoiseParams* p, dsp::NoiseSuppressor* ns) {
    denoise_ = p;
    denoiser_ = ns;
}

// How open the microphone is, 0..1, for channels that duck under it.
//
// Deliberately not the gate's own gain: ducking has to work whether or not the
// gate is switched on. It reuses the gate threshold when there is one, because
// that is already the user's answer to "this is me talking", and falls back to
// a level well under speech but over room noise.
void MicCapture::publishActivity(const float* samples, size_t count) {
    if (!activity_) return;
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) peak = (std::max)(peak, std::fabs(samples[i]));

    const float openDb = (mic_ && mic_->gate.enabled) ? mic_->gate.thresholdDb : -40.0f;
    const float peakDb = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
    // Fully in over a 12 dB window above the threshold, so a quiet word does
    // not slam the music down as hard as a shout.
    const float v = std::clamp((peakDb - openDb) / 12.0f, 0.0f, 1.0f);
    activity_->store(v, std::memory_order_relaxed);
}

void MicCapture::setDynamics(dsp::MicParams* mic, dsp::MicChain* chain) {
    mic_ = mic;
    micChain_ = chain;
}

void MicCapture::setMonitor(FloatRing* monitor) {
    monitor_ = monitor;
}

bool MicCapture::start(FloatRing* sink, const std::string& deviceMatch, std::string& err) {
    sink_ = sink;
    deviceMatch_ = deviceMatch;
    stopEvt_  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvt_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = ::CreateThread(nullptr, 0, &MicCapture::thunk, this, 0, nullptr);
    if (!thread_) {
        err = "CreateThread failed";
        return false;
    }
    ::WaitForSingleObject(readyEvt_, 4000);
    if (!ok_) {
        err = startErr_.empty() ? "capture did not start" : startErr_;
        return false;
    }
    return true;
}

void MicCapture::stop() {
    if (stopEvt_) ::SetEvent(stopEvt_);
    if (thread_) {
        ::WaitForSingleObject(thread_, 3000);
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvt_)  { ::CloseHandle(stopEvt_);  stopEvt_  = nullptr; }
    if (readyEvt_) { ::CloseHandle(readyEvt_); readyEvt_ = nullptr; }
}

void MicCapture::run() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // A microphone that is unplugged and plugged back in should come back,
    // not stay gone until openmix is restarted.
    bool announced = false;
    while (::WaitForSingleObject(stopEvt_, 0) != WAIT_OBJECT_0) {
        captureOnce();
        if (!announced) {
            ::SetEvent(readyEvt_);
            announced = true;
        }
        if (::WaitForSingleObject(stopEvt_, 500) == WAIT_OBJECT_0) break;
    }
    ::CoUninitialize();
}

bool MicCapture::captureOnce() {
    auto bail = [&](const std::string& what) {
        startErr_ = what;
        return false;
    };

    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return bail("no device enumerator");
    }

    // Never capture one of our own virtual microphones -- that would route the
    // mic endpoint straight back into itself.
    ComPtr<IMMDevice> dev;
    {
        ComPtr<IMMDeviceCollection> coll;
        devEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        UINT count = 0;
        if (coll) coll->GetCount(&count);

        if (!deviceMatch_.empty()) {
            for (UINT i = 0; i < count && !dev; ++i) {
                ComPtr<IMMDevice> cand;
                if (FAILED(coll->Item(i, &cand))) continue;
                const std::string name = friendlyName(cand.p);
                if (contains(name, "openmix")) continue;
                if (contains(name, deviceMatch_)) {
                    dev.p = cand.p;
                    cand.p = nullptr;
                    deviceName_ = name;
                }
            }
            if (!dev) {
                return bail("no input device matching \"" + deviceMatch_ + "\"");
            }
        }
        if (!dev) {
            ComPtr<IMMDevice> def;
            if (SUCCEEDED(devEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) {
                const std::string name = friendlyName(def.p);
                if (!contains(name, "openmix")) {
                    dev.p = def.p;
                    def.p = nullptr;
                    deviceName_ = name;
                }
            }
        }
        if (!dev) {
            for (UINT i = 0; i < count && !dev; ++i) {
                ComPtr<IMMDevice> cand;
                if (FAILED(coll->Item(i, &cand))) continue;
                const std::string name = friendlyName(cand.p);
                if (contains(name, "openmix")) continue;
                dev.p = cand.p;
                cand.p = nullptr;
                deviceName_ = name;
            }
        }
        if (!dev) {
            return bail("no input device available");
        }
    }

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&client)))) {
        return bail("could not activate " + deviceName_);
    }

    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = static_cast<WORD>(kChannels);
    wfx.Format.nSamplesPerSec  = kSampleRate;
    wfx.Format.wBitsPerSample  = 32;
    wfx.Format.nBlockAlign     = static_cast<WORD>(kChannels * 4);
    wfx.Format.nAvgBytesPerSec = kSampleRate * wfx.Format.nBlockAlign;
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat              = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // AUTOCONVERTPCM lets Windows handle a mono or 44.1 kHz microphone for us.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    // 0 asks for the engine's own period (typically 10 ms) rather than adding
    // a second buffer on top of it.
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                  0, 0, &wfx.Format, nullptr))) {
        return bail("Initialize failed on " + deviceName_);
    }

    HANDLE bufEvt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(client->SetEventHandle(bufEvt))) {
        ::CloseHandle(bufEvt);
        return bail("SetEventHandle failed");
    }

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&capture)))) {
        ::CloseHandle(bufEvt);
        return bail("GetService failed");
    }
    if (FAILED(client->Start())) {
        ::CloseHandle(bufEvt);
        return bail("Start failed");
    }

    ok_ = true;
    startErr_.clear();

    DWORD taskIndex = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    HANDLE waits[2] = { stopEvt_, bufEvt };
    std::vector<float> silence;
    std::vector<float> work;

    for (;;) {
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) continue;

        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD st = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &st, nullptr, nullptr))) break;

            if (frames) {
                const size_t samples = static_cast<size_t>(frames) * kChannels;
                if (st & AUDCLNT_BUFFERFLAGS_SILENT) {
                    if (silence.size() < samples) silence.assign(samples, 0.0f);
                    sink_->write(silence.data(), samples);
                    if (monitor_) monitor_->write(silence.data(), samples);
                } else {
                    const float* src = reinterpret_cast<const float*>(data);
                    const bool wantEq = eq_ && strip_ && eq_->enabled;
                    const bool wantDyn = mic_ && micChain_ &&
                                         (mic_->gate.enabled || mic_->comp.enabled);
                    const bool wantNr = denoise_ && denoiser_ && denoise_->enabled;
                    if (wantEq || wantDyn || wantNr) {
                        if (work.size() < samples) work.resize(samples);
                        std::memcpy(work.data(), src, samples * sizeof(float));
                        // Noise suppression first, so its estimate of the room
                        // is made from what the microphone actually heard
                        // rather than from a spectrum the equaliser has
                        // already reshaped.
                        if (wantNr) denoiser_->process(*denoise_, work.data(), frames, kChannels);
                        // Then EQ: the gate and compressor should react to the
                        // corrected signal, not to rumble the high-pass is
                        // about to remove.
                        if (wantEq) strip_->process(*eq_, work.data(), frames, kChannels);
                        if (wantDyn) micChain_->process(*mic_, work.data(), frames, kChannels);
                        publishActivity(work.data(), samples);
                        sink_->write(work.data(), samples);
                        if (monitor_) monitor_->write(work.data(), samples);
                    } else {
                        publishActivity(src, samples);
                        sink_->write(src, samples);
                        if (monitor_) monitor_->write(src, samples);
                    }
                }
            }
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ::CloseHandle(bufEvt);
    return true;
}
