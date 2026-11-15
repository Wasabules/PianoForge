#pragma once

#include <atomic>

namespace pf {

/// A gentle stereo compressor (both channels by the louder one), for what goes to a stream: a piano's dynamics are
/// wide, and listeners' speakers are small. Feed-forward, peak detector, a soft knee, the gain smoothed in dB
/// (attack, release), make-up gain. Off: the sound passes untouched.
class Compressor {
public:
    struct Settings {
        bool enabled = false;
        float thresholdDb = -24.0f; // dBFS
        float ratio = 3.0f;         // above the threshold, 1 dB out for `ratio` dB in
        float kneeDb = 6.0f;        // the knee's width
        float attackMs = 10.0f;
        float releaseMs = 200.0f;
        float makeupDb = 6.0f;
    };

    void prepare(double sampleRate) noexcept;
    void applySettings(const Settings& s) noexcept; // any thread; read at the next block
    Settings settings() const noexcept;
    void reset() noexcept { reductionDb = heldDb = 0.0f; }
    bool isOn() const noexcept { return enabled.load(std::memory_order_relaxed); }
    void process(float* left, float* right, int n) noexcept;
    /// The gain reduction at the end of the last block (dB, >= 0), for the interface.
    float reduction() const noexcept { return lastReduction.load(std::memory_order_relaxed); }
    /// The static curve: the gain (dB, <= 0 before make-up) for an input level (dBFS).
    static float gainDb(float inputDb, const Settings& s) noexcept;

private:
    std::atomic<bool> enabled { false };
    std::atomic<float> thresholdDb { -24.0f }, ratio { 3.0f }, kneeDb { 6.0f }, attackMs { 10.0f }, releaseMs { 200.0f },
        makeupDb { 6.0f };
    std::atomic<float> lastReduction { 0.0f };
    double fs = 48000.0;
    float heldDb = 0.0f, reductionDb = 0.0f; // the reduction held from peak to peak, then smoothed (audio thread)
};

} // namespace pf
