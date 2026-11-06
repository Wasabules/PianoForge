#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pf {

/// Damper curves: level after the key release against the same note held, in dB, measured for a few notes and
/// hold durations (forge release-curves). evaluate() interpolates linearly between notes and logarithmically
/// between hold durations, clamped at both ends.
/// Per group of partials (by partial number: groupFirst[g] up to groupFirst[g + 1] - 1), the level against that
/// overall curve: the high partials die faster under the felt.
/// The same per measured velocity (the loud strikes die faster: their energy is in the high partials): with a
/// velocity, the curves are interpolated in dB between the measured velocities around it (clamped); without one,
/// or without curves per velocity, the average over the velocities.
struct DamperCurves {
    static constexpr int kMaxPoints = 512;
    static constexpr int kMaxGroups = 8;
    double stepSeconds = 0.01;
    std::vector<int> notes;        // ascending
    std::vector<double> holds;     // seconds, ascending
    std::vector<double> logHolds;
    int length = 0;                // points per curve (<= kMaxPoints)
    std::vector<float> db;         // [note][hold][point]
    std::vector<int> groupFirst;   // first partial number of each group (empty: no curves per group)
    std::vector<float> groupDb;    // [note][hold][group][point], against the overall curve
    std::vector<int> velocities;   // ascending (empty: no curves per velocity)
    std::vector<float> dbByVelocity;      // [velocity][note][hold][point]
    std::vector<float> groupDbByVelocity; // [velocity][note][hold][group][point], against that velocity's overall

    bool empty() const noexcept { return notes.empty() || holds.empty() || length == 0; }
    int groups() const noexcept { return (int) groupFirst.size(); }
    int groupOf(int partialNumber) const noexcept
    {
        int g = 0;
        while (g + 1 < groups() && partialNumber >= groupFirst[(std::size_t) g + 1])
            ++g;
        return g;
    }
    bool byVelocity() const noexcept
    {
        return !velocities.empty() && dbByVelocity.size() == velocities.size() * notes.size() * holds.size() * (std::size_t) length;
    }
    /// Writes `length` linear gains (velocity < 0: the average). No allocation (audio thread).
    void evaluate(int note, double holdSeconds, float* gains, int velocity = -1) const noexcept;
    /// Writes groups() x `length` linear gains, against the overall curve of the same velocity. No allocation.
    void evaluateGroups(int note, double holdSeconds, float* gains, int velocity = -1) const noexcept;
};

/// The modal profile written by `forge export` (modal-profile.bin): per note and velocity layer, the modes of
/// every partial, the soundboard body's amplitudes and the attack residual. See tools/forge/forge/export.py for
/// the file format. Read-only once loaded; shared between the engine and the loader.
struct ModalProfile {
    struct Mode {
        double freq = 0.0, decay = 0.0;          // Hz, 1/s
        std::complex<float> left, right;         // complex amplitude per channel at the note-on
    };
    struct Partial {
        int number = 0, firstMode = 0, modeCount = 0;
        float level = 0.0f;                      // RMS amplitude over the first second (interpolation weight)
        // Seconds from the note-on after which the partial is faded out (0: never). The forge sets it at the end of
        // the partial's analysis window when the modes would grow again past it, where the recording does not.
        float fadeSeconds = 0.0f;
    };
    struct Layer {
        int velocity = 0;
        std::vector<Partial> partials;           // ascending partial number
        std::vector<Mode> modes;
        std::vector<std::complex<float>> bodyLeft, bodyRight; // one per soundboard resonance
        float level = 0.0f;                      // RMS of the partial levels
        float bodyLevel = 0.0f;                  // RMS of the body amplitudes
        std::vector<float> residual;             // interleaved stereo, at residualRate
        int residualFrames = 0;
    };
    struct Note {
        int midi = 0;
        double f0 = 0.0;
        std::vector<Layer> layers;               // ascending velocity
    };
    /// A free string of the sympathetic resonance: one resonator per measured mode (forge sympathetic).
    struct Resonator {
        double freq = 0.0, decay = 0.0;          // Hz, 1/s
        float gain = 0.0f;                       // coupling: input gain per second
        std::complex<float> left, right;         // output weight per channel
    };
    struct String {
        int midi = 0;
        std::vector<Resonator> resonators;
    };

    double residualRate = 44100.0;
    std::vector<double> bodyFreq, bodyDecay;     // shared soundboard resonances
    DamperCurves dampers;
    std::vector<Note> notes;
    std::array<int, 128> noteIndex {};           // index in notes, -1 when the note is missing
    /// The knock of the key release on the soundboard (forge release-noise), per measured note and velocity:
    /// complex amplitudes per resonance at the instant the damper lands, and their level; and the level against
    /// the hold (the longer the key was held, the weaker the knock), interpolated in log(hold), clamped.
    struct ReleaseNoise {
        std::vector<int> notes, velocities;      // ascending
        std::vector<float> levelDb;              // [note][velocity]
        std::vector<std::complex<float>> left, right; // [note][velocity][resonance]
        std::vector<float> holds, holdDb;        // seconds (ascending), dB against the amplitudes (empty: none)
        bool empty() const noexcept { return notes.empty() || velocities.empty(); }
    };
    ReleaseNoise releaseNoise;
    std::vector<String> strings;                 // sympathetic resonance (empty: none)
    int stringResonators = 0;
    // Each voice drives the free strings with (1 - exp(-t / onset)) exp(-t / driveDecay) (velocity / reference) ^
    // exponent (driveDecay 0: no decay).
    float stringOnsetSeconds = 0.0f, stringVelocityExponent = 0.0f, stringReferenceVelocity = 64.0f;
    float stringDriveDecaySeconds = 0.0f;
    /// How far the sustain pedal lifts the dampers, per CC64 value: 1 lifted, 0 resting on the strings.
    /// Without a measured curve: a switch at 64.
    std::array<float, 128> pedalLift = switchPedal();
    int maxModesPerLayer = 0;
    int maxPartialsPerLayer = 0;
    std::uint64_t bytes = 0;

    static constexpr std::array<float, 128> switchPedal() noexcept
    {
        std::array<float, 128> lift {};
        for (std::size_t v = 64; v < lift.size(); ++v)
            lift[v] = 1.0f;
        return lift;
    }

    const Note* find(int midi) const noexcept
    {
        return midi >= 0 && midi < 128 && noteIndex[(std::size_t) midi] >= 0 ? &notes[(std::size_t) noteIndex[(std::size_t) midi]]
                                                                              : nullptr;
    }
};

/// Loads modal-profile.bin. Returns nullptr and fills `error` on failure.
std::shared_ptr<const ModalProfile> loadModalProfile(const std::filesystem::path& path, std::string& error);

} // namespace pf
