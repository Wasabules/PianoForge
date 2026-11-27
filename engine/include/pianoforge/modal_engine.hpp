// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <pianoforge/biquad.hpp>
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
/// - Velocity: the nearest velocity layer is played, its strike as measured; two layers are two strikes whose
///   partials' phases are unrelated (adding them would cancel some partials and reinforce others, by velocity).
///   Over the attack residual, the strike can only change as a whole: the residual cancels the modes extrapolated
///   back to the note-on (16 to 32 dB over the recording in the first 5 ms), and only at the same gain and phase.
///   Between two layers (blendLayers), the whole strike (modes, residual, soundboard's excitation) therefore takes
///   one gain, which brings the attack's energy (as recorded, the profile's measure: its first attackSeconds) to
///   the one interpolated (in dB) between the two layers' attacks: the attack's loudness follows the velocity, its
///   timbre stays the nearer layer's (it steps where the nearer layer changes, as the reference's does at its own
///   layers). After the residual, each partial reaches its level interpolated between the layers' over
///   kLevelRampSeconds (the levels as the partial sounds over the sustain, the profile's measures: where two strong
///   modes of a partial cancel each other, far under the sum of their energies), then each mode takes the frequency
///   and decay moved toward the other layer's mode of the same partial nearest to it (decay in log; the partial's
///   level corrected for it, from its modes' energies), but for a partial whose modes interfere (moved, their
///   beating would change, and the level with it), which keeps its poles; partials only the other layer has come in
///   over that ramp (from -60 dB), those only the nearer has fade out toward the other. At a layer's velocity, that
///   layer exactly. Without blendLayers: the nearest layer at the overall gain over the attack, then each partial
///   at its interpolated level (studies/velocity_linearity.py). A profile without the layers' measures (older than
///   section LEVL): the overall gain over the attack, the profile's levels after it.
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
/// - One string per note: a note struck again strikes its string while it still rings, and the strikes add up, as
///   on a string. What the string plays at that instant (its modes, at their level) goes on, under the hammer: for
///   the contact (kContactSeconds) its modes lose restrikeAbsorption of their energy (the felt pressed on the
///   string), then each joins the new strike's mode of the same partial nearest to it (with the same layer, the
///   very same mode). The waveform never steps; partials the new layer lacks keep their own modes, and the previous
///   strike's attack residual plays on to its end. No voice is ever taken from another note: every note has its
///   own (kMaxVoices, one per MIDI note).
///
/// install() and prepare() run outside the audio thread and may allocate; process() does not allocate, lock or
/// perform I/O.
/// What a re-strike's hammer absorbs of the string's vibration energy by default (fitted on the reference's
/// repeated notes with the pedal down: see the analysis document).
inline constexpr float kDefaultRestrikeAbsorption = 0.5f;

