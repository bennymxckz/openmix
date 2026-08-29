#pragma once
// Microphone dynamics: a noise gate and a compressor.
//
// Both work on the louder of the two channels rather than each independently,
// so a stereo microphone never has one side duck while the other stays open --
// that shifts the image and sounds worse than doing nothing.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dsp {

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// Per-sample smoothing coefficient for a time constant in milliseconds.
inline float timeCoeff(float ms, double sampleRate) {
    if (ms <= 0.0f) return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * sampleRate)));
}

struct GateParams {
    bool enabled = false;
    float thresholdDb = -45.0f;
    float attackMs = 1.0f;
    float holdMs = 120.0f;    // keeps the gate from chattering between words
    float releaseMs = 180.0f;
};

struct CompParams {
    bool enabled = false;
    float thresholdDb = -18.0f;
    float ratio = 3.0f;
    float attackMs = 5.0f;
    float releaseMs = 120.0f;
    float makeupDb = 0.0f;
};

struct MicParams {
    GateParams gate;
    CompParams comp;

    bool operator==(const MicParams& o) const {
        return std::memcmp(this, &o, sizeof(MicParams)) == 0;
    }
    bool operator!=(const MicParams& o) const { return !(*this == o); }
};

// Gate and compressor sharing one detector pass. Reports the gain it applied
// so a meter can show the user what the processing is doing.
class MicChain {
public:
    void prepare(double sampleRate) {
        sr_ = sampleRate;
        env_ = 0.0f;
        gateGain_ = 0.0f;
        compGain_ = 1.0f;
        holdLeft_ = 0;
        open_ = false;
    }

    // Interleaved, in place.
    void process(const MicParams& p, float* samples, size_t frames, unsigned channels) {
        if (!p.gate.enabled && !p.comp.enabled) {
            gateGain_ = 1.0f;
            compGain_ = 1.0f;
            return;
        }

        const float gateThresh = dbToLin(p.gate.thresholdDb);
        const float gateAtk = timeCoeff(p.gate.attackMs, sr_);
        const float gateRel = timeCoeff(p.gate.releaseMs, sr_);
        const int holdSamples = static_cast<int>(p.gate.holdMs * 0.001f * sr_);

        const float compThresh = dbToLin(p.comp.thresholdDb);
        const float compAtk = timeCoeff(p.comp.attackMs, sr_);
        const float compRel = timeCoeff(p.comp.releaseMs, sr_);
        const float makeup = dbToLin(p.comp.makeupDb);
        const float slope = p.comp.ratio > 1.0f ? (1.0f - 1.0f / p.comp.ratio) : 0.0f;

        // The detector follows the peak, so a fast transient is caught rather
        // than averaged away.
        const float detRel = timeCoeff(20.0f, sr_);

        for (size_t i = 0; i < frames; ++i) {
            float peak = 0.0f;
            for (unsigned c = 0; c < channels; ++c) {
                peak = (std::max)(peak, std::fabs(samples[i * channels + c]));
            }
            env_ = peak > env_ ? peak : (env_ * detRel + peak * (1.0f - detRel));

            float g = 1.0f;

            if (p.gate.enabled) {
                if (env_ >= gateThresh) {
                    open_ = true;
                    holdLeft_ = holdSamples;
                } else if (holdLeft_ > 0) {
                    --holdLeft_;
                } else {
                    open_ = false;
                }
                const float target = open_ ? 1.0f : 0.0f;
                const float coeff = target > gateGain_ ? gateAtk : gateRel;
                gateGain_ = target + (gateGain_ - target) * coeff;
                g *= gateGain_;
            } else {
                gateGain_ = 1.0f;
            }

            if (p.comp.enabled) {
                float target = 1.0f;
                if (env_ > compThresh && env_ > 1e-9f) {
                    // Above the threshold, gain falls by the overshoot scaled
                    // by how much of it the ratio removes.
                    const float overDb = 20.0f * std::log10(env_ / compThresh);
                    target = dbToLin(-overDb * slope);
                }
                const float coeff = target < compGain_ ? compAtk : compRel;
                compGain_ = target + (compGain_ - target) * coeff;
                g *= compGain_ * makeup;
            } else {
                compGain_ = 1.0f;
            }

            for (unsigned c = 0; c < channels; ++c) {
                float v = samples[i * channels + c] * g;
                // Never let makeup gain push the virtual microphone into
                // clipping; applications downstream cannot recover from it.
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                samples[i * channels + c] = v;
            }
        }
    }

    // Current gain in dB, for metering. 0 means untouched.
    float gateReductionDb() const {
        return gateGain_ > 0.0001f ? 20.0f * std::log10(gateGain_) : -80.0f;
    }
    float compReductionDb() const {
        return compGain_ > 0.0001f ? 20.0f * std::log10(compGain_) : -80.0f;
    }

private:
    double sr_ = 48000.0;
    float env_ = 0.0f;
    float gateGain_ = 0.0f;
    float compGain_ = 1.0f;
    int holdLeft_ = 0;
    bool open_ = false;
};

