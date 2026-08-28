#include "audio.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <psapi.h>
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

namespace {

// "C:\Program Files\Discord\Discord.exe" -> "Discord"
std::string prettyProcessName(DWORD pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    const bool ok = ::QueryFullProcessImageNameW(h, 0, path, &len) != 0;
    ::CloseHandle(h);
    if (!ok) return {};

    std::wstring w(path, len);
    const size_t slash = w.find_last_of(L'\\');
    if (slash != std::wstring::npos) w = w.substr(slash + 1);
    if (w.size() > 4 && _wcsicmp(w.c_str() + w.size() - 4, L".exe") == 0) {
        w = w.substr(0, w.size() - 4);
    }
    std::string out = narrow(w.c_str());
    if (!out.empty()) out[0] = static_cast<char>(::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

}  // namespace

std::vector<std::string> appsOnDevice(const std::wstring& deviceId) {
    std::vector<std::string> out;
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return out;
    }
    ComPtr<IMMDevice> dev;
    if (FAILED(devEnum->GetDevice(deviceId.c_str(), &dev))) return out;

    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&mgr)))) {
        return out;
    }
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(mgr->GetSessionEnumerator(&sessions))) return out;

    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctl;
        if (FAILED(sessions->GetSession(i, &ctl))) continue;

        // Expired sessions linger after an application closes; showing them
        // would be worse than showing nothing.
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(ctl->GetState(&state)) || state == AudioSessionStateExpired) continue;

        ComPtr<IAudioSessionControl2> ctl2;
        if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                       reinterpret_cast<void**>(&ctl2)))) {
            continue;
        }
        if (ctl2->IsSystemSoundsSession() == S_OK) continue;

        DWORD pid = 0;
        if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0) continue;
        std::string name = prettyProcessName(pid);
        if (name.empty()) continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
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

    // Devices come and go: a headset is unplugged, a monitor with speakers
    // sleeps, Windows switches the default. Losing the output should be a gap
    // in the audio, not the end of it.
    bool announced = false;
    while (::WaitForSingleObject(stopEvt_, 0) != WAIT_OBJECT_0) {
        streamOnce();
        if (!announced) {
            ::SetEvent(readyEvt_);
            announced = true;
        }
        // A device coming back takes a moment; retrying instantly just burns
        // CPU while Windows is still enumerating it.
        if (::WaitForSingleObject(stopEvt_, 500) == WAIT_OBJECT_0) break;
    }
    ::CoUninitialize();
}

bool MonitorOutput::streamOnce() {
    auto bail = [&](const std::string& what) {
        startErr_ = what;
        return false;
    };

    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return bail("no device enumerator");
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
                return bail("no output device matching \"" + deviceMatch_ + "\"");
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
            return bail("no non-openmix output device available");
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

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    300000 /* 30 ms */, 0, &wfx.Format, nullptr);
    if (FAILED(hr)) return bail("Initialize failed on " + deviceName_);

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    HANDLE bufEvt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(client->SetEventHandle(bufEvt))) {
        ::CloseHandle(bufEvt);
        return bail("SetEventHandle failed");
    }

    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render)))) {
        ::CloseHandle(bufEvt);
        return bail("GetService failed");
    }
    if (FAILED(client->Start())) {
        ::CloseHandle(bufEvt);
        return bail("Start failed");
    }

    bufferMs_ = (1000.0 * bufferFrames) / kSampleRate;
    startErr_.clear();

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
        // A device that has gone away fails here; that is the signal to go
        // back around and look for it again.
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 avail = bufferFrames - padding;
        if (avail == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(avail, &out))) break;

        const size_t samples = static_cast<size_t>(avail) * kChannels;
        mix.assign(samples, 0.0f);

        bool anySolo = false;
        for (const auto& bus : *buses_) anySolo |= bus.soloed;

        for (auto& bus : *buses_) {
            // A capture bus keeps its ring for the USB capture endpoint; its
            // monitor copy lives in the second ring.
            FloatRing& src = bus.isCapture ? bus.stream : bus.ring;
            // Drained either way: a soloed-out channel must not accumulate a
            // backlog that plays back when solo is released.
            src.trimTo(maxBacklog);
            const bool audible = anySolo ? bus.soloed : !bus.muted;
            src.mixInto(mix.data(), samples, audible ? bus.gain : 0.0f);
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
    return true;
}
