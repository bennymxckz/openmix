// End-to-end audio integrity test.
//
// openmix is both ends of its own signal path: a channel's playback endpoint
// accepts audio and its capture endpoint hands the same audio back. That makes
// a full round-trip test possible with no hardware and no listening -- play a
// tone into "Openmix - Game" and record it from "Openmix - Game", then look
// for the gaps and repeats that a broken clock produces.

#include "audio.h"
#include "dsp.h"
#include "dynamics.h"

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

// ---- offline DSP checks -------------------------------------------------
//
// No devices, no audio hardware, no listening. Push a sine through the filter
// chain and measure what comes out, so a wrong coefficient is caught by a test
// rather than by ear.

namespace {

// Steady-state gain of the chain at one frequency, in dB. The first half of
// the buffer is discarded so the filter has settled before measuring.
double gainAtDb(const dsp::EqParams& p, double freq) {
    dsp::ChannelStrip strip;
    strip.prepare(kSampleRate);

    const size_t frames = 24000;
    std::vector<float> buf(frames * kChannels);
    const double step = 2.0 * 3.14159265358979 * freq / kSampleRate;
    for (size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(std::sin(step * static_cast<double>(i)));
        for (unsigned c = 0; c < kChannels; ++c) buf[i * kChannels + c] = v;
    }
    strip.process(p, buf.data(), frames, kChannels);

    double sumSq = 0.0;
    const size_t from = frames / 2;
    for (size_t i = from; i < frames; ++i) {
        const double v = buf[i * kChannels];
        sumSq += v * v;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(frames - from));
    const double ref = std::sqrt(0.5);          // RMS of a unit sine
    return 20.0 * std::log10(rms / ref);
}

bool checkDb(const char* what, double got, double want, double tolerance) {
    const bool ok = std::fabs(got - want) <= tolerance;
    std::printf("  %-44s %+6.2f dB (want %+.1f +/- %.1f)  %s\n",
                what, got, want, tolerance, ok ? "PASS" : "FAIL");
    return ok;
}

}  // namespace

