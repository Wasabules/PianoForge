#pragma once

#include "SampleBank.h"

#include <pianoforge/engine.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace pf::app {

/// Reference corpus player: plays back the takes rendered by the bench (Noire in Kontakt).
/// It is the listening reference the modal engine will be compared with, behind the same interface.
///
/// - The played velocity selects the take with the closest rendered velocity, with no crossfade between layers.
/// - The key release is approximated by an exponential decay fitted to the bench measurement
///   (-40 dB after release); notes without a damper keep ringing.
/// - If the output does not run at the corpus sample rate, playback is resampled (Lagrange).
class ReferencePlayer final : public pf::Engine {
public:
    static constexpr int kMaxVoices = 96;

    void prepare(double sampleRate, int maxBlockSize) override;
    void release() noexcept override;
    void process(float* const* outputs, int numChannels, int numSamples,
                 const pf::Event* events, int numEvents) noexcept override;
    void allSoundOff() noexcept override;

    /// Message thread: replaces the bank. The old one is only destroyed once the audio thread has let go of it.
    void installBank(std::unique_ptr<SampleBank> newBank);
    const SampleBank* bank() const noexcept { return owned.get(); }

    // Settings, read on every block.
    std::atomic<float> outputGain { 1.0f };
    std::atomic<bool> playWholeTake { false }; // true: the take plays to its end, key releases are ignored

    // Information for the user interface.
    std::atomic<int> lastNote { -1 }, lastVelocity { 0 }, lastRenderedVelocity { 0 };
    std::atomic<int> activeVoices { 0 };
    std::atomic<bool> sustainDown { false };
    std::atomic<float> outputPeak { 0.0f }; // maximum since the last read (exchanged by the UI)
    std::atomic<std::uint32_t> stolenVoices { 0 };
    std::atomic<std::uint32_t> noteOnsReceived { 0 }; // every note-on reaching the engine, for diagnostics

private:
    struct Voice {
        bool active = false, keyDown = false, released = false;
        int sample = -1, note = -1;
        std::int64_t position = 0; // in corpus frames
        float gain = 1.0f, decay = 1.0f;
        std::uint64_t age = 0;
        juce::LagrangeInterpolator left, right;
    };

    void apply(const SampleBank& b, const pf::Event& e) noexcept;
    void noteOn(const SampleBank& b, int note, int velocity) noexcept;
    void noteOff(const SampleBank& b, int note) noexcept;
    void releaseVoice(const SampleBank& b, Voice& v) noexcept;
    void render(const SampleBank& b, float* const* outputs, int numChannels, int start, int n) noexcept;
    bool produce(const SampleBank& b, Voice& v, int n) noexcept;

    std::array<Voice, kMaxVoices> voices;
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    std::vector<float> bufferL, bufferR, rawL, rawR;
    std::uint64_t ageCounter = 0;
    bool sustain = false;

    std::atomic<const SampleBank*> publishedBank { nullptr };
    std::atomic<const SampleBank*> bankSeenByAudio { nullptr };
    std::atomic<bool> audioRunning { false };
    const SampleBank* currentBank = nullptr; // audio thread only
    std::unique_ptr<SampleBank> owned;       // message thread only
};

} // namespace pf::app
