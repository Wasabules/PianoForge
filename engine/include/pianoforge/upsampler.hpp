#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace pf {

namespace detail {

/// Prototype low-pass of the integer-factor resamplers below, at the high rate: a Kaiser-windowed sinc of
/// taps x factor coefficients, cut at the low rate's Nyquist, beta 9 (about 90 dB of stop band). With 64 taps per
/// phase, the transition is about 0.09 x the low rate wide around its Nyquist: flat to 20 kHz and images rejected
/// from 24.1 kHz at 44.1 kHz. h[n] = sinc((n - c) / factor) w[n]: each phase sums to about 1.
inline std::vector<double> resamplerPrototype(int factor, int taps)
{
    const int n = taps * factor;
    const double centre = (n - 1) / 2.0, beta = 9.0;
    auto besselI0 = [](double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum)
                break;
        }
        return sum;
    };
    std::vector<double> h((std::size_t) n);
    const double norm = besselI0(beta);
    for (int i = 0; i < n; ++i) {
        const double t = (i - centre) / factor;
        const double sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(3.141592653589793 * t) / (3.141592653589793 * t);
        const double r = (i - centre) / (centre + 0.5);
        h[(std::size_t) i] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
    }
    return h;
}

} // namespace detail

/// Interpolation by an integer factor (1, 2, 4 or 8) of one channel: a polyphase FIR (detail::resamplerPrototype).
/// The engine runs at 44.1 or 48 kHz under a device set to 88.2 to 384 kHz (the Windows mixer imposes its rate),
/// and this brings its sound up to the device's: about 64 taps per output sample, 64 input samples of history, a
/// latency of (64 factor - 1) / 2 output samples (0.67 ms at 192 kHz). prepare() allocates; process() does not.
class Upsampler {
public:
    static constexpr int kTaps = 64; // per phase

    void prepare(int newFactor)
    {
        k = std::clamp(newFactor, 1, 8);
        const auto h = detail::resamplerPrototype(k, kTaps);
        // Phase p, coefficient i applies to the history's i-th oldest sample: h[p + (kTaps - 1 - i) k].
        coefficients.assign((std::size_t) (k * kTaps), 0.0f);
        for (int p = 0; p < k; ++p)
            for (int i = 0; i < kTaps; ++i)
                coefficients[(std::size_t) (p * kTaps + i)] = (float) h[(std::size_t) (p + (kTaps - 1 - i) * k)];
        history.assign((std::size_t) (2 * kTaps), 0.0f);
        pos = 0;
    }
    void reset() noexcept
    {
        std::fill(history.begin(), history.end(), 0.0f);
        pos = 0;
    }
    int factor() const noexcept { return k; }

    /// `frames` input samples in, frames x factor() out.
    void process(const float* in, int frames, float* out) noexcept
    {
        if (k == 1) {
            std::copy(in, in + frames, out);
            return;
        }
        for (int m = 0; m < frames; ++m) {
            // A ring written twice, so that the last kTaps samples are always contiguous, oldest first.
            history[(std::size_t) pos] = history[(std::size_t) (pos + kTaps)] = in[m];
            pos = (pos + 1) % kTaps;
            const float* window = history.data() + pos;
            for (int p = 0; p < k; ++p) {
                const float* c = coefficients.data() + (std::size_t) (p * kTaps);
                float sum = 0.0f;
                for (int i = 0; i < kTaps; ++i)
                    sum += c[i] * window[i];
                out[m * k + p] = sum;
            }
        }
    }

private:
    int k = 1, pos = 0;
    std::vector<float> coefficients, history;
};

/// Decimation by an integer factor (1, 2, 4 or 8) of one channel, with the same low-pass (offline uses: an MP3 at
/// 48 kHz from a recording made at the device's rate). prepare() allocates; process() does not.
class Downsampler {
public:
    void prepare(int newFactor)
    {
        k = std::clamp(newFactor, 1, 8);
        const auto h = detail::resamplerPrototype(k, Upsampler::kTaps);
        taps = (int) h.size();
        coefficients.resize(h.size());
        for (std::size_t i = 0; i < h.size(); ++i) // oldest first, gain 1 / factor (the prototype's is factor)
            coefficients[i] = (float) (h[h.size() - 1 - i] / k);
        history.assign((std::size_t) (2 * taps), 0.0f);
        pos = 0;
        phase = 0;
    }
    int factor() const noexcept { return k; }

    /// `frames` input samples in; returns how many came out (one per factor() in, over successive calls).
    int process(const float* in, int frames, float* out) noexcept
    {
        int produced = 0;
        for (int m = 0; m < frames; ++m) {
            history[(std::size_t) pos] = history[(std::size_t) (pos + taps)] = in[m];
            pos = (pos + 1) % taps;
            if (++phase < k)
                continue;
            phase = 0;
            const float* window = history.data() + pos;
            float sum = 0.0f;
            for (int i = 0; i < taps; ++i)
                sum += coefficients[(std::size_t) i] * window[i];
            out[produced++] = sum;
        }
        return produced;
    }

private:
    int k = 1, taps = 0, pos = 0, phase = 0;
    std::vector<float> coefficients, history;
};

} // namespace pf
