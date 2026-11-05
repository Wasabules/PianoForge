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
    for (auto* b : { &bufferL, &bufferR, &mixL, &mixR })
        b->assign((std::size_t) maxBlockSize, 0.0f);
    allocate();
    sustain = false;
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
    }
    body.reserve(owned != nullptr ? (int) owned->bodyFreq.size() : 0);
    if (owned != nullptr)
        for (std::size_t k = 0; k < owned->bodyFreq.size(); ++k)
            body.add(modePole(owned->bodyFreq[k], owned->bodyDecay[k], sampleRate), {}, {});
    bodyActive = false;
    bodyGain = bodyDecay = 1.0f;
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
    activeVoices.store(active, std::memory_order_relaxed);
    activeModes.store(modes, std::memory_order_relaxed);
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
            sustain = e.data2 >= 64;
            sustainDown.store(sustain, std::memory_order_relaxed);
            if (!sustain)
                for (auto& v : voices)
                    if (v.active && !v.keyDown)
                        releaseVoice(p, v);
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
    // The level ramp starts once the residual is over (output samples); without a residual there is nothing to
    // stay consistent with, and each partial starts at its own level.
    const auto residualEnd = (std::int64_t) std::ceil((double) near.residualFrames * sampleRate / p.residualRate);
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
            complete = complete && near.modes[(std::size_t) (part.firstMode + k)].freq < nyquist;
        // A partial with a mode past Nyquist (inaudible) is not ramped: its bank modes would not map back.
        const bool ramp = ramped && complete && std::abs(g - overall) > 1e-4f * overall
                          && v.ramps.size() < v.ramps.capacity();
        const float start = ramp ? overall : g;
        for (int k = 0; k < part.modeCount; ++k) {
            const auto& m = near.modes[(std::size_t) (part.firstMode + k)];
            if (m.freq < nyquist)
                v.bank.add(modePole(m.freq, m.decay, sampleRate), m.left * start, m.right * start);
        }
        if (ramp)
            v.ramps.push_back({ first, v.bank.size() - first, part.firstMode,
                                (float) std::pow((double) g / overall, 1.0 / (double) rampLength) });
    }
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
    v.note = note;
    v.layer = &near;
    v.residualPos = 0.0;
    v.residualGain = residualEnabled.load(std::memory_order_relaxed) ? overall : 0.0f;
    v.gain = v.decay = 1.0f;
    v.curveGain = 1.0f;
    v.sinceOn = v.sinceRelease = 0;
    v.age = ++ageCounter;
}

void ModalEngine::noteOff(const ModalProfile& p, int note) noexcept
{
    for (auto& v : voices)
        if (v.active && v.keyDown && v.note == note) {
            v.keyDown = false;
            if (!sustain)
                releaseVoice(p, v);
        }
}

void ModalEngine::releaseVoice(const ModalProfile& p, Voice& v) noexcept
{
    if (v.released || p.dampers.empty())
        return; // no damper data: the string keeps ringing
    p.dampers.evaluate(v.note, (double) v.sinceOn / sampleRate, v.releaseGains.data());
    v.curvePoints = p.dampers.length;
    v.sinceRelease = 0;
    v.damperCurve = true;
    v.released = true;
}

void ModalEngine::render(const ModalProfile& p, float* const* outputs, int numChannels, int start, int n) noexcept
{
    while (n > 0) {
        const int m = std::min(n, maxBlockSize);
        std::fill(mixL.begin(), mixL.begin() + m, 0.0f);
        std::fill(mixR.begin(), mixR.begin() + m, 0.0f);
        for (auto& v : voices) {
            if (!v.active)
                continue;
            const bool alive = produce(p, v, m);
            for (int k = 0; k < m; ++k) {
                mixL[(std::size_t) k] += bufferL[(std::size_t) k];
                mixR[(std::size_t) k] += bufferR[(std::size_t) k];
            }
            if (!alive)
                v.active = false;
        }
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
            if (body.energy() * g * g < kSilentEnergy) {
                body.silence();
                bodyActive = false;
                bodyGain = bodyDecay = 1.0f;
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

void ModalEngine::renderBank(Voice& v, float* left, float* right, int n) noexcept
{
    // Split at the level ramp's start and end, to the sample: in between, the ramped partials' poles are raised
    // (or lowered) by their factor; at the end, the exact poles are put back.
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
                for (int k = 0; k < ramp.count; ++k) {
                    const auto& m = v.layer->modes[(std::size_t) (ramp.profileFirst + k)];
                    v.bank.setPole(ramp.first + k, modePole(m.freq, m.decay, sampleRate));
                }
            v.ramp = Ramp::none;
        }
        std::int64_t chunk = n - done;
        if (v.ramp == Ramp::waiting)
            chunk = std::min(chunk, v.rampStart - at);
        else if (v.ramp == Ramp::running)
            chunk = std::min(chunk, v.rampEnd - at);
        v.bank.process(left + done, right + done, (int) chunk);
        done += (int) chunk;
    }
}

bool ModalEngine::produce(const ModalProfile& p, Voice& v, int m) noexcept
{
    float* l = bufferL.data();
    float* r = bufferR.data();
    std::fill(l, l + m, 0.0f);
    std::fill(r, r + m, 0.0f);
    renderBank(v, l, r, m);

    const auto& layer = *v.layer;
    bool residualLeft = false;
    if (v.residualGain > 0.0f && v.residualPos < (double) layer.residualFrames) {
        const double step = p.residualRate / sampleRate;
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

    if (v.decay < 1.0f) {
        float g = v.gain;
        const float d = v.decay;
        for (int k = 0; k < m; ++k) {
            g *= d;
            l[k] *= g;
            r[k] *= g;
        }
        v.gain = g;
    }
    if (v.damperCurve) { // linear interpolation between the curve points
        const double pointsPerFrame = 1.0 / (p.dampers.stepSeconds * sampleRate);
        const int last = v.curvePoints - 1;
        float g = v.curveGain;
        for (int k = 0; k < m; ++k) {
            const double pos = (double) (v.sinceRelease + k) * pointsPerFrame;
            const int i = (int) pos;
            g = i < last ? v.releaseGains[(std::size_t) i]
                               + (float) (pos - i) * (v.releaseGains[(std::size_t) i + 1] - v.releaseGains[(std::size_t) i])
                         : v.releaseGains[(std::size_t) last];
            l[k] *= g;
            r[k] *= g;
        }
        v.sinceRelease += m;
        v.curveGain = g;
        // Past the curve: a damped string is faded out (it is already 30 dB down or more); an undamped one rings on.
        if (!v.fadingOut && (double) v.sinceRelease * pointsPerFrame >= last
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
