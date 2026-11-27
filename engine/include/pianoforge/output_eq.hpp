// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <pianoforge/biquad.hpp>

#include <array>

namespace pf {

/// The output's tone: a low shelf (bass), a bell (presence), a high shelf (air), for the speakers or headphones
/// listened on (the application, the plugin). At 0 dB a band is skipped (flat: the sound untouched). update() and
/// process() on the audio thread.
class OutputEq {
public:
    static constexpr double kBassHz = 120.0, kPresenceHz = 2500.0, kPresenceQ = 0.8, kAirHz = 10000.0;

    /// The bands for these gains (dB) at `fs`: the filters' states kept when only their gains move.
    void update(double fs, float bassDb, float presenceDb, float airDb) noexcept
    {
        const std::array<float, 3> gains { bassDb, presenceDb, airDb };
        if (fs == rate && gains == applied)
            return;
        const Biquad made[3] = { Biquad::shelf(fs, kBassHz, bassDb, false), Biquad::bell(fs, kPresenceHz, presenceDb, kPresenceQ),
                                 Biquad::shelf(fs, kAirHz, airDb, true) };
        for (int b = 0; b < 3; ++b) {
            const bool wasFlat = bands[(std::size_t) b].flat() || fs != rate;
            bands[(std::size_t) b].take(made[b]);
            if (wasFlat)
                bands[(std::size_t) b].reset();
        }
        rate = fs;
        applied = gains;
    }
    bool flat() const noexcept { return bands[0].flat() && bands[1].flat() && bands[2].flat(); }
    void reset() noexcept
    {
        for (auto& b : bands)
            b.reset();
    }
    void process(float* left, float* right, int n) noexcept
    {
        for (auto& b : bands)
            if (!b.flat()) {
                b.process(left, n, 0);
                if (right != nullptr)
                    b.process(right, n, 1);
            }
    }

private:
    std::array<Biquad, 3> bands {};
    std::array<float, 3> applied {};
    double rate = 0.0;
};

} // namespace pf
