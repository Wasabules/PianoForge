// The tone controls' building blocks: the shelves and bells (pianoforge/biquad.hpp) and the stream's compressor.
#include <pianoforge/biquad.hpp>
#include <pianoforge/compressor.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("[%s]  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

/// The steady gain (dB) of a filter on a sine at `hz`, measured.
double measured(pf::Biquad f, double hz)
{
    const int n = (int) kRate;
    std::vector<float> x((std::size_t) n);
    for (int i = 0; i < n; ++i)
        x[(std::size_t) i] = (float) std::sin(kTwoPi * hz * i / kRate);
    f.process(x.data(), n, 0);
    double e = 0.0;
    for (int i = n / 2; i < n; ++i)
        e += (double) x[(std::size_t) i] * x[(std::size_t) i];
    return 10.0 * std::log10(e / (n / 2) / 0.5);
}

} // namespace

int main()
{
    // Shelves and bells.
    const auto high = pf::Biquad::shelf(kRate, 2000.0, -8.0, true);
    const auto low = pf::Biquad::shelf(kRate, 120.0, 6.0, false);
    const auto bell = pf::Biquad::bell(kRate, 2500.0, 4.0, 0.9);
    std::printf("      high shelf -8 dB at 2 kHz: %.2f dB at 200 Hz, %.2f at 2 kHz, %.2f at 12 kHz; low shelf +6 dB at 120 Hz: "
                "%.2f at 30 Hz, %.2f at 2 kHz; bell +4 dB: %.2f at 2.5 kHz, %.2f at 300 Hz\n",
                measured(high, 200.0), measured(high, 2000.0), measured(high, 12000.0), measured(low, 30.0),
                measured(low, 2000.0), measured(bell, 2500.0), measured(bell, 300.0));
    check(std::abs(measured(high, 200.0)) < 0.3 && std::abs(measured(high, 2000.0) + 4.0) < 0.5
              && std::abs(measured(high, 12000.0) + 8.0) < 0.5, "a high shelf: flat below, half way at its corner, its gain above");
    check(std::abs(measured(low, 30.0) - 6.0) < 0.5 && std::abs(measured(low, 2000.0)) < 0.3, "a low shelf: its gain below, flat above");
    check(std::abs(measured(bell, 2500.0) - 4.0) < 0.2 && std::abs(measured(bell, 300.0)) < 0.3, "a bell: its gain at its centre, flat away");
    check(std::abs(bell.gainDb(kRate, 2500.0) - 4.0) < 0.01, "the computed gain matches");
    check(pf::Biquad::shelf(kRate, 2000.0, 0.0, true).flat() && pf::Biquad::bell(kRate, 2500.0, 0.0, 0.9).flat(),
          "at 0 dB: exactly flat (skipped)");

    // The compressor: its static curve, its attack, off untouched.
    pf::Compressor::Settings s;
    s.enabled = true;
    s.thresholdDb = -20.0f;
    s.ratio = 4.0f;
    s.kneeDb = 0.0f;
    s.makeupDb = 0.0f;
    check(std::abs(pf::Compressor::gainDb(-30.0f, s)) < 1e-6 && std::abs(pf::Compressor::gainDb(-8.0f, s) + 9.0f) < 1e-4,
          "static curve: nothing under the threshold, 12 dB over it at 4:1 come out 3 dB over (-9 dB)");
    s.kneeDb = 6.0f;
    check(pf::Compressor::gainDb(-20.0f, s) < 0.0f && pf::Compressor::gainDb(-20.0f, s) > -1.2f
              && std::abs(pf::Compressor::gainDb(-23.0f, s)) < 1e-6, "a soft knee: a gentle start around the threshold");
    s.kneeDb = 0.0f;
    pf::Compressor c;
    c.prepare(kRate);
    c.applySettings(s);
    const int n = (int) kRate;
    std::vector<float> l((std::size_t) n), r((std::size_t) n);
    for (int i = 0; i < n; ++i)
        l[(std::size_t) i] = r[(std::size_t) i] = (float) (0.4 * std::sin(kTwoPi * 440.0 * i / kRate)); // -8 dBFS peaks
    c.process(l.data(), r.data(), n);
    double peak = 0.0;
    for (int i = n / 2; i < n; ++i)
        peak = std::max(peak, (double) std::abs(l[(std::size_t) i]));
    std::printf("      0.4 peak (-8 dBFS) at 4:1 over -20 dBFS: %.2f dBFS out, reduction %.2f dB\n", 20.0 * std::log10(peak),
                c.reduction());
    check(std::abs(20.0 * std::log10(peak) - (-8.0 - 9.0)) < 0.5 && std::abs(c.reduction() - 9.0f) < 0.3,
          "a steady tone: reduced by the static curve");
    s.enabled = false;
    c.applySettings(s);
    std::vector<float> a((std::size_t) 512, 0.5f), b((std::size_t) 512, -0.5f);
    c.process(a.data(), b.data(), 512);
    check(a[100] == 0.5f && b[100] == -0.5f && c.reduction() == 0.0f, "off: the sound passes untouched");

    return failures ? 1 : 0;
}
