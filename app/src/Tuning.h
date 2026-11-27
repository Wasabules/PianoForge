// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>

namespace pf::app {

/// Historical temperaments, as offsets in cents against equal temperament from C (C, C#, D, ..., B), built from
/// their fifths: a pure fifth is 1.955 cents wider than an equal one, the Pythagorean comma is 23.46 cents and
/// the syntonic comma 21.51 cents.
struct Temperament {
    const char* name;
    std::array<double, 12> cents;
};

inline constexpr std::array<Temperament, 7> kTemperaments { {
    { "Equal temperament", { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    // C-G-D-A and B-F# narrowed by a quarter of the Pythagorean comma, the other fifths pure.
    { "Werckmeister III", { 0, -9.775, -7.82, -5.865, -9.775, -1.955, -11.73, -3.91, -7.82, -11.73, -3.91, -7.82 } },
    // C-G-D-A-E narrowed by a quarter of the syntonic comma, F#-C# by the schisma, the others pure.
    { "Kirnberger III", { 0, -9.776, -6.843, -5.866, -13.686, -1.955, -9.776, -3.422, -7.821, -10.265, -3.911, -11.731 } },
    // F-C-G-D-A-E-B narrowed by a sixth of the Pythagorean comma, the others pure.
    { "Vallotti", { 0, -5.865, -3.91, -1.955, -7.82, 1.955, -7.82, -1.955, -3.91, -5.865, 0, -9.775 } },
    // Every fifth narrowed by a quarter of the syntonic comma (pure major thirds), from Eb to G#.
    { "Quarter-comma meantone", { 0, -23.951, -6.843, 10.265, -13.686, 3.422, -20.529, -3.422, -27.373, -10.265, 6.843, -17.108 } },
    // Pure fifths from Eb to G#.
    { "Pythagorean", { 0, 13.685, 3.91, -5.865, 7.82, -1.955, 11.73, 1.955, 15.64, 5.865, -3.91, 9.775 } },
    // Five-limit just intonation of the major key.
    { "Just intonation", { 0, 11.731, 3.91, 15.641, -13.686, -1.955, -9.776, 1.955, 13.686, -15.641, 17.596, -11.731 } },
} };

/// Offsets per pitch class (index note % 12, 0 = C) of a temperament on `root` (0 = C), with A at 0 cents: the
/// reference A4 keeps its frequency.
inline std::array<double, 12> pitchClassOffsets(const Temperament& t, int root) noexcept
{
    std::array<double, 12> out {};
    const auto at = [&](int pc) { return t.cents[(std::size_t) (((pc - root) % 12 + 12) % 12)]; };
    for (int pc = 0; pc < 12; ++pc)
        out[(std::size_t) pc] = at(pc) - at(9);
    return out;
}

} // namespace pf::app
