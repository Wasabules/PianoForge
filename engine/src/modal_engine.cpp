// SPDX-License-Identifier: AGPL-3.0-or-later

#include <pianoforge/modal_engine.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define PF_HAVE_MXCSR 1
#endif

namespace pf {

namespace {

constexpr double kTwoPi = 6.283185307179586;
// A partial whose level as it sounds over the sustain (measured) differs from its modes' energies by more than this
// (1 dB, in log): its modes interfere (between two layers, its poles are not moved).
constexpr double kInterferingLog = 0.11512925464970229;
constexpr double kFastFadeS = 0.015;     // cut, voice stealing: long enough to avoid a click
constexpr float kSilentEnergy = 1.0e-10f; // -100 dB (amplitude 1e-5) for the sum of the modes: the voice is freed
constexpr double kBodyFadeS = 0.02;       // the soundboard fading out under kBodySilentEnergy
constexpr float kBodySilentEnergy = 1.0e-12f; // -120 dB: the soundboard rings as low as the reference's (top notes:
                                              // -105 to -125 dBFS at 60-120 Hz 2 to 3 s after the strike)
constexpr float kDampedDb = -30.0f;      // a damper curve ending lower than this means the string is damped
constexpr float kMinGain = 0.25f, kMaxGain = 4.0f; // per-partial level interpolation: at most +-12 dB
// The soft pedal (una corda): the hammer, shifted, misses one string of each unison (the one tuned highest: which
// one is unknown, the effect hardly depends on it) and strikes with its softer, less worn felt: the partials above
// the fundamental lose this much per octave (an estimate: the reference has no soft pedal).
constexpr double kSoftFeltDbPerOctave = 1.5;
constexpr double kMaxUnisonEnergy = 4.0;   // a partial moved at most 6 dB above its measured energy
// A concert grand's stringing (as forge unison): the lowest 10 keys have one string, the next 18 two, the rest three.
constexpr int kSingleStringLast = 30, kDoubleStringLast = 48;

int stringsOf(int note) noexcept
{
    return note <= kSingleStringLast ? 1 : note <= kDoubleStringLast ? 2 : 3;
}

/// Weinreich's coupled unison of one partial (forge unison): eigenvalues (relative to the strings' mean pole,
/// ascending imaginary part) and residues of A = diag(2 pi i spread d) + Y J for the strike weights h (the bridge
/// sums the strings). A is complex symmetric: its eigenvectors are v_i = 1 / (lambda - s_i) and the residue of mode
/// k is (sum v) (sum v h) / (sum v^2). No allocation.
struct UnisonModes {
    int n = 0;
    std::complex<double> lambda[3], residue[3];
};

void unisonModes(const ModalProfile::Unison& u, double spread, const double* h, UnisonModes& out) noexcept
{
    using C = std::complex<double>;
    const int n = std::clamp(u.strings, 1, 3);
    const C Y(u.bridge.real(), u.bridge.imag());
    C s[3];
    double scale = std::abs(Y);
    for (int i = 0; i < n; ++i) {
        s[i] = C(0.0, 2.0 * 3.141592653589793 * spread * u.detuneHz[(std::size_t) i]);
        scale = std::max(scale, std::abs(s[i]));
    }
    scale = std::max(scale, 1e-6);
    C lam[3];
    if (n == 1) {
        lam[0] = s[0] + Y;
    } else if (n == 2) {
        const C mean = 0.5 * (s[0] + s[1]), half = 0.5 * (s[0] - s[1]), root = std::sqrt(half * half + Y * Y);
        lam[0] = mean + Y - root;
        lam[1] = mean + Y + root;
    } else {
        // The characteristic polynomial: prod (l - s_i) - Y sum_i prod_{j != i} (l - s_j), roots by Durand-Kerner.
        const C e1 = s[0] + s[1] + s[2], e2 = s[0] * s[1] + s[0] * s[2] + s[1] * s[2], e3 = s[0] * s[1] * s[2];
        const C c2 = -(e1 + 3.0 * Y), c1 = e2 + 2.0 * Y * e1, c0 = -(e3 + Y * e2);
        auto poly = [&](C l) { return ((l + c2) * l + c1) * l + c0; };
        for (int i = 0; i < 3; ++i) // near each string, apart from one another
            lam[i] = s[i] + Y + scale * C(0.01 * (i + 1), 0.013 * (i + 1));
        for (int iter = 0; iter < 80; ++iter) {
            double step = 0.0;
            for (int i = 0; i < 3; ++i) {
                C den(1.0, 0.0);
                for (int j = 0; j < 3; ++j)
                    if (j != i)
                        den *= lam[i] - lam[j];
                if (std::abs(den) < 1e-300)
                    den = C(1e-300, 0.0);
                const C d = poly(lam[i]) / den;
                lam[i] -= d;
                step = std::max(step, std::abs(d));
            }
            if (step < 1e-12 * scale)
                break;
        }
    }
    std::sort(lam, lam + n, [](const C& a, const C& b) { return a.imag() < b.imag(); });
    out.n = n;
    for (int k = 0; k < n; ++k) {
        C sv(0.0), svh(0.0), svv(0.0);
        for (int i = 0; i < n; ++i) {
            const C d = lam[k] - s[i];
            const C v = std::abs(d) > 1e-9 * scale ? 1.0 / d : C(1e9 / scale, 0.0);
            sv += v;
            svh += v * h[i];
            svv += v * v;
        }
        out.lambda[k] = lam[k];
        out.residue[k] = std::abs(svv) > 0.0 ? sv * svh / svv : C(0.0);
    }
}

/// The soft pedal and the unison tuning on one partial's measured modes. The model is fitted to them (per channel,
/// by least squares: measured ~ alpha r0). Each modelled mode is then split: what the model does not account for
/// (measured - alpha r0) keeps its measured damping and follows the model's change of frequency only (the strings
/// drawn together or apart change the beating; a damping moved under a part the model does not explain would let it
/// ring on, or die, for no reason); the model's part becomes alpha r, on the pole moved by lambda - lambda0 (with the
/// strike changed only, the pole does not move and both parts are one). The partial's
/// energy (|a|^2 / 2 sigma over those modes) stays within +6 dB of the measured. No allocation.
struct UnisonMove {
    int n = 0;
    int mode[3] {};
    std::complex<float> keepL[3], keepR[3], movedL[3], movedR[3]; // on the measured pole; on the moved one
    double dFreq[3] {}, dDecay[3] {};                              // the moved pole's shift (Hz, 1/s)
};

void moveUnison(const ModalProfile::Unison& u, const ModalProfile::Mode* modes, double spread, double soft, UnisonMove& out) noexcept
{
    using C = std::complex<double>;
    out.n = 0;
    double h[3] = { 1.0, 1.0, 1.0 }, ones[3] = { 1.0, 1.0, 1.0 };
    int missed = 0; // the string the shifted hammer misses: the one tuned highest
    for (int i = 1; i < u.strings; ++i)
        if (u.detuneHz[(std::size_t) i] > u.detuneHz[(std::size_t) missed])
            missed = i;
    h[missed] = 1.0 - soft;
    UnisonModes before, after;
    unisonModes(u, 1.0, ones, before);
    unisonModes(u, spread, h, after);
    C num[2] = {}, den = 0.0;
    for (int k = 0; k < before.n; ++k) {
        const auto& m = modes[u.modes[(std::size_t) k]];
        num[0] += std::conj(before.residue[k]) * C(m.left);
        num[1] += std::conj(before.residue[k]) * C(m.right);
        den += std::norm(before.residue[k]);
    }
    if (std::abs(den) <= 0.0)
        return;
    const C alpha[2] = { num[0] / den, num[1] / den };
    C keep[3][2], moved[3][2];
    double energyBefore = 0.0, energyAfter = 0.0;
    for (int k = 0; k < before.n; ++k) {
        const int idx = u.modes[(std::size_t) k];
        const auto& m = modes[idx];
        const C delta = after.lambda[k] - before.lambda[k];
        out.mode[k] = idx;
        out.dFreq[k] = delta.imag() / (2.0 * 3.141592653589793);
        out.dDecay[k] = -delta.real();
        const double s0 = std::max(0.01, m.decay), s1 = std::max(0.01, m.decay - delta.real());
        const C measured[2] = { C(m.left), C(m.right) };
        for (int c = 0; c < 2; ++c) {
            keep[k][c] = measured[c] - alpha[c] * before.residue[k];
            moved[k][c] = alpha[c] * after.residue[k];
            energyBefore += std::norm(measured[c]) / (2.0 * s0);
            energyAfter += std::norm(keep[k][c]) / (2.0 * s0) + std::norm(moved[k][c]) / (2.0 * s1); // (apart: an estimate)
        }
    }
    const double scale = energyAfter > kMaxUnisonEnergy * energyBefore && energyAfter > 0.0
                       ? std::sqrt(kMaxUnisonEnergy * energyBefore / energyAfter) : 1.0;
    for (int k = 0; k < before.n; ++k) {
        out.keepL[k] = std::complex<float>(keep[k][0] * scale);
        out.keepR[k] = std::complex<float>(keep[k][1] * scale);
        out.movedL[k] = std::complex<float>(moved[k][0] * scale);
        out.movedR[k] = std::complex<float>(moved[k][1] * scale);
    }
    out.n = before.n;
}

/// Flush-to-zero and denormals-are-zero for the scope (decaying modes end up denormal otherwise).
struct NoDenormals {
#if PF_HAVE_MXCSR
    unsigned int saved = _mm_getcsr();
    NoDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
    ~NoDenormals() { _mm_setcsr(saved); }
#endif
};

/// The energy over [from, to] of e^(-decay t) until `at`, e^(-moved (t - at)) times its value there after (a mode
/// whose pole is moved at `at`, its sine left out).
double movedEnergy(double decay, double moved, double at, double from, double to) noexcept
{
    auto span = [](double rate, double a, double b) { // integral of e^(-2 rate t) over [a, b]
        if (b <= a)
            return 0.0;
        const double x = 2.0 * std::max(rate, 1e-9);
        return (std::exp(-x * a) - std::exp(-x * b)) / x;
    };
    return span(decay, from, std::min(at, to)) + std::exp(-2.0 * decay * at) * span(moved, std::max(at, from) - at, to - at);
}

float interpolateLevel(float a, float b, double alpha) noexcept
{
    if (a <= 0.0f || b <= 0.0f)
        return alpha < 0.5 ? a : b;
    return (float) std::exp((1.0 - alpha) * std::log((double) a) + alpha * std::log((double) b));
}

/// Cubic Hermite (Catmull-Rom) interpolation in an interleaved stereo buffer; zero outside.
inline void readCubic(const float* x, int frames, double pos, float& l, float& r) noexcept
{
    const int i = (int) std::floor(pos);
    const float t = (float) (pos - i);
    auto at = [&](int k, int c) { return k >= 0 && k < frames ? x[2 * k + c] : 0.0f; };
    for (int c = 0; c < 2; ++c) {
        const float y0 = at(i - 1, c), y1 = at(i, c), y2 = at(i + 1, c), y3 = at(i + 2, c);
        const float v = y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + t * (3.0f * (y1 - y2) + y3 - y0)));
        (c == 0 ? l : r) = v;
    }
}

} // namespace

