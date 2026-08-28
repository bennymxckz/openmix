#pragma once
// Per-channel DSP: a high-pass and three parametric EQ bands.
//
// Coefficients follow the RBJ audio EQ cookbook, and each biquad runs as
// direct-form II transposed, which is the numerically better-behaved form for
// float and needs only two state words per channel.

#include <cmath>
#include <cstring>

namespace dsp {

constexpr double kPi = 3.14159265358979323846;

struct Band {
    float freq = 1000.0f;
    float gainDb = 0.0f;
    float q = 0.707f;
    bool on = false;
};

// What the user sees and edits. Written from the UI thread, read by the audio
// thread; a torn read costs at most one block of slightly wrong coefficients,
// which is inaudible and self-corrects on the next block.
struct EqParams {
    bool enabled = false;
    Band hp{80.0f, 0.0f, 0.707f, false};      // high-pass, gain unused
    Band low{200.0f, 0.0f, 0.707f, true};     // low shelf
    Band mid{1000.0f, 0.0f, 1.0f, true};      // peaking
    Band high{6000.0f, 0.0f, 0.707f, true};   // high shelf

    bool operator==(const EqParams& o) const {
        return std::memcmp(this, &o, sizeof(EqParams)) == 0;
    }
    bool operator!=(const EqParams& o) const { return !(*this == o); }
};

enum class Kind { HighPass, LowShelf, Peaking, HighShelf };

class Biquad {
public:
    void setBypass() { bypass_ = true; }

    void design(Kind kind, double sampleRate, double freq, double q, double gainDb) {
        bypass_ = false;
        freq = freq < 20.0 ? 20.0 : (freq > sampleRate * 0.45 ? sampleRate * 0.45 : freq);
        if (q < 0.1) q = 0.1;

        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * freq / sampleRate;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);

        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (kind) {
            case Kind::HighPass:
                b0 =  (1.0 + cw) / 2.0;
                b1 = -(1.0 + cw);
                b2 =  (1.0 + cw) / 2.0;
                a0 =   1.0 + alpha;
                a1 =  -2.0 * cw;
                a2 =   1.0 - alpha;
                break;
            case Kind::Peaking:
                b0 =  1.0 + alpha * A;
                b1 = -2.0 * cw;
                b2 =  1.0 - alpha * A;
                a0 =  1.0 + alpha / A;
                a1 = -2.0 * cw;
                a2 =  1.0 - alpha / A;
                break;
            case Kind::LowShelf: {
                const double s = 2.0 * std::sqrt(A) * alpha;
                b0 =      A * ((A + 1.0) - (A - 1.0) * cw + s);
                b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
                b2 =      A * ((A + 1.0) - (A - 1.0) * cw - s);
                a0 =           (A + 1.0) + (A - 1.0) * cw + s;
                a1 = -2.0 *    ((A - 1.0) + (A + 1.0) * cw);
                a2 =           (A + 1.0) + (A - 1.0) * cw - s;
                break;
            }
            case Kind::HighShelf: {
                const double s = 2.0 * std::sqrt(A) * alpha;
                b0 =      A * ((A + 1.0) + (A - 1.0) * cw + s);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
                b2 =      A * ((A + 1.0) + (A - 1.0) * cw - s);
                a0 =           (A + 1.0) - (A - 1.0) * cw + s;
                a1 =  2.0 *    ((A - 1.0) - (A + 1.0) * cw);
                a2 =           (A + 1.0) - (A - 1.0) * cw - s;
                break;
            }
        }

        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
    }

    void reset() {
        for (int c = 0; c < 2; ++c) z1_[c] = z2_[c] = 0.0f;
    }

    // Magnitude response at one frequency, from the coefficients actually in
    // use. Evaluating the transfer function beats approximating the curve
    // from the parameters: what gets drawn is then what is being heard.
    double magnitudeAt(double freq, double sampleRate) const {
        if (bypass_) return 1.0;
        const double w = 2.0 * kPi * freq / sampleRate;
        const double cw = std::cos(w), sw = std::sin(w);
        const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);

