// SPDX-License-Identifier: AGPL-3.0-or-later

// Room tests: measured acoustics of the reverberation (decay time, density, colour, width, modulation), its
// behaviour (bypass, block size, setting changes, extreme settings) and its cost. Exits non-zero on a failure.
#include <pianoforge/room.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
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

constexpr double kRate = 48000.0;
constexpr double kPi = 3.141592653589793;

using Signal = std::vector<float>;

pf::Room::Settings hall()
{
    pf::Room::Settings s;
    s.enabled = true;
    s.levelDb = 0.0f;
    s.size = 0.65f;
    s.decaySeconds = 2.0f;
    s.brightness = 1.0f;
    s.distance = 1.0f; // at the keyboard: the direct sound is the input
    s.keepLoudness = false;
    s.width = 1.0f;
    return s;
}

/// Runs the room over (l, r) in place, in blocks of `block`.
void run(pf::Room& room, Signal& l, Signal& r, int block)
{
    for (std::size_t s = 0; s < l.size(); s += (std::size_t) block) {
        const int n = (int) std::min<std::size_t>((std::size_t) block, l.size() - s);
        room.process(l.data() + s, r.data() + s, n);
    }
}

/// The wet impulse response (an impulse on both channels, the dry impulse taken out).
void impulse(const pf::Room::Settings& settings, double seconds, Signal& l, Signal& r, int block = 64, double rate = kRate)
{
    pf::Room room;
    room.applySettings(settings);
    room.prepare(rate);
    l.assign((std::size_t) (seconds * rate), 0.0f);
    r.assign(l.size(), 0.0f);
    l[0] = r[0] = 1.0f;
    run(room, l, r, block);
    l[0] -= 1.0f;
    r[0] -= 1.0f;
}

void fft(std::vector<std::complex<double>>& a, bool inverse = false)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2 * kPi / (double) len * (inverse ? 1 : -1);
        const std::complex<double> w(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> u(1.0, 0.0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto x = a[i + j], y = a[i + j + len / 2] * u;
                a[i + j] = x + y;
                a[i + j + len / 2] = x - y;
                u *= w;
            }
        }
    }
    if (inverse)
        for (auto& x : a)
            x /= (double) n;
}

/// The signal band-passed to [lo, hi] Hz: a causal Butterworth band-pass (a 4th-order high-pass then low-pass,
/// bilinear), whose own response is short (a brick wall by FFT smears the strong start over the whole tail).
std::vector<double> octave(const Signal& x, double lo, double hi)
{
    std::vector<double> y(x.begin(), x.end());
    auto biquad = [&](double b0, double b1, double b2, double a1, double a2) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : y) {
            const double out = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = v, y2 = y1, y1 = out, v = out;
        }
    };
    for (const double q : { 0.5411961, 1.3065630 }) { // 4th-order Butterworth: two sections
        const double wl = 2 * kPi * lo / kRate, al = std::sin(wl) / (2 * q), cl = std::cos(wl);
        biquad((1 + cl) / 2 / (1 + al), -(1 + cl) / (1 + al), (1 + cl) / 2 / (1 + al), -2 * cl / (1 + al), (1 - al) / (1 + al));
        const double wh = 2 * kPi * hi / kRate, ah = std::sin(wh) / (2 * q), ch = std::cos(wh);
        biquad((1 - ch) / 2 / (1 + ah), (1 - ch) / (1 + ah), (1 - ch) / 2 / (1 + ah), -2 * ch / (1 + ah), (1 - ah) / (1 + ah));
    }
    return y;
}

/// The signal band-passed to [lo, hi] Hz (brick wall, by FFT; zero-padded to a power of two): for energies.
std::vector<double> band(const Signal& x, double lo, double hi)
{
    std::size_t n = 1;
    while (n < x.size())
        n <<= 1;
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < x.size(); ++i)
        a[i] = x[i];
    fft(a);
    for (std::size_t k = 0; k < n; ++k) {
        const double f = (double) std::min(k, n - k) * kRate / (double) n;
        if (f < lo || f > hi)
            a[k] = 0.0;
    }
    fft(a, true);
    std::vector<double> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        out[i] = a[i].real();
    return out;
}

/// The energy of x[from, to) in [lo, hi] Hz, by Parseval: the bins of that band in the spectrum of that part alone
/// (a band-passed signal cut to the input's length would lose the ringing before an impulse at its start).
double bandEnergy(const Signal& x, std::size_t from, std::size_t to, double lo, double hi)
{
    std::size_t n = 1;
    while (n < x.size())
        n <<= 1;
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = from; i < std::min(to, x.size()); ++i)
        a[i] = x[i];
    fft(a);
    double e = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double f = (double) std::min(k, n - k) * kRate / (double) n;
        if (f >= lo && f <= hi)
            e += std::norm(a[k]);
    }
    return e / (double) n;
}

/// T60 from the Schroeder energy decay curve, fitted between -5 and -35 dB (T30), from `from` seconds.
double t60(const std::vector<double>& x, double from)
{
    const auto start = (std::size_t) (from * kRate);
    std::vector<double> edc(x.size() - start);
    double sum = 0.0;
    for (std::size_t i = x.size(); i-- > start;) {
        sum += x[i] * x[i];
        edc[i - start] = sum;
    }
    const double total = edc[0];
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int count = 0;
    for (std::size_t i = 0; i < edc.size(); ++i) {
        const double db = 10.0 * std::log10(edc[i] / total + 1e-30);
        if (db <= -5.0 && db >= -35.0) {
            const double t = (double) i / kRate;
            sx += t;
            sy += db;
            sxx += t * t;
            sxy += t * db;
            ++count;
        }
    }
    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx); // dB per second
    return -60.0 / slope;
}

