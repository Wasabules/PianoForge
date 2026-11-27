// SPDX-License-Identifier: AGPL-3.0-or-later

// The forge's variable projection, native (forge/modes.py, _refine): the poles of k damped exponentials shared by
// several channels, optimised by Levenberg-Marquardt, the complex amplitudes solved exactly at each step. The same
// problem as the Python version, with two differences that make it 10 to 50 times faster: the Jacobian is Kaufman's
// (analytic, one evaluation per step instead of one per parameter), and the basis of exponentials is built by
// recurrence (anchored on the exact exponential every 256 samples). Loaded by forge/native.py (ctypes); the Python
// version stays the reference and the fallback (tests/test_native.py compares both).
//
// Parameters, as in Python: theta = (log decay_1..k, offset_1..k Hz), the log decays clipped to [-12, 8] (decays
// stay positive); pole_j = exp((-decay_j + 2 pi i offset_j) / fs) per sample; the basis is weighted per sample.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#if defined(_WIN32)
#define PF_EXPORT extern "C" __declspec(dllexport)
#else
#define PF_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using cplx = std::complex<double>;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kLogMin = -12.0, kLogMax = 8.0;
constexpr double kXtol = 1e-8, kFtol = 1e-8;
constexpr double kRankCutoff = 1e-12; // a column this small after projection on the others is left out (lstsq)

struct Problem {
    int channels = 0, n = 0, k = 0;
    const double* weight = nullptr;
    double fs = 1.0;
    std::vector<cplx> target; // n x channels, row m holds the channels (seg * weight, transposed)
    double energy = 0.0;      // |target|^2
};

/// One evaluation: the basis, its thin QR, the amplitudes and the residual.
struct Evaluation {
    std::vector<cplx> basis; // column-major, n x k
    std::vector<cplx> q;     // column-major, n x k (zero for a column left out)
    std::vector<cplx> r;     // column-major, k x k, upper triangular
    std::vector<char> kept;  // per column
    std::vector<cplx> amps;  // k x channels
    std::vector<cplx> resid; // n x channels
    double cost = 0.0;       // 0.5 |resid|^2
};

double logDecay(double theta) { return std::clamp(theta, kLogMin, kLogMax); }

void buildBasis(const Problem& p, const double* theta, std::vector<cplx>& basis)
{
    basis.resize((std::size_t) p.n * p.k);
    for (int j = 0; j < p.k; ++j) {
        const double decay = std::exp(logDecay(theta[j]));
        const cplx s(-decay / p.fs, kTwoPi * theta[p.k + j] / p.fs);
        const cplx step = std::exp(s);
        cplx* col = basis.data() + (std::size_t) j * p.n;
        cplx power(1.0, 0.0);
        for (int m = 0; m < p.n; ++m) {
            if ((m & 255) == 0)
                power = std::exp(s * (double) m);
            col[m] = power * p.weight[m];
            power *= step;
        }
    }
}

/// q_i^H v
cplx dot(const cplx* a, const cplx* b, int n)
{
    cplx sum(0.0, 0.0);
    for (int m = 0; m < n; ++m)
        sum += std::conj(a[m]) * b[m];
    return sum;
}

double norm2(const cplx* a, int n)
{
    double sum = 0.0;
    for (int m = 0; m < n; ++m)
        sum += std::norm(a[m]);
    return sum;
}

/// Thin QR by Gram-Schmidt run twice (orthogonal to working precision for these few columns).
void factor(const Problem& p, Evaluation& e)
{
    const int n = p.n, k = p.k;
    e.q.assign((std::size_t) n * k, cplx(0.0, 0.0));
    e.r.assign((std::size_t) k * k, cplx(0.0, 0.0));
    e.kept.assign(k, 0);
    std::vector<cplx> v(n);
    for (int j = 0; j < k; ++j) {
        std::copy_n(e.basis.data() + (std::size_t) j * n, n, v.data());
        const double original = std::sqrt(norm2(v.data(), n));
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < j; ++i) {
                if (!e.kept[i])
                    continue;
                const cplx* qi = e.q.data() + (std::size_t) i * n;
                const cplx h = dot(qi, v.data(), n);
                e.r[(std::size_t) j * k + i] += h;
                for (int m = 0; m < n; ++m)
                    v[m] -= h * qi[m];
            }
        }
        const double left = std::sqrt(norm2(v.data(), n));
        if (!(left > kRankCutoff * std::max(original, 1e-300)))
            continue; // dependent on the others (or empty): left out, amplitude 0
        e.kept[j] = 1;
        e.r[(std::size_t) j * k + j] = left;
        cplx* qj = e.q.data() + (std::size_t) j * n;
        for (int m = 0; m < n; ++m)
            qj[m] = v[m] / left;
    }
}

/// v -= Q Q^H v (projection on the orthogonal complement of the basis)
void projectOut(const Problem& p, const Evaluation& e, cplx* v)
{
    for (int i = 0; i < p.k; ++i) {
        if (!e.kept[i])
            continue;
        const cplx* qi = e.q.data() + (std::size_t) i * p.n;
        const cplx h = dot(qi, v, p.n);
        for (int m = 0; m < p.n; ++m)
            v[m] -= h * qi[m];
    }
}

