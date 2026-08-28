#pragma once
// Microphone dynamics: a noise gate and a compressor.
//
// Both work on the louder of the two channels rather than each independently,
// so a stereo microphone never has one side duck while the other stays open --
// that shifts the image and sounds worse than doing nothing.

#include <algorithm>
#include <cmath>
#include <cstring>

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

}  // namespace dsp
