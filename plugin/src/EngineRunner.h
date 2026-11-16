#pragma once

#include <pianoforge/engine.hpp>
#include <pianoforge/limiter.hpp>
#include <pianoforge/modal_engine.hpp>
#include <pianoforge/output_eq.hpp>
#include <pianoforge/room.hpp>
#include <pianoforge/upsampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace pf::plugin {

/// The application's sound chain, for a host: the modal engine, the room (headphones included), the output's tone
/// and the limiter.
/// At the host's rate, or, over 50 kHz as in the application, at that rate divided by 2, 4 or 8 and brought up to it
/// (pf::Upsampler: a latency of (64 k - 1) / 2 samples, which the plugin reports to the host).
/// Any block size, down to one sample (hosts split blocks at automation points): the samples rendered and not yet
/// given wait for the next block, and the events falling on samples given from them wait for the next frame.
/// prepare() allocates; process() neither allocates nor locks.
class EngineRunner {
public:
    static constexpr int kMaxEvents = 512; // per call of process()

    pf::ModalEngine engine;
    pf::Room room;
    pf::Limiter limiter;
    /// The output's tone (dB), set on the audio thread before process().
    float toneBassDb = 0.0f, tonePresenceDb = 0.0f, toneAirDb = 0.0f;

    void prepare(double hostRate, int maxBlock)
    {
        int k = 1;
        while (k < 8 && hostRate / k > 50000.0 && std::fmod(hostRate, 2.0 * k) == 0.0)
            k *= 2;
        factor = k;
        rate = hostRate / k;
        block = std::max(1, maxBlock);
        const int engineBlock = block / k + 2;
        engine.prepare(rate, engineBlock);
        room.prepare(rate);
        limiter.prepare(rate);
        for (auto* b : { &engineL, &engineR })
            b->assign((std::size_t) engineBlock, 0.0f);
        for (auto* b : { &upL, &upR })
            b->assign((std::size_t) (engineBlock * k), 0.0f);
        upsampleL.prepare(k);
        upsampleR.prepare(k);
        carry = 0;
        waiting = 0;
    }

    /// Not on the audio thread, the audio thread kept out of process() (the plugin suspends it): a new profile, the
    /// voices allocated for it, the engine prepared again if it was.
    void install(std::shared_ptr<const pf::ModalProfile> profile)
    {
        engine.release();
        engine.install(std::move(profile));
        if (block > 0)
            engine.prepare(rate, block / factor + 2);
    }

    /// Audio thread: everything back to silence, as at a start (the engine, the room, the limiter, the upsampling).
    void reset() noexcept
    {
        engine.reset();
        room.reset();
        eq.reset();
        limiter.reset();
        upsampleL.reset();
        upsampleR.reset();
        carry = 0;
        waiting = 0;
    }

    int rateFactor() const noexcept { return factor; }
    double engineRate() const noexcept { return rate; }
    int maxBlock() const noexcept { return block; }
    /// What the upsampler delays the sound by, in host samples (0 at the host's rate).
    int latencySamples() const noexcept { return factor == 1 ? 0 : (pf::Upsampler::kTaps * factor - 1) / 2; }

    /// n <= maxBlock(); events sorted by offset, offsets < n, at most kMaxEvents.
    void process(float* left, float* right, int n, const pf::Event* events, int numEvents) noexcept
    {
        if (factor == 1) {
            render(left, right, n, events, numEvents);
            return;
        }
        const int k = factor;
        // First what was rendered last time and not given yet.
        const int given = std::min(carry, n);
        std::copy(carried[0].begin(), carried[0].begin() + given, left);
        std::copy(carried[1].begin(), carried[1].begin() + given, right);
        for (auto& c : carried)
            std::copy(c.begin() + given, c.begin() + carry, c.begin());
        carry -= given;
        const int rest = n - given;
        if (rest == 0) { // the whole block came from there: its events wait for the next frame
            for (int i = 0; i < numEvents && waiting < kMaxEvents; ++i)
                waitingEvents[(std::size_t) waiting++] = { 0, events[i].status, events[i].data1, events[i].data2 };
            return;
        }
        // Then new frames, enough for the rest of the block; the events at their frame (those that waited first).
        const int frames = (rest + k - 1) / k;
        int count = 0;
        for (int i = 0; i < waiting; ++i)
            mapped[(std::size_t) count++] = waitingEvents[(std::size_t) i];
        waiting = 0;
        for (int i = 0; i < numEvents && count < kMaxEvents; ++i) {
            const int offset = (int) events[i].offset;
            const int at = offset < given ? 0 : std::min(frames - 1, (offset - given) / k);
            mapped[(std::size_t) count++] = { (std::uint32_t) at, events[i].status, events[i].data1, events[i].data2 };
        }
        render(engineL.data(), engineR.data(), frames, mapped.data(), count);
        upsampleL.process(engineL.data(), frames, upL.data());
        upsampleR.process(engineR.data(), frames, upR.data());
        std::copy(upL.begin(), upL.begin() + rest, left + given);
        std::copy(upR.begin(), upR.begin() + rest, right + given);
        carry = frames * k - rest; // fewer than k
        std::copy(upL.begin() + rest, upL.begin() + rest + carry, carried[0].begin());
        std::copy(upR.begin() + rest, upR.begin() + rest + carry, carried[1].begin());
    }

private:
    void render(float* left, float* right, int n, const pf::Event* events, int numEvents) noexcept
    {
        float* outputs[2] = { left, right };
        engine.process(outputs, 2, n, events, numEvents);
        room.process(left, right, n);
        eq.update(rate, toneBassDb, tonePresenceDb, toneAirDb);
        eq.process(left, right, n);
        limiter.process(left, right, n);
    }

    int factor = 1, block = 0, carry = 0, waiting = 0;
    double rate = 0.0;
    std::vector<float> engineL, engineR, upL, upR;
    pf::Upsampler upsampleL, upsampleR;
    pf::OutputEq eq;
    std::array<std::array<float, 8>, 2> carried {};
    std::array<pf::Event, kMaxEvents> mapped {}, waitingEvents {};
};

} // namespace pf::plugin