void ModalEngine::prepare(double newSampleRate, int newMaxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlockSize = std::max(32, newMaxBlockSize);
    fadeFactor = (float) std::exp(-std::log(1000.0) / (kPartialFadeSeconds * sampleRate));
    for (auto* b : { &bufferL, &bufferR, &bufferD, &mixL, &mixR, &drive, &driveScratch, &stringsL, &stringsR })
        b->assign((std::size_t) maxBlockSize, 0.0f);
    driveOwn.assign((std::size_t) 128 * (std::size_t) maxBlockSize, 0.0f);
    allocate();
    lidShelf = Biquad {};
    lidApplied = -1.0f;
    pedalLift = 0.0f;
    sostenuto = false;
    sostenutoHeld.fill(false);
    audioRunning = true;
}

void ModalEngine::release() noexcept
{
    audioRunning = false;
}

void ModalEngine::allocate()
{
    // Three times the largest layer: the unison model may add a mode for each one it moves (moveUnison), and between
    // two layers the partials only the other one has come in (with a re-strike's earlier vibration besides).
    const int capacity = owned != nullptr ? 3 * owned->maxModesPerLayer : 0;
    blendFreq.assign((std::size_t) (owned != nullptr ? owned->maxModesPerLayer : 0), 0.0);
    blendScale.assign(blendFreq.size(), 1.0);
    int partialModes = 0;
    if (owned != nullptr)
        for (const auto& note : owned->notes)
            for (const auto& layer : note.layers)
                for (const auto& part : layer.partials)
                    partialModes = std::max(partialModes, part.modeCount);
    logDecays.assign((std::size_t) partialModes, 0.0);
    // (A partial has at least one mode: a profile that does not say how many partials it has at most is covered.)
    const int partials = owned != nullptr ? std::max(owned->maxPartialsPerLayer, owned->maxModesPerLayer) : 0;
    for (auto& v : voices) {
        v.active = false;
        v.tails.fill({}); // (they point into the profile)
        v.mergeAt = -1;
        v.repoleAt = -1;
        v.modeNumber.clear();
        v.modeNumber.reserve((std::size_t) capacity);
        v.targetPole.clear();
        v.targetPole.reserve((std::size_t) capacity);
        if (v.bank.capacity() < capacity)
            v.bank.reserve(capacity);
        v.bank.clear();
        v.ramps.clear();
        v.ramps.reserve((std::size_t) (2 * partials)); // (the other layer's partials, between two layers)
        v.ramp = Ramp::none;
        v.fades.clear();
        v.fades.reserve((std::size_t) partials);
        v.nextFade = 0;
        v.slots.clear();
        v.slots.reserve((std::size_t) partials);
        v.groupGains.assign((std::size_t) DamperCurves::kMaxGroups * DamperCurves::kMaxPoints, 1.0f);
        v.groupFactor.fill(1.0f);
        v.groupDamping = false;
    }
    carriedModes.reserve((std::size_t) capacity);
    body.reserve(owned != nullptr ? (int) owned->bodyFreq.size() : 0);
    if (owned != nullptr)
        for (std::size_t k = 0; k < owned->bodyFreq.size(); ++k)
            body.add(modePole(owned->bodyFreq[k], owned->bodyDecay[k], sampleRate), {}, {});
    bodyActive = false;
    bodyGain = bodyDecay = 1.0f;

    // The free strings: one group per string. A damper resting on a string damps it as the note's own curve
    // does over its first 0.1 s after a release, as a single exponential (the steep start of the curve: the
    // reference's resonance is gone 0.4 to 0.5 s after the pedal is lifted).
    stringGroup.fill(-1);
    stringDamper.fill(1.0f);
    stringApplied.fill(1.0f);
    stringRinging.fill(false);
    stringsCut = false;
    const int count = owned != nullptr ? (int) owned->strings.size() : 0;
    stringsTuning = tuningVersion.load(std::memory_order_relaxed);
    for (int note = 0; note < 128; ++note) { // the drive's shape of each note's register
        float onset = owned != nullptr ? owned->stringOnsetSeconds : 0.0f;
        float decay = owned != nullptr ? owned->stringDriveDecaySeconds : 0.0f;
        float gainDb = 0.0f, exponent = owned != nullptr ? owned->stringVelocityExponent : 0.0f;
        if (owned != nullptr)
            for (const auto& shape : owned->stringDriveShapes)
                if (note >= shape.firstNote) {
                    onset = shape.onsetSeconds;
                    decay = shape.decaySeconds;
                    gainDb = shape.gainDb;
                    exponent = shape.velocityExponent >= 0.0f ? shape.velocityExponent : owned->stringVelocityExponent;
                }
        driveGain[(std::size_t) note] = (float) std::pow(10.0, (double) gainDb / 20.0);
        driveExponent[(std::size_t) note] = exponent;
        onsetFactor[(std::size_t) note] = onset > 0.0f ? (float) std::exp(-1.0 / ((double) onset * sampleRate)) : 0.0f;
        driveFadeFactor[(std::size_t) note] = decay > 0.0f ? (float) std::exp(-1.0 / ((double) decay * sampleRate)) : 1.0f;
    }
    strings.reserve(owned != nullptr ? owned->stringResonators : 0, count);
    std::array<float, DamperCurves::kMaxPoints> curve {};
    for (int s = 0; s < count; ++s) {
        const auto& string = owned->strings[(std::size_t) s];
        if (string.midi < 0 || string.midi > 127 || string.resonators.empty())
            continue;
        const int g = strings.beginGroup();
        if (g < 0)
            break;
        const double ratio = tuningRatio(string.midi);
        for (const auto& res : string.resonators)
            strings.add(modePole(res.freq * ratio, res.decay, sampleRate), (float) (res.gain / sampleRate), res.left, res.right);
        strings.endGroup();
        stringGroup[(std::size_t) string.midi] = g;
        if (!owned->dampers.empty()) {
            owned->dampers.evaluate(string.midi, 2.0, curve.data());
            const int at = std::min(owned->dampers.length - 1, (int) std::lround(0.1 / owned->dampers.stepSeconds));
            const double sigma = at > 0 ? std::max(0.0, -std::log(std::max((double) curve[(std::size_t) at], 1e-9)) / (at * owned->dampers.stepSeconds)) : 0.0;
            stringDamper[(std::size_t) string.midi] = (float) std::exp(-sigma / sampleRate);
        }
    }
}

double ModalEngine::tuningRatio(int note) const noexcept
{
    const double a4 = std::clamp((double) referenceA4.load(std::memory_order_relaxed), 300.0, 600.0);
    const double cents = std::clamp((double) pitchClassCents[(std::size_t) (((note % 12) + 12) % 12)].load(std::memory_order_relaxed),
                                    -100.0, 100.0);
    return a4 / 440.0 * std::pow(2.0, cents / 1200.0);
}

ModalEngine::Settings ModalEngine::settings() const noexcept
{
    Settings s;
    s.blendLayers = blendLayers.load();
    s.restrikeAbsorption = restrikeAbsorption.load();
    s.body = bodyEnabled.load();
    s.residual = residualEnabled.load();
    s.sympathetic = sympatheticEnabled.load();
    s.heldKeys = heldKeysResonate.load();
    s.sympatheticGain = sympatheticGain.load();
    s.pedalCatch = pedalCatch.load();
    s.releaseNoise = releaseNoiseEnabled.load();
    s.referenceA4 = referenceA4.load();
    for (std::size_t pc = 0; pc < 12; ++pc)
        s.pitchClassCents[pc] = pitchClassCents[pc].load();
    s.unisonSpread = unisonSpread.load();
    s.voicingBassDb = voicingBassDb.load();
    s.voicingMiddleDb = voicingMiddleDb.load();
    s.voicingTrebleDb = voicingTrebleDb.load();
    s.voicingVelocity = voicingVelocity.load();
    s.voicingCorner = voicingCorner.load();
    s.dynamicRange = dynamicRange.load();
    s.sustainLength = sustainLength.load();
    s.damperRate = damperRate.load();
    s.attackSoftDb = attackSoftDb.load();
    s.attackSoftMs = attackSoftMs.load();
    s.lidOpen = lidOpen.load();
    return s;
}