int runDspTest() {
    std::printf("DSP checks at %u Hz\n\n", kSampleRate);
    bool ok = true;

    {
        dsp::EqParams p;
        p.enabled = true;
        ok &= checkDb("flat chain passes 1 kHz unchanged", gainAtDb(p, 1000.0), 0.0, 0.2);
    }
    {
        // Disabled EQ must be bit-transparent, not merely close.
        dsp::EqParams p;
        p.enabled = false;
        ok &= checkDb("disabled chain passes 100 Hz unchanged", gainAtDb(p, 100.0), 0.0, 0.01);
    }
    {
        dsp::EqParams p;
        p.enabled = true;
        p.low.on = p.mid.on = p.high.on = false;
        p.hp.on = true;
        p.hp.freq = 1000.0f;
        // A 2nd-order high-pass rolls off 12 dB per octave, so 250 Hz is two
        // octaves down and should land near -24 dB.
        ok &= checkDb("high-pass 1 kHz attenuates 250 Hz", gainAtDb(p, 250.0), -24.0, 3.0);
        ok &= checkDb("high-pass 1 kHz passes 8 kHz", gainAtDb(p, 8000.0), 0.0, 0.5);
        ok &= checkDb("high-pass is -3 dB at its corner", gainAtDb(p, 1000.0), -3.0, 0.7);
    }
    {
        dsp::EqParams p;
        p.enabled = true;
        p.low.on = p.high.on = false;
        p.mid.on = true;
        p.mid.freq = 1000.0f;
        p.mid.gainDb = 12.0f;
        p.mid.q = 2.0f;
        ok &= checkDb("peak +12 dB at 1 kHz", gainAtDb(p, 1000.0), 12.0, 0.3);
        ok &= checkDb("peak leaves 100 Hz alone", gainAtDb(p, 100.0), 0.0, 0.5);
    }
    {
        dsp::EqParams p;
        p.enabled = true;
        p.mid.on = p.high.on = false;
        p.low.on = true;
        p.low.freq = 200.0f;
        p.low.gainDb = -12.0f;
        ok &= checkDb("low shelf -12 dB reaches full cut at 40 Hz",
                      gainAtDb(p, 40.0), -12.0, 1.5);
        ok &= checkDb("low shelf leaves 8 kHz alone", gainAtDb(p, 8000.0), 0.0, 0.5);
    }
    {
        dsp::EqParams p;
        p.enabled = true;
        p.low.on = p.mid.on = false;
        p.high.on = true;
        p.high.freq = 6000.0f;
        p.high.gainDb = 9.0f;
        ok &= checkDb("high shelf +9 dB reaches full boost at 16 kHz",
                      gainAtDb(p, 16000.0), 9.0, 1.5);
        ok &= checkDb("high shelf leaves 200 Hz alone", gainAtDb(p, 200.0), 0.0, 0.5);
    }

    // The EQ panel draws a response curve. If it does not agree with what the
    // filters actually do, it is decoration pretending to be information.
    {
        struct Case { const char* what; dsp::EqParams p; double freq; };
        std::vector<Case> cases;

        dsp::EqParams peak;
        peak.enabled = true;
        peak.low.on = peak.high.on = false;
        peak.mid.on = true;
        peak.mid.freq = 1000.0f;
        peak.mid.gainDb = 9.0f;
        peak.mid.q = 1.5f;
        cases.push_back({"curve matches measurement at a peak", peak, 1000.0});
        cases.push_back({"curve matches measurement beside a peak", peak, 300.0});

        dsp::EqParams hp;
        hp.enabled = true;
        hp.low.on = hp.mid.on = hp.high.on = false;
        hp.hp.on = true;
        hp.hp.freq = 500.0f;
        cases.push_back({"curve matches measurement on a high-pass", hp, 200.0});

        dsp::EqParams shelf;
        shelf.enabled = true;
        shelf.mid.on = shelf.high.on = false;
        shelf.low.on = true;
        shelf.low.freq = 300.0f;
        shelf.low.gainDb = -9.0f;
        cases.push_back({"curve matches measurement on a shelf", shelf, 60.0});

        for (const auto& c : cases) {
            const double drawn = dsp::ChannelStrip::responseDb(c.p, c.freq, kSampleRate);
            const double heard = gainAtDb(c.p, c.freq);
            ok &= checkDb(c.what, drawn, heard, 0.35);
        }
    }

    std::printf("\n%s\n", ok ? "PASS - all DSP checks" : "FAIL");
    return ok ? 0 : 1;
}

namespace {

// Steady-state output level in dBFS for a sine of a given input level, after
// the dynamics have had time to settle.
double dynLevelDb(const dsp::MicParams& p, double inputDb, double freq = 440.0) {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);

    const size_t frames = 48000;                 // one second, plenty to settle
    std::vector<float> buf(frames * kChannels);
    const double amp = std::pow(10.0, inputDb / 20.0);
    const double step = 2.0 * 3.14159265358979 * freq / kSampleRate;
    for (size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(std::sin(step * static_cast<double>(i)) * amp);
        for (unsigned c = 0; c < kChannels; ++c) buf[i * kChannels + c] = v;
    }
    chain.process(p, buf.data(), frames, kChannels);

    double peak = 0.0;
    for (size_t i = frames * 3 / 4; i < frames; ++i) {
        peak = (std::max)(peak, std::fabs(static_cast<double>(buf[i * kChannels])));
    }
    return peak > 1e-9 ? 20.0 * std::log10(peak) : -120.0;
}

}  // namespace

