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
/// - Key release: the measured damper curve of the note and velocity (level against the note held), applied to the
///   voice, and
///   per group of partials the measured level against it, applied to their poles (the high partials die faster
///   under the felt). The
///   pedal sets how fast the curve runs: not at all with the dampers lifted, fully with them resting on the
///   strings, in between for a half pedal (profile pedal curve). With pedalCatch, pressing the pedal again stops
///   the curve where it is, and what is left of the note rings on, as on a piano (the reference library lets the
///   release run to its end).
/// - Release noise: when a damper lands, the soundboard gets the knock measured on the reference for the nearest
///   note and velocity (forge release-noise), weaker the longer the key was held, and scaled by how firmly the
///   damper lands.
/// - Sostenuto (CC66): pressing it catches every damper raised at that instant (keys held, and all of them when
///   the sustain pedal is down, as the rod of a grand does); they stay up until it is released, whatever the keys
///   and the sustain pedal do. Dampers raised later are not caught.
/// - Sympathetic resonance: every string whose damper is up (sustain pedal, sostenuto, or its key held when
///   heldKeysResonate) is a bank of resonators, its measured modes, driven by the voices of the other notes
///   (forge sympathetic), each voice with the drive shape (rise, decay) of its note's register. A damper landing
///   on a string adds the note's damping to its resonators.
/// - Tuning: the profile plays as recorded at referenceA4 = 440 Hz; another reference and a temperament (cents
///   against equal temperament per pitch class) scale every frequency of a note at its note-on (modes, residual),
///   and retune the free strings.
/// - Re-strike: the previous voice of the note keeps ringing and fades to -40 dB within restrikeFadeSeconds, as in
///   the reference player.
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
    /// From any thread: from the next block, everything silent (voices, soundboard, free strings) and the pedals up,
    /// as a fresh start (a host's reset: the same input then gives the same output).
    void reset() noexcept { resetPending.store(true, std::memory_order_release); }

    /// Message thread: replaces the profile (nullptr: silence). Waits for the audio thread to let go of the old one.
    void install(std::shared_ptr<const ModalProfile> profile);
    const ModalProfile* profile() const noexcept { return owned.get(); }
    std::shared_ptr<const ModalProfile> sharedProfile() const noexcept { return owned; } // message thread

    // Settings, read on every block or note-on.
    std::atomic<float> outputGain { 1.0f };
    std::atomic<float> restrikeFadeSeconds { 0.8f };
    std::atomic<bool> bodyEnabled { true }, residualEnabled { true };
    std::atomic<bool> sympatheticEnabled { true };
    std::atomic<bool> heldKeysResonate { true }; // a piano does; the reference library does not
    std::atomic<float> sympatheticGain { 1.0f };
    std::atomic<bool> pedalCatch { true };       // a piano does; the reference library does not
    std::atomic<bool> releaseNoiseEnabled { true };
    /// Tuning (note-on; the free strings follow at the next block after tuningVersion changes).
    std::atomic<float> referenceA4 { 440.0f };
    std::array<std::atomic<float>, 12> pitchClassCents {}; // against equal temperament, index note % 12 (0 = C)
    std::atomic<std::uint32_t> tuningVersion { 0 };
    /// The strings of each unison detuned by this much against the measured (1: as measured; 0: in perfect unison,
    /// no beating, a single decay; 2 to 4: an old upright's honky-tonk). Applies to the notes struck from then on.
    std::atomic<float> unisonSpread { 1.0f };
    /// Frequency ratio of a note against the profile: (referenceA4 / 440) x 2^(cents / 1200).
    double tuningRatio(int note) const noexcept;

    /// Every setting above but the output gain, as one value: for another engine (the offline render of a take) to
    /// sound exactly as this one does.
    struct Settings {
        float restrikeFadeSeconds = 0.8f;
        bool body = true, residual = true, sympathetic = true, heldKeys = true;
        float sympatheticGain = 1.0f;
        bool pedalCatch = true, releaseNoise = true;
        float referenceA4 = 440.0f;
        std::array<float, 12> pitchClassCents {};
        float unisonSpread = 1.0f; // the strings' detunings within each unison against the measured (0 to 4)
    };
    Settings settings() const noexcept;
    /// Applies `s` (the free strings are retuned on the next block).
    void applySettings(const Settings& s) noexcept;

    // Information for the user interface.
    std::atomic<int> lastNote { -1 }, lastVelocity { 0 }, lastLayerVelocity { 0 };
    std::atomic<int> activeVoices { 0 }, activeModes { 0 }, ringingStrings { 0 };
    std::atomic<bool> sustainDown { false }, sostenutoDown { false };
    std::atomic<float> softPedalPosition { 0.0f }; // the soft pedal (CC67), 0 to 1
    std::atomic<int> pedalValue { 0 };              // the last sustain pedal value received (CC64, 0 to 127)
    std::atomic<float> pedalPosition { 0.0f }; // damper lift, 0 to 1
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
    /// A partial's modes in the bank, [first, first + count), its damper group, and whether its fade has started.
    struct PartialSlot {
        int first = 0, count = 0, group = 0;
        bool faded = false;
    };
    /// From sample `at` (since the note-on), the partial in slot `slot` gets the extra damping of a fade.
    struct PartialFade {
        std::int64_t at = 0;
        int slot = 0;
    };

    struct Voice {
        // released: key up with the damper curve started, or re-struck (first in line for stealing).
        bool active = false, keyDown = false, released = false, damperCurve = false, fadingOut = false;
        bool restruck = false; // fades out (restrikeFadeSeconds), whatever the dampers do
        int note = -1, velocity = 0;
        const ModalProfile::Layer* layer = nullptr;
        ModeBank bank;
        std::vector<PartialRamp> ramps; // reserved for the largest layer
        Ramp ramp = Ramp::none;
        std::int64_t rampStart = 0, rampEnd = 0; // samples since the note-on
        std::vector<PartialFade> fades;          // ascending `at`, reserved for the largest layer
        std::size_t nextFade = 0;
        std::vector<PartialSlot> slots;          // every partial in the bank, reserved for the largest layer
        std::vector<float> groupGains;           // per damper group, its curve against the overall one (gains)
        std::array<float, DamperCurves::kMaxGroups> groupFactor {}; // pole factor per sample over this block
        bool groupDamping = false;               // the poles carry group factors
        float driveGain = 0.0f, driveOnset = 0.0f; // drive of the free strings: velocity factor, rise so far
        float driveFade = 1.0f;                    // and its decay so far
        double residualPos = 0.0;      // in residual frames
        double residualStep = 1.0;     // residual frames per output sample (rates and tuning)
        float residualGain = 1.0f;
        float gain = 1.0f, decay = 1.0f; // exponential envelope (re-strike, fast fades)
        std::int64_t sinceOn = 0;
        int curvePoints = 0;
        double curvePos = 0.0;   // in curve points
        float damperSpeed = 0.0f; // curve points run per point of time: 0 dampers lifted, 1 resting
        float curveGain = 1.0f;
        std::array<float, DamperCurves::kMaxPoints> releaseGains {};
        std::uint64_t age = 0;
    };

    void allocate();
    void apply(const ModalProfile& p, const Event& e) noexcept;
    void noteOn(const ModalProfile& p, int note, int velocity) noexcept;
    void noteOff(const ModalProfile& p, int note) noexcept;
    void updateDamping(const ModalProfile& p, Voice& v) noexcept;
    void updateAllDampers(const ModalProfile& p) noexcept; // a pedal moved: the dampers of the keys up follow
    bool damperRaised(const Voice& v) const noexcept { return v.keyDown || sostenutoHeld[(std::size_t) v.note]; }
    void dampGroups(const ModalProfile& p, Voice& v, int n) noexcept;
    void releaseKnock(const ModalProfile& p, const Voice& v, float strength) noexcept;
    void render(const ModalProfile& p, float* const* outputs, int numChannels, int start, int n) noexcept;
    bool produce(const ModalProfile& p, Voice& v, int n) noexcept;
    void renderBank(Voice& v, float* left, float* right, int n) noexcept;
    void renderStrings(int n) noexcept;

    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    float fadeFactor = 1.0f; // pole magnitude factor of a partial fade, per sample
    // Per note (its register's shape), per sample: what is left of the drive's rise (exp(-1 / (onset fs))), and the
    // drive's decay (exp(-1 / (decay fs))).
    std::array<float, 128> onsetFactor {}, driveFadeFactor {};
    std::array<float, 128> driveGain {}, driveExponent {}; // per note: the drive's gain and velocity exponent
    std::array<Voice, kMaxVoices> voices;
    ModeBank body;
    float bodyGain = 1.0f, bodyDecay = 1.0f;
    bool bodyActive = false;
    std::vector<float> bufferL, bufferR, bufferD, mixL, mixR; // bufferD: a voice's drive of the free strings
    std::uint64_t ageCounter = 0;
    float pedalLift = 0.0f; // 0: dampers resting, 1: lifted
    float softPedal = 0.0f; // the soft pedal: how far the action is shifted, 0 to 1 (audio thread)
    bool sostenuto = false;                       // CC66 down
    std::array<bool, 128> sostenutoHeld {};       // dampers caught by the sostenuto
    float knockScale = 1.0f; // release noise of dampers landing together (see apply)

    // Sympathetic resonance: one group of the bank per string; the drive of a string is every voice but its own.
    DrivenBank strings;
    std::array<int, 128> stringGroup {};          // -1: no string for this note
    std::array<float, 128> stringDamper {};       // pole factor per sample of a damper resting on the string
    std::array<float, 128> stringApplied {};      // pole factor currently applied
    std::array<bool, 128> stringRinging {}, noteDriven {}, keyHeld {};
    bool stringsCut = false;                      // all sound off: the strings fade out fast
    std::uint32_t stringsTuning = 0;              // tuningVersion the strings are tuned to
    void retuneStrings() noexcept;
    std::vector<float> drive, driveOwn, driveScratch, stringsL, stringsR; // driveOwn: 128 x maxBlockSize

    std::shared_ptr<const ModalProfile> owned;          // message thread
    std::atomic<const ModalProfile*> published { nullptr };
    std::atomic<const ModalProfile*> seenByAudio { nullptr };
    const ModalProfile* current = nullptr;              // audio thread
    std::atomic<bool> audioRunning { false };
    std::atomic<bool> resetPending { false };
};

} // namespace pf
