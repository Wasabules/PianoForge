#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

// Groups of 8 modes interleaved in the AVX2 sample loop (2 measured best with MSVC: 4 spills registers).
#ifndef PF_INTERLEAVE
#define PF_INTERLEAVE 2
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace pf {

/// A bank of decaying complex exponentials (modes) with a stereo output, the core of the modal engine.
///
/// Mode k is a phasor u_k that turns and decays by its pole p_k every sample, u_k <- p_k u_k, and is heard as
/// out_c += Re(a_ck u_k) on each channel c. This complex (coupled) form keeps the frequency exact in float32
/// even in the bass, where the direct two-pole form detunes by up to 1.6 cent (pre-analysis section 4.5), and it
/// lets a pole change while the mode rings, without a click (dampers, settings).
///
/// The modes are stored as structures of arrays in lanes of kLanes: one lane loop is one SIMD operation once the
/// compiler vectorises it (AVX2: 8 floats). A block is rendered lane group by lane group, each group running
/// through the whole block with its state in registers, accumulating into a per-sample, per-lane buffer that is
/// reduced once at the end.
///
/// Not thread-safe: one bank belongs to one voice (or to the shared soundboard) on the audio thread. reserve()
/// allocates; add(), clear(), process() and the pole changes do not.
class ModeBank {
public:
    static constexpr int kLanes = 8;
    static constexpr int kMaxBlock = 256; // process() splits longer blocks

    void reserve(int modes)
    {
        const std::size_t n = roundUp((std::size_t) std::max(modes, 0));
        for (auto* v : { &ur, &ui, &pr, &pi, &er, &ei, &lr, &li, &rr, &ri })
            v->assign(n, 0.0f);
        acc.assign((std::size_t) kMaxBlock * kLanes * 2, 0.0f);
        count = 0;
    }

    int capacity() const noexcept { return (int) ur.size(); }
    int size() const noexcept { return count; }
    void clear() noexcept
    {
        const auto used = (std::ptrdiff_t) roundUp((std::size_t) count);
        for (auto* v : { &ur, &ui, &pr, &pi, &er, &ei, &lr, &li, &rr, &ri })
            std::fill(v->begin(), v->begin() + used, 0.0f); // padding lanes must stay silent
        count = 0;
    }

    /// Adds a mode: pole per sample (from freq, decay and the sample rate), complex amplitude per channel at the
    /// first sample of the next process(). Returns false when the bank is full.
    bool add(std::complex<double> pole, std::complex<float> left, std::complex<float> right) noexcept
    {
        if (count >= capacity())
            return false;
        const auto k = (std::size_t) count++;
        er[k] = pr[k] = (float) pole.real();
        ei[k] = pi[k] = (float) pole.imag();
        // The amplitudes are folded into a unit phasor: u starts at 1, a carries the amplitude.
        ur[k] = 1.0f;
        ui[k] = 0.0f;
        lr[k] = left.real();
        li[k] = left.imag();
        rr[k] = right.real();
        ri[k] = right.imag();
        return true;
    }

    /// Adds to the amplitudes of mode k (shared banks, such as the soundboard: every note excites the same
    /// resonators, and the amplitudes are defined against the current phasor).
    void excite(int k, std::complex<float> left, std::complex<float> right) noexcept
    {
        const auto i = (std::size_t) k;
        // a u stays the same plus the new excitation at the current phasor: a' = a + e / u.
        const std::complex<float> u { ur[i], ui[i] };
        const float m = std::norm(u);
        if (m < 1e-30f) { // a mode that has died out restarts from a unit phasor
            ur[i] = 1.0f;
            ui[i] = 0.0f;
            lr[i] = left.real();
            li[i] = left.imag();
            rr[i] = right.real();
            ri[i] = right.imag();
            return;
        }
        const std::complex<float> inv = std::conj(u) / m;
        const auto l = std::complex<float> { lr[i], li[i] } + left * inv;
        const auto r = std::complex<float> { rr[i], ri[i] } + right * inv;
        lr[i] = l.real();
        li[i] = l.imag();
        rr[i] = r.real();
        ri[i] = r.imag();
    }

    /// Silences every mode but keeps the poles (a shared bank waiting for its next excitation).
    void silence() noexcept
    {
        for (std::size_t k = 0; k < (std::size_t) count; ++k) {
            ur[k] = 1.0f;
            ui[k] = lr[k] = li[k] = rr[k] = ri[k] = 0.0f;
        }
    }

    /// Multiplies the magnitude of every pole by `factor` (< 1: extra damping, e.g. a damper landing).
    void scalePoles(float factor) noexcept { scalePoles(0, count, factor); }

    /// Same for the modes [first, first + n) only. A factor slightly above 1 for N samples raises their level
    /// by factor^N, smoothly (an exponential ramp, no step in the waveform).
    void scalePoles(int first, int n, float factor) noexcept
    {
        const auto end = (std::size_t) std::min(first + n, count);
        for (auto k = (std::size_t) std::max(first, 0); k < end; ++k) {
            pr[k] *= factor;
            pi[k] *= factor;
        }
    }

    /// Replaces the pole of mode k, which becomes its exact pole.
    void setPole(int k, std::complex<double> pole) noexcept
    {
        if (k < 0 || k >= count)
            return;
        er[(std::size_t) k] = pr[(std::size_t) k] = (float) pole.real();
        ei[(std::size_t) k] = pi[(std::size_t) k] = (float) pole.imag();
    }

    /// The poles of the modes [first, first + n): their exact poles (as added) times `factor`. Unlike
    /// scalePoles(), nothing accumulates: a damping set every block stays exact.
    void setPoleFactor(int first, int n, float factor) noexcept
    {
        const auto end = (std::size_t) std::min(first + n, count);
        for (auto k = (std::size_t) std::max(first, 0); k < end; ++k) {
            pr[k] = er[k] * factor;
            pi[k] = ei[k] * factor;
        }
    }

    /// Accumulates numSamples into left and right (adds, does not overwrite).
    void process(float* left, float* right, int numSamples) noexcept
    {
        for (int start = 0; start < numSamples; start += kMaxBlock) {
            const int n = std::min(kMaxBlock, numSamples - start);
            renderBlock(left + start, right + start, n);
        }
    }

    /// Sum of |a| |u| over every mode and channel: an upper bound of the output level, to retire a silent voice.
    float level() const noexcept
    {
        float s = 0.0f;
        for (std::size_t k = 0; k < (std::size_t) count; ++k) {
            const float u = std::sqrt(ur[k] * ur[k] + ui[k] * ui[k]);
            s += u * (std::sqrt(lr[k] * lr[k] + li[k] * li[k]) + std::sqrt(rr[k] * rr[k] + ri[k] * ri[k]));
        }
        return s;
    }

    /// Sum of |a|^2 |u|^2 over every mode and channel (cheaper than level(), for silence detection).
    float energy() const noexcept
    {
        float s = 0.0f;
        for (std::size_t k = 0; k < (std::size_t) count; ++k)
            s += (ur[k] * ur[k] + ui[k] * ui[k]) * (lr[k] * lr[k] + li[k] * li[k] + rr[k] * rr[k] + ri[k] * ri[k]);
        return s;
    }

    /// Brings the phasors back to unit magnitude (folding the magnitude into the amplitudes): over a long note,
    /// float32 phasors of slow modes would otherwise go denormal long after the amplitudes would have.
    void renormalise() noexcept
    {
        for (std::size_t k = 0; k < (std::size_t) count; ++k) {
            const float m = std::sqrt(ur[k] * ur[k] + ui[k] * ui[k]);
            if (m < 1e-30f || m > 0.5f)
                continue;
            ur[k] /= m;
            ui[k] /= m;
            lr[k] *= m;
            li[k] *= m;
            rr[k] *= m;
            ri[k] *= m;
        }
    }

private:
    static std::size_t roundUp(std::size_t n) noexcept { return (n + kLanes - 1) / kLanes * kLanes; }

#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
    /// G consecutive groups of 8 modes from mode o, through n samples, accumulated into the per-lane buffers.
    template <int G>
    void renderGroupsAvx(std::size_t o, float* accL, float* accR, int n) noexcept
    {
        __m256 xr[G], xi[G], qr[G], qi[G];
        for (int j = 0; j < G; ++j) {
            const std::size_t k = o + (std::size_t) j * kLanes;
            xr[j] = _mm256_loadu_ps(&ur[k]);
            xi[j] = _mm256_loadu_ps(&ui[k]);
            qr[j] = _mm256_loadu_ps(&pr[k]);
            qi[j] = _mm256_loadu_ps(&pi[k]);
        }
        const float* al = &lr[o];
        const float* bl = &li[o];
        const float* ar = &rr[o];
        const float* br = &ri[o];
        for (int s = 0; s < n; ++s) {
            float* oL = accL + (std::size_t) s * kLanes;
            float* oR = accR + (std::size_t) s * kLanes;
            __m256 l = _mm256_loadu_ps(oL), r = _mm256_loadu_ps(oR);
            for (int j = 0; j < G; ++j) {
                const std::size_t k = (std::size_t) j * kLanes;
                // out += Re(a u) = a_re u_re - a_im u_im
                l = _mm256_fnmadd_ps(_mm256_loadu_ps(bl + k), xi[j], _mm256_fmadd_ps(_mm256_loadu_ps(al + k), xr[j], l));
                r = _mm256_fnmadd_ps(_mm256_loadu_ps(br + k), xi[j], _mm256_fmadd_ps(_mm256_loadu_ps(ar + k), xr[j], r));
                // u <- p u
                const __m256 nr = _mm256_fmsub_ps(xr[j], qr[j], _mm256_mul_ps(xi[j], qi[j]));
                xi[j] = _mm256_fmadd_ps(xr[j], qi[j], _mm256_mul_ps(xi[j], qr[j]));
                xr[j] = nr;
            }
            _mm256_storeu_ps(oL, l);
            _mm256_storeu_ps(oR, r);
        }
        for (int j = 0; j < G; ++j) {
            const std::size_t k = o + (std::size_t) j * kLanes;
            _mm256_storeu_ps(&ur[k], xr[j]);
            _mm256_storeu_ps(&ui[k], xi[j]);
        }
    }
#endif

    void renderBlock(float* left, float* right, int n) noexcept
    {
        float* accL = acc.data();
        float* accR = acc.data() + (std::size_t) kMaxBlock * kLanes;
        std::fill(accL, accL + (std::size_t) n * kLanes, 0.0f);
        std::fill(accR, accR + (std::size_t) n * kLanes, 0.0f);
        const std::size_t groups = roundUp((std::size_t) count) / kLanes;
#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER)) // MSVC: /arch:AVX2 implies FMA
        // Each phasor update waits for the previous one (a multiply then a fused multiply-add, about 8 cycles):
        // several groups are interleaved in the sample loop so that their updates overlap.
        std::size_t g = 0;
        if constexpr (PF_INTERLEAVE >= 4)
            for (; g + 4 <= groups; g += 4)
                renderGroupsAvx<4>(g * kLanes, accL, accR, n);
        if constexpr (PF_INTERLEAVE >= 2)
            for (; g + 2 <= groups; g += 2)
                renderGroupsAvx<2>(g * kLanes, accL, accR, n);
        for (; g < groups; ++g)
            renderGroupsAvx<1>(g * kLanes, accL, accR, n);
#else
        for (std::size_t g = 0; g < groups; ++g) {
            const std::size_t o = g * kLanes;
            float xr[kLanes], xi[kLanes], qr[kLanes], qi[kLanes], al[kLanes], bl[kLanes], ar[kLanes], br[kLanes];
            for (int j = 0; j < kLanes; ++j) {
                xr[j] = ur[o + j];
                xi[j] = ui[o + j];
                qr[j] = pr[o + j];
                qi[j] = pi[o + j];
                al[j] = lr[o + j];
                bl[j] = li[o + j];
                ar[j] = rr[o + j];
                br[j] = ri[o + j];
            }
            for (int s = 0; s < n; ++s) {
                float* oL = accL + (std::size_t) s * kLanes;
                float* oR = accR + (std::size_t) s * kLanes;
                for (int j = 0; j < kLanes; ++j) {
                    oL[j] += al[j] * xr[j] - bl[j] * xi[j];
                    oR[j] += ar[j] * xr[j] - br[j] * xi[j];
                    const float nr = xr[j] * qr[j] - xi[j] * qi[j];
                    const float ni = xr[j] * qi[j] + xi[j] * qr[j];
                    xr[j] = nr;
                    xi[j] = ni;
                }
            }
            for (int j = 0; j < kLanes; ++j) {
                ur[o + j] = xr[j];
                ui[o + j] = xi[j];
            }
        }
#endif
        for (int s = 0; s < n; ++s) {
            float l = 0.0f, r = 0.0f;
            for (int j = 0; j < kLanes; ++j) {
                l += accL[(std::size_t) s * kLanes + j];
                r += accR[(std::size_t) s * kLanes + j];
            }
            left[s] += l;
            right[s] += r;
        }
    }

    // Structure of arrays, padded to a multiple of kLanes (padding lanes have zero amplitude); er, ei: the exact
    // poles, as added.
    std::vector<float> ur, ui, pr, pi, er, ei, lr, li, rr, ri;
    std::vector<float> acc;
    int count = 0;
};

/// A bank of driven resonators in groups that share one input: the free strings of the sympathetic resonance
/// (one group per string, driven by everything else that sounds).
///
/// Resonator k is a phasor u_k <- p_k u_k + b_k x (x: its group's input sample, b_k real), heard as
/// out_c += Re(a_ck u_k). Groups start on a lane boundary, so that a lane loop never mixes two inputs. Each
/// resonator keeps its exact pole, so that extra damping (a damper on the string) can be undone.
///
/// Not thread-safe (audio thread). reserve() allocates; the rest does not.
class DrivenBank {
public:
    static constexpr int kLanes = ModeBank::kLanes;
    static constexpr int kMaxBlock = ModeBank::kMaxBlock;

    void reserve(int resonators, int maxGroups)
    {
        const std::size_t n = (std::size_t) std::max(resonators, 0) + (std::size_t) std::max(maxGroups, 0) * kLanes;
        for (auto* v : { &ur, &ui, &pr, &pi, &er, &ei, &gain, &lr, &li, &rr, &ri })
            v->assign(n, 0.0f);
        acc.assign((std::size_t) kMaxBlock * kLanes * 2, 0.0f);
        starts.assign(1, 0);
        starts.reserve((std::size_t) std::max(maxGroups, 0) + 1);
        groupLimit = std::max(maxGroups, 0);
        count = 0;
    }

    int groups() const noexcept { return (int) starts.size() - 1; }
    int size() const noexcept { return count; }

    /// Opens a new group; the resonators added next belong to it. Returns its index, or -1 when full.
    int beginGroup() noexcept
    {
        if (groups() >= groupLimit)
            return -1;
        starts.push_back(starts.back());
        return groups() - 1;
    }

    /// Adds a resonator to the open group: pole per sample, input gain per sample, complex output per channel.
    bool add(std::complex<double> pole, float inputGain, std::complex<float> left, std::complex<float> right) noexcept
    {
        const auto k = (std::size_t) starts.back();
        if (groups() == 0 || k >= ur.size())
            return false;
        er[k] = pr[k] = (float) pole.real();
        ei[k] = pi[k] = (float) pole.imag();
        ur[k] = ui[k] = 0.0f;
        gain[k] = inputGain;
        lr[k] = left.real();
        li[k] = left.imag();
        rr[k] = right.real();
        ri[k] = right.imag();
        starts.back() = (int) k + 1;
        ++count;
        return true;
    }

    /// Closes the open group: its padding lanes (silent, zero gain) run up to the next lane boundary.
    void endGroup() noexcept { starts.back() = (int) std::min(roundUp((std::size_t) starts.back()), ur.size()); }

    /// Silences every resonator of group g (the poles stay).
    void silence(int g) noexcept
    {
        for (auto k = first(g); k < last(g); ++k)
            ur[k] = ui[k] = 0.0f;
    }

    /// Replaces the exact pole of resonator k of group g (retuning); dampGroup() then applies its damping.
    void setExactPole(int g, int k, std::complex<double> pole) noexcept
    {
        const auto i = first(g) + (std::size_t) k;
        if (k < 0 || i >= last(g))
            return;
        er[i] = (float) pole.real();
        ei[i] = (float) pole.imag();
    }

    /// The poles of group g: the exact ones times `factor` (1: exact; < 1: extra damping per sample).
    void dampGroup(int g, float factor) noexcept
    {
        for (auto k = first(g); k < last(g); ++k) {
            pr[k] = er[k] * factor;
            pi[k] = ei[k] * factor;
        }
    }

    /// Sum of |a|^2 |u|^2 over group g (silence detection).
    float energy(int g) const noexcept
    {
        float s = 0.0f;
        for (auto k = first(g); k < last(g); ++k)
            s += (ur[k] * ur[k] + ui[k] * ui[k]) * (lr[k] * lr[k] + li[k] * li[k] + rr[k] * rr[k] + ri[k] * ri[k]);
        return s;
    }

    /// Runs group g through n samples driven by `input` (nullptr: no input, the resonators ring freely) and
    /// accumulates into left and right.
    void process(int g, const float* input, float* left, float* right, int n) noexcept
    {
        for (int start = 0; start < n; start += kMaxBlock) {
            const int m = std::min(kMaxBlock, n - start);
            renderBlock(g, input != nullptr ? input + start : nullptr, left + start, right + start, m);
        }
    }

private:
    static std::size_t roundUp(std::size_t n) noexcept { return (n + kLanes - 1) / kLanes * kLanes; }
    std::size_t first(int g) const noexcept { return (std::size_t) starts[(std::size_t) g]; }
    std::size_t last(int g) const noexcept { return (std::size_t) starts[(std::size_t) g + 1]; }

#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
    /// G consecutive lane groups from resonator o (one string), through n samples of one input.
    template <int G>
    void renderLanesAvx(std::size_t o, const float* input, float* accL, float* accR, int n) noexcept
    {
        __m256 xr[G], xi[G], qr[G], qi[G], b[G];
        for (int j = 0; j < G; ++j) {
            const std::size_t k = o + (std::size_t) j * kLanes;
            xr[j] = _mm256_loadu_ps(&ur[k]);
            xi[j] = _mm256_loadu_ps(&ui[k]);
            qr[j] = _mm256_loadu_ps(&pr[k]);
            qi[j] = _mm256_loadu_ps(&pi[k]);
            b[j] = _mm256_loadu_ps(&gain[k]);
        }
        const float* al = &lr[o];
        const float* bl = &li[o];
        const float* ar = &rr[o];
        const float* br = &ri[o];
        for (int s = 0; s < n; ++s) {
            float* oL = accL + (std::size_t) s * kLanes;
            float* oR = accR + (std::size_t) s * kLanes;
            const __m256 x = _mm256_set1_ps(input != nullptr ? input[s] : 0.0f);
            __m256 l = _mm256_loadu_ps(oL), r = _mm256_loadu_ps(oR);
            for (int j = 0; j < G; ++j) {
                const std::size_t k = (std::size_t) j * kLanes;
                l = _mm256_fnmadd_ps(_mm256_loadu_ps(bl + k), xi[j], _mm256_fmadd_ps(_mm256_loadu_ps(al + k), xr[j], l));
                r = _mm256_fnmadd_ps(_mm256_loadu_ps(br + k), xi[j], _mm256_fmadd_ps(_mm256_loadu_ps(ar + k), xr[j], r));
                // u <- p u + b x
                const __m256 nr = _mm256_fmadd_ps(b[j], x, _mm256_fmsub_ps(xr[j], qr[j], _mm256_mul_ps(xi[j], qi[j])));
                xi[j] = _mm256_fmadd_ps(xr[j], qi[j], _mm256_mul_ps(xi[j], qr[j]));
                xr[j] = nr;
            }
            _mm256_storeu_ps(oL, l);
            _mm256_storeu_ps(oR, r);
        }
        for (int j = 0; j < G; ++j) {
            const std::size_t k = o + (std::size_t) j * kLanes;
            _mm256_storeu_ps(&ur[k], xr[j]);
            _mm256_storeu_ps(&ui[k], xi[j]);
        }
    }
#endif

    void renderBlock(int g, const float* input, float* left, float* right, int n) noexcept
    {
        float* accL = acc.data();
        float* accR = acc.data() + (std::size_t) kMaxBlock * kLanes;
        std::fill(accL, accL + (std::size_t) n * kLanes, 0.0f);
        std::fill(accR, accR + (std::size_t) n * kLanes, 0.0f);
        const std::size_t end = last(g);
        std::size_t o = first(g);
#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
        for (; o + 2 * kLanes <= end; o += 2 * kLanes)
            renderLanesAvx<2>(o, input, accL, accR, n);
        for (; o + kLanes <= end; o += kLanes)
            renderLanesAvx<1>(o, input, accL, accR, n);
#else
        for (; o + kLanes <= end; o += kLanes) {
            float xr[kLanes], xi[kLanes], qr[kLanes], qi[kLanes];
            const float* b = &gain[o];
            const float* al = &lr[o];
            const float* bl = &li[o];
            const float* ar = &rr[o];
            const float* br = &ri[o];
            for (int j = 0; j < kLanes; ++j) {
                xr[j] = ur[o + j];
                xi[j] = ui[o + j];
                qr[j] = pr[o + j];
                qi[j] = pi[o + j];
            }
            for (int s = 0; s < n; ++s) {
                float* oL = accL + (std::size_t) s * kLanes;
                float* oR = accR + (std::size_t) s * kLanes;
                const float x = input != nullptr ? input[s] : 0.0f;
                for (int j = 0; j < kLanes; ++j) {
                    oL[j] += al[j] * xr[j] - bl[j] * xi[j];
                    oR[j] += ar[j] * xr[j] - br[j] * xi[j];
                    const float nr = xr[j] * qr[j] - xi[j] * qi[j] + b[j] * x;
                    const float ni = xr[j] * qi[j] + xi[j] * qr[j];
                    xr[j] = nr;
                    xi[j] = ni;
                }
            }
            for (int j = 0; j < kLanes; ++j) {
                ur[o + j] = xr[j];
                ui[o + j] = xi[j];
            }
        }
#endif
        for (int s = 0; s < n; ++s) {
            float l = 0.0f, r = 0.0f;
            for (int j = 0; j < kLanes; ++j) {
                l += accL[(std::size_t) s * kLanes + j];
                r += accR[(std::size_t) s * kLanes + j];
            }
            left[s] += l;
            right[s] += r;
        }
    }

    // Structure of arrays; er, ei: the exact poles; gain: input gain per sample. Padding lanes stay silent.
    std::vector<float> ur, ui, pr, pi, er, ei, gain, lr, li, rr, ri;
    std::vector<float> acc;
    std::vector<int> starts; // group g holds [starts[g], starts[g + 1]); the last entry grows while a group is open
    int groupLimit = 0, count = 0;
};

/// Pole per sample of a mode: exp((-decay + 2 pi i freq) / sampleRate).
inline std::complex<double> modePole(double freq, double decay, double sampleRate) noexcept
{
    constexpr double twoPi = 6.283185307179586;
    return std::exp(std::complex<double>(-decay, twoPi * freq) / sampleRate);
}

} // namespace pf
