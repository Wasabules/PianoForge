// SPDX-License-Identifier: AGPL-3.0-or-later

// Mode bank tests (no framework): exits with a non-zero code on the first failure.
#include <pianoforge/mode_bank.hpp>

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

struct TestMode {
    double freq, decay;
    std::complex<double> left, right;
};

/// Exact output in double precision: sum of Re(a exp((-decay + 2 pi i f) t)).
void reference(const std::vector<TestMode>& modes, int n, std::vector<double>& l, std::vector<double>& r)
{
    l.assign((std::size_t) n, 0.0);
    r.assign((std::size_t) n, 0.0);
    for (const auto& m : modes)
        for (int s = 0; s < n; ++s) {
            const auto e = std::exp(std::complex<double>(-m.decay, kTwoPi * m.freq) * ((double) s / kRate));
            l[(std::size_t) s] += (m.left * e).real();
            r[(std::size_t) s] += (m.right * e).real();
        }
}

double errorDb(const std::vector<float>& got, const std::vector<double>& want)
{
    double e = 0.0, p = 0.0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        e += (got[i] - want[i]) * (got[i] - want[i]);
        p = std::max(p, std::abs(want[i]));
    }
    return 10.0 * std::log10(e / (double) want.size() + 1e-300) - 20.0 * std::log10(p);
}

/// Renders in blocks of `block` (with a renormalisation between blocks, as the engine does).
void render(pf::ModeBank& bank, int n, int block, std::vector<float>& l, std::vector<float>& r)
{
    l.assign((std::size_t) n, 0.0f);
    r.assign((std::size_t) n, 0.0f);
    for (int s = 0; s < n; s += block) {
        bank.process(l.data() + s, r.data() + s, std::min(block, n - s));
        bank.renormalise();
    }
}

} // namespace

int main()
{
    // A beating bass unison (A0, two strings, two polarisations) plus a treble partial: 5 s.
    const std::vector<TestMode> modes {
        { 27.50, 0.35, { 0.3, 0.1 }, { 0.25, -0.05 } },
        { 27.62, 0.90, { 0.2, -0.2 }, { 0.1, 0.15 } },
        { 27.41, 2.50, { 0.05, 0.0 }, { 0.0, 0.05 } },
        { 55.03, 0.60, { 0.1, 0.05 }, { 0.12, 0.0 } },
        { 4186.0, 3.0, { 0.02, 0.01 }, { 0.0, -0.02 } },
    };
    const int n = (int) (5.0 * kRate);
    std::vector<double> wantL, wantR;
    reference(modes, n, wantL, wantR);

    pf::ModeBank bank;
    bank.reserve(16);
    for (const auto& m : modes)
        bank.add(pf::modePole(m.freq, m.decay, kRate), std::complex<float>(m.left), std::complex<float>(m.right));
    std::vector<float> l, r;
    render(bank, n, 64, l, r);
    const double el = errorDb(l, wantL), er = errorDb(r, wantR);
    std::printf("      error against double precision over 5 s: %.1f / %.1f dB under the peak\n", el, er);
    check(el < -60.0 && er < -60.0, "float32 bank matches the exact output (< -60 dB over 5 s, bass unison)");

    // The block size must not change the output (blocks longer than kMaxBlock are split).
    pf::ModeBank b2;
    b2.reserve(16);
    for (const auto& m : modes)
        b2.add(pf::modePole(m.freq, m.decay, kRate), std::complex<float>(m.left), std::complex<float>(m.right));
    std::vector<float> l2, r2;
    render(b2, n, 1000, l2, r2);
    check(errorDb(l2, wantL) < -60.0, "blocks of 1000 samples give the same result");

    // A shared bank: exciting a ringing mode adds a new excitation at the current time (superposition).
    {
        pf::ModeBank shared;
        shared.reserve(8);
        const auto pole = pf::modePole(120.0, 1.5, kRate);
        shared.add(pole, { 0.5f, 0.0f }, { 0.0f, 0.5f });
        const int half = 12000;
        std::vector<float> sl((std::size_t) n, 0.0f), sr((std::size_t) n, 0.0f);
        shared.process(sl.data(), sr.data(), half);
        shared.excite(0, { 0.0f, 0.3f }, { 0.2f, 0.0f });
        shared.process(sl.data() + half, sr.data() + half, n - half);
        double e = 0.0;
        for (int s = 0; s < n; ++s) {
            const auto t = (double) s / kRate, t2 = (double) (s - half) / kRate;
            const auto z = std::exp(std::complex<double>(-1.5, kTwoPi * 120.0) * t);
            const auto z2 = std::exp(std::complex<double>(-1.5, kTwoPi * 120.0) * t2);
            double want = (0.5 * z).real();
            if (s >= half)
                want += (std::complex<double>(0.0, 0.3) * z2).real();
            e = std::max(e, std::abs(sl[(std::size_t) s] - want));
        }
        check(e < 1e-4, "excite() superposes a second strike on a ringing mode");
    }

    // A damper: scaling the poles mid-note shortens the decay from that sample on, without a jump.
    {
        pf::ModeBank d;
        d.reserve(8);
        d.add(pf::modePole(440.0, 0.5, kRate), { 1.0f, 0.0f }, { 1.0f, 0.0f });
        std::vector<float> dl(2000, 0.0f), dr(2000, 0.0f);
        d.process(dl.data(), dr.data(), 1000);
        const float extra = (float) std::exp(-20.0 / kRate); // + 20 1/s
        d.scalePoles(extra);
        d.process(dl.data() + 1000, dr.data() + 1000, 1000);
        const double expected = 2.0 * std::exp(-0.5 * 2000.0 / kRate - 20.0 * 1000.0 / kRate); // both channels
        check(std::abs(d.level() - expected) < 1e-4 * expected, "scalePoles() adds the damping from then on");
        check(std::abs(dl[1000] - dl[999]) < 0.07, "no jump when the poles change");
    }

    // Level and retirement: the level bound follows the decay.
    {
        pf::ModeBank v;
        v.reserve(8);
        v.add(pf::modePole(1000.0, 10.0, kRate), { 1.0f, 0.0f }, { 0.0f, 0.0f });
        const float before = v.level();
        std::vector<float> a((std::size_t) kRate, 0.0f), b((std::size_t) kRate, 0.0f);
        v.process(a.data(), b.data(), (int) kRate);
        const float after = v.level();
        check(std::abs(before - 1.0f) < 1e-6f && std::abs(after - (float) std::exp(-10.0)) < 1e-5f,
              "level() is the current amplitude bound");
        v.clear();
        check(v.size() == 0 && v.level() == 0.0f, "clear() empties the bank");
    }

    std::printf(failures ? "%d failure(s)\n" : "All mode bank tests passed\n", failures);
    return failures ? 1 : 0;
}
