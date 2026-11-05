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
///   The attack stays the nearest layer's strike: over the attack residual, everything (modes, body, residual)
///   has the one overall gain, since the residual cancels the modes extrapolated back to the note-on and only
///   does so at the same gain. Each partial then reaches its own level over kLevelRampSeconds.
/// - Soundboard: one bank of resonances shared by every note; a note-on adds its excitation to it, at the
///   overall gain (the body is in the residual too).
/// - Past its analysis window, where the forge found the recording far under the modes, a partial is faded out
///   (profile fade time, an extra -60 dB per kPartialFadeSeconds).
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
    static constexpr double kLevelRampSeconds = 0.05; // after the residual: overall gain -> each partial's level
    static constexpr double kPartialFadeSeconds = 1.0; // a partial faded out (profile fade time): extra T60

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
    /// A partial whose level differs from the overall gain: its modes [first, first + count) in the bank (the
    /// profile modes from profileFirst), raised by `factor` per sample during the level ramp.
    struct PartialRamp {
        int first = 0, count = 0, profileFirst = 0;
        float factor = 1.0f;
    };
    enum class Ramp : std::uint8_t { none, waiting, running };
    /// From sample `at` (since the note-on), the modes [first, first + count) get the extra damping of a fade.
    struct PartialFade {
        std::int64_t at = 0;
        int first = 0, count = 0;
    };

    struct Voice {
        bool active = false, keyDown = false, released = false, damperCurve = false, fadingOut = false;
        int note = -1;
        const ModalProfile::Layer* layer = nullptr;
        ModeBank bank;
        std::vector<PartialRamp> ramps; // reserved for the largest layer
        Ramp ramp = Ramp::none;
        std::int64_t rampStart = 0, rampEnd = 0; // samples since the note-on
        std::vector<PartialFade> fades;          // ascending `at`, reserved for the largest layer
        std::size_t nextFade = 0;
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
    void renderBank(Voice& v, float* left, float* right, int n) noexcept;

    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    float fadeFactor = 1.0f; // pole magnitude factor of a partial fade, per sample
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
