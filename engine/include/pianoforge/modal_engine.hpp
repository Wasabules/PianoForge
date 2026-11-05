#pragma once

#include <pianoforge/engine.hpp>
#include <pianoforge/modal_profile.hpp>
#include <pianoforge/mode_bank.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace pf {

/// The modal engine: every note is a bank of resonators measured on the reference (forge export), played from
/// the note-on with no sample playback except a short attack residual.
///
/// - Velocity: the nearest velocity layer is played, as it was measured (a real strike, with its own beating);
///   each partial's level is interpolated between the two layers around the velocity, so that loudness and
///   timbre follow the velocity continuously (studies/velocity_linearity.py).
/// - Soundboard: one bank of resonances shared by every note; a note-on adds its excitation to it.
/// - Key release: the measured damper curve of the note (level against the note held), applied to the voice.
/// - Sustain pedal (CC64 >= 64): the dampers stay up. Re-strike: the previous voice of the note keeps ringing and
///   fades to -40 dB within restrikeFadeSeconds, as in the reference player.
///
/// install() and prepare() run outside the audio thread and may allocate; process() does not allocate, lock or
/// perform I/O.
class ModalEngine final : public Engine {
public:
    static constexpr int kMaxVoices = 64;
    static constexpr int kMaxVoicesPerNote = 3;

    void prepare(double sampleRate, int maxBlockSize) override;
    void release() noexcept override;
    void process(float* const* outputs, int numChannels, int numSamples, const Event* events,
                 int numEvents) noexcept override;
    void allSoundOff() noexcept override;

    /// Message thread: replaces the profile (nullptr: silence). Waits for the audio thread to let go of the old one.
    void install(std::shared_ptr<const ModalProfile> profile);
    const ModalProfile* profile() const noexcept { return owned.get(); }
    std::shared_ptr<const ModalProfile> sharedProfile() const noexcept { return owned; } // message thread

    // Settings, read on every block or note-on.
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> restrikeFadeSeconds { 0.8f };
    std::atomic<bool> bodyEnabled { true }, residualEnabled { true };

    // Information for the user interface.
    std::atomic<int> lastNote { -1 }, lastVelocity { 0 }, lastLayerVelocity { 0 };
    std::atomic<int> activeVoices { 0 }, activeModes { 0 };
    std::atomic<bool> sustainDown { false };
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<std::uint32_t> stolenVoices { 0 }, noteOnsReceived { 0 };

private:
    struct Voice {
        bool active = false, keyDown = false, released = false, damperCurve = false, fadingOut = false;
        int note = -1;
        const ModalProfile::Layer* layer = nullptr;
        ModeBank bank;
        double residualPos = 0.0;      // in residual frames
        float residualGain = 1.0f;
        float gain = 1.0f, decay = 1.0f; // exponential envelope (re-strike, fast fades)
        std::int64_t sinceOn = 0, sinceRelease = 0;
        int curvePoints = 0;
        float curveGain = 1.0f;
        std::array<float, DamperCurves::kMaxPoints> releaseGains {};
        std::uint64_t age = 0;
    };

    void allocate();
    void apply(const ModalProfile& p, const Event& e) noexcept;
    void noteOn(const ModalProfile& p, int note, int velocity) noexcept;
    void noteOff(const ModalProfile& p, int note) noexcept;
    void releaseVoice(const ModalProfile& p, Voice& v) noexcept;
    void render(const ModalProfile& p, float* const* outputs, int numChannels, int start, int n) noexcept;
    bool produce(const ModalProfile& p, Voice& v, int n) noexcept;

    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    std::array<Voice, kMaxVoices> voices;
    ModeBank body;
    float bodyGain = 1.0f, bodyDecay = 1.0f;
    bool bodyActive = false;
    std::vector<float> bufferL, bufferR, mixL, mixR;
    std::uint64_t ageCounter = 0;
    bool sustain = false;

    std::shared_ptr<const ModalProfile> owned;          // message thread
    std::atomic<const ModalProfile*> published { nullptr };
    std::atomic<const ModalProfile*> seenByAudio { nullptr };
    const ModalProfile* current = nullptr;              // audio thread
    std::atomic<bool> audioRunning { false };
};

} // namespace pf
