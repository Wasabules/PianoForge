// Room tests: measured acoustics of the reverberation (decay time, density, colour, width, modulation), its
// behaviour (bypass, block size, setting changes, extreme settings) and its cost. Exits non-zero on a failure.
#include <pianoforge/room.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
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
    s.predelayMs = 20.0f;
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

    // 4. Level: in the piano's band (100 Hz - 4 kHz), the wet energy of an impulse is the early energy (1) plus the
    //    late one (3 sqrt(T60 / 2 s)), against the impulse's own energy in that band.
    {
        for (const float decay : { 2.0f, 4.0f }) {
            auto s = hall();
            s.decaySeconds = decay;
            impulse(s, decay * 3.0, l, r);
            const double want = 1.0 + 3.0 * std::sqrt(decay / 2.0), in = 2.0 * (4000.0 - 100.0) / kRate;
            auto bandEnergy = [&](const Signal& x) {
                double e = 0.0;
                for (const double v : band(x, 100.0, 4000.0))
                    e += v * v;
                return e;
            };
            const double got = 0.5 * (bandEnergy(l) + bandEnergy(r)) / in;
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

    // 9. A change of size while ringing: no click (no jump larger than the tail's own).
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
        const float before = jump(x2, change - 9600, change - 1000), around = jump(x2, change - 1000, change + 2400);
        std::printf("      size changed while ringing: largest step %.4f around the change, %.4f before it\n", around, before);
        check(around <= before, "a change of size: no click");
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
                        s.predelayMs = sz > 0.5f ? 100.0f : 0.0f;
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
        check(asleep < 0.01 && a == c && b == d, "asleep after the tail: silence costs nothing, the next note starts clean");
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
        check(seconds < 0.3, "the room costs less than 3 % of a core");
    }

    return failures ? 1 : 0;
}