void ModalEngine::applySettings(const Settings& s) noexcept
{
    blendLayers = s.blendLayers;
    restrikeAbsorption = std::clamp(s.restrikeAbsorption, 0.0f, 0.99f);
    bodyEnabled = s.body;
    residualEnabled = s.residual;
    sympatheticEnabled = s.sympathetic;
    heldKeysResonate = s.heldKeys;
    sympatheticGain = s.sympatheticGain;
    pedalCatch = s.pedalCatch;
    releaseNoiseEnabled = s.releaseNoise;
    referenceA4 = s.referenceA4;
    for (std::size_t pc = 0; pc < 12; ++pc)
        pitchClassCents[pc] = s.pitchClassCents[pc];
    unisonSpread = std::clamp(s.unisonSpread, 0.0f, 4.0f);
    voicingBassDb = s.voicingBassDb;
    voicingMiddleDb = s.voicingMiddleDb;
    voicingTrebleDb = s.voicingTrebleDb;
    voicingVelocity = std::clamp(s.voicingVelocity, 0.0f, 1.0f);
    voicingCorner = std::clamp(s.voicingCorner, 1.0f, 16.0f);
    dynamicRange = std::clamp(s.dynamicRange, 0.25f, 1.5f);
    sustainLength = std::clamp(s.sustainLength, 0.25f, 4.0f);
    damperRate = std::clamp(s.damperRate, 0.2f, 5.0f);
    attackSoftDb = std::clamp(s.attackSoftDb, -24.0f, 12.0f);
    attackSoftMs = std::clamp(s.attackSoftMs, 5.0f, 300.0f);
    lidOpen = std::clamp(s.lidOpen, 0.0f, 1.0f);
    tuningVersion.fetch_add(1);
}

float ModalEngine::voicingDb(int note, int velocity) const noexcept
{
    const float bass = voicingBassDb.load(std::memory_order_relaxed);
    const float middle = voicingMiddleDb.load(std::memory_order_relaxed);
    const float treble = voicingTrebleDb.load(std::memory_order_relaxed);
    const float x = (float) std::clamp(note, 21, 108);
    const float db = x <= 60.0f ? bass + (middle - bass) * (x - 21.0f) / 39.0f : middle + (treble - middle) * (x - 60.0f) / 48.0f;
    const float follow = std::clamp(voicingVelocity.load(std::memory_order_relaxed), 0.0f, 1.0f);
    return db * ((1.0f - follow) + follow * (float) std::clamp(velocity, 0, 127) / 127.0f);
}

void ModalEngine::setVoicing(Voice& v, int note, int velocity, bool keepState) noexcept
{
    auto& f = v.voicing;
    const float db = voicingDb(note, velocity);
    if (std::abs(db) < 0.01f) {
        f.on = false; // the sound as measured, exactly
        return;
    }
    // A high shelf (RBJ, slope 1) whose midpoint follows the note: the partials above the corner move by db.
    const double f0 = 440.0 * std::pow(2.0, (note - 69) / 12.0) * tuningRatio(note);
    const double corner = std::clamp((double) voicingCorner.load(std::memory_order_relaxed), 1.0, 16.0) * f0;
    f.f.take(Biquad::shelf(sampleRate, corner, db, true));
    if (!keepState || !f.on)
        f.f.reset();
    f.on = true;
}

void ModalEngine::setAttack(Voice& v, bool restrike) noexcept
{
    auto& f = v.attack;
    const float db = std::clamp(attackSoftDb.load(std::memory_order_relaxed), -24.0f, 12.0f);
    if (std::abs(db) < 0.01f) {
        f.on = false;
        return;
    }
    const double ms = std::clamp((double) attackSoftMs.load(std::memory_order_relaxed), 5.0, 300.0);
    f.f.take(Biquad::shelf(sampleRate, kAttackCornerHz, db, true));
    if (!f.on)
        f.f.reset();
    f.on = true;
    v.attackSamples = std::max<std::int64_t>(1, std::llround(ms * 1e-3 * sampleRate));
    v.attackRise = restrike ? std::max<std::int64_t>(1, std::llround(kRestrikeRiseSeconds * sampleRate)) : 0;
}

void ModalEngine::softenAttack(Voice& v, float* l, float* r, int m) noexcept
{
    // The sound through the attack's shelf, mixed back to the sound itself as the attack ends (no filter switched
    // while it sounds: at the end, the mix is the sound alone).
    auto& f = v.attack.f;
    const double total = (double) v.attackSamples, rise = (double) v.attackRise;
    float l1 = f.s1[0], l2 = f.s2[0], r1 = f.s1[1], r2 = f.s2[1];
    for (int k = 0; k < m; ++k) {
        const double t = (double) (v.sinceOn + k);
        float w = t < total ? (float) (0.5 + 0.5 * std::cos(0.5 * kTwoPi * t / total)) : 0.0f;
        if (t < rise)
            w *= (float) (t / rise);
        const float inL = l[k], outL = f.b0 * inL + l1;
        l1 = f.b1 * inL - f.a1 * outL + l2;
        l2 = f.b2 * inL - f.a2 * outL;
        l[k] = inL + w * (outL - inL);
        const float inR = r[k], outR = f.b0 * inR + r1;
        r1 = f.b1 * inR - f.a1 * outR + r2;
        r2 = f.b2 * inR - f.a2 * outR;
        r[k] = inR + w * (outR - inR);
    }
    f.s1[0] = l1;
    f.s2[0] = l2;
    f.s1[1] = r1;
    f.s2[1] = r2;
    if (v.sinceOn + m >= v.attackSamples)
        v.attack.on = false;
}

