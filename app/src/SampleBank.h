#pragma once

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
    std::vector<std::int16_t> pcm;
    juce::File file;
};

class SampleBank {
public:
    double sampleRate = 0.0;
    std::vector<Sample> samples;
    std::array<std::vector<int>, 128> byNote; // indices of a note's samples, sorted by velocity

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
