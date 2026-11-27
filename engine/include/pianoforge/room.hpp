// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace pf {

/// A room around the piano, heard from a listening position: the direct sound as it reaches that position, and the
/// room's reflections added to it. Nothing delays the direct sound (no latency); the reflections arrive after it.
///
/// The engine's sound is taken as the piano heard from the keyboard, one metre away (kReferenceDistance). From a
/// listening position `distance` metres away, in a room of the volume the size gives (a shoebox, 60 to 40 000 m3)
/// and of the decay time set, the energies follow Barron's revised theory (Sabine's diffuse field, with the decay
/// that has already happened by the time the direct sound arrives), for a source of directivity Q = 2 on the
/// listener's axis:
///   direct      (r_ref / r)^2
///   early       16 pi r_ref^2 / (Q A) x exp(-13.82 r / (c T)) x (1 - exp(-13.82 x 0.08 / T))   (the first 80 ms)
///   late        16 pi r_ref^2 / (Q A) x exp(-13.82 r / (c T)) x exp(-13.82 x 0.08 / T)
/// with A = 0.161 V / T (Sabine). At the critical distance sqrt(Q A / (16 pi)), the reflections carry as much energy
/// as the direct sound; past it, more. Further on, the direct sound also loses its highs to the air (ISO 9613-1,
/// 20 degrees C, 50 % humidity: two first-order low-passes fitted at 10 kHz), and narrows: the piano (1.5 m wide) is
/// seen under a smaller angle (each ear keeping its energy: a distant piano is a point). Keep loudness scales everything so that the total energy stays that heard from the
/// keyboard (moving away then changes the balance, not the volume).
///
/// - Early reflections: an image-source model of the room (the images of the first and second order, but the
///   floor's: the engine's sound already has it), from the piano to two microphones 0.6 m apart at the listening
///   position, each wall keeping sqrt(1 - alpha) of the amplitude (alpha = A / S, Sabine's mean absorption for this
///   decay in this room). Their energy is normalized; the network's input waits for the first of them (the pre-delay comes
///   from the geometry). Their gain and the network's meet the early and the late energies above together (in a
///   large room, reflections of the second order arrive after 80 ms; in a small one, the network starts before).
/// - Late reverberation: a feedback delay network of 16 lines (prime numbers of samples, following the size),
///   mixed by a Hadamard matrix (lossless). Each line's loss sets the decay time, and a first-order high shelf
///   (crossover 3 kHz) makes the highs decay faster (Jot's absorption filters): T60 = decaySeconds under the
///   crossover, decaySeconds x (0.15 + 0.85 brightness) over it (a one-pole low-pass, the usual choice, slopes from
///   DC to Nyquist and hardly shortens the 2 to 8 kHz where a piano's brilliance is). Each line's length is slowly
///   modulated (0.25 to 1 Hz, +-0.125 ms: at most 1.4 cents of pitch, far under a chorus) so that the tail has no
///   metallic ring; the fractional reads go through first-order all-passes (unit gain: an interpolation inside the
///   loop would damp the highs at every pass). The input is high-passed (60 Hz) and diffused by four all-passes per
///   channel before the network. Its energy is estimated from the energy stored and its decay.
/// - levelDb trims the reflections against what the physics gives (0 dB: as computed).
/// - Headphones (binaural): the listener's head is a sphere (radius kHeadRadius). The engine's two sides are two
///   sources at the piano's angle left and right of the front (37 degrees at the keyboard, 5 at 8 m). Each reaches
///   the nearer ear through the head's shadow toward it, and the farther one around the head: later by Woodworth's
///   delay, (a / c)(theta + sin theta) (0.32 ms at the keyboard, 0.66 ms from the side), and through its shadow.
///   The shadow is Brown and Duda's first-order model, its gains taken against a source in front, since headphones
///   are equalized for one: a mono sound passes at its level. Each early reflection reaches each ear likewise, from
///   where its image source is. The late tail is made as coherent between the ears as a diffuse field is at that
///   spacing, 1 / (1 + (f / 590 Hz)^2): nearly whole under 200 Hz, little over 2 kHz, each ear keeping its energy.
///   With the room off, the engine's sound alone is heard so, from the keyboard. On speakers, none of this: the
///   room hears the sound through the air already.
///
/// Settings from any thread (atomics, read at each block). prepare() allocates for the largest room at the given
/// rate; process() allocates nothing and runs sample by sample: the result does not depend on the block size. A
/// change of size, distance, decay or of speakers and headphones fades the reflections out and back in over 10 ms
/// (the lines are then read at their new lengths: no click, the tail goes on); the gains, and the direct sound
/// between speakers and headphones, glide over 20 ms. Switched off, the reflections fade out and
/// the direct sound comes back to the engine's, then the room is cleared and the input passes through bit for bit.
/// At the keyboard (1 m), the direct sound is the input, bit for bit. Asleep: once the input has been silent long
/// enough for everything to have left the lines, and a whole block of reverberation stays under -120 dB, the room is
/// cleared and costs nothing until the input comes back (it then starts from a clean state).
class Room {
public:
    static constexpr int kLines = 16;
    static constexpr int kTaps = 24;      // early reflections per microphone (17 used)
    static constexpr int kDiffusers = 4;  // all-passes per channel
    static constexpr float kMaxPredelayMs = 150.0f;
    static constexpr float kReferenceDistance = 1.0f; // m: the engine's sound is the piano heard from there
    static constexpr float kMaxDistance = 30.0f;
    static constexpr float kDirectivity = 2.0f;       // Q of the piano toward the listener
    static constexpr float kSourceWidth = 1.5f;       // m
    static constexpr float kHeadRadius = 0.0875f;     // m: the listener's head, on headphones

