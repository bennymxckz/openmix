// End-to-end audio integrity test.
//
// openmix is both ends of its own signal path: a channel's playback endpoint
// accepts audio and its capture endpoint hands the same audio back. That makes
// a full round-trip test possible with no hardware and no listening -- play a
// tone into "Openmix - Game" and record it from "Openmix - Game", then look
// for the gaps and repeats that a broken clock produces.

#include "audio.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
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

bool openDevice(const std::wstring& id, ComPtr<IAudioClient>& client) {
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return false;
    }
    ComPtr<IMMDevice> dev;
    if (FAILED(devEnum->GetDevice(id.c_str(), &dev))) return false;
    return SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&client)));
}

}  // namespace

int runSelfTest(const std::string& channel, int seconds) {
    const std::string want = "Openmix - " + channel;

    std::wstring renderId, captureId;
    for (const auto& d : listRenderDevices()) {
        if (d.name.find(want) != std::string::npos) { renderId = d.id; break; }
    }
    for (const auto& d : listCaptureDevices()) {
        if (d.name.find(want) != std::string::npos) { captureId = d.id; break; }
    }
    if (renderId.empty() || captureId.empty()) {
        std::printf("Could not find both sides of \"%s\".\n"
                    "Start openmix first; the channel must be duplex.\n", want.c_str());
        return 1;
    }

    WAVEFORMATEXTENSIBLE wfx;
    fillFormat(wfx);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    std::atomic<bool> stop{false};
    std::vector<float> recorded;
    recorded.reserve(static_cast<size_t>(seconds + 2) * kSampleRate * kChannels);

    // ---- capture side ----
    std::thread capThread([&] {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IAudioClient> client;
        if (!openDevice(captureId, client)) { ::CoUninitialize(); return; }
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                      &wfx.Format, nullptr))) {
            ::CoUninitialize();
            return;
        }
        HANDLE evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client->SetEventHandle(evt);
        ComPtr<IAudioCaptureClient> cap;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&cap)))) {
            ::CloseHandle(evt);
            ::CoUninitialize();
            return;
        }
        client->Start();
        while (!stop.load()) {
            if (::WaitForSingleObject(evt, 200) != WAIT_OBJECT_0) continue;
            for (;;) {
                UINT32 packet = 0;
                if (FAILED(cap->GetNextPacketSize(&packet)) || packet == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD st = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &st, nullptr, nullptr))) break;
                const size_t n = static_cast<size_t>(frames) * kChannels;
                if (st & AUDCLNT_BUFFERFLAGS_SILENT) {
                    recorded.insert(recorded.end(), n, 0.0f);
                } else {
                    const float* f = reinterpret_cast<const float*>(data);
                    recorded.insert(recorded.end(), f, f + n);
                }
                cap->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        ::CloseHandle(evt);
        ::CoUninitialize();
    });

    // ---- render side: a steady 440 Hz tone ----
    std::thread renThread([&] {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IAudioClient> client;
        if (!openDevice(renderId, client)) { ::CoUninitialize(); return; }
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0,
                                      &wfx.Format, nullptr))) {
            ::CoUninitialize();
            return;
        }
        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);
        HANDLE evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client->SetEventHandle(evt);
        ComPtr<IAudioRenderClient> ren;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void**>(&ren)))) {
            ::CloseHandle(evt);
            ::CoUninitialize();
            return;
        }
        client->Start();
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979 * 440.0 / kSampleRate;
        while (!stop.load()) {
            if (::WaitForSingleObject(evt, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 avail = bufferFrames - padding;
            if (!avail) continue;
            BYTE* out = nullptr;
            if (FAILED(ren->GetBuffer(avail, &out))) break;
            float* f = reinterpret_cast<float*>(out);
            for (UINT32 i = 0; i < avail; ++i) {
                const float v = static_cast<float>(std::sin(phase)) * 0.5f;
                phase += step;
                for (unsigned c = 0; c < kChannels; ++c) *f++ = v;
            }
            ren->ReleaseBuffer(avail, 0);
        }
        client->Stop();
        ::CloseHandle(evt);
        ::CoUninitialize();
    });

    std::printf("Playing 440 Hz into \"%s\" and recording it back for %d s...\n",
                want.c_str(), seconds);
    ::Sleep(static_cast<DWORD>(seconds) * 1000);
    stop.store(true);
    renThread.join();
    capThread.join();

    // ---- analysis ----
    const size_t frames = recorded.size() / kChannels;
    if (frames == 0) {
        std::printf("FAIL: nothing was recorded.\n");
        return 1;
    }

    const double expected = static_cast<double>(seconds) * kSampleRate;
    const double ratio = static_cast<double>(frames) / expected;

    // Silence in the middle of a continuous tone is a dropout. Ignore the
    // leading gap before audio starts flowing.
    size_t firstAudio = frames;
    for (size_t i = 0; i < frames; ++i) {
        if (std::fabs(recorded[i * kChannels]) > 0.01f) { firstAudio = i; break; }
    }

    size_t gapCount = 0, longestGap = 0, run = 0;
    double sumSq = 0.0;
    size_t counted = 0;
    for (size_t i = firstAudio; i < frames; ++i) {
        const float v = recorded[i * kChannels];
        sumSq += static_cast<double>(v) * v;
        ++counted;
        if (std::fabs(v) < 0.0005f) {
            ++run;
        } else {
            // A 440 Hz sine crosses zero every ~55 frames, so only runs well
            // past that are real gaps rather than the waveform itself.
            if (run > 96) {
                ++gapCount;
                longestGap = (std::max)(longestGap, run);
            }
            run = 0;
        }
    }
    if (run > 96) {
        ++gapCount;
        longestGap = (std::max)(longestGap, run);
    }

    const double rms = counted ? std::sqrt(sumSq / static_cast<double>(counted)) : 0.0;
    const double gapMs = 1000.0 * static_cast<double>(longestGap) / kSampleRate;

    std::printf("\n  frames recorded : %zu (%.1f%% of real time)\n", frames, ratio * 100.0);
    std::printf("  signal RMS      : %.3f (a clean 0.5 sine is about 0.354)\n", rms);
    std::printf("  dropouts        : %zu, longest %.1f ms\n", gapCount, gapMs);

    // A healthy round trip runs at real time, carries the tone, and never goes
    // quiet in the middle of it.
    const bool rateOk = ratio > 0.97 && ratio < 1.03;
    const bool toneOk = rms > 0.20;
    const bool gapsOk = gapCount == 0;

    std::printf("\n  rate  %s\n  tone  %s\n  gaps  %s\n",
                rateOk ? "PASS" : "FAIL",
                toneOk ? "PASS" : "FAIL",
                gapsOk ? "PASS" : "FAIL");

    if (rateOk && toneOk && gapsOk) {
        std::printf("\nPASS - audio is continuous end to end.\n");
        return 0;
    }
    std::printf("\nFAIL\n");
    return 1;
}
