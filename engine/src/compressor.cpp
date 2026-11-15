#include "pianoforge/compressor.hpp"

#include <algorithm>
#include <cmath>

namespace pf {

void Compressor::prepare(double sampleRate) noexcept
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void Compressor::applySettings(const Settings& s) noexcept
{
    thresholdDb = std::clamp(s.thresholdDb, -60.0f, 0.0f);
    ratio = std::clamp(s.ratio, 1.0f, 20.0f);
    kneeDb = std::clamp(s.kneeDb, 0.0f, 24.0f);
    attackMs = std::clamp(s.attackMs, 0.1f, 500.0f);
    releaseMs = std::clamp(s.releaseMs, 5.0f, 5000.0f);
    makeupDb = std::clamp(s.makeupDb, 0.0f, 30.0f);
    enabled = s.enabled;
}

Compressor::Settings Compressor::settings() const noexcept
{
    Settings s;
    s.enabled = enabled.load();
    s.thresholdDb = thresholdDb.load();
    s.ratio = ratio.load();
    s.kneeDb = kneeDb.load();
    s.attackMs = attackMs.load();
    s.releaseMs = releaseMs.load();
    s.makeupDb = makeupDb.load();
    return s;
}

float Compressor::gainDb(float x, const Settings& s) noexcept
{
    const float over = x - s.thresholdDb, slope = 1.0f / std::max(s.ratio, 1.0f) - 1.0f; // <= 0
    if (s.kneeDb > 0.0f && std::abs(over) <= 0.5f * s.kneeDb) { // the knee: quadratic from 0 to the full slope
        const float t = over + 0.5f * s.kneeDb;
        return slope * t * t / (2.0f * s.kneeDb);
    }
    return over > 0.0f ? slope * over : 0.0f;
}

void Compressor::process(float* left, float* right, int n) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || left == nullptr || n <= 0) {
        reductionDb = heldDb = 0.0f;
        lastReduction.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const Settings s = settings();
    const float attack = (float) std::exp(-1.0 / (0.001 * s.attackMs * fs));
    const float release = (float) std::exp(-1.0 / (0.001 * s.releaseMs * fs));
    // The smooth decoupled peak detector (Giannoulis, Massberg, Reiss 2012), on the reduction wanted: held from one
    // peak of a wave to the next by the release, then smoothed by the attack.
    float held = heldDb, r = reductionDb;
    for (int k = 0; k < n; ++k) {
        const float peak = std::max(std::abs(left[k]), right != nullptr ? std::abs(right[k]) : 0.0f);
        const float level = 20.0f * std::log10(std::max(peak, 1e-9f));
        const float wanted = -gainDb(level, s); // >= 0
        held = std::max(wanted, release * held + (1.0f - release) * wanted);
        r = attack * r + (1.0f - attack) * held;
        const float g = std::pow(10.0f, (s.makeupDb - r) / 20.0f);
        left[k] *= g;
        if (right != nullptr)
            right[k] *= g;
    }
    heldDb = held;
    reductionDb = r;
    lastReduction.store(r, std::memory_order_relaxed);
}

} // namespace pf