std::vector<double> toDouble(const Signal& x)
{
    return std::vector<double>(x.begin(), x.end());
}

double energy(const Signal& x, double from = 0.0, double to = 1e9)
{
    double e = 0.0;
    for (std::size_t i = (std::size_t) (from * kRate); i < std::min(x.size(), (std::size_t) (to * kRate)); ++i)
        e += (double) x[i] * x[i];
    return e;
}

/// The normalized correlation of a and b over [from, to) seconds, in [lo, hi] Hz (zero lag).
double coherence(const Signal& a, const Signal& b, double from, double to, double lo, double hi)
{
    const auto x = band(a, lo, hi), y = band(b, lo, hi);
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (std::size_t i = (std::size_t) (from * kRate); i < std::min(x.size(), (std::size_t) (to * kRate)); ++i)
        xy += x[i] * y[i], xx += x[i] * x[i], yy += y[i] * y[i];
    return xy / std::sqrt(xx * yy + 1e-300);
}

/// The lag (samples, parabolic) at which b best matches a, within +-maxLag, both band-passed to [lo, hi] Hz.
double lagOf(const Signal& a, const Signal& b, int maxLag, double lo, double hi)
{
    const auto x = band(a, lo, hi), y = band(b, lo, hi);
    std::vector<double> c((std::size_t) (2 * maxLag + 1));
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (std::size_t i = (std::size_t) maxLag; i + (std::size_t) maxLag < x.size(); ++i)
            sum += x[i] * y[(std::size_t) ((long long) i + lag)];
        c[(std::size_t) (lag + maxLag)] = sum;
    }
    const auto best = (std::size_t) (std::max_element(c.begin(), c.end()) - c.begin());
    double shift = 0.0;
    if (best > 0 && best + 1 < c.size()) {
        const double l = c[best - 1], m = c[best], r = c[best + 1];
        shift = 0.5 * (l - r) / (l - 2 * m + r);
    }
    return (double) best - maxLag + shift;
}

} // namespace