class ModalEngine final : public Engine {
public:
    static constexpr int kMaxVoices = 128; // one per MIDI note: the voice of note n is voices[n]
    static constexpr int kTails = 4;       // attack residuals of earlier strikes still playing, per note
    static constexpr double kContactSeconds = 0.003; // a re-strike's hammer contact, over which it absorbs
    static constexpr double kLevelRampSeconds = 0.05; // after the residual: overall gain -> each partial's level
    static constexpr float kDefaultVoicingVelocity = 0.6f;
    static constexpr float kDefaultVoicingCorner = 4.0f; // the shelf's midpoint, in partial numbers
    static constexpr float kDefaultAttackSoftMs = 40.0f;  // the attack's softness: how long it lasts
    static constexpr double kAttackCornerHz = 1200.0;     // its shelf's midpoint
    static constexpr double kRestrikeRiseSeconds = 0.002; // a re-strike's: the string already rings, no step
    static constexpr double kLidCornerHz = 1500.0, kLidClosedDb = -9.0, kLidClosedLevelDb = -2.0; // the lid closed
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
    /// Velocity: between two layers, the attack's loudness, the partials' levels, frequencies and decays follow it
    /// (see above); off, the nearest layer at the overall gain, its partials' levels interpolated after the attack.
    std::atomic<bool> blendLayers { true };
    /// A re-strike: the share of the string's vibration energy the hammer absorbs (0: the strikes add exactly).
    std::atomic<float> restrikeAbsorption { kDefaultRestrikeAbsorption };
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
    /// Voicing, as a technician needles or hardens the hammers: the partials above `voicingCorner` (a partial number)
    /// lowered (negative dB: softer hammers, a rounder tone) or raised (harder: brighter), by a high shelf on each
    /// voice's whole sound (its modes and attack residual alike: the residual still cancels the modes extrapolated to
    /// the note-on). The amount is set for the bass (A0), the middle (C4) and the treble (C8), linear in between, and
    /// follows the velocity by `voicingVelocity` (0: alike at every velocity; 1: proportional to it, felt mostly in
    /// the forte, as a needled felt is). 0 dB everywhere: no filter, the sound as measured. Applies at note-on.
    std::atomic<float> voicingBassDb { 0.0f }, voicingMiddleDb { 0.0f }, voicingTrebleDb { 0.0f };
    std::atomic<float> voicingVelocity { kDefaultVoicingVelocity };
    std::atomic<float> voicingCorner { kDefaultVoicingCorner };
    /// The voicing of one strike (dB) at this note and velocity.
    float voicingDb(int note, int velocity) const noexcept;
    /// Touch and resonance:
    /// - dynamicRange: each strike's level below the note's loudest layer, in dB, times this (1: as measured; 0.5:
    ///   the pianissimo half as far below the fortissimo; above 1, further). Every gain of the voice moves together
    ///   (its modes, its attack residual, the soundboard's excitation). Note-on.
    /// - sustainLength: the strings' decay times (and the partials' fade times) times this (1: as measured). Note-on.
    /// - damperRate: the dampers' curves run this much faster (1: as measured; 0.5: a slower, longer release). At once.
    /// - attackSoftDb, attackSoftMs: the sound above kAttackCornerHz lowered (negative: a softer attack) or raised (a
    ///   harder one), fading out over the first attackSoftMs of each strike (a raised cosine). Note-on.
    /// - lidOpen: the lid, 1 open (as measured), 0.5 on the half stick, 0 closed: the treble above kLidCornerHz down
    ///   to kLidClosedDb and the whole sound to kLidClosedLevelDb, in proportion. At once.
    /// All at their defaults: the sound as measured, bit for bit.
    std::atomic<float> dynamicRange { 1.0f }, sustainLength { 1.0f }, damperRate { 1.0f };
    std::atomic<float> attackSoftDb { 0.0f }, attackSoftMs { kDefaultAttackSoftMs };
    std::atomic<float> lidOpen { 1.0f };
    /// Frequency ratio of a note against the profile: (referenceA4 / 440) x 2^(cents / 1200).
    double tuningRatio(int note) const noexcept;

    /// Every setting above but the output gain, as one value: for another engine (the offline render of a take) to
    /// sound exactly as this one does.
    struct Settings {
        bool blendLayers = true;
        float restrikeAbsorption = kDefaultRestrikeAbsorption;
        bool body = true, residual = true, sympathetic = true, heldKeys = true;
        float sympatheticGain = 1.0f;
        bool pedalCatch = true, releaseNoise = true;
        float referenceA4 = 440.0f;
        std::array<float, 12> pitchClassCents {};
        float unisonSpread = 1.0f; // the strings' detunings within each unison against the measured (0 to 4)
        float voicingBassDb = 0.0f, voicingMiddleDb = 0.0f, voicingTrebleDb = 0.0f;
        float voicingVelocity = kDefaultVoicingVelocity, voicingCorner = kDefaultVoicingCorner;
        float dynamicRange = 1.0f, sustainLength = 1.0f, damperRate = 1.0f;
        float attackSoftDb = 0.0f, attackSoftMs = kDefaultAttackSoftMs;
        float lidOpen = 1.0f;
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
    std::atomic<std::uint32_t> stolenVoices { 0 }, noteOnsReceived { 0 }; // (stolen: none, one string per note)

private:
    /// A partial whose level differs from the overall gain: its modes [first, first + count) in the bank (the
    /// profile modes from profileFirst), raised by `factor` per sample during the level ramp.
    struct PartialRamp {
        int first = 0, count = 0, profileFirst = 0;
        float factor = 1.0f;
    };
    enum class Ramp : std::uint8_t { none, waiting, running };
    /// A partial's modes in the bank, [first, first + count), its number, its damper group, and whether its fade
    /// has started.
    struct PartialSlot {
        int first = 0, count = 0, number = 0, group = 0;
        bool faded = false;
    };
    /// The attack residual of an earlier strike of the note, playing on to its end.
    struct Tail {
        const ModalProfile::Layer* layer = nullptr;
        double pos = 0.0, step = 1.0; // in residual frames
        float gain = 0.0f;            // 0: free
    };
    /// What a string plays when it is struck again: one of its modes (its partial, exact pole, a u per channel).
    struct Carried {
        int number = 0;
        std::complex<float> pole, left, right;
    };
    /// From sample `at` (since the note-on), the partial in slot `slot` gets the extra damping of a fade.
    struct PartialFade {
        std::int64_t at = 0;
        int slot = 0;
    };