bool evaluate(const Problem& p, const double* theta, Evaluation& e)
{
    const int n = p.n, k = p.k, c = p.channels;
    buildBasis(p, theta, e.basis);
    factor(p, e);
    e.amps.assign((std::size_t) k * c, cplx(0.0, 0.0));
    e.resid.assign((std::size_t) n * c, cplx(0.0, 0.0));
    std::vector<cplx> column(n), qht(k);
    double cost = 0.0;
    for (int ch = 0; ch < c; ++ch) {
        for (int m = 0; m < n; ++m)
            column[m] = p.target[(std::size_t) m * c + ch];
        for (int i = 0; i < k; ++i)
            qht[i] = e.kept[i] ? dot(e.q.data() + (std::size_t) i * n, column.data(), n) : cplx(0.0, 0.0);
        for (int i = k - 1; i >= 0; --i) { // R a = Q^H t, over the columns kept
            if (!e.kept[i])
                continue;
            cplx sum = qht[i];
            for (int j = i + 1; j < k; ++j)
                if (e.kept[j])
                    sum -= e.r[(std::size_t) j * k + i] * e.amps[(std::size_t) j * c + ch];
            e.amps[(std::size_t) i * c + ch] = sum / e.r[(std::size_t) i * k + i];
        }
        projectOut(p, e, column.data());
        for (int m = 0; m < n; ++m) {
            e.resid[(std::size_t) m * c + ch] = column[m];
            cost += std::norm(column[m]);
        }
    }
    e.cost = 0.5 * cost;
    return std::isfinite(e.cost);
}

/// Kaufman's Jacobian of the real residual (re, im of every sample and channel): J_i = -P (dB/dtheta_i) a_j, P the
/// projection away from the basis. Returns J^T J (P x P) and J^T r.
void normalEquations(const Problem& p, const double* theta, const Evaluation& e, std::vector<double>& jtj,
                     std::vector<double>& jtr)
{
    const int n = p.n, k = p.k, c = p.channels, params = 2 * k;
    std::vector<std::vector<double>> columns(params, std::vector<double>((std::size_t) 2 * n * c, 0.0));
    std::vector<cplx> u(n);
    for (int i = 0; i < params; ++i) {
        const int j = i % k;
        if (!e.kept[j])
            continue;
        cplx factor;
        if (i < k) {
            if (theta[j] < kLogMin || theta[j] > kLogMax)
                continue; // clipped: no gradient
            factor = cplx(-std::exp(theta[j]) / p.fs, 0.0);
        } else {
            factor = cplx(0.0, kTwoPi / p.fs);
        }
        const cplx* b = e.basis.data() + (std::size_t) j * n;
        for (int ch = 0; ch < c; ++ch) {
            const cplx a = e.amps[(std::size_t) j * c + ch] * factor;
            for (int m = 0; m < n; ++m)
                u[m] = b[m] * (double) m * a;
            projectOut(p, e, u.data());
            double* col = columns[i].data();
            for (int m = 0; m < n; ++m) {
                const std::size_t at = 2 * ((std::size_t) m * c + ch);
                col[at] = -u[m].real();
                col[at + 1] = -u[m].imag();
            }
        }
    }
    const std::size_t rows = (std::size_t) 2 * n * c;
    jtj.assign((std::size_t) params * params, 0.0);
    jtr.assign(params, 0.0);
    const double* r = reinterpret_cast<const double*>(e.resid.data()); // re, im interleaved: the same layout
    for (int a = 0; a < params; ++a) {
        const double* ca = columns[a].data();
        double s = 0.0;
        for (std::size_t m = 0; m < rows; ++m)
            s += ca[m] * r[m];
        jtr[a] = s;
        for (int b = a; b < params; ++b) {
            const double* cb = columns[b].data();
            double t = 0.0;
            for (std::size_t m = 0; m < rows; ++m)
                t += ca[m] * cb[m];
            jtj[(std::size_t) a * params + b] = jtj[(std::size_t) b * params + a] = t;
        }
    }
}

/// Solves A x = b for a symmetric positive definite A (Cholesky); false when it is not.
bool cholesky(std::vector<double> a, std::vector<double> b, int n, std::vector<double>& x)
{
    for (int j = 0; j < n; ++j) {
        double d = a[(std::size_t) j * n + j];
        for (int k = 0; k < j; ++k)
            d -= a[(std::size_t) j * n + k] * a[(std::size_t) j * n + k];
        if (!(d > 0.0))
            return false;
        d = std::sqrt(d);
        a[(std::size_t) j * n + j] = d;
        for (int i = j + 1; i < n; ++i) {
            double s = a[(std::size_t) i * n + j];
            for (int k = 0; k < j; ++k)
                s -= a[(std::size_t) i * n + k] * a[(std::size_t) j * n + k];
            a[(std::size_t) i * n + j] = s / d;
        }
    }
    for (int i = 0; i < n; ++i) { // L y = b
        double s = b[i];
        for (int k = 0; k < i; ++k)
            s -= a[(std::size_t) i * n + k] * b[k];
        b[i] = s / a[(std::size_t) i * n + i];
    }
    x.assign(n, 0.0);
    for (int i = n - 1; i >= 0; --i) { // L^T x = y
        double s = b[i];
        for (int k = i + 1; k < n; ++k)
            s -= a[(std::size_t) k * n + i] * x[k];
        x[i] = s / a[(std::size_t) i * n + i];
    }
    return true;
}