    struct Settings {
        bool enabled = false;
        float levelDb = 0.0f;      // the reflections against the physics' (a trim)
        float size = 0.85f;        // 0: 60 m3 (6.1 x 4.4 x 2.6 m), 1: 40 000 m3 (48 x 35 x 21 m), geometric
        float decaySeconds = 2.2f; // T60 at low frequencies
        float brightness = 0.5f;   // 1: the highs decay as long as the lows, 0: 0.15 times as long
        float distance = 1.0f;     // m from the piano, kReferenceDistance to kMaxDistance (the room may be shorter)
        bool keepLoudness = true;  // the total energy kept at what it is from the keyboard
        float width = 1.0f;        // the reverberation's: 0 mono, 1 full width
        bool headphones = false;   // heard on headphones: binaural, through a spherical head (even with the room off)
    };

    /// What a setting gives: the room, where the listener is, and the energies (dB against the engine's sound).
    struct Acoustics {
        double volume = 0.0, length = 0.0, widthM = 0.0, height = 0.0; // m3, m
        double distance = 0.0;         // where the listener is (the room may be shorter than asked)
        double criticalDistance = 0.0; // where the reflections carry as much energy as the direct sound
        double directDb = 0.0, earlyDb = 0.0, lateDb = 0.0; // with the trim, before keep loudness
        double reflectionsOverDirectDb = 0.0;
        double loudnessDb = 0.0;       // what keep loudness adds (0 without it)
        double firstReflectionMs = 0.0;
        double airDbAt10k = 0.0;       // what the air takes from the direct sound at 10 kHz (beyond 1 m)
        double directWidth = 1.0;      // the direct sound's stereo width (1 at the keyboard)
        double sideAngle = 0.0;        // degrees: where each side of the piano is, left and right of the front
        double interauralMs = 0.0;     // on headphones, a side's delay from one ear to the other
    };
    static Acoustics acoustics(const Settings& s) noexcept;
    /// Headphones: the delay between the ears for a source at this azimuth from the front, radians (Woodworth), s.
    static double interauralDelay(double azimuth) noexcept;
    /// Headphones: the gain of the highs at an ear for a source at this angle from the ear's axis, radians, against
    /// a source in front (Brown and Duda's head shadow: about 2 toward the ear, 0.13 behind it, 1 in front).
    static double headShadow(double earAngle) noexcept;
    /// ISO 9613-1 air absorption at 20 degrees C, 50 % relative humidity, 101.325 kPa, dB per metre.
    static double airAbsorptionDbPerMetre(double frequency) noexcept;

    std::atomic<bool> enabled { false }, keepLoudness { true }, headphones { false };
    std::atomic<float> levelDb { 0.0f }, size { 0.85f }, decaySeconds { 2.2f }, brightness { 0.5f },
        distance { 1.0f }, width { 1.0f };

    Settings settings() const noexcept;
    void applySettings(const Settings& s) noexcept;