void ModalEngine::applyLid(float* const* outputs, int numChannels, int n, float gain) noexcept
{
    // The lid: a shelf on the treble and a level, both in proportion to how far it is closed; a level moved glides
    // over the block. Open: neither (the output gain alone, as it always was).
    const float lid = std::clamp(lidOpen.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (lid != lidApplied) {
        const double closed = 1.0 - (double) lid;
        const bool wasFlat = lidShelf.flat();
        lidShelf.take(Biquad::shelf(sampleRate, kLidCornerHz, kLidClosedDb * closed, true));
        if (wasFlat)
            lidShelf.reset();
        lidTarget = (float) std::pow(10.0, kLidClosedLevelDb * closed / 20.0);
        if (lidApplied < 0.0f)
            lidLevel = lidTarget;
        lidApplied = lid;
    }
    if (!lidShelf.flat())
        for (int c = 0; c < std::min(numChannels, 2); ++c)
            if (outputs[c] != nullptr)
                lidShelf.process(outputs[c], n, c);
    if (lidLevel == 1.0f && lidTarget == 1.0f) {
        for (int c = 0; c < std::min(numChannels, 2); ++c)
            if (outputs[c] != nullptr)
                for (int k = 0; k < n; ++k)
                    outputs[c][k] *= gain;
        return;
    }
    const float step = (lidTarget - lidLevel) / (float) std::max(1, n);
    for (int c = 0; c < std::min(numChannels, 2); ++c)
        if (outputs[c] != nullptr)
            for (int k = 0; k < n; ++k)
                outputs[c][k] *= gain * (lidLevel + step * (float) (k + 1));
    lidLevel = lidTarget;
}

void ModalEngine::retuneStrings() noexcept
{
    // Every resonator of a free string at its note's tuning (the exact poles), then its damping again.
    if (current == nullptr) // audio thread: its own profile
        return;
    for (const auto& string : current->strings) {
        if (string.midi < 0 || string.midi > 127)
            continue;
        const int g = stringGroup[(std::size_t) string.midi];
        if (g < 0)
            continue;
        const double ratio = tuningRatio(string.midi);
        for (int k = 0; k < (int) string.resonators.size(); ++k) {
            const auto& res = string.resonators[(std::size_t) k];
            strings.setExactPole(g, k, modePole(res.freq * ratio, res.decay, sampleRate));
        }
        strings.dampGroup(g, stringApplied[(std::size_t) string.midi]);
    }
}

void ModalEngine::install(std::shared_ptr<const ModalProfile> profile)
{
    published.store(nullptr, std::memory_order_release);
#if !defined(__EMSCRIPTEN__) // in a browser, the profile is installed on the audio thread itself: nothing to wait for
    if (owned != nullptr) {
        // The audio thread sees the null pointer on its next block and says so; then the voices are ours.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (audioRunning.load() && seenByAudio.load(std::memory_order_acquire) != nullptr
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#endif
    owned = std::move(profile);
    allocate();
    published.store(owned.get(), std::memory_order_release);
}

void ModalEngine::allSoundOff() noexcept
{
    const auto fast = (float) std::exp(-1.0 / (kFastFadeS * sampleRate));
    for (auto& v : voices)
        if (v.active) {
            v.keyDown = false;
            v.released = true;
            v.decay = std::min(v.decay, fast);
        }
    bodyDecay = std::min(bodyDecay, fast);
    stringsCut = true;
}

void ModalEngine::process(float* const* outputs, int numChannels, int n, const Event* events, int numEvents) noexcept
{
    [[maybe_unused]] NoDenormals noDenormals;
    for (int c = 0; c < numChannels; ++c)
        if (outputs[c] != nullptr)
            std::fill(outputs[c], outputs[c] + n, 0.0f);

    const auto* p = published.load(std::memory_order_acquire);
    const bool fresh = resetPending.exchange(false, std::memory_order_acq_rel);
    if (p != current || fresh) {
        for (auto& v : voices)
            v.active = false;
        body.silence();
        bodyActive = false;
        for (int g = 0; g < strings.groups(); ++g)
            strings.silence(g);
        stringRinging.fill(false);
        if (fresh) { // a host's reset: the rest of the state as at a start
            bodyGain = bodyDecay = 1.0f;
            stringsCut = false;
            ageCounter = 0;
            pedalLift = 0.0f;
            softPedal = 0.0f;
            sostenuto = false;
            sostenutoHeld.fill(false);
            keyHeld.fill(false);
            noteDriven.fill(false);
            knockScale = 1.0f;
            sustainDown.store(false, std::memory_order_relaxed);
            sostenutoDown.store(false, std::memory_order_relaxed);
            pedalValue.store(0, std::memory_order_relaxed);
            pedalPosition.store(0.0f, std::memory_order_relaxed);
            softPedalPosition.store(0.0f, std::memory_order_relaxed);
        }
        current = p;
        seenByAudio.store(p, std::memory_order_release);
    }
    if (p == nullptr || maxBlockSize == 0) {
        activeVoices.store(0, std::memory_order_relaxed);
        activeModes.store(0, std::memory_order_relaxed);
        return;
    }

    int start = 0;
    for (int i = 0; i <= numEvents; ++i) { // every event lands on its exact sample
        const int end = i < numEvents ? std::clamp((int) events[i].offset, start, n) : n;
        if (end > start)
            render(*p, outputs, numChannels, start, end - start);
        if (i < numEvents)
            apply(*p, events[i]);
        start = end;
    }

    applyLid(outputs, numChannels, n, outputGain.load(std::memory_order_relaxed));
    float peak = 0.0f;
    for (int c = 0; c < std::min(numChannels, 2); ++c)
        if (outputs[c] != nullptr)
            for (int k = 0; k < n; ++k)
                peak = std::max(peak, std::abs(outputs[c][k]));
    float previous = outputPeak.load(std::memory_order_relaxed);
    while (peak > previous && !outputPeak.compare_exchange_weak(previous, peak)) {}

    int active = 0, modes = bodyActive ? body.size() : 0;
    for (const auto& v : voices)
        if (v.active) {
            ++active;
            modes += v.bank.size();
        }
    int ringing = 0;
    for (const bool r : stringRinging)
        ringing += r ? 1 : 0;
    activeVoices.store(active, std::memory_order_relaxed);
    activeModes.store(modes, std::memory_order_relaxed);
    ringingStrings.store(ringing, std::memory_order_relaxed);
}

void ModalEngine::apply(const ModalProfile& p, const Event& e) noexcept
{
    const int type = e.status & 0xF0;
    if (type == 0x90 && e.data2 > 0)
        noteOn(p, e.data1, e.data2);
    else if (type == 0x80 || type == 0x90)
        noteOff(p, e.data1);
    else if (type == 0xB0) {
        if (e.data1 == 64) {
            pedalLift = std::clamp(p.pedalLift[(std::size_t) std::min<int>(e.data2, 127)], 0.0f, 1.0f);
            sustainDown.store(pedalLift >= 0.5f, std::memory_order_relaxed);
            pedalPosition.store(pedalLift, std::memory_order_relaxed);
            pedalValue.store(std::min<int>(e.data2, 127), std::memory_order_relaxed);
            updateAllDampers(p);
        } else if (e.data1 == 66) {
            const bool down = e.data2 >= 64;
            if (down && !sostenuto) {
                // The rod catches every damper raised now: those of the keys held, all of them under the sustain
                // pedal (a damper on its way down is not raised any more, and is not caught).
                if (pedalLift >= 0.5f)
                    sostenutoHeld.fill(true);
                else
                    for (const auto& v : voices)
                        if (v.active && v.keyDown)
                            sostenutoHeld[(std::size_t) v.note] = true;
            } else if (!down && sostenuto) {
                sostenutoHeld.fill(false);
            }
            if (down != sostenuto) {
                sostenuto = down;
                sostenutoDown.store(down, std::memory_order_relaxed);
                updateAllDampers(p);
            }
        } else if (e.data1 == 67) { // the soft pedal: continuous, as the action shifts
            softPedal = (float) std::min<int>(e.data2, 127) / 127.0f;
            softPedalPosition.store(softPedal, std::memory_order_relaxed);
        } else if (e.data1 == 120 || e.data1 == 123)
            allSoundOff();
    }
}

void ModalEngine::noteOn(const ModalProfile& p, int note, int velocity) noexcept
{
    noteOnsReceived.fetch_add(1, std::memory_order_relaxed);
    lastNote.store(note, std::memory_order_relaxed);
    lastVelocity.store(velocity, std::memory_order_relaxed);
    const auto* entry = p.find(note);
    if (entry == nullptr) {
        lastLayerVelocity.store(0, std::memory_order_relaxed);
        return;
    }

    // The two layers around the velocity, the nearest one (played as measured) and the other one (level only).
    const auto& layers = entry->layers;
    std::size_t ia = 0, ib = 0;
    if (velocity >= layers.back().velocity)
        ia = ib = layers.size() - 1;
    else if (velocity > layers.front().velocity) {
        while (layers[ib].velocity < velocity)
            ++ib;
        ia = layers[ib].velocity == velocity ? ib : ib - 1;
    }
    const auto& la = layers[ia];
    const auto& lb = layers[ib];
    const double alpha = ia == ib ? 0.0 : (double) (velocity - la.velocity) / (double) (lb.velocity - la.velocity);
    const bool nearA = alpha <= 0.5;
    const auto& near = nearA ? la : lb;
    const auto& other = nearA ? lb : la;
    lastLayerVelocity.store(near.velocity, std::memory_order_relaxed);

    // Overall gain: the interpolated level against the nearest layer's; below the softest layer, the level slope
    // of the two softest layers is extended (at most -26 dB).
    float overall = interpolateLevel(la.level, lb.level, alpha) / std::max(near.level, 1e-20f);
    if (velocity < layers.front().velocity) {
        double dbPerStep = 20.0 / std::max(1, layers.front().velocity); // one layer: proportional
        if (layers.size() > 1 && layers[0].level > 0.0f && layers[1].level > 0.0f)
            dbPerStep = 20.0 * std::log10((double) layers[1].level / layers[0].level)
                        / (double) (layers[1].velocity - layers[0].velocity);
        overall = (float) std::max(0.05, std::pow(10.0, -dbPerStep * (layers.front().velocity - velocity) / 20.0));
    }
    overall = std::clamp(overall, 0.0f, kMaxGain);
    // The dynamic range: this strike's level below the note's loudest layer (in dB) scaled, every gain of the voice
    // below moving together. The sustain length: every mode's decay rate divided by it.
    float dyn = 1.0f;
    if (const float range = std::clamp(dynamicRange.load(std::memory_order_relaxed), 0.25f, 1.5f); range != 1.0f) {
        const double top = layers.back().level, here = (double) overall * near.level;
        if (top > 0.0 && here > 0.0)
            dyn = (float) std::clamp(std::pow(top / here, 1.0 - (double) range), 0.05, 20.0);
    }
    const double sustain = std::clamp((double) sustainLength.load(std::memory_order_relaxed), 0.25, 4.0);
    // Between two layers (blendLayers): how far toward the other layer (0 to 0.5), and the weights, in amplitude, of
    // a partial only the lower layer has (1 - alpha) or only the upper one (alpha).
    const bool blend = &near != &other && alpha > 0.0 && alpha < 1.0 && blendLayers.load(std::memory_order_relaxed)
                    && blendFreq.size() >= near.modes.size();
    const double toward = nearA ? alpha : 1.0 - alpha;
    const float nearOnly = (float) (nearA ? 1.0 - alpha : alpha), otherOnly = (float) (nearA ? alpha : 1.0 - alpha);

    // The note's string. Struck again while it rings: what it plays now (its output envelope folded in) is kept,
    // to be carried into the new strike once its modes are in place; the previous strike's residual plays on.
    auto& v = voices[(std::size_t) std::clamp(note, 0, kMaxVoices - 1)];
    carriedModes.clear();
    const bool carry = v.active;
    float carriedOnset = 0.0f;
    if (carry) {
        if (v.mergeAt >= 0)
            mergeEarlier(v); // struck again within the last contact: that one is over
        const float held = v.gain * v.curveGain;
        for (int k = 0; k < v.bank.size() && k < (int) v.modeNumber.size(); ++k) {
            Carried mode;
            mode.number = v.modeNumber[(std::size_t) k];
            v.bank.current(k, mode.pole, mode.left, mode.right);
            mode.left *= held;
            mode.right *= held;
            if (carriedModes.size() < carriedModes.capacity())
                carriedModes.push_back(mode);
        }
        for (auto& t : v.tails) // the earlier residuals go on, under the new strike's envelope
            t.gain *= held;
        if (v.layer != nullptr && v.residualGain > 0.0f && v.residualPos < (double) v.layer->residualFrames) {
            auto* slot = &v.tails[0]; // a free one, else the one nearest its end
            for (auto& t : v.tails)
                if (t.gain <= 0.0f || (slot->gain > 0.0f && t.pos / std::max(1, t.layer->residualFrames)
                                                                > slot->pos / std::max(1, slot->layer->residualFrames)))
                    slot = &t;
            *slot = { v.layer, v.residualPos, v.residualStep, v.residualGain * held };
        }
        carriedOnset = v.driveOnset; // the string's drive of the free strings has built up already
    } else {
        for (auto& t : v.tails)
            t.gain = 0.0f;
    }

    // The voicing of this strike (a re-strike keeps the filter's state: what rings goes on through it).
    setVoicing(v, note, velocity, carry);
    setAttack(v, carry);
    v.bank.clear();
    v.modeNumber.clear();
    v.targetPole.clear();
    v.mergeAt = -1;
    v.repoleAt = -1;
    v.ramps.clear();
    v.fades.clear();
    v.slots.clear();
    v.groupFactor.fill(1.0f);
    v.groupDamping = false;
    // Tuning: every frequency of the note scaled (the residual played faster or slower by as much).
    const double tuning = tuningRatio(note);
    // The level ramp starts once the residual is over (output samples); without a residual there is nothing to
    // stay consistent with, and each partial starts at its own level.
    const auto residualEnd = (std::int64_t) std::ceil((double) near.residualFrames * sampleRate / (p.residualRate * tuning));
    const bool ramped = residualEnd > 0 && overall > 1e-6f;
    const auto rampLength = std::max<std::int64_t>(1, (std::int64_t) std::llround(kLevelRampSeconds * sampleRate));
    const double nyquist = 0.5 * sampleRate;
    // The coupled strings: the soft pedal and the unison tuning move the measured modes by what the model of each
    // partial's unison gives against the strike and tuning measured (exactly the measured modes otherwise).
    const double spread = std::clamp((double) unisonSpread.load(std::memory_order_relaxed), 0.02, 4.0);
    const double soft = std::clamp((double) softPedal, 0.0, 1.0);
    const bool moved = soft > 0.0 || std::abs(spread - 1.0) > 1e-6;
    const int noteStrings = stringsOf(note);

    // Between two layers: each mode's pole moved toward the other layer's mode of the partial nearest to it (taken
    // once the attack's ramp is over), and the attack's gain: over the residual the strike can only change as a whole
    // (the residual cancels the modes extrapolated back to the note-on), so one gain, bringing its energy to the one
    // interpolated (in dB) between the layers' attacks as recorded. (A filter would shape the attack's timbre too,
    // but filtering the modes by their response at their poles misses the filter's own transient, as large as the
    // modes extrapolated to the note-on: the cancellation would go.)
    // The layers' measures (profile section LEVL): without them, the overall gain over the attack and the
    // profile's levels after it.
    const bool measured = p.measured();
    auto sustainOf = [&](const ModalProfile::Layer& layer, std::size_t partial) {
        return measured && partial < layer.sustainLevels.size() ? layer.sustainLevels[partial] : 0.0f;
    };
    // A partial whose modes interfere: moving their frequencies would change their beating (modes that nearly cancel
    // each other might not, moved), and its level with them.
    auto interferes = [&](const ModalProfile::Layer& layer, std::size_t partial) {
        if (!measured || partial >= layer.sustainLevels.size() || partial >= layer.separateLevels.size())
            return false;
        const float together = layer.sustainLevels[partial], apart = layer.separateLevels[partial];
        return together > 0.0f && apart > 0.0f && std::abs(std::log((double) together / apart)) > kInterferingLog;
    };
    float attackGain = overall;
    if (blend) {
        std::size_t o = 0, index = 0;
        for (const auto& part : near.partials) {
            const std::size_t pi = index++;
            while (o < other.partials.size() && other.partials[o].number < part.number)
                ++o;
            const bool shared = o < other.partials.size() && other.partials[o].number == part.number
                             && other.partials[o].modeCount <= (int) logDecays.size() && !interferes(near, pi);
            // The poles: each mode toward the other layer's mode of this partial nearest to it (in frequency, then
            // decay: the other partial's log decays once); a partial whose modes interfere keeps its own.
            const auto* op = shared ? &other.partials[o] : nullptr;
            if (op != nullptr)
                for (int q = 0; q < op->modeCount; ++q)
                    logDecays[(std::size_t) q] = std::log(std::max(other.modes[(std::size_t) (op->firstMode + q)].decay, 1e-3));
            for (int k = 0; k < part.modeCount; ++k) {
                const auto mi = (std::size_t) (part.firstMode + k);
                const auto& m = near.modes[mi];
                blendFreq[mi] = 0.0;
                blendScale[mi] = 1.0;
                if (op == nullptr)
                    continue;
                const double logDecay = std::log(std::max(m.decay, 1e-3)), perHz = 1000.0 / std::max(m.freq, 1.0);
                int best = -1;
                double bestDistance = 1e300;
                for (int q = 0; q < op->modeCount; ++q) {
                    const double d = std::abs(other.modes[(std::size_t) (op->firstMode + q)].freq - m.freq) * perHz
                                   + std::abs(logDecays[(std::size_t) q] - logDecay);
                    if (d < bestDistance) {
                        bestDistance = d;
                        best = q;
                    }
                }
                if (best >= 0) {
                    blendFreq[mi] = toward * (other.modes[(std::size_t) (op->firstMode + best)].freq - m.freq);
                    blendScale[mi] = std::exp(toward * (logDecays[(std::size_t) best] - logDecay));
                }
            }
        }
        if (measured && near.attackEnergy > 0.0f) {
            const double wanted = interpolateLevel(la.attackEnergy, lb.attackEnergy, alpha);
            attackGain = (float) std::clamp(std::sqrt(wanted / near.attackEnergy), 0.1, 10.0);
        }
    }
    auto addModes = [&](const ModalProfile::Layer& layer, std::size_t pi, const auto& part, float start, bool movePoles) {
        UnisonMove move;
        float common = 1.0f;
        if (moved) {
            if (soft > 0.0)
                common = (float) std::pow(10.0, -soft * kSoftFeltDbPerOctave * std::log2((double) std::max(1, part.number)) / 20.0);
            const auto* u = pi < layer.unison.size() ? &layer.unison[pi] : nullptr;
            if (u != nullptr && u->strings >= 2)
                moveUnison(*u, layer.modes.data() + part.firstMode, spread, soft, move);
            else if (noteStrings >= 2 && soft > 0.0)
                common *= (float) ((noteStrings - soft) / noteStrings);
        }
        const float scaled = start * common * dyn;
        auto push = [&](double freq, double decay, std::complex<float> l, std::complex<float> r, double df, double ds) {
            if (sustain != 1.0)
                decay /= sustain;
            const auto pole = modePole(freq * tuning, decay, sampleRate);
            if (!v.bank.add(pole, l * scaled, r * scaled))
                return;
            v.modeNumber.push_back(part.number);
            // The moved pole: the measured one times e^z, z = (-(its decay's change) + 2 pi i (its frequency's)) / fs,
            // tiny (its series, rather than an exponential per mode; the bank keeps its poles in single precision).
            const std::complex<double> z(-(std::max(0.01, decay * ds) - decay), kTwoPi * df * tuning);
            const auto step = z / sampleRate;
            const auto factor = std::abs(step) < 1e-2 ? 1.0 + step * (1.0 + step / 2.0 * (1.0 + step / 3.0 * (1.0 + step / 4.0)))
                                                      : std::exp(step);
            v.targetPole.push_back(std::complex<float>(df == 0.0 && ds == 1.0 ? pole : pole * factor));
        };
        for (int k = 0; k < part.modeCount; ++k) {
            const auto mi = (std::size_t) (part.firstMode + k);
            const auto& m = layer.modes[mi];
            const double df = movePoles ? blendFreq[mi] : 0.0, ds = movePoles ? blendScale[mi] : 1.0;
            if (m.freq * tuning >= nyquist)
                continue;
            std::complex<float> l = m.left, r = m.right;
            double shift = 0.0; // the part the model does not explain: the model's change of frequency only
            for (int q = 0; q < move.n; ++q)
                if (move.mode[q] == k) {
                    const bool apart = std::abs(move.dFreq[q]) > 1e-9 || std::abs(move.dDecay[q]) > 1e-9;
                    l = apart ? move.keepL[q] : move.keepL[q] + move.movedL[q];
                    r = apart ? move.keepR[q] : move.keepR[q] + move.movedR[q];
                    if (apart && v.bank.size() < v.bank.capacity() - 1) { // the model's part, on its moved pole
                        push(m.freq + move.dFreq[q], std::max(0.01, m.decay + move.dDecay[q]), move.movedL[q], move.movedR[q], df, ds);
                        shift = move.dFreq[q];
                    }
                }
            push(m.freq + shift, m.decay, l, r, df, ds);
        }
    };
    std::size_t j = 0; // walks the other layer's partials (both ascending)
    std::size_t partIndex = 0;
    for (const auto& part : near.partials) {
        const std::size_t pi = partIndex++;
        while (j < other.partials.size() && other.partials[j].number < part.number)
            ++j;
        float g = overall;
        const bool shared = j < other.partials.size() && other.partials[j].number == part.number;
        // Between two layers: what moving its poles (once the ramp is over) does to the partial's level over the
        // sustain (without the measures, over the first second, as the profile's levels), from its modes' energies
        // (its modes do not interfere: those of a partial whose modes do are not moved).
        double poleShift = 1.0;
        const float nearSustain = sustainOf(near, pi);
        if (blend && shared && !interferes(near, pi)) {
            const double tr = (double) (residualEnd + rampLength) / sampleRate;
            const double from = measured ? p.sustainFrom : 0.0, to = measured ? p.sustainTo : 1.0;
            double e0 = 0.0, e1 = 0.0; // the modes' energies, measured and moved poles
            for (int k = 0; k < part.modeCount; ++k) {
                const auto mi = (std::size_t) (part.firstMode + k);
                const auto& m = near.modes[mi];
                const double a2 = (double) std::norm(m.left) + (double) std::norm(m.right);
                if (!measured)
                    e0 += a2 * movedEnergy(m.decay, m.decay, tr, from, to);
                e1 += a2 * movedEnergy(m.decay, m.decay * blendScale[mi], tr, from, to);
            }
            if (measured && pi < near.separateLevels.size()) // the same energies, measured poles (as the loader's)
                e0 = 4.0 * (to - from) * (double) near.separateLevels[pi] * near.separateLevels[pi];
            if (e0 > 0.0 && e1 > 0.0)
                poleShift = std::sqrt(e1 / e0);
        }
        if (&near != &other && shared && part.level > 0.0f && other.partials[j].level > 0.0f) {
            // In dB between the two layers' levels as the partial sounds over the sustain (the profile's levels
            // without them); the nearer layer's partial brought to it, its poles moved.
            float nearLevel = part.level, otherLevel = other.partials[j].level;
            if (const float t = sustainOf(other, j); nearSustain > 0.0f && t > 0.0f) {
                nearLevel = nearSustain;
                otherLevel = t;
            }
            const float wanted = nearA ? interpolateLevel(nearLevel, otherLevel, alpha) : interpolateLevel(otherLevel, nearLevel, alpha);
            g = std::clamp((float) (wanted / nearLevel / poleShift), kMinGain * overall, kMaxGain * overall);
        } else if (blend && !shared) {
            g = overall * nearOnly; // a partial the other layer lacks fades out toward it
        }
        const int first = v.bank.size();
        bool complete = true;
        for (int k = 0; k < part.modeCount; ++k)
            complete = complete && near.modes[(std::size_t) (part.firstMode + k)].freq * tuning < nyquist;
        // A partial with a mode past Nyquist (inaudible) is not ramped: its bank modes would not map back. Between two
        // layers, the attack is the nearer layer's strike at the attack's gain.
        const double from = blend ? (double) attackGain : (double) overall;
        const bool ramp = ramped && complete && std::abs(g - from) > 1e-4 * from && v.ramps.size() < v.ramps.capacity();
        const float start = blend ? (ramped ? attackGain : g) : ramp ? overall : g;
        addModes(near, pi, part, start, blend && shared);
        if (ramp)
            v.ramps.push_back({ first, v.bank.size() - first, part.firstMode,
                                (float) std::pow((double) g / from, 1.0 / (double) rampLength) });
        if (v.bank.size() == first || v.slots.size() == v.slots.capacity())
            continue;
        v.slots.push_back({ first, v.bank.size() - first, part.number, p.dampers.groupOf(part.number), false });
        // Past its fade time, the partial dies out (after the level ramp, whose end puts the exact poles back).
        if (part.fadeSeconds > 0.0f && v.fades.size() < v.fades.capacity())
            v.fades.push_back({ std::max((std::int64_t) std::llround(part.fadeSeconds * sustain * sampleRate), residualEnd + rampLength),
                                (int) v.slots.size() - 1 });
    }
    // Re-strike: each mode of the string's vibration added to the new mode of the same partial nearest to it (pole:
    // frequency and decay; with the same layer, the very same mode: the strikes add exactly, as on a string). Each
    // old mode going whole into one new mode, the waveform carries on exactly, and a slow mode (the after-sound,
    // most of what is left) stays slow. A partial the new layer lacks keeps its own modes and poles (outside any
    // slot: the dampers' curve applies to it, not the group curves).
    // The earlier vibration under the hammer: its modes after the new strike's, those of partials the new strike
    // lacks first (they stay), then the others (they join it at the contact's end); all of them losing the
    // absorbed energy over the contact, smoothly (no step in the waveform).
    // Between two layers: the partials only the other layer has come in over the ramp, from -60 dB (their attack is
    // its residual's business: extrapolated back to the note-on, they would sound a burst), as that layer has them.
    if (blend) {
        const float otherGain = std::clamp(interpolateLevel(la.level, lb.level, alpha) / std::max(other.level, 1e-20f), 0.0f, kMaxGain)
                              * otherOnly;
        constexpr float kQuiet = 1e-3f;
        std::size_t index = 0, w = 0;
        for (const auto& part : other.partials) {
            const std::size_t pi = index++;
            while (w < near.partials.size() && near.partials[w].number < part.number)
                ++w;
            if ((w < near.partials.size() && near.partials[w].number == part.number) || v.ramps.size() == v.ramps.capacity())
                continue;
            const int first = v.bank.size();
            addModes(other, pi, part, ramped ? otherGain * kQuiet : otherGain, false);
            if (ramped && v.bank.size() > first)
                v.ramps.push_back({ first, v.bank.size() - first, part.firstMode, (float) std::pow(1.0 / kQuiet, 1.0 / (double) rampLength) });
        }
    }
    if (!carriedModes.empty()) {
        auto hasSlot = [&](int number) {
            for (const auto& slot : v.slots)
                if (slot.number == number)
                    return true;
            return false;
        };
        v.keepFirst = v.bank.size();
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1)
                v.mergeFirst = v.bank.size();
            for (const auto& mode : carriedModes)
                if (hasSlot(mode.number) == (pass == 1) && v.bank.size() < v.bank.capacity()) {
                    v.bank.add(std::complex<double>(mode.pole.real(), mode.pole.imag()), mode.left, mode.right);
                    v.modeNumber.push_back(mode.number);
                    v.targetPole.push_back(mode.pole);
                }
        }
        const auto contact = std::max<std::int64_t>(1, (std::int64_t) std::llround(kContactSeconds * sampleRate));
        const double kept = 1.0 - std::clamp((double) restrikeAbsorption.load(std::memory_order_relaxed), 0.0, 0.99);
        v.bank.scalePoles(v.keepFirst, v.bank.size() - v.keepFirst, (float) std::pow(kept, 0.5 / (double) contact));
        v.mergeAt = contact;
    }

    std::sort(v.fades.begin(), v.fades.end(), [](const PartialFade& a, const PartialFade& b) { return a.at < b.at; });
    v.nextFade = 0;
    v.ramp = v.ramps.empty() ? Ramp::none : Ramp::waiting;
    v.rampStart = residualEnd;
    v.rampEnd = residualEnd + rampLength;
    v.repoleAt = blend ? v.rampEnd : -1; // between two layers: the moved poles once the ramp is over

    if (bodyEnabled.load(std::memory_order_relaxed) && body.size() == (int) near.bodyLeft.size()) {
        // The overall gain, like the residual: the fast resonances extrapolated back to the note-on are cancelled
        // by the residual only at the same gain. (Against the strings, the body hardly changes between layers.)
        const float gb = (blend ? attackGain : overall) * dyn;
        if (bodyDecay < 1.0f) { // the body was fading out (all sound off): start again from silence
            body.silence();
            bodyDecay = bodyGain = 1.0f;
        }
        for (int k = 0; k < body.size(); ++k)
            body.excite(k, near.bodyLeft[(std::size_t) k] * (gb / bodyGain), near.bodyRight[(std::size_t) k] * (gb / bodyGain));
        bodyActive = true;
    }

    v.active = true;
    v.keyDown = true;
    v.released = false;
    v.damperCurve = false;
    v.fadingOut = false;
    v.note = note;
    v.velocity = velocity;
    v.layer = &near;
    v.residualPos = 0.0;
    v.residualStep = p.residualRate / sampleRate * tuning;
    v.residualGain = residualEnabled.load(std::memory_order_relaxed) ? (blend ? attackGain : overall) * dyn : 0.0f;
    v.gain = v.decay = 1.0f;
    v.curveGain = 1.0f;
    v.curvePos = 0.0;
    v.damperSpeed = 0.0f;
    v.driveGain = driveGain[(std::size_t) note]
                * (float) std::pow((double) velocity / (double) p.stringReferenceVelocity, (double) driveExponent[(std::size_t) note]);
    v.driveOnset = carriedOnset;
    v.driveFade = 1.0f;
    v.sinceOn = 0;
    v.age = ++ageCounter;
}

