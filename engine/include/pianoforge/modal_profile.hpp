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
struct DamperCurves {
    static constexpr int kMaxPoints = 512;
    double stepSeconds = 0.01;
    std::vector<int> notes;        // ascending
    std::vector<double> holds;     // seconds, ascending
    std::vector<double> logHolds;
    int length = 0;                // points per curve (<= kMaxPoints)
    std::vector<float> db;         // [note][hold][point]

    bool empty() const noexcept { return notes.empty() || holds.empty() || length == 0; }
    /// Writes `length` linear gains. No allocation (audio thread).
    void evaluate(int note, double holdSeconds, float* gains) const noexcept;
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

    double residualRate = 44100.0;
    std::vector<double> bodyFreq, bodyDecay;     // shared soundboard resonances
    DamperCurves dampers;
    std::vector<Note> notes;
    std::array<int, 128> noteIndex {};           // index in notes, -1 when the note is missing
    int maxModesPerLayer = 0;
    int maxPartialsPerLayer = 0;
    std::uint64_t bytes = 0;

    const Note* find(int midi) const noexcept
    {
        return midi >= 0 && midi < 128 && noteIndex[(std::size_t) midi] >= 0 ? &notes[(std::size_t) noteIndex[(std::size_t) midi]]
                                                                              : nullptr;
    }
};

/// Loads modal-profile.bin. Returns nullptr and fills `error` on failure.
std::shared_ptr<const ModalProfile> loadModalProfile(const std::filesystem::path& path, std::string& error);

} // namespace pf