// Everything a channel does to its audio that is not the equaliser: stereo
// placement, a delay for lining it up against video, a safety limiter, and
// ducking under the microphone.
struct MixParams {
    bool mono = false;
    float balance = 0.0f;     // -1 hard left .. +1 hard right
    float delayMs = 0.0f;     // 0..250, for lining a channel up with video
    bool limiter = false;     // brick wall just below full scale
    bool duck = false;        // pull down while the microphone is open
    float duckDb = -12.0f;    // how far down
    float duckReleaseMs = 400.0f;

    bool operator==(const MixParams& o) const {
        return std::memcmp(this, &o, sizeof(MixParams)) == 0;
    }
    bool operator!=(const MixParams& o) const { return !(*this == o); }
};

// A channel's post-equaliser stage. Holds the delay line and the smoothed
// duck gain, so it has to be per-channel state rather than a free function.
class MixChain {
public:
    void prepare(double sampleRate) {
        sr_ = sampleRate;
        // 250 ms of stereo, the most the delay control offers.
        delay_.assign(static_cast<size_t>(sampleRate * 0.25) * 2 + 2, 0.0f);
        write_ = 0;
        duckGain_ = 1.0f;
        limGain_ = 1.0f;
    }

    // Interleaved, in place. `activity` is 0..1 from the microphone; a channel
    // with ducking off ignores it.
    void process(const MixParams& p, float activity,
                 float* samples, size_t frames, unsigned channels) {
        // A channel with nothing switched on must be bit-transparent, not
        // merely close, and must not pay for a per-sample loop to prove it.
        if (!p.mono && !p.limiter && !p.duck &&
            p.balance == 0.0f && p.delayMs < 0.5f) {
            duckGain_ = 1.0f;
            limGain_ = 1.0f;
            return;
        }

        // Ducking first: it is a level move, and doing it before the limiter
        // means the limiter sees what will actually be heard.
        // Interpolated in dB, not in amplitude: half activity should be half
        // the reduction the user asked for, which is what the number on the
        // control says.
        const float duckTarget = p.duck ? dbToLin(p.duckDb * clamp01(activity)) : 1.0f;
        // Attack is deliberately quicker than release: catching the start of a
        // word matters, and coming back up slowly avoids pumping between them.
        const float atk = timeCoeff(40.0f, sr_);
        const float rel = timeCoeff((std::max)(p.duckReleaseMs, 10.0f), sr_);

        const float bal = std::clamp(p.balance, -1.0f, 1.0f);
        const float lGain = bal > 0.0f ? 1.0f - bal : 1.0f;
        const float rGain = bal < 0.0f ? 1.0f + bal : 1.0f;

        const size_t delaySamples =
            static_cast<size_t>(std::clamp(p.delayMs, 0.0f, 250.0f) * 0.001f * sr_) * channels;
        const bool useDelay = delaySamples > 0 && delay_.size() > delaySamples + channels;

        // -0.3 dBFS: a hair of headroom for whatever resamples this later.
        const float ceiling = 0.9660f;
        const float limRel = timeCoeff(80.0f, sr_);

        for (size_t i = 0; i < frames; ++i) {
            float* f = samples + i * channels;

            if (p.mono && channels >= 2) {
                const float m = (f[0] + f[1]) * 0.5f;
                f[0] = f[1] = m;
            }
            if (channels >= 2 && bal != 0.0f) {
                f[0] *= lGain;
                f[1] *= rGain;
            }

            const float c = duckTarget < duckGain_ ? atk : rel;
            duckGain_ = duckTarget + (duckGain_ - duckTarget) * c;
            if (p.duck) {
                for (unsigned ch = 0; ch < channels; ++ch) f[ch] *= duckGain_;
            }

            if (useDelay) {
                for (unsigned ch = 0; ch < channels; ++ch) {
                    const size_t w = (write_ + ch) % delay_.size();
                    const size_t r = (write_ + ch + delay_.size() - delaySamples) % delay_.size();
                    const float out = delay_[r];
                    delay_[w] = f[ch];
                    f[ch] = out;
                }
                write_ = (write_ + channels) % delay_.size();
            }

            if (p.limiter) {
                float peak = 0.0f;
                for (unsigned ch = 0; ch < channels; ++ch) peak = (std::max)(peak, std::fabs(f[ch]));
                const float want = peak * limGain_ > ceiling ? ceiling / (std::max)(peak, 1e-6f) : 1.0f;
                // Down instantly, back up gently -- a limiter that releases
                // fast is a distortion box.
                limGain_ = want < limGain_ ? want : want + (limGain_ - want) * limRel;
                for (unsigned ch = 0; ch < channels; ++ch) f[ch] *= limGain_;
            } else {
                limGain_ = 1.0f;
            }
        }
    }

    float duckReductionDb() const {
        return duckGain_ > 0.0001f ? 20.0f * std::log10(duckGain_) : -80.0f;
    }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    std::vector<float> delay_;
    size_t write_ = 0;
    float duckGain_ = 1.0f;
    float limGain_ = 1.0f;
    double sr_ = 48000.0;
};

}  // namespace dsp