double norm(const std::vector<double>& v)
{
    double s = 0.0;
    for (double x : v)
        s += x * x;
    return std::sqrt(s);
}

} // namespace

PF_EXPORT int pf_native_version() { return 5; } // 2: resonators; 3: damped sums; 4-5: three-string unison

/// seg: channels x n complex (re, im interleaved), the partial at baseband over its window; weight: n; theta0: 2k
/// (log decays, offsets in Hz). Writes theta (2k) and the amplitudes at the window start (channels x k complex).
/// Returns the weighted relative residual |target - basis amps|^2 / |target|^2, or -1 when nothing finite came out.
PF_EXPORT double pf_vp_refine(const double* seg, int channels, int n, const double* weight, double fs, int k,
                              const double* theta0, int max_nfev, double* theta_out, double* amps_out)
{
    if (channels <= 0 || n <= 0 || k <= 0 || fs <= 0.0)
        return -1.0;
    Problem p;
    p.channels = channels;
    p.n = n;
    p.k = k;
    p.weight = weight;
    p.fs = fs;
    p.target.resize((std::size_t) n * channels);
    const cplx* s = reinterpret_cast<const cplx*>(seg);
    for (int ch = 0; ch < channels; ++ch)
        for (int m = 0; m < n; ++m) {
            const cplx t = s[(std::size_t) ch * n + m] * weight[m];
            p.target[(std::size_t) m * channels + ch] = t;
            p.energy += std::norm(t);
        }

    const int params = 2 * k;
    std::vector<double> theta(theta0, theta0 + params), trial(params), step, jtj, jtr;
    Evaluation now, next;
    if (!evaluate(p, theta.data(), now))
        return -1.0;
    int nfev = 1;
    double mu = 1e-3, nu = 2.0;
    bool done = false;
    while (!done && nfev < max_nfev) {
        normalEquations(p, theta.data(), now, jtj, jtr);
        std::vector<double> diag(params);
        for (int i = 0; i < params; ++i)
            diag[i] = std::max(jtj[(std::size_t) i * params + i], 1e-300);
        bool accepted = false;
        while (!accepted && nfev < max_nfev) {
            std::vector<double> a = jtj, minus(params);
            for (int i = 0; i < params; ++i) {
                a[(std::size_t) i * params + i] += mu * diag[i];
                minus[i] = -jtr[i];
            }
            if (!cholesky(a, minus, params, step)) {
                mu *= 10.0;
                if (mu > 1e30)
                    done = true;
                if (done)
                    break;
                continue;
            }
            for (int i = 0; i < params; ++i)
                trial[i] = theta[i] + step[i];
            const double stepNorm = norm(step), thetaNorm = norm(theta);
            if (!evaluate(p, trial.data(), next)) {
                ++nfev;
                mu *= nu;
                nu *= 2.0;
                continue;
            }
            ++nfev;
            // predicted reduction of the quadratic model: -(g.step + 0.5 step.H.step)
            double predicted = 0.0;
            for (int i = 0; i < params; ++i) {
                double hs = 0.0;
                for (int j = 0; j < params; ++j)
                    hs += jtj[(std::size_t) i * params + j] * step[j];
                predicted -= jtr[i] * step[i] + 0.5 * step[i] * hs;
            }
            const double actual = now.cost - next.cost;
            if (actual > 0.0) {
                const double rho = predicted > 0.0 ? actual / predicted : 1.0;
                mu *= std::max(1.0 / 3.0, 1.0 - std::pow(2.0 * rho - 1.0, 3.0));
                nu = 2.0;
                theta = trial;
                std::swap(now, next);
                accepted = true;
                if (actual <= kFtol * (now.cost + actual) || stepNorm <= kXtol * (kXtol + thetaNorm))
                    done = true;
            } else {
                mu *= nu;
                nu *= 2.0;
                if (stepNorm <= kXtol * (kXtol + thetaNorm))
                    done = true;
                if (done)
                    break;
            }
        }
        if (!accepted)
            break;
    }

    for (int i = 0; i < params; ++i)
        theta_out[i] = theta[i];
    cplx* amps = reinterpret_cast<cplx*>(amps_out);
    for (int ch = 0; ch < channels; ++ch)
        for (int j = 0; j < k; ++j)
            amps[(std::size_t) ch * k + j] = now.amps[(std::size_t) j * channels + ch];
    const double res = 2.0 * now.cost / (p.energy + 1e-300);
    return std::isfinite(res) ? res : -1.0;
}
