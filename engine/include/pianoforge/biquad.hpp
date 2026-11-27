// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

namespace pf {

/// A second-order filter (transposed direct form II), two channels: the shelves and bells of the tone controls
/// (Audio Eq Cookbook, R. Bristow-Johnson). A filter at 0 dB is exactly flat (flat() says so: skip it).
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float s1[2] {}, s2[2] {};

    bool flat() const noexcept { return b0 == 1.0f && b1 == 0.0f && b2 == 0.0f && a1 == 0.0f && a2 == 0.0f; }
    void reset() noexcept { s1[0] = s1[1] = s2[0] = s2[1] = 0.0f; }

    void process(float* x, int n, int channel) noexcept
    {
        float z1 = s1[channel], z2 = s2[channel];
        for (int k = 0; k < n; ++k) {
            const float in = x[k];
            const float out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            x[k] = out;
        }
        s1[channel] = z1;
        s2[channel] = z2;
    }

    /// The coefficients of another filter, the state kept (a control moved while sound goes through).
    void take(const Biquad& o) noexcept
    {
        b0 = o.b0;
        b1 = o.b1;
        b2 = o.b2;
        a1 = o.a1;
        a2 = o.a2;
    }

    /// A shelf above (high) or below (low) `hz`, `db` there; slope 1 (the steepest without overshoot).
    static Biquad shelf(double fs, double hz, double db, bool high) noexcept
    {
        Biquad f;
        if (std::abs(db) < 1e-4 || fs <= 0.0)
            return f;
        const double w0 = 2.0 * 3.14159265358979323846 * std::clamp(hz, 1.0, 0.45 * fs) / fs;
        const double A = std::pow(10.0, db / 40.0), c = std::cos(w0), alpha = std::sin(w0) / std::sqrt(2.0);
        const double root = 2.0 * std::sqrt(A) * alpha, sign = high ? 1.0 : -1.0;
        const double a0 = (A + 1.0) - sign * (A - 1.0) * c + root;
        f.b0 = (float) (A * ((A + 1.0) + sign * (A - 1.0) * c + root) / a0);
        f.b1 = (float) (-2.0 * sign * A * ((A - 1.0) + sign * (A + 1.0) * c) / a0);
        f.b2 = (float) (A * ((A + 1.0) + sign * (A - 1.0) * c - root) / a0);
        f.a1 = (float) (2.0 * sign * ((A - 1.0) - sign * (A + 1.0) * c) / a0);
        f.a2 = (float) (((A + 1.0) - sign * (A - 1.0) * c - root) / a0);
        return f;
    }

    /// A bell at `hz`, `db` at its centre, bandwidth `q`.
    static Biquad bell(double fs, double hz, double db, double q) noexcept
    {
        Biquad f;
        if (std::abs(db) < 1e-4 || fs <= 0.0)
            return f;
        const double w0 = 2.0 * 3.14159265358979323846 * std::clamp(hz, 1.0, 0.45 * fs) / fs;
        const double A = std::pow(10.0, db / 40.0), alpha = std::sin(w0) / (2.0 * std::max(q, 0.05)), c = std::cos(w0);
        const double a0 = 1.0 + alpha / A;
        f.b0 = (float) ((1.0 + alpha * A) / a0);
        f.b1 = (float) (-2.0 * c / a0);
        f.b2 = (float) ((1.0 - alpha * A) / a0);
        f.a1 = (float) (-2.0 * c / a0);
        f.a2 = (float) ((1.0 - alpha / A) / a0);
        return f;
    }

    /// The gain (dB) at `hz`, for checks.
    double gainDb(double fs, double hz) const noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * hz / fs;
        const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2.0 * w), c2i = -std::sin(2.0 * w);
        const double nr = b0 + b1 * cr + b2 * c2r, ni = b1 * ci + b2 * c2i;
        const double dr = 1.0 + a1 * cr + a2 * c2r, di = a1 * ci + a2 * c2i;
        return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

} // namespace pf
