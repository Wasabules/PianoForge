// A bank of one-pole complex resonators driven by one signal, summed in stereo (forge/sympathetic.py, response): the
// free strings answering a played note. In Python each resonator was an lfilter call and a sum, thousands of times
// per take; here the resonators run their recurrences eight at a time (independent lanes the compiler vectorises),
// and the output is written once per block of eight instead of once per resonator. Loaded by forge/native.py.
//
// Resonator k: u_k[n] = b_k x[n] + p_k u_k[n - 1], p_k = exp((-decay_k - extra_k + 2 pi i freq_k) / fs), b_k =
// gain_k / fs (lfilter([b], [1, -p], x)); y_left += Re(wl_k u_k), y_right += Re(wr_k u_k), wl, wr complex weights.

#include <algorithm>
#include <complex>
#include <cstddef>
#include <vector>

#if defined(_WIN32)
#define PF_EXPORT extern "C" __declspec(dllexport)
#else
#define PF_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr int kLanes = 8;

struct Lanes {
    alignas(64) double pr[kLanes] {}, pi[kLanes] {}, b[kLanes] {};
    alignas(64) double lr[kLanes] {}, li[kLanes] {}, rr[kLanes] {}, ri[kLanes] {};
};

} // namespace

/// x: n samples; res: k x 6 (freq Hz, decay 1/s, wl re, wl im, wr re, wr im), row-major; gains, extras: k (gain
/// per second, extra decay 1/s; null: 1 and 0). y: 2 x n, row-major, overwritten. Resonators with gain 0 are left out.
PF_EXPORT void pf_resonator_bank(const double* x, int n, double fs, const double* res, int k, const double* gains,
                                 const double* extras, double* y)
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    double* left = y;
    double* right = y + n;
    for (int m = 0; m < n; ++m)
        left[m] = right[m] = 0.0;

    std::vector<int> active;
    active.reserve(k);
    for (int j = 0; j < k; ++j)
        if (gains == nullptr || gains[j] != 0.0)
            active.push_back(j);

    for (std::size_t first = 0; first < active.size(); first += kLanes) {
        Lanes c; // a lane without a resonator keeps b = 0 and weights 0: it adds nothing
        const int count = (int) std::min<std::size_t>(kLanes, active.size() - first);
        for (int l = 0; l < count; ++l) {
            const int j = active[first + l];
            const double* r = res + (std::size_t) j * 6;
            const double extra = extras != nullptr ? extras[j] : 0.0;
            const std::complex<double> pole = std::exp(std::complex<double>(-r[1] - extra, kTwoPi * r[0]) / fs);
            c.pr[l] = pole.real();
            c.pi[l] = pole.imag();
            c.b[l] = (gains != nullptr ? gains[j] : 1.0) / fs;
            c.lr[l] = r[2];
            c.li[l] = r[3];
            c.rr[l] = r[4];
            c.ri[l] = r[5];
        }
        alignas(64) double ur[kLanes] {}, ui[kLanes] {};
        for (int m = 0; m < n; ++m) {
            const double xm = x[m];
            double sumL = 0.0, sumR = 0.0;
            for (int l = 0; l < kLanes; ++l) {
                const double nr = c.b[l] * xm + c.pr[l] * ur[l] - c.pi[l] * ui[l];
                const double ni = c.pr[l] * ui[l] + c.pi[l] * ur[l];
                ur[l] = nr;
                ui[l] = ni;
                sumL += c.lr[l] * nr - c.li[l] * ni;
                sumR += c.rr[l] * nr - c.ri[l] * ni;
            }
            left[m] += sumL;
            right[m] += sumR;
        }
    }
}
