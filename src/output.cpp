#include "audio.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
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

std::vector<RenderDevice> listRenderDevices() {
    std::vector<RenderDevice> out;
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(devEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return out;

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

bool renameEndpoint(const std::wstring& deviceId, const std::wstring& newName) {
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return false;
    }
    ComPtr<IMMDevice> dev;
    if (FAILED(devEnum->GetDevice(deviceId.c_str(), &dev))) return false;

    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READWRITE, &props))) return false;

    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = const_cast<wchar_t*>(newName.c_str());
    const HRESULT hr = props->SetValue(PKEY_Device_FriendlyName, pv);
    // The string is borrowed, so zero the variant rather than clearing it.
    pv.vt = VT_EMPTY;
    pv.pwszVal = nullptr;
    if (FAILED(hr)) return false;
    return SUCCEEDED(props->Commit());
}

DWORD WINAPI MonitorOutput::thunk(LPVOID self) {
    static_cast<MonitorOutput*>(self)->run();
    return 0;
}

bool MonitorOutput::start(std::vector<Bus>* buses, const std::string& deviceMatch,
                          std::string& err) {
    buses_ = buses;
    deviceMatch_ = deviceMatch;
    stopEvt_  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvt_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = ::CreateThread(nullptr, 0, &MonitorOutput::thunk, this, 0, nullptr);
    if (!thread_) {
        err = "CreateThread failed";
        return false;
    }
    ::WaitForSingleObject(readyEvt_, 4000);
    if (bufferMs_ <= 0.0) {
        err = startErr_.empty() ? "render client did not start" : startErr_;
        return false;
    }
    return true;
}

void MonitorOutput::stop() {
    if (stopEvt_) ::SetEvent(stopEvt_);
    if (thread_) {
        ::WaitForSingleObject(thread_, 3000);
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvt_)  { ::CloseHandle(stopEvt_);  stopEvt_  = nullptr; }
    if (readyEvt_) { ::CloseHandle(readyEvt_); readyEvt_ = nullptr; }
}

void MonitorOutput::run() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto bail = [&](const std::string& what) {
        startErr_ = what;
        ::SetEvent(readyEvt_);
        ::CoUninitialize();
    };

    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        bail("no device enumerator");
        return;
    }

    // Choosing the device matters: rendering the monitor mix into one of our
    // own virtual endpoints would feed the engine straight back into itself.
    ComPtr<IMMDevice> dev;
    {
        ComPtr<IMMDeviceCollection> coll;
        devEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        UINT count = 0;
        if (coll) coll->GetCount(&count);

        // 1. explicit --out match, never one of ours
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
                bail("no output device matching \"" + deviceMatch_ + "\"");
                return;
            }
        }

        // 2. the system default, as long as it is not one of ours
        if (!dev) {
            ComPtr<IMMDevice> def;
            if (SUCCEEDED(devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
                const std::string name = friendlyName(def.p);
                if (!contains(name, "openmix")) {
                    dev.p = def.p;
                    def.p = nullptr;
                    deviceName_ = name;
                }
            }
        }

        // 3. the default is one of ours, so fall back to any real device
        if (!dev) {
            for (UINT i = 0; i < count && !dev; ++i) {
                ComPtr<IMMDevice> cand;
                if (FAILED(coll->Item(i, &cand))) continue;
                const std::string name = friendlyName(cand.p);
                if (contains(name, "openmix")) continue;
                dev.p = cand.p;
                cand.p = nullptr;
                deviceName_ = name;
                fellBack_ = true;
            }
        }
        if (!dev) {
            bail("no non-openmix output device available");
            return;
        }
    }

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&client)))) {
        bail("could not activate " + deviceName_);
        return;
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

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    300000 /* 30 ms */, 0, &wfx.Format, nullptr);
    if (FAILED(hr)) { bail("Initialize failed on " + deviceName_); return; }

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    HANDLE bufEvt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(client->SetEventHandle(bufEvt))) {
        ::CloseHandle(bufEvt);
        bail("SetEventHandle failed");
        return;
    }

    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render)))) {
        ::CloseHandle(bufEvt);
        bail("GetService failed");
        return;
    }
    if (FAILED(client->Start())) {
        ::CloseHandle(bufEvt);
        bail("Start failed");
        return;
    }

    bufferMs_ = (1000.0 * bufferFrames) / kSampleRate;
    ::SetEvent(readyEvt_);

    DWORD taskIndex = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // Bus producers and this render thread run on independent clocks. Until a
    // real adaptive resampler lands, cap each backlog so drift shows up as an
    // occasional dropped block rather than unbounded latency growth.
    const size_t maxBacklog = static_cast<size_t>(kSampleRate * kChannels) / 10;  // 100 ms

    std::vector<float> mix;
    HANDLE waits[2] = { stopEvt_, bufEvt };

    for (;;) {
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) continue;

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 avail = bufferFrames - padding;
        if (avail == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(avail, &out))) break;

        const size_t samples = static_cast<size_t>(avail) * kChannels;
        mix.assign(samples, 0.0f);

        for (auto& bus : *buses_) {
            if (bus.isCapture) continue;   // its ring belongs to the USB IN path
            bus.ring.trimTo(maxBacklog);
            const float g = bus.muted ? 0.0f : bus.gain;
            bus.ring.mixInto(mix.data(), samples, g);
        }

        // Soft clip so a hot sum cannot produce digital overs in the monitor.
        for (size_t i = 0; i < samples; ++i) {
            float v = mix[i];
            if (v > 1.0f || v < -1.0f) v = std::tanh(v);
            mix[i] = v;
        }

        std::memcpy(out, mix.data(), samples * sizeof(float));
        render->ReleaseBuffer(avail, 0);
    }

    client->Stop();
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ::CloseHandle(bufEvt);
    ::CoUninitialize();
}
