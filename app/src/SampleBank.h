// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <pianoforge/modal_profile.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pf::app {

/// One take of the reference corpus, held in memory as interleaved 16-bit stereo.
/// The signal is normalized to its own peak (and dithered): quantization stays 96 dB below the take's
/// peak, even for a very soft note.
struct Sample {
    int note = 0;
    int velocity = 0;
    float peak = 0.0f;              // original linear peak: scale factor back to the original level
    float releaseDecay40 = -1.0f;   // seconds to drop 40 dB after the key release; < 0: string not damped
    std::int64_t length = 0;        // in frames
    std::int64_t noteOnFrame = 0;   // where the note-on falls in the take (the bench records a short pre-roll)
    std::vector<std::int16_t> pcm;
    juce::File file;
};

/// Damper curves measured by the forge (release-curves.json next to the corpus passes): level after the key
/// release relative to the same note held, in dB, for a few measured notes, hold durations and velocities. The
/// modal engine's (its overall curves; the groups of partials have no meaning for a recording).
using ReleaseCurves = pf::DamperCurves;

class SampleBank {
public:
    double sampleRate = 0.0;
    std::vector<Sample> samples;
    std::array<std::vector<int>, 128> byNote; // indices of a note's samples, sorted by velocity
    ReleaseCurves releaseCurves;              // empty: the player falls back to an exponential decay

    /// Sample whose rendered velocity is the closest to `velocity`; -1 if the note has none.
    int pick(int note, int velocity) const noexcept;
    std::uint64_t bytes() const noexcept;
};

/// Loads one pass of the corpus (JSON + FLAC files written by the bench) on several threads, in the background.
class BankLoader final : private juce::Thread {
public:
    using Filter = std::function<bool(int note, int velocity)>;

    BankLoader();
    ~BankLoader() override;

    void load(const juce::File& folder, Filter filter = {});
    bool isLoading() const noexcept { return isThreadRunning(); }
    float progress() const noexcept;
    std::unique_ptr<SampleBank> takeResult();
    juce::String status() const;

private:
    void run() override;

    juce::File folder;
    Filter filter;
    std::atomic<int> done { 0 }, total { 0 };
    std::unique_ptr<SampleBank> result;
    mutable juce::CriticalSection lock;
    juce::String message;
};

} // namespace pf::app
