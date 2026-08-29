#pragma once
// Spectral noise suppression for the microphone.
//
// A noise gate can only choose between "all of it" and "none of it". It does
// nothing about a fan or a keyboard while you are actually talking, which is
// exactly when the noise is most audible to everyone else. This works on the
// spectrum instead: estimate what the room sounds like when nobody is
// speaking, then take that much out of every frame, bin by bin.
//
// Short-time Fourier transform, Wiener-style gains, overlap-add. The noise
// estimate is a running per-bin minimum, which needs no "be quiet for three
// seconds" step: speech is intermittent enough that the minimum over a couple
// of seconds is the noise floor whether or not anyone is talking.
//
// Costs one window of latency -- 512 samples, about 10.7 ms at 48 kHz.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dsp {

struct DenoiseParams {
    bool enabled = false;
    // 0..1. How much of the estimated noise to take out, and how far a bin is
    // allowed to fall. Past about 0.8 the artefacts cost more than the noise
    // did.
    float strength = 0.5f;

    bool operator==(const DenoiseParams& o) const {
        return std::memcmp(this, &o, sizeof(DenoiseParams)) == 0;
    }
    bool operator!=(const DenoiseParams& o) const { return !(*this == o); }
};

class NoiseSuppressor {
public:
    static constexpr size_t kFft = 512;
    static constexpr size_t kHop = kFft / 2;
    static constexpr size_t kBins = kFft / 2 + 1;
    static constexpr size_t kRing = kFft * 2;

    void prepare(double sampleRate) {
        sr_ = sampleRate;
        window_.resize(kFft);
        for (size_t i = 0; i < kFft; ++i) {
            // The square root of a Hann window, because it is applied twice --
            // once on analysis and once on synthesis. Their product is a plain
            // Hann, which at 50% overlap sums to exactly one, so overlap-add
            // needs no normalisation. Windowing twice with a full Hann would
            // sum to 0.5(1 + cos^2) instead: a 6 dB ripple at the hop rate,
            // which reads as roughly 2.5 dB of level quietly lost.
            const float hann = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f *
                                                      static_cast<float>(i) /
                                                      static_cast<float>(kFft));
            window_[i] = std::sqrt(hann);
        }
        buildTwiddles();
        reset();
    }

    void reset() {
        in_.assign(kFft, 0.0f);
        smoothPow_.assign(kBins, 0.0f);
        accum_.assign(kHop, 0.0f);
        wet_.assign(kRing, 0.0f);
        dry_.assign(kRing * 2, 0.0f);      // stereo worst case
        noise_.assign(kBins, 0.0f);
        minTrack_.assign(kBins, 0.0f);
        gain_.assign(kBins, 1.0f);
        filled_ = 0;
        cursor_ = 0;
        frames_ = 0;
        reduction_ = 1.0f;
    }

    // Interleaved, in place. One gain mask is computed from the channel sum
    // and applied to both, so the stereo image cannot wobble as the mask
    // moves. What leaves is what arrived a window ago; the delay is carried
    // here so nothing upstream has to know about it.
    void process(const DenoiseParams& p, float* samples, size_t frames, unsigned channels) {
        if (!p.enabled) {
            if (frames_ != 0) reset();
            return;
        }
        if (window_.empty()) prepare(sr_);
        // Sized to whatever arrives rather than clamped to stereo: clamping
        // would change the stride the loop below indexes with, which reads
        // every frame at the wrong offset.
        if (dry_.size() < kRing * channels) dry_.assign(kRing * channels, 0.0f);

        for (size_t i = 0; i < frames; ++i) {
            float* f = samples + i * channels;

            float mono = 0.0f;
            for (unsigned c = 0; c < channels; ++c) {
                dry_[cursor_ * channels + c] = f[c];
                mono += f[c];
            }
            mono /= static_cast<float>(channels);
            in_[filled_++] = mono;

            // One window back is where the finished audio is.
            const size_t rd = (cursor_ + kRing - kFft) % kRing;
            const float* orig = dry_.data() + rd * channels;
            float ref = 0.0f;
            for (unsigned c = 0; c < channels; ++c) ref += orig[c];
            ref /= static_cast<float>(channels);
            // Applied as a ratio, so only the level moves and never the
            // balance. A suppressor never adds gain, hence the clamp.
            const float g = std::fabs(ref) > 1e-7f
                                ? std::clamp(wet_[rd] / ref, 0.0f, 1.0f)
                                : 1.0f;
            for (unsigned c = 0; c < channels; ++c) f[c] = orig[c] * g;

            cursor_ = (cursor_ + 1) % kRing;

            if (filled_ == kFft) {
                analyse(p);
                std::memmove(in_.data(), in_.data() + kHop, kHop * sizeof(float));
                filled_ = kHop;
            }
        }
    }

    // Average gain across the spectrum, in dB. Negative means noise is being
    // taken out; near zero means there was nothing to take.
    float reductionDb() const {
        return reduction_ > 0.0001f ? 20.0f * std::log10(reduction_) : -60.0f;
    }

