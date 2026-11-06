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

constexpr double kFastFadeS = 0.015;     // cut, voice stealing: long enough to avoid a click
constexpr float kSilentEnergy = 1.0e-10f; // -100 dB (amplitude 1e-5) for the sum of the modes: the voice is freed
constexpr double kBodyFadeS = 0.02;       // the soundboard fading out under -100 dB
constexpr float kDampedDb = -30.0f;      // a damper curve ending lower than this means the string is damped
constexpr float kMinGain = 0.25f, kMaxGain = 4.0f; // per-partial level interpolation: at most +-12 dB

/// Flush-to-zero and denormals-are-zero for the scope (decaying modes end up denormal otherwise).
struct NoDenormals {
#if PF_HAVE_MXCSR
    unsigned int saved = _mm_getcsr();
    NoDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
    ~NoDenormals() { _mm_setcsr(saved); }
#endif
};

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
    const int capacity = owned != nullptr ? owned->maxModesPerLayer : 0;
    const int partials = owned != nullptr ? owned->maxPartialsPerLayer : 0;
    for (auto& v : voices) {
        v.active = false;
        if (v.bank.capacity() < capacity)
            v.bank.reserve(capacity);
        v.bank.clear();
        v.ramps.clear();
        v.ramps.reserve((std::size_t) partials);
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
        if (owned != nullptr)
            for (const auto& shape : owned->stringDriveShapes)
                if (note >= shape.firstNote) {
                    onset = shape.onsetSeconds;
                    decay = shape.decaySeconds;
                }
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
    s.restrikeFadeSeconds = restrikeFadeSeconds.load();
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
    return s;
}

