// Sums of damped sinusoids (forge/modes.py synthesize, forge/soundboard.py synthesize_body): the modes of a note, or
// the body's resonances, as the forge subtracts them from a recording. In Python each mode cost a complex
// exponential per sample; here phasors turn by recurrence, eight modes at a time, anchored on the exact exponential
// every 1024 samples (no drift over minute-long takes). Loaded by forge/native.py (ctypes).
//
// out_c[n] = sum_k Re(a_ck exp((-decay_k + 2 pi i freq_k) (start + n) / fs)), c the channel.

#include <algorithm>
#include <complex>
#include <cstddef>

#if defined(_WIN32)
#define PF_EXPORT extern "C" __declspec(dllexport)
#else
#define PF_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
constexpr int kLanes = 8;
constexpr int kAnchor = 1024;
} // namespace

/// freqs, decays: k; amps: channels x k complex (re, im interleaved), row-major; out: channels x length, row-major,
/// overwritten.
PF_EXPORT void pf_damped_sum(const double* freqs, const double* decays, const double* amps, int k, int channels,
                             int length, double fs, long long start, double* out)
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    using cplx = std::complex<double>;
    for (std::size_t i = 0; i < (std::size_t) channels * length; ++i)
        out[i] = 0.0;
    const cplx* a = reinterpret_cast<const cplx*>(amps);
    for (int c = 0; c < channels; ++c) {
        double* y = out + (std::size_t) c * length;
        for (int first = 0; first < k; first += kLanes) {
            const int count = std::min(kLanes, k - first);
            alignas(64) double sr[kLanes] {}, si[kLanes] {}, zr[kLanes] {}, zi[kLanes] {}, ar[kLanes] {}, ai[kLanes] {};
            cplx s[kLanes];
            for (int l = 0; l < kLanes; ++l) {
                s[l] = l < count ? cplx(-decays[first + l], kTwoPi * freqs[first + l]) / fs : cplx(0.0, 0.0);
                const cplx step = std::exp(s[l]);
                sr[l] = step.real();
                si[l] = step.imag();
                if (l < count) {
                    const cplx w = a[(std::size_t) c * k + first + l];
                    ar[l] = w.real();
                    ai[l] = w.imag();
                }
            }
            for (int block = 0; block < length; block += kAnchor) {
                for (int l = 0; l < kLanes; ++l) { // the exact phasor at the block's start
                    const cplx z = std::exp(s[l] * (double) (start + block));
                    zr[l] = z.real();
                    zi[l] = z.imag();
                }
                const int end = std::min(length, block + kAnchor);
                for (int m = block; m < end; ++m) {
                    double sum = 0.0;
                    for (int l = 0; l < kLanes; ++l) {
                        sum += ar[l] * zr[l] - ai[l] * zi[l];
                        const double nr = zr[l] * sr[l] - zi[l] * si[l];
                        const double ni = zr[l] * si[l] + zi[l] * sr[l];
                        zr[l] = nr;
                        zi[l] = ni;
                    }
                    y[m] += sum;
                }
            }
        }
    }
}