int main()
{
    Signal l, r;


    // 1. Off: the input passes through bit for bit; switched off while ringing, it fades out, then is bypassed.
    {
        pf::Room room;
        room.prepare(kRate);
        std::mt19937 rng(1);
        std::normal_distribution<float> noise(0.0f, 0.1f);
        Signal a(48000), b(48000);
        for (std::size_t i = 0; i < a.size(); ++i)
            a[i] = noise(rng), b[i] = noise(rng);
        Signal x = a, y = b;
        run(room, x, y, 128);
        check(x == a && y == b, "room off: the input passes through, bit for bit");

        auto s = hall();
        room.applySettings(s);
        x = a, y = b;
        run(room, x, y, 128); // ringing
        room.enabled = false;
        Signal sx(48000, 0.0f), sy(48000, 0.0f);
        run(room, sx, sy, 128); // fading out, then cleared
        Signal tx(4800, 0.0f), ty(4800, 0.0f);
        run(room, tx, ty, 128);
        const bool bypassed = std::all_of(tx.begin(), tx.end(), [](float v) { return v == 0.0f; });
        room.enabled = true;
        Signal ux(4800, 0.0f), uy(4800, 0.0f);
        run(room, ux, uy, 128);
        const bool clean = std::all_of(ux.begin(), ux.end(), [](float v) { return v == 0.0f; });
        check(bypassed && clean, "switched off while ringing: faded out and cleared (no old tail when switched on again)");
    }

    // 2. The result does not depend on the block size.
    {
        Signal l2, r2;
        impulse(hall(), 1.0, l, r, 64);
        impulse(hall(), 1.0, l2, r2, 509);
        check(l == l2 && r == r2, "blocks of 64 or 509 samples: the same output, sample for sample");
    }

    // 3. Decay time: broadband T30, and the highs shorter than the lows with a low brightness.
    {
        auto s = hall();
        impulse(s, 5.0, l, r);
        Signal m(l.size());
        for (std::size_t i = 0; i < l.size(); ++i)
            m[i] = 0.5f * (l[i] + r[i]);
        const double broad = t60(octave(m, 100.0, 4000.0), 0.15);
        std::printf("      decay 2.0 s, brightness 1: measured T60 %.2f s (100 Hz - 4 kHz)\n", broad);
        check(std::abs(broad - 2.0) < 0.2, "the decay time is the one set (within 10 %)");
        s.decaySeconds = 4.0f;
        impulse(s, 8.0, l, r);
        for (std::size_t i = 0; i < l.size(); ++i)
            m.resize(l.size()), m[i] = 0.5f * (l[i] + r[i]);
        const double longer = t60(octave(m, 100.0, 4000.0), 0.15);
        std::printf("      decay 4.0 s: measured T60 %.2f s\n", longer);
        check(std::abs(longer - 4.0) < 0.4, "a 4 s decay too");
        s.decaySeconds = 2.0f;
        s.brightness = 0.3f;
        impulse(s, 5.0, l, r);
        m.assign(l.size(), 0.0f);
        for (std::size_t i = 0; i < l.size(); ++i)
            m[i] = 0.5f * (l[i] + r[i]);
        const double low = t60(octave(m, 200.0, 500.0), 0.15), high = t60(octave(m, 6000.0, 12000.0), 0.15);
        std::printf("      brightness 0.3: T60 %.2f s at 200-500 Hz, %.2f s at 6-12 kHz; per band:", low, high);
        for (const double edge : { 100.0, 200.0, 500.0, 1000.0, 2000.0, 4000.0 })
            std::printf(" %.0f-%.0f Hz %.2f s", edge, 2 * edge, t60(octave(m, edge, 2 * edge), 0.15));
        std::printf("\n");
        check(high < 0.7 * low && low > 1.6, "low brightness: the highs die faster, the lows keep the decay set");
    }

    // 4. Level: in the mid band (250 Hz - 2 kHz: where the decay set holds, and Barron's figures are), the wet energy
    //    of an impulse is the early energy plus the late one his theory gives at the keyboard, against the impulse's
    //    own energy in that band.
    {
        for (const float decay : { 2.0f, 4.0f }) {
            auto s = hall();
            s.decaySeconds = decay;
            impulse(s, decay * 3.0, l, r);
            const auto ac = pf::Room::acoustics(s);
            const double want = std::pow(10.0, ac.earlyDb / 10.0) + std::pow(10.0, ac.lateDb / 10.0);
            const double in = 2.0 * (2000.0 - 250.0) / kRate;
            const double got = 0.5 * (bandEnergy(l, 0, l.size(), 250.0, 2000.0) + bandEnergy(r, 0, r.size(), 250.0, 2000.0)) / in;
            std::printf("      decay %.0f s: wet energy %.2f (%.1f dB against the estimate %.2f)\n", decay, got,
                        10 * std::log10(got / want), want);
            check(std::abs(10 * std::log10(got / want)) < 1.5, "the reverberation's level follows its normalization (1.5 dB)");
        }
    }

    // 5. Density: the normalized echo density (Abel and Huang) reaches that of noise within 0.15 s.
    {
        impulse(hall(), 1.0, l, r);
        const int window = (int) (0.02 * kRate);
        auto ned = [&](double at) {
            const auto c = (std::size_t) (at * kRate);
            double mean = 0.0, var = 0.0;
            for (int i = -window / 2; i < window / 2; ++i)
                mean += l[c + (std::size_t) i];
            mean /= window;
            for (int i = -window / 2; i < window / 2; ++i)
                var += std::pow(l[c + (std::size_t) i] - mean, 2.0);
            const double sd = std::sqrt(var / window);
            int outside = 0;
            for (int i = -window / 2; i < window / 2; ++i)
                outside += std::abs(l[c + (std::size_t) i] - mean) > sd ? 1 : 0;
            return outside / (double) window / std::erfc(1.0 / std::sqrt(2.0));
        };
        const double at150 = ned(0.15), at300 = ned(0.3);
        std::printf("      echo density %.2f at 0.15 s, %.2f at 0.3 s (noise: 1)\n", at150, at300);
        check(at150 > 0.85 && at300 > 0.85, "dense like noise from 0.15 s on");
    }

    // 6. Colour: the late spectrum scatters like noise's (Rayleigh: 5.6 dB) around its trend, no ringing modes.
    {
        impulse(hall(), 1.0, l, r);
        const std::size_t a = (std::size_t) (0.2 * kRate), n = 16384;
        std::vector<std::complex<double>> seg(n);
        for (std::size_t i = 0; i < n; ++i)
            seg[i] = l[a + i] * (0.5 - 0.5 * std::cos(2 * kPi * (double) i / (double) n));
        fft(seg);
        std::vector<double> db(n / 2);
        for (std::size_t k = 0; k < n / 2; ++k)
            db[k] = 20 * std::log10(std::abs(seg[k]) + 1e-20);
        double sum = 0.0, sum2 = 0.0;
        int count = 0;
        const auto k0 = (std::size_t) (200.0 * (double) n / kRate), k1 = (std::size_t) (8000.0 * (double) n / kRate);
        const std::size_t half = 40;
        for (std::size_t k = k0; k < k1; ++k) {
            double trend = 0.0;
            for (std::size_t j = k - half; j <= k + half; ++j)
                trend += db[j];
            trend /= (double) (2 * half + 1);
            sum += db[k] - trend;
            sum2 += (db[k] - trend) * (db[k] - trend);
            ++count;
        }
        const double sd = std::sqrt(sum2 / count - (sum / count) * (sum / count));
        std::printf("      late spectrum scatter %.1f dB around its trend (noise: 5.6 dB)\n", sd);
        check(sd > 4.5 && sd < 7.0, "the tail's spectrum is as smooth as noise's");
    }

    // 7. Width: left and right tails decorrelated at full width, identical at none.
    {
        auto corr = [&]() {
            double lr = 0, ll = 0, rr = 0;
            for (std::size_t i = (std::size_t) (0.1 * kRate); i < l.size(); ++i) {
                lr += (double) l[i] * r[i];
                ll += (double) l[i] * l[i];
                rr += (double) r[i] * r[i];
            }
            return lr / std::sqrt(ll * rr);
        };
        impulse(hall(), 1.0, l, r);
        const double full = corr();
        auto s = hall();
        s.width = 0.0f;
        impulse(s, 1.0, l, r);
        const double none = corr();
        std::printf("      correlation of the tails: %.2f at full width, %.3f at none\n", full, none);
        check(std::abs(full) < 0.3 && none > 0.999, "width: decorrelated at full width, mono at none");
    }

    // 8. Modulation: a 1 kHz tone keeps its pitch (energy within 4 Hz of it), no chorus.
    {
        pf::Room room;
        room.applySettings(hall());
        room.prepare(kRate);
        Signal x((std::size_t) (2.0 * kRate)), y(x.size());
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = y[i] = 0.1f * (float) std::sin(2 * kPi * 1000.0 * (double) i / kRate);
        const Signal dry = x;
        run(room, x, y, 128);
        const std::size_t a = (std::size_t) (0.6 * kRate), n = 65536; // 1.37 s, 0.73 Hz per bin
        std::vector<std::complex<double>> seg(n);
        for (std::size_t i = 0; i < n; ++i)
            seg[i] = (x[a + i] - dry[a + i]) * (0.5 - 0.5 * std::cos(2 * kPi * (double) i / (double) n));
        fft(seg);
        double near = 0.0, all = 0.0;
        for (std::size_t k = 0; k < n / 2; ++k) {
            const double f = (double) k * kRate / (double) n, p = std::norm(seg[k]);
            if (f > 900 && f < 1100)
                all += p;
            if (std::abs(f - 1000.0) < 4.0)
                near += p;
        }
        std::printf("      1 kHz through the reverberation: %.1f %% of the energy within 4 Hz\n", 100 * near / all);
        check(near / all > 0.95, "the modulation does not spread the pitch (no chorus)");
    }

    // 9. A change of size while ringing: no click (no jump larger than the tail's own, before or after).
    {
        pf::Room room;
        room.applySettings(hall());
        room.prepare(kRate);
        std::mt19937 rng(2);
        std::normal_distribution<float> noise(0.0f, 0.1f);
        Signal x((std::size_t) kRate, 0.0f), y(x.size(), 0.0f);
        for (std::size_t i = 0; i < 4800; ++i)
            x[i] = noise(rng), y[i] = noise(rng);
        const std::size_t change = (std::size_t) (0.5 * kRate);
        run(room, x, y, 128);
        // The same again, with the size changed at 0.5 s.
        pf::Room other;
        other.applySettings(hall());
        other.prepare(kRate);
        Signal x2(x.size(), 0.0f), y2(x.size(), 0.0f);
        std::mt19937 rng2(2);
        for (std::size_t i = 0; i < 4800; ++i)
            x2[i] = noise(rng2), y2[i] = noise(rng2);
        for (std::size_t s = 0; s < x2.size(); s += 128) {
            if (s == change - change % 128)
                other.size = 0.3f;
            other.process(x2.data() + s, y2.data() + s, 128);
        }
        auto jump = [&](const Signal& v, std::size_t a, std::size_t b) {
            float m = 0.0f;
            for (std::size_t i = a; i < b; ++i)
                m = std::max(m, std::abs(v[i] - v[i - 1]));
            return m;
        };
        // (Around: the fade out and in, 20 ms. A smaller room is louder: its tail after the change is compared too.)
        const float before = jump(x2, change - 9600, change - 1000), around = jump(x2, change - 1000, change + 1000),
                    after = jump(x2, change + 1000, change + 8000);
        std::printf("      size changed while ringing: largest step %.4f around the change, %.4f before it, %.4f after\n", around, before, after);
        check(around <= std::max(before, after), "a change of size: no click");
    }

    // 10. Extreme settings and rates: finite, and silent again after the tail.
    {
        bool finite = true, silent = true;
        for (const double rate : { 44100.0, 96000.0, 192000.0 })
            for (const float sz : { 0.0f, 1.0f })
                for (const float decay : { 0.1f, 30.0f })
                    for (const float bright : { 0.0f, 1.0f }) {
                        auto s = hall();
                        s.size = sz;
                        s.decaySeconds = decay;
                        s.brightness = bright;
                        s.distance = sz > 0.5f ? 30.0f : 1.0f;
                        pf::Room room;
                        room.applySettings(s);
                        room.prepare(rate);
                        Signal x((std::size_t) (0.5 * rate), 0.0f), y(x.size(), 0.0f);
                        x[0] = y[0] = 1.0f;
                        run(room, x, y, 256);
                        finite = finite && std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
                        if (decay < 1.0f) { // 0.1 s: silent (under -100 dB) after 1.5 s
                            Signal t((std::size_t) (1.0 * rate), 0.0f), u(t.size(), 0.0f);
                            run(room, t, u, 256);
                            silent = silent && std::all_of(t.end() - 4800, t.end(), [](float v) { return std::abs(v) < 1e-5f; });
                        }
                    }
        check(finite && silent, "extreme settings at 44.1 to 192 kHz: finite, and silent after a short tail");
    }

    // 10b. Asleep: once the tail has died out, silence costs nothing, and the next note starts from a clean room.
    {
        auto s = hall();
        s.decaySeconds = 0.5f;
        pf::Room room;
        room.applySettings(s);
        room.prepare(kRate);
        Signal x((std::size_t) (3.0 * kRate), 0.0f), y(x.size(), 0.0f);
        x[0] = y[0] = 1.0f;
        run(room, x, y, 128); // the tail dies out (0.5 s)
        Signal quiet((std::size_t) (60.0 * kRate), 0.0f), quiet2(quiet.size(), 0.0f);
        const auto t0 = std::chrono::steady_clock::now();
        run(room, quiet, quiet2, 128);
        const double asleep = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Signal a(48000, 0.0f), b(48000, 0.0f);
        a[0] = b[0] = 1.0f;
        run(room, a, b, 128);
        pf::Room fresh;
        fresh.applySettings(s);
        fresh.prepare(kRate);
        Signal c(48000, 0.0f), d(48000, 0.0f);
        c[0] = d[0] = 1.0f;
        run(fresh, c, d, 128);
        std::printf("      60 s of silence after the tail in %.4f s\n", asleep);
        check(a == c && b == d, "after the tail: the next note starts clean");
        timed(asleep < 0.01, "asleep after the tail: silence costs nothing");
    }

    // 12. The listening position (a concert hall: 15 000 m3, T60 2.2 s). An impulse on both channels.
    {
        auto at = [](float distance, bool keep) {
            auto s = hall();
            s.size = 0.85f;
            s.decaySeconds = 2.2f;
            s.brightness = 0.5f;
            s.distance = distance;
            s.keepLoudness = keep;
            return s;
        };
        auto response = [&](const pf::Room::Settings& s, float left, float right, Signal& l, Signal& r) {
            pf::Room room;
            room.applySettings(s);
            room.prepare(kRate);
            l.assign((std::size_t) (s.decaySeconds * 3.0 * kRate), 0.0f);
            r.assign(l.size(), 0.0f);
            l[0] = left;
            r[0] = right;
            run(room, l, r, 64);
        };
        // Energies in the mid band (250 Hz - 2 kHz), where the decay set holds and Barron's figures are.
        auto mid = [&](const Signal& x, std::size_t from, std::size_t to) { return bandEnergy(x, from, to, 250.0, 2000.0); };

        // At the keyboard: the input, bit for bit, until the first reflection.
        {
            const auto s = at(1.0f, true);
            const auto ac = pf::Room::acoustics(s);
            response(s, 1.0f, 1.0f, l, r);
            const auto first = (std::size_t) (ac.firstReflectionMs / 1000.0 * kRate);
            bool exact = l[0] == 1.0f && r[0] == 1.0f;
            for (std::size_t i = 1; i < first; ++i)
                exact = exact && l[i] == 0.0f && r[i] == 0.0f;
            std::printf("      at the keyboard: first reflection after %.1f ms, critical distance %.1f m, the room %.1f dB under the direct sound\n",
                        ac.firstReflectionMs, ac.criticalDistance, -ac.reflectionsOverDirectDb);
            check(exact && first > 0, "at the keyboard, the direct sound is the input, bit for bit");
        }

        // Further: the direct sound in 1 / r (its sum: the air takes only highs), and the energies of Barron's theory,
        // in four rooms (as the presets have them), the listener 2 to 16 m away (the studio is shorter).
        double worstLevel = 0.0, worstRatio = 0.0, worstSplit = 0.0;
        struct Kind {
            const char* name;
            float size, decay;
        };
        for (const Kind kind : { Kind { "studio", 0.15f, 0.6f }, Kind { "chamber", 0.5f, 1.4f }, Kind { "concert hall", 0.85f, 2.2f },
                                 Kind { "cathedral", 1.0f, 5.5f } })
            for (const float distance : { 2.0f, 4.0f, 8.0f, 16.0f }) {
                auto s = at(distance, false);
                s.size = kind.size;
                s.decaySeconds = kind.decay;
                const auto ac = pf::Room::acoustics(s);
                if (ac.distance < distance - 0.01)
                    continue; // the room is shorter
                response(s, 1.0f, 1.0f, l, r);
                const auto first = (std::size_t) (ac.firstReflectionMs / 1000.0 * kRate);
                const auto split = (std::size_t) (0.08 * kRate);
                double sum = 0.0;
                for (std::size_t i = 0; i < first; ++i)
                    sum += l[i];
                const double direct = mid(l, 0, first), early = mid(l, first, split), late = mid(l, split, l.size());
                const double ratio = 10 * std::log10((early + late) / direct) - ac.reflectionsOverDirectDb;
                const double earlyErr = 10 * std::log10(early / direct) - (ac.earlyDb - ac.directDb);
                const double lateErr = 10 * std::log10(late / direct) - (ac.lateDb - ac.directDb);
                std::printf("      %-12s %4.1f m: direct %.4f (1/r %.4f); reflections over direct %+5.1f dB (theory %+5.1f), early %+.1f, late %+.1f dB off\n",
                            kind.name, distance, sum, 1.0 / distance, 10 * std::log10((early + late) / direct), ac.reflectionsOverDirectDb, earlyErr,
                            lateErr);
                worstLevel = std::max(worstLevel, std::abs(sum * distance - 1.0));
                worstRatio = std::max(worstRatio, std::abs(ratio));
                worstSplit = std::max({ worstSplit, std::abs(earlyErr), std::abs(lateErr) });
            }
        check(worstLevel < 0.01, "the direct sound falls as 1 / r");
        check(worstRatio < 1.5, "the reflections against the direct sound: Barron's theory (1.5 dB)");
        check(worstSplit < 2.5, "early (first 80 ms) and late energies: Barron's theory (2.5 dB)");

        // Keep loudness: the total energy at 16 m is the keyboard's.
        {
            response(at(1.0f, true), 1.0f, 1.0f, l, r);
            const double near = mid(l, 0, l.size());
            response(at(16.0f, true), 1.0f, 1.0f, l, r);
            const double far = mid(l, 0, l.size());
            std::printf("      keep loudness: 16 m against 1 m, %+.2f dB\n", 10 * std::log10(far / near));
            check(std::abs(10 * std::log10(far / near)) < 1.0, "keep loudness: as loud at 16 m as at the keyboard (1 dB)");
        }

        // The air: ISO 9613-1 (4.66 dB/km at 1 kHz), and the direct sound's response at 20 m.
        {
            const double at1k = 1000.0 * pf::Room::airAbsorptionDbPerMetre(1000.0);
            const auto s = at(20.0f, false);
            const auto ac = pf::Room::acoustics(s);
            response(s, 1.0f, 1.0f, l, r);
            const auto first = (std::size_t) (ac.firstReflectionMs / 1000.0 * kRate);
            double worst = 0.0; // up to 10 kHz (above, first-order sections fall short of the air's f^2: 16 kHz shown)
            std::printf("      air at 20 m (ISO 9613-1, %.2f dB/km at 1 kHz):", at1k);
            for (const double f : { 2000.0, 4000.0, 8000.0, 10000.0, 16000.0 }) {
                std::complex<double> h = 0.0;
                for (std::size_t i = 0; i < first; ++i)
                    h += (double) l[i] * std::polar(1.0, -2 * kPi * f * (double) i / kRate);
                const double got = -20 * std::log10(std::abs(h) * ac.distance), want = pf::Room::airAbsorptionDbPerMetre(f) * (ac.distance - 1.0);
                std::printf(" %.0f kHz %.1f dB (ISO %.1f)", f / 1000, got, want);
                if (f <= 10000.0)
                    worst = std::max(worst, std::abs(got - want));
            }
            std::printf("\n");
            check(std::abs(at1k - 4.66) < 0.1 && worst < 0.5, "the air's absorption: ISO 9613-1, within 0.5 dB from 2 to 10 kHz at 20 m");
        }

        // The width: a left-only impulse at 8 m; the direct sound's side against its mid, the piano's angle.
        {
            const auto s = at(8.0f, false);
            const auto ac = pf::Room::acoustics(s);
            response(s, 1.0f, 0.0f, l, r);
            const auto first = (std::size_t) (ac.firstReflectionMs / 1000.0 * kRate);
            double sl = 0.0, sr = 0.0;
            for (std::size_t i = 0; i < first; ++i)
                sl += l[i], sr += r[i];
            const double width = (sl - sr) / (sl + sr), want = std::atan(0.75 / 8.0) / std::atan(0.75);
            std::printf("      width of the direct sound at 8 m: %.3f (the piano's angle: %.3f)\n", width, want);
            check(std::abs(width - want) < 0.005, "the direct sound narrows as the piano's angle");
        }

        // A short room: the listener goes no further than its end.
        {
            auto s = at(30.0f, false);
            s.size = 0.15f;
            const auto ac = pf::Room::acoustics(s);
            std::printf("      a studio (%.0f m3, %.1f m long), 30 m asked: the listener at %.1f m\n", ac.volume, ac.length, ac.distance);
            check(ac.distance < ac.length - 1.0 && ac.distance > 1.0, "a short room: the listener stays inside it");
        }

        // A change of distance while playing: no click.
        {
            pf::Room room;
            room.applySettings(at(1.0f, true));
            room.prepare(kRate);
            Signal x((std::size_t) kRate), y(x.size());
            for (std::size_t i = 0; i < x.size(); ++i)
                x[i] = y[i] = (float) (0.3 * std::sin(2 * kPi * 440.0 * (double) i / kRate));
            const std::size_t change = (std::size_t) (0.5 * kRate);
            for (std::size_t s = 0; s < x.size(); s += 128) {
                if (s == change - change % 128)
                    room.distance = 8.0f;
                room.process(x.data() + s, y.data() + s, 128);
            }
            float before = 0.0f, around = 0.0f;
            for (std::size_t i = 1; i < x.size(); ++i) {
                const float step = std::abs(x[i] - x[i - 1]);
                (i < change - 1000 ? before : around) = std::max(i < change - 1000 ? before : around, step);
            }
            std::printf("      distance changed while playing: largest step %.4f around the change, %.4f before it\n", around, before);
            check(around <= before * 1.05f, "a change of distance: no click");
        }
    }

    // 13. Headphones (binaural, a spherical head). Room off: the engine's sound at the ears, from the keyboard (each
    //     side of the piano 36.9 degrees from the front).
    {
        const double side = std::atan(0.75);
        const double itd = pf::Room::interauralDelay(side);
        std::printf("      Woodworth: %.3f ms from the side (90 degrees), %.3f ms at the keyboard (36.9 degrees)\n",
                    1000.0 * pf::Room::interauralDelay(kPi / 2), 1000.0 * itd);
        check(std::abs(pf::Room::interauralDelay(kPi / 2) - 0.0875 / 343.0 * (kPi / 2 + 1.0)) < 1e-9
                  && std::abs(1000.0 * pf::Room::interauralDelay(kPi / 2) - 0.656) < 0.002,
              "the delay between the ears: 0.656 ms from the side (head radius 8.75 cm)");
        check(std::abs(pf::Room::headShadow(kPi / 2) - 1.0) < 1e-12 && pf::Room::headShadow(0.0) > 2.5
                  && pf::Room::headShadow(5 * kPi / 6) < 0.15,
              "the head's shadow: 1 in front, more toward the ear, least 150 degrees from it");

        auto phones = [](bool roomOn) {
            pf::Room::Settings s = hall();
            s.enabled = roomOn;
            s.headphones = true;
            return s;
        };
        std::mt19937 rng(11);
        std::normal_distribution<float> noise(0.0f, 0.1f);
        const auto n = (std::size_t) kRate;
        auto render = [&](const pf::Room::Settings& settings, Signal& l, Signal& r, int block = 128) {
            pf::Room room;
            room.applySettings(settings);
            room.prepare(kRate);
            run(room, l, r, block);
        };

        // One side only (the left): its delay to the far ear, and the difference of level between the ears.
        {
            Signal l(n), r(n, 0.0f);
            for (auto& v : l)
                v = noise(rng);
            const Signal in = l;
            render(phones(false), l, r);
            // In the highs, the delay around the head (Woodworth); in the lows, the head's diffraction adds to it,
            // toward Kuhn's 3 (a / c) sin(theta) (the shadow's minimum phase gives it, as Brown and Duda note).
            const double lag = lagOf(l, r, 64, 3000.0, 10000.0) / kRate, lowLag = lagOf(l, r, 64, 0.0, 500.0) / kRate;
            const double kuhn = 3.0 * 0.0875 / 343.0 * std::sin(side);
            const double lows = 10 * std::log10(bandEnergy(l, 0, n, 50, 300) / bandEnergy(r, 0, n, 50, 300));
            const double highs = 10 * std::log10(bandEnergy(l, 0, n, 4000, 8000) / bandEnergy(r, 0, n, 4000, 8000));
            const double perEar = 10 * std::log10(0.5 * (bandEnergy(l, 0, n, 50, 300) + bandEnergy(r, 0, n, 50, 300))
                                                  / (0.5 * bandEnergy(in, 0, n, 50, 300)));
            std::printf("      one side: the far ear later by %.3f ms over 3 kHz (Woodworth %.3f), %.3f ms under 500 Hz (Kuhn %.3f); "
                        "between the ears %.2f dB under 300 Hz, %.1f dB at 4-8 kHz; each ear %+.2f dB in the lows against the input's\n",
                        1000 * lag, 1000 * itd, 1000 * lowLag, 1000 * kuhn, lows, highs, perEar);
            check(std::abs(lag - itd) < 0.05e-3 && std::abs(lowLag - kuhn) < 0.06e-3,
                  "one side: the far ear later by Woodworth's delay in the highs, Kuhn's in the lows");
            check(std::abs(lows) < 1.0 && highs > 12.0 && highs < 20.0, "one side: the ears alike in the lows, the far one shadowed in the highs");
            check(std::abs(perEar) < 0.3, "two uncorrelated sides: each ear keeps the input's energy in the lows");
        }
        // Mono: both ears alike, at the input's level.
        {
            Signal l(n), r(n);
            for (std::size_t i = 0; i < n; ++i)
                l[i] = r[i] = noise(rng);
            const Signal in = l;
            render(phones(false), l, r);
            float worst = 0.0f;
            for (std::size_t i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(l[i] - r[i]));
            const double lows = 10 * std::log10(bandEnergy(l, 0, n, 50, 300) / bandEnergy(in, 0, n, 50, 300));
            const double highs = 10 * std::log10(bandEnergy(l, 0, n, 2000, 8000) / bandEnergy(in, 0, n, 2000, 8000));
            std::printf("      mono: ears differ by %.1g at most; %+.2f dB under 300 Hz, %+.2f dB at 2-8 kHz\n", worst, lows, highs);
            check(worst == 0.0f, "mono: both ears alike (a source in front)");
            check(std::abs(lows) < 0.3 && std::abs(highs) < 1.0, "mono: heard at its level (headphones equalized for a source in front)");
        }
        // Back to speakers: bypassed again, bit for bit.
        {
            pf::Room room;
            room.applySettings(phones(false));
            room.prepare(kRate);
            Signal l(n), r(n);
            for (std::size_t i = 0; i < n; ++i)
                l[i] = noise(rng), r[i] = noise(rng);
            const Signal inL = l, inR = r;
            const std::size_t change = n / 4;
            for (std::size_t at = 0; at < n; at += 128) {
                if (at == change - change % 128)
                    room.headphones = false;
                room.process(l.data() + at, r.data() + at, 128);
            }
            std::size_t from = n; // the first sample from which the output is the input, bit for bit
            while (from > 0 && l[from - 1] == inL[from - 1] && r[from - 1] == inR[from - 1])
                --from;
            std::printf("      back to speakers: the input passes through bit for bit %.0f ms after the switch\n",
                        1000.0 * ((double) from - (double) (change - change % 128)) / kRate);
            check(from < change + (std::size_t) (0.35 * kRate), "back to speakers: the input passes through bit for bit again");
        }

        // With the room (8 m in a hall): the tail as two ears hear a diffuse field.
        auto at8 = [](bool headphones) {
            auto s = hall();
            s.distance = 8.0f;
            s.headphones = headphones;
            return s;
        };
        Signal sl, sr, hl, hr;
        impulse(at8(false), 2.5, sl, sr);
        impulse(at8(true), 2.5, hl, hr);
        const double speakersLow = coherence(sl, sr, 0.3, 2.0, 50, 200), headphonesLow = coherence(hl, hr, 0.3, 2.0, 50, 200);
        const double headphonesHigh = coherence(hl, hr, 0.3, 2.0, 2000, 6000);
        std::printf("      the tail's coherence between the ears, 50-200 Hz: %.2f on speakers, %.2f on headphones; 2-6 kHz: %.2f\n",
                    speakersLow, headphonesLow, headphonesHigh);
        check(headphonesLow > 0.75 && std::abs(speakersLow) < 0.4 && std::abs(headphonesHigh) < 0.3,
              "headphones: the tail coherent between the ears in the lows, not in the highs");
        const double tail = 10 * std::log10((bandEnergy(hl, (size_t) (0.3 * kRate), hl.size(), 100, 4000) + bandEnergy(hr, (size_t) (0.3 * kRate), hr.size(), 100, 4000))
                                            / (bandEnergy(sl, (size_t) (0.3 * kRate), sl.size(), 100, 4000) + bandEnergy(sr, (size_t) (0.3 * kRate), sr.size(), 100, 4000)));
        const double early = 10 * std::log10((bandEnergy(hl, 48, (size_t) (0.08 * kRate), 250, 2000) + bandEnergy(hr, 48, (size_t) (0.08 * kRate), 250, 2000))
                                             / (bandEnergy(sl, 48, (size_t) (0.08 * kRate), 250, 2000) + bandEnergy(sr, 48, (size_t) (0.08 * kRate), 250, 2000)));
        std::printf("      headphones against speakers: the tail %+.2f dB (100 Hz - 4 kHz), the first 80 ms %+.2f dB (250 Hz - 2 kHz)\n", tail, early);
        check(std::abs(tail) < 0.5 && std::abs(early) < 1.5, "headphones: the room's energy as on speakers");

        // The result does not depend on the block size; a switch while playing makes no click.
        {
            Signal a(n), b(n), c(n), d(n);
            for (std::size_t i = 0; i < n; ++i)
                a[i] = c[i] = noise(rng), b[i] = d[i] = noise(rng);
            render(at8(true), a, b, 64);
            render(at8(true), c, d, 509);
            check(a == c && b == d, "headphones: the result does not depend on the block size");
        }
        {
            pf::Room room;
            room.applySettings(at8(false));
            room.prepare(kRate);
            Signal x(n), y(n);
            for (std::size_t i = 0; i < n; ++i)
                x[i] = y[i] = (float) (0.3 * std::sin(2 * kPi * 440.0 * (double) i / kRate));
            const std::size_t change = n / 2;
            for (std::size_t at = 0; at < n; at += 128) {
                if (at == change - change % 128)
                    room.headphones = true;
                room.process(x.data() + at, y.data() + at, 128);
            }
            float before = 0.0f, around = 0.0f;
            for (std::size_t i = 1; i < n; ++i) {
                const float step = std::max(std::abs(x[i] - x[i - 1]), std::abs(y[i] - y[i - 1]));
                (i < change - 1000 ? before : around) = std::max(i < change - 1000 ? before : around, step);
            }
            std::printf("      switched to headphones while playing: largest step %.4f around the switch, %.4f before it\n", around, before);
            check(around <= before * 1.1f, "a switch to headphones: no click");
        }
        // Cost, on headphones against speakers (the same run: what the machine is doing besides weighs on both).
        {
            auto cost = [&](bool headphones) {
                double best = 1e9;
                for (int pass = 0; pass < 3; ++pass) {
                    pf::Room room;
                    room.applySettings(at8(headphones));
                    room.prepare(kRate);
                    Signal x((std::size_t) (4 * kRate)), y(x.size());
                    for (std::size_t i = 0; i < x.size(); ++i)
                        x[i] = noise(rng), y[i] = noise(rng);
                    const auto t0 = std::chrono::steady_clock::now();
                    run(room, x, y, 128);
                    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
                return best;
            };
            const double speakers = cost(false), headphones = cost(true);
            std::printf("      on headphones: %.2f %% of real time, %.2f times the cost on speakers\n", 100 * headphones / 4.0, headphones / speakers);
            timed(headphones < 1.4 * speakers, "on headphones, the room costs at most 1.4 times as much");
        }
    }

    // 11. Cost: 10 s of stereo noise at 48 kHz, blocks of 128.
    {
        pf::Room room;
        room.applySettings(hall());
        room.prepare(kRate);
        std::mt19937 rng(3);
        std::normal_distribution<float> noise(0.0f, 0.1f);
        Signal x((std::size_t) (10 * kRate)), y(x.size());
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = noise(rng), y[i] = noise(rng);
        const auto t0 = std::chrono::steady_clock::now();
        run(room, x, y, 128);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("      10 s at 48 kHz in %.3f s: %.2f %% of real time\n", seconds, 100 * seconds / 10.0);
        timed(seconds < 0.3, "the room costs less than 3 % of a core");
    }

    return failures ? 1 : 0;
}
