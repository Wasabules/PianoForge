#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace pf {

/// A room around the piano: an algorithmic reverberation added to the dry sound, which goes through untouched (no
/// latency on the direct sound; only the reverberation has its pre-delay).
///
/// - Early reflections: an image-source model of a rectangular room (the 24 images of the first and second order),
///   from the piano to two microphones 0.6 m apart; the room's dimensions follow the size.
/// - Late reverberation: a feedback delay network of 16 lines (prime numbers of samples, following the size),
///   mixed by a Hadamard matrix (lossless). Each line's loss sets the decay time, and a first-order high shelf
///   (crossover 3 kHz) makes the highs decay faster (Jot's absorption filters): T60 = decaySeconds under the
///   crossover, decaySeconds x (0.15 + 0.85 brightness) over it (a one-pole low-pass, the usual choice, slopes
///   from DC to Nyquist and hardly shortens the 2 to 8 kHz where a piano's brilliance is). Each line's length is slowly modulated (0.25 to 1 Hz, +-0.125 ms: at most
///   1.4 cents of pitch, far under a chorus) so that the tail has no metallic ring; the fractional reads go through
///   first-order all-passes (unit gain: an interpolation inside the loop would damp the highs at every pass). The input is high-passed (60 Hz)
///   and diffused by four all-passes per channel before the network.
/// - Levels: the early energy is normalized, the late one is sqrt(decay / 2 s) x 3 times it (an estimate of the
///   network's energy, from the stored energy and its decay); levelDb sets the reverberation against the dry sound.
///
/// Settings from any thread (atomics, read at each block). prepare() allocates for the largest room at the given
/// rate; process() allocates nothing and runs sample by sample: the result does not depend on the block size. A
/// change of size or pre-delay fades the reverberation out and back in over 10 ms (the lines are then read at their
/// new lengths: no click, the tail goes on); the other settings glide over 20 ms. Switched off, the reverberation
/// fades out, then the room is cleared and the input passes through bit for bit. Asleep: once the input has been
/// silent long enough for everything to have left the lines, and a whole block of reverberation stays under
/// -120 dB, the room is cleared and costs nothing until the input comes back (it then starts from a clean state).
class Room {
public:
    static constexpr int kLines = 16;
    static constexpr int kTaps = 24;      // early reflections per microphone
    static constexpr int kDiffusers = 4;  // all-passes per channel
    static constexpr float kMaxPredelayMs = 100.0f;

    struct Settings {
        bool enabled = false;
        float levelDb = -16.0f;    // the reverberation against the dry sound
        float size = 0.65f;        // 0: a small room (4.7 x 3.4 m), 1: a large hall (21 x 15 m)
        float decaySeconds = 2.2f; // T60 at low frequencies
        float brightness = 0.5f;   // 1: the highs decay as long as the lows, 0: 0.15 times as long
        float predelayMs = 20.0f;
        float width = 1.0f;        // 0: a mono reverberation, 1: full width
    };

    std::atomic<bool> enabled { false };
    std::atomic<float> levelDb { -16.0f }, size { 0.65f }, decaySeconds { 2.2f }, brightness { 0.5f },
        predelayMs { 20.0f }, width { 1.0f };

    Settings settings() const noexcept;
    void applySettings(const Settings& s) noexcept;

    /// Outside the audio thread: allocates for the largest room at this rate, and clears.
    void prepare(double sampleRate);
    /// Audio thread (or before it runs): silence in every line.
    void reset() noexcept;
    /// Audio thread: adds the reverberation of (left, right) to them, in place.
    void process(float* left, float* right, int n) noexcept;
    /// How long the reverberation rings once the input stops (to about -80 dB), with the current settings.
    double tailSeconds() const noexcept { return tailSeconds(settings()); }
    static double tailSeconds(const Settings& s) noexcept;

private:
    struct Delay { // a power-of-two ring
        std::vector<float> data;
        std::uint32_t mask = 0;
        void allocate(std::size_t minimum);
        float at(std::uint32_t pos, std::uint32_t delay) const noexcept { return data[(pos - delay) & mask]; }
    };
    struct Config { // what changes with the size and the pre-delay (applied while the reverberation is faded out)
        float size = -1.0f, predelayMs = -1.0f;
        std::array<std::uint32_t, kLines> lineDelay {};
        std::array<std::uint32_t, kDiffusers> diffuserDelay {};
        std::array<std::uint32_t, kTaps> tapDelayL {}, tapDelayR {};
        std::array<float, kTaps> tapGainL {}, tapGainR {};
        std::uint32_t predelay = 0;
    };

    void configure(Config& c, float size, float predelayMs) const noexcept;
    void updateTargets() noexcept; // decay, brightness, level, width -> the gains and filters to glide to
    void readSettings() noexcept;

    double fs = 0.0;
    bool cleared = true, sleeping = false;
    std::uint64_t silentSamples = 0; // the input silent for this long
    std::uint32_t pos = 0;      // write position, every ring
    std::uint64_t samples = 0;  // since prepare / reset (LFO renormalization)
    Delay pre[2], mono;         // the high-passed input (stereo, for the network; mono, for the reflections)
    std::array<Delay, kLines> lines;
    std::array<Delay, kDiffusers> diffusers[2];
    Config cfg, pending;
    bool hasPending = false;
    float fade = 1.0f, fadeStep = 0.0f;
    float glide = 0.0f;         // per sample, toward the targets (20 ms)

    // Targets and current values.
    float wetTarget = 0.0f, wet = 0.0f, widthTarget = 1.0f, widthNow = 1.0f;
    float lateTarget = 0.0f, late = 0.0f;
    // Per line: the loss (gain under the crossover) and the high shelf (b0, b1, a1: unit gain under the crossover,
    // the highs' gain over it), targets and current values; the shelf's state, and the all-pass read's.
    struct Absorption {
        float gain = 1.0f, b0 = 1.0f, b1 = 0.0f, a1 = 0.0f;
    };
    std::array<Absorption, kLines> absTarget {}, absNow {};
    std::array<float, kLines> shelfX {}, shelfY {}, apY {};
    float appliedDecay = -1.0f, appliedBrightness = -1.0f;
    bool on = false;

    // Filters: input high-pass (per channel), reflections low-pass (per channel).
    float hpCoef = 0.0f, hpX[2] {}, hpY[2] {};
    float lpCoef = 0.0f, lpY[2] {};
    // Modulation: a phasor per line, and its rotation per sample.
    std::array<float, kLines> lfoC {}, lfoS {}, rotC {}, rotS {};
    float depth = 0.0f;         // samples
};

} // namespace pf