    /// Outside the audio thread: allocates for the largest room at this rate, and clears.
    void prepare(double sampleRate);
    /// Audio thread (or before it runs): silence in every line.
    void reset() noexcept;
    /// Audio thread: the direct sound at the listening position and the room's reflections, in place.
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
    struct Config { // what changes with the size, the distance, the decay and the headphones (applied while the reflections are faded out)
        float size = -1.0f, distance = -1.0f, decay = -1.0f;
        bool headphones = false; // the taps reach the ears (each with its gain of the highs), not two microphones
        float heard = 1.0f; // the distance the listener is at (the room may be shorter than asked)
        float earlyShare = 1.0f; // the early reflections' energy arriving in the first 80 ms
        std::array<std::uint32_t, kLines> lineDelay {};
        std::array<std::uint32_t, kDiffusers> diffuserDelay {};
        std::array<std::uint32_t, kTaps> tapDelayL {}, tapDelayR {};
        std::array<float, kTaps> tapGainL {}, tapGainR {};
        std::array<float, kTaps> tapHighL {}, tapHighR {}; // on headphones: the highs' gain at each ear
        std::uint32_t predelay = 0;
    };

    void configure(Config& c, float size, float distance, float decay, bool headphones) const noexcept;
    void clearRoom() noexcept;  // the reflections' state (not the ears', nor the input's energies)
    void clearEars() noexcept;  // the ears' state
    /// Headphones: the direct sound (gain and air as set) at the two ears.
    void earsDirect(float inL, float inR, float& outL, float& outR) noexcept;
    /// Headphones with the room off: the engine's sound at the ears, nothing else.
    void processEarsOnly(float* left, float* right, int n, bool silent) noexcept;
    void updateTargets() noexcept; // decay, brightness, level, distance -> the gains and filters to glide to
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

    // Targets and current values: the reflections' gain (on / off and trim), the reverberation's width, the early
    // and late gains, and the direct sound's gain, width and air low-pass coefficient.
    float wetTarget = 0.0f, wet = 0.0f, widthTarget = 1.0f, widthNow = 1.0f;
    float earlyTarget = 0.0f, early = 0.0f, lateTarget = 0.0f, late = 0.0f;
    float directTarget = 1.0f, direct = 1.0f, directWidthTarget = 1.0f, directWidth = 1.0f, airTarget = 0.0f, air = 0.0f;
    float airY[2] {}, airZ[2] {}; // the air's two low-passes
    float midEnergy = 0.0f, sideEnergy = 0.0f, imageCoef = 0.0f; // the input's, for the narrowed image's gain
    // Per line: the loss (gain under the crossover) and the high shelf (b0, b1, a1: unit gain under the crossover,
    // the highs' gain over it), targets and current values; the shelf's state, and the all-pass read's.
    struct Absorption {
        float gain = 1.0f, b0 = 1.0f, b1 = 0.0f, a1 = 0.0f;
    };
    std::array<Absorption, kLines> absTarget {}, absNow {};
    std::array<float, kLines> shelfX {}, shelfY {}, apY {};
    float appliedDecay = -1.0f, appliedBrightness = -1.0f, appliedLevel = 1e9f;
    bool appliedKeep = false;
    bool on = false;

    // Filters: input high-pass (per channel), reflections low-pass (per channel).
    float hpCoef = 0.0f, hpX[2] {}, hpY[2] {};
    float lpCoef = 0.0f, lpY[2] {};
    // Modulation: a phasor per line, and its rotation per sample.
    std::array<float, kLines> lfoC {}, lfoS {}, rotC {}, rotS {};
    float depth = 0.0f;         // samples

    // Headphones: the mix toward the ears (0 speakers, 1 headphones), the shadow's gains of the highs at the nearer and
    // the farther ear and the delay between them (samples), targets and current values. The head's crossover (its
    // lows and highs), the late tail's coherence filters, the direct sound's air and crossover states, the farther
    // ear's path of each side, and the reflections' line in the lows.
    float earsTarget = 0.0f, ears = 0.0f;
    float ipsiTarget = 1.0f, ipsi = 1.0f, contraTarget = 1.0f, contra = 1.0f, itdTarget = 0.0f, itd = 0.0f;
    float earCoef = 0.0f, coherenceCoef = 0.0f;
    float earAirY[2] {}, earAirZ[2] {}, earLow[2] {};
    float coherenceMid = 0.0f, coherenceSide = 0.0f;
    Delay farPath[2], monoLow;
    float monoLowY = 0.0f;
    bool earsActive = false;
};

} // namespace pf