void ModalEngine::noteOff(const ModalProfile& p, int note) noexcept
{
    for (auto& v : voices)
        if (v.active && v.keyDown && v.note == note) {
            v.keyDown = false;
            updateDamping(p, v);
        }
}

void ModalEngine::updateAllDampers(const ModalProfile& p) noexcept
{
    // Dampers landing together (the pedal lifted on many notes): one knock of the damper rail, their levels summed
    // in energy rather than in amplitude (the nearby notes share one measured knock).
    int landing = 0;
    for (const auto& v : voices)
        landing += v.active && !damperRaised(v) && !v.damperCurve && pedalLift < 1.0f ? 1 : 0;
    knockScale = landing > 1 ? 1.0f / std::sqrt((float) landing) : 1.0f;
    for (auto& v : voices)
        if (v.active && !v.keyDown)
            updateDamping(p, v);
    knockScale = 1.0f;
}

void ModalEngine::updateDamping(const ModalProfile& p, Voice& v) noexcept
{
    if (!v.active || p.dampers.empty())
        return; // no damper data: the string keeps ringing
    float speed = damperRaised(v) ? 0.0f : 1.0f - pedalLift;
    if (v.damperCurve && !pedalCatch.load(std::memory_order_relaxed))
        speed = std::max(speed, v.damperSpeed); // the reference: a release, once started, runs to its end
    if (speed > 0.0f && !v.damperCurve) { // the damper reaches the string: its curves for the time held
        p.dampers.evaluate(v.note, (double) v.sinceOn / sampleRate, v.releaseGains.data(), v.velocity);
        if (p.dampers.groups() > 0)
            p.dampers.evaluateGroups(v.note, (double) v.sinceOn / sampleRate, v.groupGains.data(), v.velocity);
        v.curvePoints = p.dampers.length;
        v.curvePos = 0.0;
        v.damperCurve = true;
        v.released = true;
        releaseKnock(p, v, speed * knockScale);
    }
    v.damperSpeed = speed;
}

