#include "ReferencePlayer.h"

#include <algorithm>
#include <cmath>

namespace pf::app {

namespace {
constexpr double kFastFadeS = 0.015;    // re-strike, cut: long enough to avoid a click
constexpr float kSilentGain = 1.0e-4f;  // -80 dB: the voice is freed
constexpr double kMaxRatio = 4.0;       // supported resampling: output rate >= corpus rate / 4
} // namespace

void ReferencePlayer::prepare(double newSampleRate, int newMaxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlockSize = juce::jmax(32, newMaxBlockSize);
    bufferL.assign((size_t) maxBlockSize, 0.0f);
    bufferR.assign((size_t) maxBlockSize, 0.0f);
    const auto raw = (size_t) std::ceil(maxBlockSize * kMaxRatio) + 16;
    rawL.assign(raw, 0.0f);
    rawR.assign(raw, 0.0f);
    for (auto& v : voices)
        v.active = false;
    sustain = false;
    audioRunning = true;
}

void ReferencePlayer::release() noexcept
{
    audioRunning = false;
}

void ReferencePlayer::installBank(std::unique_ptr<SampleBank> newBank)
{
    publishedBank.store(nullptr, std::memory_order_release);
    if (owned != nullptr) {
        // The audio thread sees the null pointer on its next block, stops its voices and says so.
        const auto deadline = juce::Time::getMillisecondCounter() + 1000;
        while (audioRunning.load() && bankSeenByAudio.load(std::memory_order_acquire) != nullptr
               && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep(2);
    }
    owned = std::move(newBank);
    publishedBank.store(owned.get(), std::memory_order_release);
}

void ReferencePlayer::allSoundOff() noexcept
{
    const auto fast = (float) std::exp(-1.0 / (kFastFadeS * sampleRate));
    for (auto& v : voices)
        if (v.active) {
            v.keyDown = false;
            v.released = true;
            v.decay = juce::jmin(v.decay, fast);
        }
}

void ReferencePlayer::process(float* const* outputs, int numChannels, int n,
                              const pf::Event* events, int numEvents) noexcept
{
    for (int c = 0; c < numChannels; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear(outputs[c], n);

    const auto* b = publishedBank.load(std::memory_order_acquire);
    if (b != currentBank) {
        for (auto& v : voices)
            v.active = false;
        currentBank = b;
        bankSeenByAudio.store(b, std::memory_order_release);
    }
    if (b == nullptr || maxBlockSize == 0) {
        activeVoices.store(0, std::memory_order_relaxed);
        return;
    }

    // Render segment by segment between events: every event lands on its exact sample.
    int start = 0;
    for (int i = 0; i <= numEvents; ++i) {
        const int end = i < numEvents ? std::clamp((int) events[i].offset, start, n) : n;
        if (end > start)
            render(*b, outputs, numChannels, start, end - start);
        if (i < numEvents)
            apply(*b, events[i]);
        start = end;
    }

    const float gain = outputGain.load(std::memory_order_relaxed);
    float peak = 0.0f;
    for (int c = 0; c < juce::jmin(numChannels, 2); ++c)
        if (outputs[c] != nullptr) {
            juce::FloatVectorOperations::multiply(outputs[c], gain, n);
            peak = juce::jmax(peak, juce::FloatVectorOperations::findMaximum(outputs[c], n),
                              -juce::FloatVectorOperations::findMinimum(outputs[c], n));
        }
    float previous = outputPeak.load(std::memory_order_relaxed);
    while (peak > previous && !outputPeak.compare_exchange_weak(previous, peak)) {}

    int active = 0;
    for (const auto& v : voices)
        active += v.active ? 1 : 0;
    activeVoices.store(active, std::memory_order_relaxed);
}

void ReferencePlayer::apply(const SampleBank& b, const pf::Event& e) noexcept
{
    const int type = e.status & 0xF0;
    if (type == 0x90 && e.data2 > 0)
        noteOn(b, e.data1, e.data2);
    else if (type == 0x80 || type == 0x90)
        noteOff(b, e.data1);
    else if (type == 0xB0) {
        if (e.data1 == 64) {
            sustain = e.data2 >= 64;
            sustainDown.store(sustain, std::memory_order_relaxed);
            if (!sustain && !playWholeTake.load(std::memory_order_relaxed))
                for (auto& v : voices)
                    if (v.active && !v.keyDown)
                        releaseVoice(b, v);
        } else if (e.data1 == 120 || e.data1 == 123)
            allSoundOff();
    }
}

void ReferencePlayer::noteOn(const SampleBank& b, int note, int velocity) noexcept
{
    const int index = b.pick(note, velocity);
    noteOnsReceived.fetch_add(1, std::memory_order_relaxed);
    lastNote.store(note, std::memory_order_relaxed);
    lastVelocity.store(velocity, std::memory_order_relaxed);
    lastRenderedVelocity.store(index >= 0 ? b.samples[(size_t) index].velocity : 0, std::memory_order_relaxed);
    if (index < 0)
        return;

    // Re-strike: the string is still vibrating, so the previous voices of the same note keep sounding and fade
    // to -40 dB within restrikeFadeSeconds (an existing faster decay, such as the damper's, is kept). Only when
    // too many voices of the note pile up is the oldest one cut quickly.
    const double fadeS = juce::jmax((double) restrikeFadeSeconds.load(std::memory_order_relaxed), kFastFadeS);
    const auto restrike = (float) std::exp(-std::log(100.0) / (fadeS * sampleRate));
    const auto fast = (float) std::exp(-1.0 / (kFastFadeS * sampleRate));
    Voice* oldest = nullptr;
    int sameNote = 0;
    for (auto& v : voices)
        if (v.active && v.note == note) {
            ++sameNote;
            v.keyDown = false;
            v.released = true;
            v.decay = juce::jmin(v.decay, restrike);
            if (oldest == nullptr || v.age < oldest->age)
                oldest = &v;
        }
    if (sameNote >= kMaxVoicesPerNote && oldest != nullptr)
        oldest->decay = juce::jmin(oldest->decay, fast);

    Voice* target = nullptr;
    for (auto& v : voices)
        if (!v.active) {
            target = &v;
            break;
        }
    if (target == nullptr) { // steal: the quietest released voice, otherwise the oldest one
        stolenVoices.fetch_add(1, std::memory_order_relaxed);
        for (auto& v : voices)
            if (v.released && (target == nullptr || v.gain < target->gain))
                target = &v;
        if (target == nullptr)
            for (auto& v : voices)
                if (target == nullptr || v.age < target->age)
                    target = &v;
    }

    auto& v = *target;
    v.active = true;
    v.keyDown = true;
    v.released = false;
    v.sample = index;
    v.note = note;
    v.position = b.samples[(size_t) index].noteOnFrame; // skip the take's pre-roll: no added latency
    v.sinceOn = 0;
    v.gain = 1.0f;
    v.decay = 1.0f;
    v.damperCurve = false;
    v.curveGain = 1.0f;
    v.age = ++ageCounter;
    v.left.reset();
    v.right.reset();
}

void ReferencePlayer::noteOff(const SampleBank& b, int note) noexcept
{
    const bool whole = playWholeTake.load(std::memory_order_relaxed);
    for (auto& v : voices)
        if (v.active && v.keyDown && v.note == note) {
            v.keyDown = false;
            if (!whole && !sustain)
                releaseVoice(b, v);
        }
}

void ReferencePlayer::releaseVoice(const SampleBank& b, Voice& v) noexcept
{
    if (v.released)
        return;
    if (!b.releaseCurves.empty()) { // measured damper: depends on the note and on how long it was held
        b.releaseCurves.evaluate(v.note, (double) v.sinceOn / sampleRate, v.releaseGains.data());
        v.curvePoints = b.releaseCurves.length;
        v.sinceRelease = 0;
        v.damperCurve = true;
        v.released = true;
        return;
    }
    const float t40 = b.samples[(size_t) v.sample].releaseDecay40;
    if (t40 <= 0.0f)
        return; // no damper measured (top treble): the string keeps ringing
    const double tau = t40 / std::log(100.0); // -40 dB = amplitude divided by 100
    v.decay = (float) std::exp(-1.0 / (tau * sampleRate));
    v.released = true;
}

void ReferencePlayer::render(const SampleBank& b, float* const* outputs, int numChannels, int start, int n) noexcept
{
    while (n > 0) {
        const int m = juce::jmin(n, maxBlockSize);
        for (auto& v : voices) {
            if (!v.active)
                continue;
            const bool alive = produce(b, v, m);
            if (numChannels >= 2) {
                if (outputs[0] != nullptr) juce::FloatVectorOperations::add(outputs[0] + start, bufferL.data(), m);
                if (outputs[1] != nullptr) juce::FloatVectorOperations::add(outputs[1] + start, bufferR.data(), m);
            } else if (numChannels == 1 && outputs[0] != nullptr) {
                juce::FloatVectorOperations::addWithMultiply(outputs[0] + start, bufferL.data(), 0.5f, m);
                juce::FloatVectorOperations::addWithMultiply(outputs[0] + start, bufferR.data(), 0.5f, m);
            }
            if (!alive)
                v.active = false;
        }
        start += m;
        n -= m;
    }
}

bool ReferencePlayer::produce(const SampleBank& b, Voice& v, int m) noexcept
{
    const auto& s = b.samples[(size_t) v.sample];
    const float scale = s.peak / 32767.0f;
    const std::int16_t* pcm = s.pcm.data();
    auto read = [&](std::int64_t from, int count, float* l, float* r) {
        for (int k = 0; k < count; ++k) {
            const std::int64_t i = from + k;
            if (i < s.length) {
                l[k] = (float) pcm[2 * i] * scale;
                r[k] = (float) pcm[2 * i + 1] * scale;
            } else {
                l[k] = r[k] = 0.0f;
            }
        }
    };

    const double ratio = b.sampleRate / sampleRate;
    if (std::abs(ratio - 1.0) < 1.0e-9) {
        read(v.position, m, bufferL.data(), bufferR.data());
        v.position += m;
    } else if (ratio <= kMaxRatio) {
        const int needed = juce::jmin((int) std::ceil(m * ratio) + 8, (int) rawL.size());
        read(v.position, needed, rawL.data(), rawR.data());
        const int consumed = v.left.process(ratio, rawL.data(), bufferL.data(), m);
        v.right.process(ratio, rawR.data(), bufferR.data(), m);
        v.position += consumed;
    } else {
        juce::FloatVectorOperations::clear(bufferL.data(), m);
        juce::FloatVectorOperations::clear(bufferR.data(), m);
        return false;
    }

    if (v.released && v.decay < 1.0f) {
        float g = v.gain;
        const float d = v.decay;
        for (int k = 0; k < m; ++k) {
            g *= d;
            bufferL[(size_t) k] *= g;
            bufferR[(size_t) k] *= g;
        }
        v.gain = g;
    }
    if (v.damperCurve) { // linear interpolation between the curve points
        const double pointsPerFrame = 1.0 / (b.releaseCurves.stepSeconds * sampleRate);
        const int last = v.curvePoints - 1;
        float g = v.curveGain;
        for (int k = 0; k < m; ++k) {
            const double p = (double) (v.sinceRelease + k) * pointsPerFrame;
            const int i = (int) p;
            g = i < last ? v.releaseGains[(size_t) i] + (float) (p - i) * (v.releaseGains[(size_t) i + 1] - v.releaseGains[(size_t) i])
                         : v.releaseGains[(size_t) last];
            bufferL[(size_t) k] *= g;
            bufferR[(size_t) k] *= g;
        }
        v.sinceRelease += m;
        v.curveGain = g;
    }
    v.sinceOn += m;
    return v.position < s.length && v.gain * v.curveGain > kSilentGain;
}

} // namespace pf::app
