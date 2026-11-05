#pragma once

#include <pianoforge/engine.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

namespace pf::app {

/// Routes the audio and the MIDI to one of two engines, for A/B listening from the keyboard. On a switch, the
/// engine left behind gets an all-sound-off and keeps rendering (without new events) for a short fade, so
/// that no note is cut with a click.
class EngineSwitch final : public pf::Engine {
public:
    EngineSwitch(pf::Engine& first, pf::Engine& second) : engines { &first, &second } {}

    std::atomic<int> selected { 0 }; // 0: first, 1: second (any thread)

    void prepare(double sampleRate, int maxBlockSize) override
    {
        rate = sampleRate;
        for (auto* e : engines)
            e->prepare(sampleRate, maxBlockSize);
        for (auto& s : scratch)
            s.assign((size_t) std::max(32, maxBlockSize), 0.0f);
        active = selected.load();
        fadeLeft = 0;
    }

    void release() noexcept override
    {
        for (auto* e : engines)
            e->release();
    }

    void process(float* const* outputs, int numChannels, int n, const pf::Event* events, int numEvents) noexcept override
    {
        const int wanted = std::clamp(selected.load(std::memory_order_relaxed), 0, 1);
        if (wanted != active) {
            engines[(size_t) active]->allSoundOff();
            fading = active;
            fadeLeft = (int) (0.1 * rate);
            active = wanted;
        }
        engines[(size_t) active]->process(outputs, numChannels, n, events, numEvents);
        if (fadeLeft > 0 && n <= (int) scratch[0].size()) {
            float* tail[2] = { scratch[0].data(), scratch[1].data() };
            engines[(size_t) fading]->process(tail, std::min(numChannels, 2), n, nullptr, 0);
            for (int c = 0; c < std::min(numChannels, 2); ++c)
                if (outputs[c] != nullptr)
                    for (int k = 0; k < n; ++k)
                        outputs[c][k] += tail[c][k];
            fadeLeft -= n;
        }
    }

    void allSoundOff() noexcept override
    {
        for (auto* e : engines)
            e->allSoundOff();
    }

private:
    std::array<pf::Engine*, 2> engines;
    std::array<std::vector<float>, 2> scratch;
    double rate = 48000.0;
    int active = 0, fading = 0, fadeLeft = 0;
};

} // namespace pf::app