void ModalEngine::applySettings(const Settings& s) noexcept
{
    restrikeFadeSeconds = s.restrikeFadeSeconds;
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
    tuningVersion.fetch_add(1);
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
    if (owned != nullptr) {
        // The audio thread sees the null pointer on its next block and says so; then the voices are ours.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (audioRunning.load() && seenByAudio.load(std::memory_order_acquire) != nullptr
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
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
    NoDenormals noDenormals;
    for (int c = 0; c < numChannels; ++c)
        if (outputs[c] != nullptr)
            std::fill(outputs[c], outputs[c] + n, 0.0f);

    const auto* p = published.load(std::memory_order_acquire);
    if (p != current) {
        for (auto& v : voices)
            v.active = false;
        body.silence();
        bodyActive = false;
        for (int g = 0; g < strings.groups(); ++g)
            strings.silence(g);
        stringRinging.fill(false);
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

    const float gain = outputGain.load(std::memory_order_relaxed);
    float peak = 0.0f;
    for (int c = 0; c < std::min(numChannels, 2); ++c)
        if (outputs[c] != nullptr)
            for (int k = 0; k < n; ++k) {
                outputs[c][k] *= gain;
                peak = std::max(peak, std::abs(outputs[c][k]));
            }
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

    // Re-strike: the previous voices of the note keep ringing and fade out (see the reference player).
    const double fadeS = std::max((double) restrikeFadeSeconds.load(std::memory_order_relaxed), kFastFadeS);
    const auto restrike = (float) std::exp(-std::log(100.0) / (fadeS * sampleRate));
    const auto fast = (float) std::exp(-1.0 / (kFastFadeS * sampleRate));
    Voice* oldest = nullptr;
    int sameNote = 0;
    for (auto& v : voices)
        if (v.active && v.note == note) {
            ++sameNote;
            v.keyDown = false;
            v.released = true;
            v.restruck = true;
            v.decay = std::min(v.decay, restrike);
            if (oldest == nullptr || v.age < oldest->age)
                oldest = &v;
        }
    if (sameNote >= kMaxVoicesPerNote && oldest != nullptr)
        oldest->decay = std::min(oldest->decay, fast);

    Voice* target = nullptr;
    for (auto& v : voices)
        if (!v.active) {
            target = &v;
            break;
        }
    if (target == nullptr) { // steal: the quietest released voice, otherwise the oldest one
        stolenVoices.fetch_add(1, std::memory_order_relaxed);
        for (auto& v : voices)
            if (v.released && (target == nullptr || v.gain * v.curveGain < target->gain * target->curveGain))
                target = &v;
        if (target == nullptr)
            for (auto& v : voices)
                if (target == nullptr || v.age < target->age)
                    target = &v;
    }

    auto& v = *target;
    v.bank.clear();
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
    std::size_t j = 0; // walks the other layer's partials (both ascending)
    for (const auto& part : near.partials) {
        while (j < other.partials.size() && other.partials[j].number < part.number)
            ++j;
        float g = overall;
        if (&near != &other && j < other.partials.size() && other.partials[j].number == part.number
            && part.level > 0.0f && other.partials[j].level > 0.0f) {
            const float wanted = nearA ? interpolateLevel(part.level, other.partials[j].level, alpha)
                                       : interpolateLevel(other.partials[j].level, part.level, alpha);
            g = std::clamp(wanted / part.level, kMinGain * overall, kMaxGain * overall);
        }
        const int first = v.bank.size();
        bool complete = true;
        for (int k = 0; k < part.modeCount; ++k)
            complete = complete && near.modes[(std::size_t) (part.firstMode + k)].freq * tuning < nyquist;
        // A partial with a mode past Nyquist (inaudible) is not ramped: its bank modes would not map back.
        const bool ramp = ramped && complete && std::abs(g - overall) > 1e-4f * overall
                          && v.ramps.size() < v.ramps.capacity();
        const float start = ramp ? overall : g;
        for (int k = 0; k < part.modeCount; ++k) {
            const auto& m = near.modes[(std::size_t) (part.firstMode + k)];
            if (m.freq * tuning < nyquist)
                v.bank.add(modePole(m.freq * tuning, m.decay, sampleRate), m.left * start, m.right * start);
        }
        if (ramp)
            v.ramps.push_back({ first, v.bank.size() - first, part.firstMode,
                                (float) std::pow((double) g / overall, 1.0 / (double) rampLength) });
        if (v.bank.size() == first || v.slots.size() == v.slots.capacity())
            continue;
        v.slots.push_back({ first, v.bank.size() - first, p.dampers.groupOf(part.number), false });
        // Past its fade time, the partial dies out (after the level ramp, whose end puts the exact poles back).
        if (part.fadeSeconds > 0.0f && v.fades.size() < v.fades.capacity())
            v.fades.push_back({ std::max((std::int64_t) std::llround(part.fadeSeconds * sampleRate), residualEnd + rampLength),
                                (int) v.slots.size() - 1 });
    }
    std::sort(v.fades.begin(), v.fades.end(), [](const PartialFade& a, const PartialFade& b) { return a.at < b.at; });
    v.nextFade = 0;
    v.ramp = v.ramps.empty() ? Ramp::none : Ramp::waiting;
    v.rampStart = residualEnd;
    v.rampEnd = residualEnd + rampLength;

    if (bodyEnabled.load(std::memory_order_relaxed) && body.size() == (int) near.bodyLeft.size()) {
        // The overall gain, like the residual: the fast resonances extrapolated back to the note-on are cancelled
        // by the residual only at the same gain. (Against the strings, the body hardly changes between layers.)
        const float gb = overall;
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
    v.restruck = false;
    v.note = note;
    v.velocity = velocity;
    v.layer = &near;
    v.residualPos = 0.0;
    v.residualStep = p.residualRate / sampleRate * tuning;
    v.residualGain = residualEnabled.load(std::memory_order_relaxed) ? overall : 0.0f;
    v.gain = v.decay = 1.0f;
    v.curveGain = 1.0f;
    v.curvePos = 0.0;
    v.damperSpeed = 0.0f;
    v.driveGain = (float) std::pow((double) velocity / (double) p.stringReferenceVelocity, (double) p.stringVelocityExponent);
    v.driveOnset = 0.0f;
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
        landing += v.active && !damperRaised(v) && !v.restruck && !v.damperCurve && pedalLift < 1.0f ? 1 : 0;
    knockScale = landing > 1 ? 1.0f / std::sqrt((float) landing) : 1.0f;
    for (auto& v : voices)
        if (v.active && !v.keyDown)
            updateDamping(p, v);
    knockScale = 1.0f;
}

void ModalEngine::updateDamping(const ModalProfile& p, Voice& v) noexcept
{
    if (!v.active || v.restruck || p.dampers.empty())
        return; // re-struck: fading out anyway; no damper data: the string keeps ringing
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
            // Under -100 dB the body fades out over kBodyFadeS, then stops (a cut there would splatter a little).
            const float energy = body.energy() * g * g;
            if (energy < kSilentEnergy * 1e-4f) {
                body.silence();
                bodyActive = false;
                bodyGain = bodyDecay = 1.0f;
            } else if (energy < kSilentEnergy && bodyDecay >= 1.0f) {
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
        // Fades are never earlier than the ramp's end (noteOn), so the exact poles are back before they apply.
        while (v.ramp == Ramp::none && v.nextFade < v.fades.size() && v.fades[v.nextFade].at <= at) {
            auto& slot = v.slots[(std::size_t) v.fades[v.nextFade++].slot];
            slot.faded = true;
            v.bank.setPoleFactor(slot.first, slot.count, fadeFactor * v.groupFactor[(std::size_t) slot.group]);
        }
        std::int64_t chunk = n - done;
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
    const double step = v.damperSpeed / (p.dampers.stepSeconds * sampleRate);
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
        const double step = v.damperSpeed / (p.dampers.stepSeconds * sampleRate);
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