void ModalEngine::releaseKnock(const ModalProfile& p, const Voice& v, float strength) noexcept
{
    const auto& noise = p.releaseNoise;
    if (noise.empty() || !releaseNoiseEnabled.load(std::memory_order_relaxed) || !bodyEnabled.load(std::memory_order_relaxed)
        || body.size() == 0 || noise.left.size() != noise.notes.size() * noise.velocities.size() * (std::size_t) body.size())
        return;
    // The nearest measured note; between the two measured velocities around the voice's, the level in dB.
    std::size_t n = 0;
    for (std::size_t i = 1; i < noise.notes.size(); ++i)
        if (std::abs(noise.notes[i] - v.note) < std::abs(noise.notes[n] - v.note))
            n = i;
    const auto& vel = noise.velocities;
    std::size_t lo = 0;
    while (lo + 1 < vel.size() && vel[lo + 1] <= v.velocity)
        ++lo;
    const std::size_t hi = std::min(lo + 1, vel.size() - 1);
    const double w = hi == lo ? 0.0 : std::clamp((double) (v.velocity - vel[lo]) / (double) (vel[hi] - vel[lo]), 0.0, 1.0);
    const std::size_t near = w < 0.5 ? lo : hi;
    const auto level = [&](std::size_t k) { return (double) noise.levelDb[n * vel.size() + k]; };
    double db = level(lo) * (1.0 - w) + level(hi) * w - level(near);
    if (v.velocity < vel.front()) // below the softest measured velocity: proportional to the velocity
        db += 20.0 * std::log10(std::max(1.0, (double) v.velocity) / vel.front());
    // The longer the key was held, the weaker the knock (interpolated in log(hold), clamped at both ends).
    if (const auto& h = noise.holds; !h.empty() && noise.holdDb.size() == h.size()) {
        const double x = std::log(std::max((double) v.sinceOn / sampleRate, 1e-3));
        std::size_t i = 0;
        while (i + 1 < h.size() && std::log((double) h[i + 1]) <= x)
            ++i;
        double offset = noise.holdDb[i];
        if (i + 1 < h.size() && x > std::log((double) h[i])) {
            const double a = std::log((double) h[i]), b = std::log((double) h[i + 1]);
            offset += (noise.holdDb[i + 1] - noise.holdDb[i]) * (x - a) / (b - a);
        }
        db += offset;
    }
    const auto g = (float) (std::clamp((double) strength, 0.0, 1.0) * std::pow(10.0, std::clamp(db, -60.0, 12.0) / 20.0));
    if (g <= 0.0f)
        return;
    if (bodyDecay < 1.0f) { // the body was fading out (all sound off): start again from silence
        body.silence();
        bodyDecay = bodyGain = 1.0f;
    }
    const std::size_t base = (n * vel.size() + near) * (std::size_t) body.size();
    for (int k = 0; k < body.size(); ++k)
        body.excite(k, noise.left[base + (std::size_t) k] * (g / bodyGain), noise.right[base + (std::size_t) k] * (g / bodyGain));
    bodyActive = true;
}

