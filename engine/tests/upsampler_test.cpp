// Integer-factor resampler tests: gain, flatness, image rejection, latency, cost. Exits non-zero on a failure.
#include <pianoforge/upsampler.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

/// A check on how long something took: informative only when PIANOFORGE_NO_TIMING is set (a shared machine, the CI).
void timed(bool condition, const char* what)
{
    if (std::getenv("PIANOFORGE_NO_TIMING") != nullptr && !condition) {
        std::printf("[INFO]  %s (timing not checked here)\n", what);
        return;
    }
    check(condition, what);
}

constexpr double kPi = 3.141592653589793;

/// Power at one frequency (Goertzel over a Hann window), against the window's own.
double tone(const std::vector<float>& x, std::size_t from, std::size_t n, double f, double fs)
{
    std::complex<double> acc = 0.0;
    double w2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double) i / (double) n);
        acc += w * x[from + i] * std::polar(1.0, -2 * kPi * f * (double) i / fs);
        w2 += w;
    }
    return std::norm(acc) / (w2 * w2) * 4.0; // a sine of amplitude a gives a^2
}

} // namespace

int main()
{
    for (const int factor : { 2, 4 }) {
        const double fsIn = factor == 4 ? 44100.0 : 48000.0, fsOut = fsIn * factor;
        pf::Upsampler up;
        up.prepare(factor);
        // Sines at 1 kHz and 19 kHz: kept at unit gain; their images (fsIn - f, fsIn + f...) rejected.
        double worstGain = 0.0, worstImage = -999.0;
        for (const double f : { 1000.0, 19000.0 }) {
            const int n = 32768;
            std::vector<float> in((std::size_t) n), out((std::size_t) n * factor);
            for (int i = 0; i < n; ++i)
                in[(std::size_t) i] = (float) (0.5 * std::sin(2 * kPi * f * i / fsIn));
            up.reset();
            up.process(in.data(), n, out.data());
            const std::size_t from = 4096 * (std::size_t) factor, len = 16384 * (std::size_t) factor;
            const double kept = tone(out, from, len, f, fsOut);
            worstGain = std::max(worstGain, std::abs(10 * std::log10(kept / 0.25)));
            for (int image = 1; image < factor; ++image)
                for (const double g : { image * fsIn - f, image * fsIn + f }) {
                    if (g >= fsOut / 2)
                        continue;
                    worstImage = std::max(worstImage, 10 * std::log10(tone(out, from, len, g, fsOut) / kept + 1e-30));
                }
        }
        std::printf("      x%d (%.1f -> %.1f kHz): gain within %.3f dB at 1 and 19 kHz, worst image %.1f dB\n", factor,
                    fsIn / 1000, fsOut / 1000, worstGain, worstImage);
        check(worstGain < 0.05 && worstImage < -85.0, "flat to 19 kHz, images rejected by 85 dB or more");

        // Latency: an impulse comes out (64 factor - 1) / 2 samples later, its peak there.
        std::vector<float> imp(256, 0.0f), out(256 * (std::size_t) factor);
        imp[10] = 1.0f;
        up.reset();
        up.process(imp.data(), 256, out.data());
        std::size_t peak = 0;
        for (std::size_t i = 0; i < out.size(); ++i)
            if (std::abs(out[i]) > std::abs(out[peak]))
                peak = i;
        const double expected = 10.0 * factor + (64.0 * factor - 1) / 2.0;
        check(std::abs((double) peak - expected) <= 1.0, "the latency is half the filter");

        // Decimation: a 1 kHz sine back down, unit gain; a tone above the low rate's Nyquist removed.
        pf::Downsampler down;
        down.prepare(factor);
        const int n = 65536 * factor;
        std::vector<float> hi((std::size_t) n), lo((std::size_t) n / factor + 1);
        for (int i = 0; i < n; ++i)
            hi[(std::size_t) i] = (float) (0.5 * std::sin(2 * kPi * 1000.0 * i / fsOut) + 0.5 * std::sin(2 * kPi * (fsIn * 0.75) * i / fsOut));
        const int produced = down.process(hi.data(), n, lo.data());
        const double kept = tone(lo, 8192, 32768, 1000.0, fsIn);
        // The 0.75 fsIn tone folds to 0.25 fsIn once decimated: it must be gone.
        const double folded = tone(lo, 8192, 32768, fsIn * 0.25, fsIn);
        std::printf("      decimation /%d: %d samples, gain %.3f dB, a tone above the Nyquist %.1f dB\n", factor, produced,
                    10 * std::log10(kept / 0.25), 10 * std::log10(folded / 0.25 + 1e-30));
        check(produced == n / factor && std::abs(10 * std::log10(kept / 0.25)) < 0.05 && 10 * std::log10(folded / 0.25 + 1e-30) < -85.0,
              "decimation: unit gain, no folding");
    }

    // Cost: x4, 10 s of 48 kHz, one channel.
    {
        pf::Upsampler up;
        up.prepare(4);
        std::vector<float> in(480000), out(480000 * 4);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = (float) std::sin(0.01 * (double) i);
        const auto t0 = std::chrono::steady_clock::now();
        up.process(in.data(), (int) in.size(), out.data());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("      x4, 10 s of one channel in %.3f s: %.2f %% of real time\n", s, 10.0 * s);
        timed(s < 0.3, "the upsampling costs less than 3 % of a core per channel");
    }
    return failures ? 1 : 0;
}
