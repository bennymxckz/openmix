#include "audio.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <ksmedia.h>
#include <avrt.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

std::string hrToString(HRESULT hr) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

namespace {

std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// Minimal RAII for COM pointers.
template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ActivateAudioInterfaceAsync delivers its result on a threadpool thread;
// this handler only signals the event the caller waits on.
//
// It must be agile: the activation call marshals the callback across
// apartments, and a handler that does not answer QI for IAgileObject makes
// ActivateAudioInterfaceAsync fail with E_ILLEGAL_METHOD_CALL (0x8000000E).
// Microsoft's ApplicationLoopback sample gets this from WRL's FtmBase.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler,
                                public IAgileObject {
public:
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ActivationHandler() { if (done) ::CloseHandle(done); }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        ::SetEvent(done);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return 2; }  // lifetime is the stack frame
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

void fillFormat(WAVEFORMATEXTENSIBLE& wfx) {
    wfx = {};
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
}

}  // namespace

// ---- process discovery -------------------------------------------------

std::vector<ProcEntry> enumProcesses() {
    std::vector<ProcEntry> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            out.push_back({pe.th32ProcessID, pe.th32ParentProcessID, toLower(pe.szExeFile)});
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return out;
}

std::vector<DWORD> findRootPids(const std::vector<ProcEntry>& all, const std::wstring& name) {
    const std::wstring want = toLower(name);
    std::vector<DWORD> matching;
    for (const auto& p : all) {
        if (p.name == want) matching.push_back(p.pid);
    }
    // A PID is a tree root when its parent is not also in the match set.
    std::vector<DWORD> roots;
    for (const auto& p : all) {
        if (p.name != want) continue;
        const bool parentMatches =
            std::find(matching.begin(), matching.end(), p.ppid) != matching.end();
        if (!parentMatches) roots.push_back(p.pid);
    }
    return roots;
}

std::vector<DWORD> enumAudioSessionPids() {
    std::vector<DWORD> pids;
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return pids;
    }
    ComPtr<IMMDevice> dev;
    if (FAILED(devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return pids;

    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&mgr)))) {
        return pids;
    }
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(mgr->GetSessionEnumerator(&sessions))) return pids;

    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctl;
        if (FAILED(sessions->GetSession(i, &ctl))) continue;
        ComPtr<IAudioSessionControl2> ctl2;
        if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                       reinterpret_cast<void**>(&ctl2)))) {
            continue;
        }
        if (ctl2->IsSystemSoundsSession() == S_OK) continue;
        DWORD pid = 0;
        if (SUCCEEDED(ctl2->GetProcessId(&pid)) && pid != 0) pids.push_back(pid);
    }
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
}

// ---- capture -----------------------------------------------------------

ProcessLoopbackCapture::~ProcessLoopbackCapture() { stop(); }

unsigned long long ProcessLoopbackCapture::framesCaptured() const {
    return frames_.load(std::memory_order_relaxed);
}

DWORD WINAPI ProcessLoopbackCapture::thunk(LPVOID self) {
    static_cast<ProcessLoopbackCapture*>(self)->run();
    return 0;
}

bool ProcessLoopbackCapture::start(DWORD pid, bool includeTree, FloatRing* sink, std::string& err) {
    pid_ = pid;
    includeTree_ = includeTree;
    sink_ = sink;
    stopEvt_  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvt_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = ::CreateThread(nullptr, 0, &ProcessLoopbackCapture::thunk, this, 0, nullptr);
    if (!thread_) {
        err = "CreateThread failed";
        return false;
    }
    ::WaitForSingleObject(readyEvt_, 4000);
    if (!startOk_) {
        err = startErr_;
        stop();
        return false;
    }
    return true;
}

void ProcessLoopbackCapture::stop() {
    if (stopEvt_) ::SetEvent(stopEvt_);
    if (thread_) {
        ::WaitForSingleObject(thread_, 3000);
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvt_)  { ::CloseHandle(stopEvt_);  stopEvt_  = nullptr; }
    if (readyEvt_) { ::CloseHandle(readyEvt_); readyEvt_ = nullptr; }
}

void ProcessLoopbackCapture::run() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto fail = [&](const std::string& what) {
        startErr_ = what;
        startOk_ = false;
        ::SetEvent(readyEvt_);
        ::CoUninitialize();
    };

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid_;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        includeTree_ ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                     : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ActivationHandler handler;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                               __uuidof(IAudioClient), &pv, &handler, &op);
    if (FAILED(hr)) {
        fail("ActivateAudioInterfaceAsync " + hrToString(hr));
        return;
    }

    ::WaitForSingleObject(handler.done, 3000);

    ComPtr<IAudioClient> client;
    HRESULT activateHr = E_FAIL;
    hr = op->GetActivateResult(&activateHr, reinterpret_cast<IUnknown**>(&client));
    if (FAILED(hr))         { fail("GetActivateResult " + hrToString(hr)); return; }
    if (FAILED(activateHr)) { fail("activate " + hrToString(activateHr)); return; }

    // GetMixFormat returns E_NOTIMPL on the process-loopback client, so the
    // format is asserted rather than negotiated; the mixer converts internally.
    WAVEFORMATEXTENSIBLE wfx;
    fillFormat(wfx);

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            200000 /* 20 ms, in 100 ns units */, 0, &wfx.Format, nullptr);
    if (FAILED(hr)) { fail("Initialize " + hrToString(hr)); return; }

    HANDLE bufEvt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = client->SetEventHandle(bufEvt);
    if (FAILED(hr)) { ::CloseHandle(bufEvt); fail("SetEventHandle " + hrToString(hr)); return; }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (FAILED(hr)) { ::CloseHandle(bufEvt); fail("GetService " + hrToString(hr)); return; }

    hr = client->Start();
    if (FAILED(hr)) { ::CloseHandle(bufEvt); fail("Start " + hrToString(hr)); return; }

    startOk_ = true;
    ::SetEvent(readyEvt_);

    DWORD taskIndex = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    HANDLE waits[2] = { stopEvt_, bufEvt };
    std::vector<float> silence;

    for (;;) {
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w == WAIT_OBJECT_0) break;    // stop requested
        if (w == WAIT_TIMEOUT) continue;  // target is idle: the API sends silence, not errors

        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (frames) {
                const size_t samples = static_cast<size_t>(frames) * kChannels;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    if (silence.size() < samples) silence.assign(samples, 0.0f);
                    sink_->write(silence.data(), samples);
                } else {
                    sink_->write(reinterpret_cast<const float*>(data), samples);
                }
                frames_.fetch_add(frames, std::memory_order_relaxed);
            }
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ::CloseHandle(bufEvt);
    ::CoUninitialize();
}