void ModalEngine::render(const ModalProfile& p, float* const* outputs, int numChannels, int start, int n) noexcept
{
    const bool sympathetic = strings.groups() > 0;
    while (n > 0) {
        const int m = std::min(n, maxBlockSize);
        std::fill(mixL.begin(), mixL.begin() + m, 0.0f);
        std::fill(mixR.begin(), mixR.begin() + m, 0.0f);
        if (sympathetic) {
            std::fill(drive.begin(), drive.begin() + m, 0.0f);
            noteDriven.fill(false);
        }
        for (auto& v : voices) {
            if (!v.active)
                continue;
            const bool alive = produce(p, v, m);
            for (int k = 0; k < m; ++k) {
                mixL[(std::size_t) k] += bufferL[(std::size_t) k];
                mixR[(std::size_t) k] += bufferR[(std::size_t) k];
            }
            if (sympathetic) { // the drive of the free strings (produce): the voice's strings
                float* own = driveOwn.data() + (std::size_t) v.note * (std::size_t) maxBlockSize;
                if (!noteDriven[(std::size_t) v.note]) {
                    std::fill(own, own + m, 0.0f);
                    noteDriven[(std::size_t) v.note] = true;
                }
                for (int k = 0; k < m; ++k) {
                    const float x = bufferD[(std::size_t) k];
                    drive[(std::size_t) k] += x;
                    own[k] += x;
                }
            }
            if (!alive)
                v.active = false;
        }
        if (sympathetic)
            renderStrings(m);
        if (bodyActive && !bodyEnabled.load(std::memory_order_relaxed)) {
            body.silence();
            bodyActive = false;
        }
        if (bodyActive) {
            std::fill(bufferL.begin(), bufferL.begin() + m, 0.0f);
            std::fill(bufferR.begin(), bufferR.begin() + m, 0.0f);
            body.process(bufferL.data(), bufferR.data(), m);
            float g = bodyGain;
            for (int k = 0; k < m; ++k) {
                g *= bodyDecay;
                mixL[(std::size_t) k] += bufferL[(std::size_t) k] * g;
                mixR[(std::size_t) k] += bufferR[(std::size_t) k] * g;
            }
            bodyGain = g;
            // Under -120 dB the body fades out over kBodyFadeS, then stops (a cut there would splatter a little).
            const float energy = body.energy() * g * g;
            if (energy < kBodySilentEnergy * 1e-4f) {
                body.silence();
                bodyActive = false;
                bodyGain = bodyDecay = 1.0f;
            } else if (energy < kBodySilentEnergy && bodyDecay >= 1.0f) {
                bodyDecay = (float) std::exp(-1.0 / (kBodyFadeS * sampleRate));
            }
        }
        if (numChannels >= 2) {
            if (outputs[0] != nullptr)
                for (int k = 0; k < m; ++k)
                    outputs[0][start + k] += mixL[(std::size_t) k];
            if (outputs[1] != nullptr)
                for (int k = 0; k < m; ++k)
                    outputs[1][start + k] += mixR[(std::size_t) k];
        } else if (numChannels == 1 && outputs[0] != nullptr) {
            for (int k = 0; k < m; ++k)
                outputs[0][start + k] += 0.5f * (mixL[(std::size_t) k] + mixR[(std::size_t) k]);
        }
        start += m;
        n -= m;
    }
}

void ModalEngine::renderStrings(int m) noexcept
{
    if (const auto version = tuningVersion.load(std::memory_order_relaxed); version != stringsTuning) {
        stringsTuning = version;
        retuneStrings();
    }
    const bool enabled = sympatheticEnabled.load(std::memory_order_relaxed) && !stringsCut;
    const bool held = heldKeysResonate.load(std::memory_order_relaxed);
    keyHeld.fill(false);
    bool driven = false;
    for (const auto& v : voices)
        if (v.active) {
            driven = true;
            if (v.keyDown && held)
                keyHeld[(std::size_t) v.note] = true;
        }
    std::fill(stringsL.begin(), stringsL.begin() + m, 0.0f);
    std::fill(stringsR.begin(), stringsR.begin() + m, 0.0f);
    const auto fast = (float) std::exp(-1.0 / (kFastFadeS * sampleRate));
    bool anyRinging = false;
    for (int note = 0; note < 128; ++note) {
        const int g = stringGroup[(std::size_t) note];
        if (g < 0)
            continue;
        // The damper's lift on this string: the pedal's, or fully lifted under a held key or by the sostenuto.
        const float lift = !enabled ? 0.0f : keyHeld[(std::size_t) note] || sostenutoHeld[(std::size_t) note] ? 1.0f : pedalLift;
        const float damper = stringDamper[(std::size_t) note];
        const float want = stringsCut ? fast
                         : lift >= 1.0f ? 1.0f
                         : lift <= 0.0f ? damper
                                        : (float) std::pow((double) damper, 1.0 - (double) lift);
        if (want != stringApplied[(std::size_t) note]) {
            strings.dampGroup(g, want);
            stringApplied[(std::size_t) note] = want;
        }
        const float* input = nullptr;
        if (lift > 0.0f && driven) {
            input = drive.data();
            if (noteDriven[(std::size_t) note]) { // everything but the string's own voices
                const float* own = driveOwn.data() + (std::size_t) note * (std::size_t) maxBlockSize;
                for (int k = 0; k < m; ++k)
                    driveScratch[(std::size_t) k] = drive[(std::size_t) k] - own[k];
                input = driveScratch.data();
            }
        }
        if (input == nullptr && !stringRinging[(std::size_t) note])
            continue;
        strings.process(g, input, stringsL.data(), stringsR.data(), m);
        stringRinging[(std::size_t) note] = true;
        if (input == nullptr && strings.energy(g) < kSilentEnergy) {
            strings.silence(g);
            stringRinging[(std::size_t) note] = false;
        }
        anyRinging = anyRinging || stringRinging[(std::size_t) note];
    }
    if (stringsCut && !anyRinging)
        stringsCut = false;
    const float gain = sympatheticGain.load(std::memory_order_relaxed);
    for (int k = 0; k < m; ++k) {
        mixL[(std::size_t) k] += gain * stringsL[(std::size_t) k];
        mixR[(std::size_t) k] += gain * stringsR[(std::size_t) k];
    }
}