private:
    void analyse(const DenoiseParams& p) {
        for (size_t i = 0; i < kFft; ++i) {
            re_[i] = in_[i] * window_[i];
            im_[i] = 0.0f;
        }
        fft(false);

        const float strength = std::clamp(p.strength, 0.0f, 1.0f);
        // Over-subtraction: taking out exactly the estimate leaves an audible
        // residue, so a little more comes out as the strength rises.
        const float over = 1.0f + strength * 1.5f;
        // A floor, so a suppressed bin fades rather than vanishing. A hard
        // zero is what makes spectral subtraction sound like birdsong.
        const float floorGain = std::pow(10.0f, (-6.0f - strength * 18.0f) / 20.0f);
        // The minimum rises about 6 dB a second in power, so a held vowel is
        // not learnt as noise, and falls at once so it keeps up when the room
        // gets quieter. The additive floor is what lets it climb again after
        // true silence, which a purely multiplicative rise never could.
        const float rise = 1.0f + 1.4f / static_cast<float>(sr_ / kHop);

        float gainSum = 0.0f;
        for (size_t b = 0; b < kBins; ++b) {
            const float power = re_[b] * re_[b] + im_[b] * im_[b];
            const float mag = std::sqrt(power);

            // Smoothing the power before taking the minimum is what makes the
            // minimum usable: a raw bin fluctuates wildly frame to frame, and
            // its minimum lands far below its mean.
            if (frames_ < 4) smoothPow_[b] = power;
            else smoothPow_[b] = smoothPow_[b] * 0.85f + power * 0.15f;

            if (frames_ < 4) minTrack_[b] = smoothPow_[b];
            else if (smoothPow_[b] < minTrack_[b]) minTrack_[b] = smoothPow_[b];
            else minTrack_[b] = (std::max)(minTrack_[b] * rise, 1e-14f);

            // Even smoothed, a minimum still sits under the mean it stands
            // for, so it is corrected back up. This is the bias compensation
            // every minimum-statistics estimator needs; without it the
            // suppressor barely does anything.
            const float noisePow = minTrack_[b] * 2.6f;
            noise_[b] = noise_[b] * 0.9f + std::sqrt(noisePow) * 0.1f;

            float g = mag > 1e-9f ? (mag - over * noise_[b]) / mag : 1.0f;
            g = std::clamp(g, floorGain, 1.0f);
            // Smoothed across time as well: a mask that jumps frame to frame
            // is exactly what musical noise is.
            gain_[b] = gain_[b] * 0.5f + g * 0.5f;
            gainSum += gain_[b];

            re_[b] *= gain_[b];
            im_[b] *= gain_[b];
            if (b > 0 && b < kFft / 2) {
                // Mirror, so the inverse transform comes back real.
                re_[kFft - b] =  re_[b];
                im_[kFft - b] = -im_[b];
            }
        }
        reduction_ = gainSum / static_cast<float>(kBins);
        ++frames_;

        fft(true);

        // The window just analysed covers the kFft samples ending at the write
        // cursor, so its first hop is exactly what the reader is about to
        // reach. The second hop is held back to be added to the next window.
        const size_t base = (cursor_ + kRing - kFft) % kRing;
        const float norm = 1.0f / static_cast<float>(kFft);
        for (size_t j = 0; j < kHop; ++j) {
            wet_[(base + j) % kRing] = accum_[j] + re_[j] * window_[j] * norm;
        }
        for (size_t j = 0; j < kHop; ++j) {
            accum_[j] = re_[kHop + j] * window_[kHop + j] * norm;
        }
    }

    void buildTwiddles() {
        cos_.resize(kFft / 2);
        sin_.resize(kFft / 2);
        for (size_t i = 0; i < kFft / 2; ++i) {
            const double a = -2.0 * 3.14159265358979 * static_cast<double>(i) /
                             static_cast<double>(kFft);
            cos_[i] = static_cast<float>(std::cos(a));
            sin_[i] = static_cast<float>(std::sin(a));
        }
    }

    // In-place iterative radix-2. 512 points at 187 frames a second is not
    // worth a dependency.
    void fft(bool inverse) {
        for (size_t i = 1, j = 0; i < kFft; ++i) {
            size_t bit = kFft >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re_[i], re_[j]); std::swap(im_[i], im_[j]); }
        }
        for (size_t len = 2; len <= kFft; len <<= 1) {
            const size_t step = kFft / len;
            for (size_t i = 0; i < kFft; i += len) {
                for (size_t k = 0; k < len / 2; ++k) {
                    const size_t t = k * step;
                    const float wr = cos_[t];
                    const float wi = inverse ? -sin_[t] : sin_[t];
                    const size_t a = i + k, b = i + k + len / 2;
                    const float xr = re_[b] * wr - im_[b] * wi;
                    const float xi = re_[b] * wi + im_[b] * wr;
                    re_[b] = re_[a] - xr; im_[b] = im_[a] - xi;
                    re_[a] += xr;         im_[a] += xi;
                }
            }
        }
    }

    double sr_ = 48000.0;
    std::vector<float> window_, in_, accum_, wet_, dry_, noise_, minTrack_, smoothPow_,
                   gain_, cos_, sin_;
    float re_[kFft]{}, im_[kFft]{};
    size_t filled_ = 0;
    size_t cursor_ = 0;
    unsigned frames_ = 0;
    float reduction_ = 1.0f;
};

}  // namespace dsp