    struct Voice {
        // released: key up with the damper curve started.
        bool active = false, keyDown = false, released = false, damperCurve = false, fadingOut = false;
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
        std::array<Tail, kTails> tails {}; // earlier strikes' residuals
        // Between two layers: each mode's pole once the ramp is over (exact, as the bank's), set at repoleAt
        // (samples since the note-on).
        std::vector<std::complex<float>> targetPole;
        std::int64_t repoleAt = -1;
        std::vector<int> modeNumber;       // the partial of every mode in the bank (reserved with it)
        // A re-strike under way: until mergeAt (samples since the note-on), the earlier vibration's modes, those of
        // partials the new strike lacks [keepFirst, mergeFirst) then the others [mergeFirst, end), lose energy to
        // the hammer; then the others join the new strike's modes.
        std::int64_t mergeAt = -1;
        int keepFirst = 0, mergeFirst = 0;
        float gain = 1.0f, decay = 1.0f; // exponential envelope (fast fades)
        std::int64_t sinceOn = 0;
        int curvePoints = 0;
        double curvePos = 0.0;   // in curve points
        float damperSpeed = 0.0f; // curve points run per point of time: 0 dampers lifted, 1 resting
        float curveGain = 1.0f;
        std::array<float, DamperCurves::kMaxPoints> releaseGains {};
        std::uint64_t age = 0;
        /// The voicing's high shelf, off at 0 dB; the attack's, faded out over attackSamples from the note-on (a
        /// re-strike's fading in over attackRise first: the string already rings).
        struct Shelf {
            bool on = false;
            Biquad f;
        } voicing, attack;
        std::int64_t attackSamples = 0, attackRise = 0;
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
    void setVoicing(Voice& v, int note, int velocity, bool keepState) noexcept;
    void setAttack(Voice& v, bool restrike) noexcept;
    void softenAttack(Voice& v, float* left, float* right, int n) noexcept;
    void applyLid(float* const* outputs, int numChannels, int n, float gain) noexcept;
    void renderBank(Voice& v, float* left, float* right, int n) noexcept;
    void mergeEarlier(Voice& v) noexcept; // a re-strike's hammer contact over
    void renderStrings(int n) noexcept;

    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    float fadeFactor = 1.0f; // pole magnitude factor of a partial fade, per sample
    // Per note (its register's shape), per sample: what is left of the drive's rise (exp(-1 / (onset fs))), and the
    // drive's decay (exp(-1 / (decay fs))).
    std::array<float, 128> onsetFactor {}, driveFadeFactor {};
    std::array<float, 128> driveGain {}, driveExponent {}; // per note: the drive's gain and velocity exponent
    std::vector<Voice> voices = std::vector<Voice>(kMaxVoices); // on the heap: 128 voices are too big for a stack
    // The lid (audio thread): its shelf, the setting it was made for (-1: none yet), its level now and wanted.
    Biquad lidShelf;
    float lidApplied = -1.0f, lidLevel = 1.0f, lidTarget = 1.0f;
    std::vector<Carried> carriedModes; // a re-strike's (allocated with the voices)
    std::vector<double> blendFreq, blendScale; // a strike's moved poles, per mode of the nearer layer (scratch)
    std::vector<double> logDecays; // a partial's log decays (scratch, as many as the profile's largest partial has modes)
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