void ModalEngine::renderBank(Voice& v, float* left, float* right, int n) noexcept
{
    // Split at the level ramp's start and end and at the partial fades, to the sample: during the ramp, the ramped
    // partials' poles are raised (or lowered) by their factor; at its end, the exact poles are put back; from a
    // fade on, the partial's poles get the fade's extra damping.
    int done = 0;
    while (done < n) {
        const std::int64_t at = v.sinceOn + done;
        if (v.mergeAt >= 0 && at >= v.mergeAt)
            mergeEarlier(v);
        if (v.ramp == Ramp::waiting && at >= v.rampStart) {
            for (const auto& ramp : v.ramps)
                v.bank.scalePoles(ramp.first, ramp.count, ramp.factor);
            v.ramp = Ramp::running;
        }
        if (v.ramp == Ramp::running && at >= v.rampEnd) {
            for (const auto& ramp : v.ramps)
                v.bank.setPoleFactor(ramp.first, ramp.count, 1.0f); // the exact poles back
            v.ramp = Ramp::none;
        }
        if (v.repoleAt >= 0 && at >= v.repoleAt && v.ramp != Ramp::running) { // between two layers: the moved poles
            for (int k = 0; k < v.bank.size() && k < (int) v.targetPole.size(); ++k) {
                const auto t = v.targetPole[(std::size_t) k];
                v.bank.setPole(k, std::complex<double>(t.real(), t.imag()));
            }
            v.repoleAt = -1;
        }
        // Fades are never earlier than the ramp's end (noteOn), so the exact poles are back before they apply.
        while (v.ramp == Ramp::none && v.nextFade < v.fades.size() && v.fades[v.nextFade].at <= at) {
            auto& slot = v.slots[(std::size_t) v.fades[v.nextFade++].slot];
            slot.faded = true;
            v.bank.setPoleFactor(slot.first, slot.count, fadeFactor * v.groupFactor[(std::size_t) slot.group]);
        }
        std::int64_t chunk = n - done;
        if (v.mergeAt >= 0)
            chunk = std::min(chunk, v.mergeAt - at);
        if (v.repoleAt >= 0 && v.repoleAt > at)
            chunk = std::min(chunk, v.repoleAt - at);
        if (v.ramp == Ramp::waiting)
            chunk = std::min(chunk, v.rampStart - at);
        else if (v.ramp == Ramp::running)
            chunk = std::min(chunk, v.rampEnd - at);
        else if (v.nextFade < v.fades.size())
            chunk = std::min(chunk, v.fades[v.nextFade].at - at);
        v.bank.process(left + done, right + done, (int) chunk);
        done += (int) chunk;
    }
}

void ModalEngine::mergeEarlier(Voice& v) noexcept
{
    // The hammer's contact is over: the earlier vibration's modes of partials the new strike has join its mode of
    // the same partial nearest to them (exact pole against exact pole: with the same layer, the very same mode), and
    // leave the bank; the waveform carries on exactly. The others ring on, at their own poles again.
    v.bank.setPoleFactor(v.keepFirst, v.mergeFirst - v.keepFirst, 1.0f);
    for (int k = v.mergeFirst; k < v.bank.size() && k < (int) v.modeNumber.size(); ++k) {
        std::complex<float> pole, l, r;
        v.bank.current(k, pole, l, r);
        const int number = v.modeNumber[(std::size_t) k];
        for (const auto& slot : v.slots) {
            if (slot.number != number)
                continue;
            int nearest = slot.first;
            float best = 1e30f;
            for (int j = slot.first; j < slot.first + slot.count; ++j) {
                std::complex<float> other, a, b;
                v.bank.current(j, other, a, b);
                if (const float d = std::abs(other - pole); d < best) {
                    best = d;
                    nearest = j;
                }
            }
            v.bank.excite(nearest, l, r);
            break;
        }
    }
    v.bank.truncate(v.mergeFirst);
    v.modeNumber.resize((std::size_t) std::min<int>(v.mergeFirst, (int) v.modeNumber.size()));
    v.targetPole.resize((std::size_t) std::min<int>(v.mergeFirst, (int) v.targetPole.size()));
    v.mergeAt = -1;
}

void ModalEngine::dampGroups(const ModalProfile& p, Voice& v, int m) noexcept
{
    // While the damper curve runs (after the level ramp), each group of partials follows its own curve against
    // the overall one: over this block, its poles get the factor that takes it from one point to the next.
    const int groups = std::min(p.dampers.groups(), DamperCurves::kMaxGroups);
    const bool running = groups > 0 && v.damperCurve && v.damperSpeed > 0.0f && v.ramp == Ramp::none && m > 0;
    if (!running) {
        if (v.groupDamping) { // back to the poles of the partials alone
            v.groupFactor.fill(1.0f);
            for (const auto& slot : v.slots)
                v.bank.setPoleFactor(slot.first, slot.count, slot.faded ? fadeFactor : 1.0f);
            v.groupDamping = false;
        }
        return;
    }
    const double step = v.damperSpeed * std::clamp((double) damperRate.load(std::memory_order_relaxed), 0.2, 5.0)
                      / (p.dampers.stepSeconds * sampleRate);
    const int last = v.curvePoints - 1;
    auto gainAt = [&](int g, double pos) {
        const float* curve = v.groupGains.data() + (std::size_t) g * (std::size_t) p.dampers.length;
        const int i = (int) pos;
        return i < last ? curve[i] + (float) (pos - i) * (curve[i + 1] - curve[i]) : curve[last];
    };
    for (int g = 0; g < groups; ++g) {
        const double ratio = (double) gainAt(g, v.curvePos + m * step) / std::max((double) gainAt(g, v.curvePos), 1e-9);
        v.groupFactor[(std::size_t) g] = (float) std::pow(ratio, 1.0 / m);
    }
    for (const auto& slot : v.slots)
        v.bank.setPoleFactor(slot.first, slot.count, v.groupFactor[(std::size_t) slot.group] * (slot.faded ? fadeFactor : 1.0f));
    v.groupDamping = true;
}

bool ModalEngine::produce(const ModalProfile& p, Voice& v, int m) noexcept
{
    float* l = bufferL.data();
    float* r = bufferR.data();
    std::fill(l, l + m, 0.0f);
    std::fill(r, r + m, 0.0f);
    dampGroups(p, v, m);
    renderBank(v, l, r, m);

    const auto& layer = *v.layer;
    bool residualLeft = false;
    if (v.residualGain > 0.0f && v.residualPos < (double) layer.residualFrames) {
        const double step = v.residualStep;
        const float g = v.residualGain;
        const float* x = layer.residual.data();
        if (std::abs(step - 1.0) < 1e-9) {
            auto i = (int) v.residualPos;
            for (int k = 0; k < m && i < layer.residualFrames; ++k, ++i) {
                l[k] += g * x[2 * i];
                r[k] += g * x[2 * i + 1];
            }
        } else {
            for (int k = 0; k < m; ++k) {
                float a, b;
                readCubic(x, layer.residualFrames, v.residualPos + k * step, a, b);
                l[k] += g * a;
                r[k] += g * b;
            }
        }
        v.residualPos += m * step;
        residualLeft = v.residualPos < (double) layer.residualFrames + 2;
    }
    for (auto& t : v.tails) { // the earlier strikes' residuals, to their end
        if (t.gain <= 0.0f)
            continue;
        const float* x = t.layer->residual.data();
        for (int k = 0; k < m; ++k) {
            float a, b;
            readCubic(x, t.layer->residualFrames, t.pos + k * t.step, a, b);
            l[k] += t.gain * a;
            r[k] += t.gain * b;
        }
        t.pos += m * t.step;
        if (t.pos >= (double) t.layer->residualFrames + 2)
            t.gain = 0.0f;
        else
            residualLeft = true;
    }

    if (v.voicing.on) { // the voicing: the voice's whole sound through its shelf
        v.voicing.f.process(l, m, 0);
        v.voicing.f.process(r, m, 1);
    }
    if (v.attack.on) // the attack's softness, fading out
        softenAttack(v, l, r, m);

    // The drive of the free strings: the voice's sound (strings and attack residual, as calibrated), rising from
    // the note-on and fading (forge sympathetic: the reference's resonance builds up later and fades on its own;
    // the rise also keeps the hammer's knock from setting every string ringing at once), times the velocity factor.
    float* d = bufferD.data();
    if (strings.groups() > 0) {
        float rise = v.driveOnset, fade = v.driveFade;
        const float onset = onsetFactor[(std::size_t) v.note], decay = driveFadeFactor[(std::size_t) v.note];
        for (int k = 0; k < m; ++k) {
            rise = 1.0f - (1.0f - rise) * onset;
            fade *= decay;
            d[k] = 0.5f * (l[k] + r[k]) * v.driveGain * rise * fade;
        }
        v.driveOnset = rise;
        v.driveFade = fade;
    } else {
        std::fill(d, d + m, 0.0f);
    }

    if (v.decay < 1.0f) {
        float g = v.gain;
        const float decay = v.decay;
        for (int k = 0; k < m; ++k) {
            g *= decay;
            l[k] *= g;
            r[k] *= g;
            d[k] *= g; // (only read when the strings are driven)
        }
        v.gain = g;
    }
    if (v.damperCurve) { // linear interpolation between the curve points, run at the damper's speed
        const double step = v.damperSpeed * std::clamp((double) damperRate.load(std::memory_order_relaxed), 0.2, 5.0)
                          / (p.dampers.stepSeconds * sampleRate);
        const int last = v.curvePoints - 1;
        float g = v.curveGain;
        for (int k = 0; k < m; ++k) {
            const double pos = v.curvePos + k * step;
            const int i = (int) pos;
            g = i < last ? v.releaseGains[(std::size_t) i]
                               + (float) (pos - i) * (v.releaseGains[(std::size_t) i + 1] - v.releaseGains[(std::size_t) i])
                         : v.releaseGains[(std::size_t) last];
            l[k] *= g;
            r[k] *= g;
            d[k] *= g;
        }
        v.curvePos += m * step;
        v.curveGain = g;
        // Past the curve: a damped string is faded out (it is already 30 dB down or more); an undamped one rings on.
        if (!v.fadingOut && v.curvePos >= last
            && 20.0f * std::log10(std::max(v.releaseGains[(std::size_t) last], 1e-10f)) < kDampedDb) {
            v.fadingOut = true;
            v.decay = std::min(v.decay, (float) std::exp(-1.0 / (0.05 * sampleRate)));
        }
    }
    v.sinceOn += m;
    const float g = v.gain * v.curveGain;
    return residualLeft || v.bank.energy() * g * g > kSilentEnergy;
}

} // namespace pf