        const double nr = b0_ + b1_ * cw + b2_ * c2;
        const double ni = -(b1_ * sw + b2_ * s2);
        const double dr = 1.0 + a1_ * cw + a2_ * c2;
        const double di = -(a1_ * sw + a2_ * s2);

        const double den = dr * dr + di * di;
        if (den < 1e-20) return 1.0;
        return std::sqrt((nr * nr + ni * ni) / den);
    }

    inline float process(float x, int ch) {
        if (bypass_) return x;
        const float y = b0_ * x + z1_[ch];
        z1_[ch] = b1_ * x - a1_ * y + z2_[ch];
        z2_[ch] = b2_ * x - a2_ * y;
        return y;
    }

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_[2]{}, z2_[2]{};
    bool bypass_ = true;
};

// One channel's filter chain. Redesigns itself only when the parameters
// actually change, so the audio thread does no trigonometry per block.
class ChannelStrip {
public:
    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& b : bands_) b.reset();
        applied_ = EqParams{};
        applied_.mid.freq = -1.0f;   // force a rebuild on the first block
    }

    // Interleaved stereo, in place. Safe to call with EQ disabled.
    void process(const EqParams& p, float* samples, size_t frames, unsigned channels) {
        if (!p.enabled) {
            if (dirty_) { reset(); dirty_ = false; }
            return;
        }
        if (p != applied_) {
            rebuild(p);
            applied_ = p;
        }
        dirty_ = true;

        for (size_t i = 0; i < frames; ++i) {
            for (unsigned c = 0; c < channels && c < 2; ++c) {
                float v = samples[i * channels + c];
                for (auto& b : bands_) v = b.process(v, static_cast<int>(c));
                samples[i * channels + c] = v;
            }
        }
    }

    void reset() {
        for (auto& b : bands_) b.reset();
    }

    // Combined response of the chain in dB, for drawing. Designs a scratch
    // chain from the given parameters so the curve can be shown for a channel
    // that is not currently processing audio.
    static double responseDb(const EqParams& p, double freq, double sampleRate) {
        if (!p.enabled) return 0.0;
        Biquad b;
        double mag = 1.0;
        if (p.hp.on)   { b.design(Kind::HighPass,  sampleRate, p.hp.freq,   p.hp.q,   0.0); mag *= b.magnitudeAt(freq, sampleRate); }
        if (p.low.on)  { b.design(Kind::LowShelf,  sampleRate, p.low.freq,  p.low.q,  p.low.gainDb); mag *= b.magnitudeAt(freq, sampleRate); }
        if (p.mid.on)  { b.design(Kind::Peaking,   sampleRate, p.mid.freq,  p.mid.q,  p.mid.gainDb); mag *= b.magnitudeAt(freq, sampleRate); }
        if (p.high.on) { b.design(Kind::HighShelf, sampleRate, p.high.freq, p.high.q, p.high.gainDb); mag *= b.magnitudeAt(freq, sampleRate); }
        return mag > 1e-9 ? 20.0 * std::log10(mag) : -60.0;
    }

private:
    void rebuild(const EqParams& p) {
        if (p.hp.on)   bands_[0].design(Kind::HighPass,  sampleRate_, p.hp.freq,   p.hp.q,   0.0);
        else           bands_[0].setBypass();
        if (p.low.on)  bands_[1].design(Kind::LowShelf,  sampleRate_, p.low.freq,  p.low.q,  p.low.gainDb);
        else           bands_[1].setBypass();
        if (p.mid.on)  bands_[2].design(Kind::Peaking,   sampleRate_, p.mid.freq,  p.mid.q,  p.mid.gainDb);
        else           bands_[2].setBypass();
        if (p.high.on) bands_[3].design(Kind::HighShelf, sampleRate_, p.high.freq, p.high.q, p.high.gainDb);
        else           bands_[3].setBypass();
    }

    Biquad bands_[4];
    EqParams applied_{};
    double sampleRate_ = 48000.0;
    bool dirty_ = false;
};

}  // namespace dsp
