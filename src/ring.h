#pragma once
#include <atomic>
#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

// Lock-free single-producer / single-consumer float ring buffer.
// Capacity is rounded up to a power of two so index wrapping is a mask.
class FloatRing {
public:
    FloatRing() = default;

    // Buses are moved once, while the config is being built and before any
    // audio thread exists. Relaxed loads are fine at that point.
    FloatRing(FloatRing&& o) noexcept { *this = std::move(o); }
    FloatRing& operator=(FloatRing&& o) noexcept {
        buf_  = std::move(o.buf_);
        cap_  = o.cap_;
        mask_ = o.mask_;
        ch_   = o.ch_;
        r_.store(o.r_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        w_.store(o.w_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        peak_.store(o.peak_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    void reset(size_t frames, unsigned channels) {
        ch_ = channels;
        size_t want = frames * channels;
        size_t cap = 1;
        while (cap < want) cap <<= 1;
        cap_ = cap;
        mask_ = cap - 1;
        buf_.assign(cap_, 0.0f);
        r_.store(0, std::memory_order_relaxed);
        w_.store(0, std::memory_order_relaxed);
        peak_.store(0.0f, std::memory_order_relaxed);
    }

    unsigned channels() const { return ch_; }

    size_t readable() const {
        return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed);
    }

    // Highest absolute sample seen since the last call. Reading resets it.
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

    // Producer side. On overrun the oldest samples are dropped so the stream
    // stays live rather than accumulating latency.
    void write(const float* src, size_t n) {
        if (cap_ == 0) return;
        if (n > cap_) { src += (n - cap_); n = cap_; }

        float pk = peak_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            const float a = std::fabs(src[i]);
            if (a > pk) pk = a;
        }
        peak_.store(pk, std::memory_order_relaxed);

        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        size_t freeSamples = cap_ - (w - r);
        if (n > freeSamples) {
            r_.store(r + (n - freeSamples), std::memory_order_release);
        }
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = src[i];
        w_.store(w + n, std::memory_order_release);
    }

    // Consumer side: sums into dst rather than overwriting, so several rings
    // mix into one output block. Returns how many samples were available.
    size_t mixInto(float* dst, size_t n, float gain) {
        if (cap_ == 0) return 0;
        size_t r = r_.load(std::memory_order_relaxed);
        size_t avail = w_.load(std::memory_order_acquire) - r;
        size_t take = std::min(n, avail);
        if (gain != 0.0f) {
            for (size_t i = 0; i < take; ++i) dst[i] += buf_[(r + i) & mask_] * gain;
        }
        r_.store(r + take, std::memory_order_release);
        return take;
    }

    // Consumer side for the capture path: copies out rather than mixing, and
    // zero-fills any shortfall so an IN packet is always full length.
    size_t read(float* dst, size_t n) {
        if (cap_ == 0) { std::fill(dst, dst + n, 0.0f); return 0; }
        size_t r = r_.load(std::memory_order_relaxed);
        size_t avail = w_.load(std::memory_order_acquire) - r;
        size_t take = std::min(n, avail);
        for (size_t i = 0; i < take; ++i) dst[i] = buf_[(r + i) & mask_];
        if (take < n) std::fill(dst + take, dst + n, 0.0f);
        r_.store(r + take, std::memory_order_release);
        return take;
    }

    // Bound the backlog. Capture and render run on independent clocks, so
    // without this the queue grows until latency is audible.
    void trimTo(size_t keep) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t avail = w_.load(std::memory_order_acquire) - r;
        if (avail > keep) r_.store(r + (avail - keep), std::memory_order_release);
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0, mask_ = 0;
    unsigned ch_ = 2;
    std::atomic<size_t> r_{0}, w_{0};
    std::atomic<float> peak_{0.0f};
};
