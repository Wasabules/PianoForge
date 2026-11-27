// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace pf {

/// A safety limiter at the output: transparent until a peak would pass the ceiling, and with no latency (no
/// look-ahead: the instrument is played live). One gain for both channels (the image does not move): the sample
/// that would pass the ceiling is brought exactly to it, the gain is held 5 ms, then comes back with a 100 ms time
/// constant, never letting a later sample pass the ceiling. While it does not act, the input passes bit for bit.
/// Switched off, it passes the input untouched. (Inter-sample peaks are not looked for: the ceiling, -1 dBFS by
/// default, leaves room for them.)
///
/// Settings from any thread (atomics); prepare() outside the audio thread; process() allocates nothing and runs
/// sample by sample (the result does not depend on the block size).
class Limiter {
public:
    std::atomic<bool> enabled { true };
    std::atomic<float> ceilingDb { -1.0f };

    void prepare(double sampleRate) noexcept
    {
        fs = sampleRate;
        holdSamples = (int) std::lround(0.005 * fs);
        release = (float) std::exp(-1.0 / (0.1 * fs));
        reset();
    }

    void reset() noexcept
    {
        gain = 1.0f;
        hold = 0;
    }

    void process(float* left, float* right, int n) noexcept
    {
        if (!enabled.load(std::memory_order_relaxed) || fs <= 0.0) {
            reset();
            return;
        }
        const float ceiling = std::pow(10.0f, std::clamp(ceilingDb.load(std::memory_order_relaxed), -24.0f, 0.0f) / 20.0f);
        float deepest = 1.0f;
        for (int k = 0; k < n; ++k) {
            const float peak = std::max(std::abs(left[k]), right != nullptr ? std::abs(right[k]) : 0.0f);
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;
            if (needed < gain) {
                gain = needed; // at once: this sample at the ceiling
                hold = holdSamples;
            } else if (hold > 0) {
                --hold;
            } else if (gain < 1.0f) {
                gain = std::min(1.0f - (1.0f - gain) * release, needed);
                if (gain > 0.9995f && needed >= 1.0f) // within 0.004 dB: unity, and the input bit for bit again
                    gain = 1.0f;
            }
            if (gain < 1.0f) {
                left[k] *= gain;
                if (right != nullptr)
                    right[k] *= gain;
                deepest = std::min(deepest, gain);
            }
        }
        if (deepest < 1.0f) { // the deepest reduction since the interface last looked (dB)
            const float db = 20.0f * std::log10(deepest);
            float seen = reduction.load(std::memory_order_relaxed);
            while (db < seen && !reduction.compare_exchange_weak(seen, db, std::memory_order_relaxed)) {
            }
            activations.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// Interface: the deepest gain reduction (dB, <= 0) since the last call, then back to 0.
    float takeReduction() noexcept { return reduction.exchange(0.0f, std::memory_order_relaxed); }
    /// How many blocks it acted on, ever.
    unsigned activationsCount() const noexcept { return activations.load(std::memory_order_relaxed); }

private:
    double fs = 0.0;
    float gain = 1.0f, release = 0.0f;
    int hold = 0, holdSamples = 0;
    std::atomic<float> reduction { 0.0f };
    std::atomic<unsigned> activations { 0 };
};

} // namespace pf
