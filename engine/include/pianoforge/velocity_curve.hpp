// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace pf {

/// Maps the velocities a MIDI keyboard actually produces onto the full 1..127 range.
///
/// The input range [inputMin, inputMax] is what the keyboard really sends (the Korg B2 tops out around
/// 114). The curve goes through kPoints control points evenly spaced over that range; between them it is a
/// monotone cubic (Fritsch-Carlson), so it never overshoots and never decreases, whatever the points.
///
/// map() is lock-free and safe on the MIDI or audio thread; set() is meant for the UI thread. While set()
/// rewrites the table, a concurrent map() may read an old or a new entry: both are valid velocities.
class VelocityCurve {
public:
    static constexpr int kPoints = 5;

    struct Settings {
        int inputMin = 1;
        int inputMax = 127;
        std::array<float, kPoints> points { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }; // output (0..1) at each point

        bool operator==(const Settings&) const = default;
    };

    VelocityCurve() noexcept { set({}); }

    void set(const Settings& s) noexcept
    {
        table[0].store(0, std::memory_order_relaxed); // a note-on with velocity 0 is a note-off: keep it so
        for (int v = 1; v < 128; ++v)
            table[(size_t) v].store(output(s, v), std::memory_order_relaxed);
    }

    std::uint8_t map(int velocity) const noexcept
    {
        return table[(size_t) std::clamp(velocity, 0, 127)].load(std::memory_order_relaxed);
    }

    /// Output velocity (1..127) for an input velocity (1..127), computed directly from the settings.
    static std::uint8_t output(const Settings& s, int velocity) noexcept
    {
        const int lo = std::clamp(s.inputMin, 1, 126), hi = std::clamp(s.inputMax, lo + 1, 127);
        const double x = std::clamp((double) (velocity - lo) / (double) (hi - lo), 0.0, 1.0);
        const double y = evaluate(s, x);
        return (std::uint8_t) std::clamp(1 + (int) std::lround(y * 126.0), 1, 127);
    }

    /// Normalized curve: x in [0, 1] -> y in [0, 1].
    static double evaluate(const Settings& s, double x) noexcept
    {
        constexpr int n = kPoints;
        constexpr double h = 1.0 / (n - 1);
        std::array<double, n> y {}, m {}, d {};
        double running = 0.0;
        for (int i = 0; i < n; ++i) // control points forced into [0, 1] and non-decreasing
            y[(size_t) i] = running = std::max(running, std::clamp((double) s.points[(size_t) i], 0.0, 1.0));
        for (int i = 0; i < n - 1; ++i)
            d[(size_t) i] = (y[(size_t) i + 1] - y[(size_t) i]) / h;
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (int i = 1; i < n - 1; ++i)
            m[(size_t) i] = d[(size_t) i - 1] * d[(size_t) i] <= 0.0 ? 0.0 : 0.5 * (d[(size_t) i - 1] + d[(size_t) i]);
        for (int i = 0; i < n - 1; ++i) { // Fritsch-Carlson: limit the tangents to keep the curve monotone
            if (d[(size_t) i] == 0.0) {
                m[(size_t) i] = m[(size_t) i + 1] = 0.0;
                continue;
            }
            const double a = m[(size_t) i] / d[(size_t) i], b = m[(size_t) i + 1] / d[(size_t) i];
            if (const double r = a * a + b * b; r > 9.0) {
                const double t = 3.0 / std::sqrt(r);
                m[(size_t) i] = t * a * d[(size_t) i];
                m[(size_t) i + 1] = t * b * d[(size_t) i];
            }
        }
        const double xc = std::clamp(x, 0.0, 1.0);
        const int k = std::min((int) (xc / h), n - 2);
        const double t = (xc - k * h) / h, t2 = t * t, t3 = t2 * t;
        const double v = (2 * t3 - 3 * t2 + 1) * y[(size_t) k] + (t3 - 2 * t2 + t) * h * m[(size_t) k]
                       + (-2 * t3 + 3 * t2) * y[(size_t) k + 1] + (t3 - t2) * h * m[(size_t) k + 1];
        return std::clamp(v, 0.0, 1.0);
    }

    /// Ready-made shapes: y = x^exponent sampled at the control points (1 = linear, < 1 = softer touch).
    static Settings shaped(Settings s, double exponent) noexcept
    {
        for (int i = 0; i < kPoints; ++i)
            s.points[(size_t) i] = (float) std::pow((double) i / (kPoints - 1), exponent);
        return s;
    }

private:
    std::array<std::atomic<std::uint8_t>, 128> table {};
};

/// Maps what a sustain pedal sends (CC64) onto the 0..127 the engines read. [inputMin, inputMax] is the travel the
/// pedal really covers (some never reach 0 or 127, some send a few steps only); between them, the same monotone
/// curve through kPoints points as the velocities'. At or under inputMin: 0 (up); at or over inputMax: 127 (down).
/// map() is lock-free (MIDI thread); set() is meant for the UI thread.
class PedalCurve {
public:
    using Settings = VelocityCurve::Settings;
    static constexpr int kPoints = VelocityCurve::kPoints;

    static Settings defaults() noexcept
    {
        Settings s;
        s.inputMin = 0;
        s.inputMax = 127;
        return s;
    }

    PedalCurve() noexcept { set(defaults()); }

    void set(const Settings& s) noexcept
    {
        for (int v = 0; v < 128; ++v)
            table[(size_t) v].store(output(s, v), std::memory_order_relaxed);
    }

    std::uint8_t map(int value) const noexcept { return table[(size_t) std::clamp(value, 0, 127)].load(std::memory_order_relaxed); }

    static std::uint8_t output(const Settings& s, int value) noexcept
    {
        const int lo = std::clamp(s.inputMin, 0, 126), hi = std::clamp(s.inputMax, lo + 1, 127);
        if (value <= lo)
            return 0;
        if (value >= hi)
            return 127;
        const double x = (double) (value - lo) / (double) (hi - lo);
        return (std::uint8_t) std::clamp((int) std::lround(VelocityCurve::evaluate(s, x) * 127.0), 0, 127);
    }

private:
    std::array<std::atomic<std::uint8_t>, 128> table {};
};

} // namespace pf