int runDynamicsTest() {
    std::printf("Dynamics checks at %u Hz\n\n", kSampleRate);
    bool ok = true;

    {
        dsp::MicParams p;
        p.gate.enabled = true;
        p.gate.thresholdDb = -45.0f;
        p.gate.releaseMs = 50.0f;
        p.gate.holdMs = 10.0f;
        ok &= checkDb("gate closes on a -60 dB signal", dynLevelDb(p, -60.0), -120.0, 25.0);
        ok &= checkDb("gate passes a -20 dB signal", dynLevelDb(p, -20.0), -20.0, 0.5);
    }
    {
        dsp::MicParams p;
        p.comp.enabled = true;
        p.comp.thresholdDb = -18.0f;
        p.comp.ratio = 4.0f;
        p.comp.attackMs = 1.0f;
        // 12 dB over a -18 dB threshold at 4:1 leaves 3 dB over, so -15 dBFS.
        ok &= checkDb("compressor 4:1 turns -6 dB into -15 dB", dynLevelDb(p, -6.0), -15.0, 1.0);
        ok &= checkDb("compressor leaves -30 dB below threshold alone",
                      dynLevelDb(p, -30.0), -30.0, 0.5);
    }
    {
        dsp::MicParams p;
        p.comp.enabled = true;
        p.comp.thresholdDb = -18.0f;
        p.comp.ratio = 4.0f;
        p.comp.attackMs = 1.0f;
        p.comp.makeupDb = 6.0f;
        ok &= checkDb("makeup gain lifts the compressed result", dynLevelDb(p, -6.0), -9.0, 1.0);
    }
    {
        // Makeup must never push the virtual microphone into clipping.
        dsp::MicParams p;
        p.comp.enabled = true;
        p.comp.thresholdDb = -30.0f;
        p.comp.ratio = 2.0f;
        p.comp.makeupDb = 24.0f;
        const double out = dynLevelDb(p, -3.0);
        const bool clipOk = out <= 0.05;
        std::printf("  %-44s %+6.2f dBFS (must not exceed 0)  %s\n",
                    "heavy makeup stays below full scale", out, clipOk ? "PASS" : "FAIL");
        ok &= clipOk;
    }

    std::printf("\n%s\n", ok ? "PASS - all dynamics checks" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Output shaping: balance, mono, delay, limiter, ducking.

namespace {

// Runs a steady tone through the mix chain and reports the RMS of one channel
// in dBFS. `left` and `right` let a test feed the two sides differently, which
// is the only way to see balance and mono do anything.
double mixLevelDb(const dsp::MixParams& p, float activity, int channel,
                  double left = 0.5, double right = 0.5) {
    dsp::MixChain chain;
    chain.prepare(kSampleRate);

    const size_t frames = 48000;                 // a full second, so the
    std::vector<float> buf(frames * kChannels);  // duck settles and the delay
    const double step = 2.0 * 3.14159265358979 * 440.0 / kSampleRate;  // fills
    for (size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(std::sin(step * static_cast<double>(i)));
        buf[i * kChannels] = static_cast<float>(v * left);
        if (kChannels > 1) buf[i * kChannels + 1] = static_cast<float>(v * right);
    }
    chain.process(p, activity, buf.data(), frames, kChannels);

    double sumSq = 0.0;
    const size_t from = frames / 2;              // measure after settling
    for (size_t i = from; i < frames; ++i) {
        const double v = buf[i * kChannels + channel];
        sumSq += v * v;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(frames - from));
    return rms > 1e-9 ? 20.0 * std::log10(rms) : -120.0;
}

}  // namespace

int runMixTest() {
    std::printf("\nOutput shaping at %u Hz\n\n", kSampleRate);
    bool ok = true;

    // A unit sine at half amplitude is -9.03 dBFS RMS. Everything below is
    // measured against that.
    const double ref = 20.0 * std::log10(0.5 / std::sqrt(2.0));

    {
        dsp::MixParams p;
        ok &= checkDb("untouched chain passes audio unchanged", mixLevelDb(p, 0.0f, 0), ref, 0.05);
        ok &= checkDb("...and does not duck without being asked",
                      mixLevelDb(p, 1.0f, 0), ref, 0.05);
    }
    {
        dsp::MixParams p;
        p.balance = 1.0f;                        // hard right
        ok &= checkDb("hard right silences the left", mixLevelDb(p, 0.0f, 0), -120.0, 1.0);
        ok &= checkDb("hard right leaves the right alone", mixLevelDb(p, 0.0f, 1), ref, 0.05);
    }
    {
        dsp::MixParams p;
        p.balance = -0.5f;                       // half left
        ok &= checkDb("half left drops the right 6 dB", mixLevelDb(p, 0.0f, 1), ref - 6.02, 0.1);
    }
    {
        // One side only: mono has to put half of it in each ear.
        dsp::MixParams p;
        p.mono = true;
        ok &= checkDb("mono folds a one-sided source into both",
                      mixLevelDb(p, 0.0f, 1, 0.5, 0.0), ref - 6.02, 0.1);
    }
    {
        dsp::MixParams p;
        p.duck = true;
        p.duckDb = -12.0f;
        ok &= checkDb("full activity ducks by the amount set",
                      mixLevelDb(p, 1.0f, 0), ref - 12.0, 0.3);
        ok &= checkDb("half activity ducks half as far",
                      mixLevelDb(p, 0.5f, 0), ref - 6.0, 0.3);
        p.duck = false;
        ok &= checkDb("ducking off ignores the microphone",
                      mixLevelDb(p, 1.0f, 0), ref, 0.05);
    }
    {
        // The delay must move audio in time without changing its level.
        dsp::MixParams p;
        p.delayMs = 100.0f;
        ok &= checkDb("delay preserves level", mixLevelDb(p, 0.0f, 0), ref, 0.05);

        dsp::MixChain chain;
        chain.prepare(kSampleRate);
        const size_t frames = 12000;                 // comfortably past 100 ms
        std::vector<float> buf(frames * kChannels, 0.0f);
        for (unsigned c = 0; c < kChannels; ++c) buf[c] = 1.0f;   // one impulse
        chain.process(p, 0.0f, buf.data(), frames, kChannels);

        size_t at = 0;
        bool found = false;
        for (size_t i = 0; i < frames; ++i) {
            if (std::fabs(buf[i * kChannels]) > 0.5f) { at = i; found = true; break; }
        }
        const double ms = 1000.0 * static_cast<double>(at) / kSampleRate;
        const bool delayOk = found && std::fabs(ms - 100.0) < 1.0;
        std::printf("  %-44s %6.1f ms (want 100.0 +/- 1.0)  %s\n",
                    "100 ms delay lands the impulse late", ms, delayOk ? "PASS" : "FAIL");
        ok &= delayOk;
    }
    {
        // Loud in, and it must not come out over the ceiling.
        dsp::MixParams p;
        p.limiter = true;
        dsp::MixChain chain;
        chain.prepare(kSampleRate);
        const size_t frames = 48000;
        std::vector<float> buf(frames * kChannels);
        const double step = 2.0 * 3.14159265358979 * 440.0 / kSampleRate;
        for (size_t i = 0; i < frames; ++i) {
            const float v = static_cast<float>(std::sin(step * static_cast<double>(i)) * 4.0);
            for (unsigned c = 0; c < kChannels; ++c) buf[i * kChannels + c] = v;
        }
        chain.process(p, 0.0f, buf.data(), frames, kChannels);
        float peak = 0.0f;
        for (size_t i = frames / 2; i < frames * kChannels; ++i) {
            peak = (std::max)(peak, std::fabs(buf[i]));
        }
        const double peakDb = 20.0 * std::log10((std::max)(peak, 1e-6f));
        const bool limOk = peakDb <= 0.0;
        std::printf("  %-44s %+6.2f dBFS (must not exceed 0)  %s\n",
                    "limiter holds a 4x signal under full scale", peakDb,
                    limOk ? "PASS" : "FAIL");
        ok &= limOk;
    }

    std::printf("\n%s\n", ok ? "PASS - all output shaping checks" : "FAIL");
    return ok ? 0 : 1;
}
