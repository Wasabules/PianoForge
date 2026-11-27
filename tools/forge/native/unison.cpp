// SPDX-License-Identifier: AGPL-3.0-or-later

// The three-string unison's fit (forge/unison.py, fit_partial): the bridge admittance Y for which three strings
// with their own poles s_i give the measured poles, with one damping common to the strings. In Python each try was
// scipy's least squares over numpy polynomials and their roots, thousands of small calls per layer; here the same
// least squares (Levenberg-Marquardt, a forward-difference Jacobian, the same six starting points) runs on a closed
// form: the s_i are the roots of p = q + Y q' + Y^2 q'' + Y^3 q''' (q = prod (lambda - pole_k)), a cubic solved by
// Cardano and polished by Newton. Loaded by forge/native.py; unison.py keeps the reference and the checks.

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#if defined(_WIN32)
#define PF_EXPORT extern "C" __declspec(dllexport)
#else
#define PF_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using cplx = std::complex<double>;

/// The three roots of x^3 + a x^2 + b x + c (complex coefficients).
void cubicRoots(cplx a, cplx b, cplx c, cplx roots[3])
{
    const cplx shift = a / 3.0;
    const cplx p = b - a * a / 3.0;
    const cplx q = 2.0 * a * a * a / 27.0 - a * b / 3.0 + c;
    const cplx disc = std::sqrt(q * q / 4.0 + p * p * p / 27.0);
    cplx u = -q / 2.0 + disc;
    if (std::abs(-q / 2.0 - disc) > std::abs(u))
        u = -q / 2.0 - disc; // the larger: no cancellation
    const cplx C = std::abs(u) > 0.0 ? std::pow(u, 1.0 / 3.0) : cplx(0.0, 0.0);
    const cplx w(-0.5, std::sqrt(3.0) / 2.0);
    cplx k = C;
    for (int i = 0; i < 3; ++i) {
        const cplx t = std::abs(k) > 0.0 ? k - p / (3.0 * k) : cplx(0.0, 0.0);
        cplx x = t - shift;
        for (int it = 0; it < 3; ++it) { // Newton: the roots to working precision
            const cplx f = ((x + a) * x + b) * x + c;
            const cplx df = (3.0 * x + 2.0 * a) * x + b;
            if (std::abs(df) == 0.0)
                break;
            x -= f / df;
        }
        roots[i] = x;
        k *= w;
    }
}

/// unison.py _strings_for for three poles: the s_i for which diag(s) + Y J has the eigenvalues `poles`.
void stringsFor(const cplx poles[3], cplx Y, cplx s[3])
{
    // q = (x - r0)(x - r1)(x - r2) = x^3 + q1 x^2 + q2 x + q3
    const cplx q1 = -(poles[0] + poles[1] + poles[2]);
    const cplx q2 = poles[0] * poles[1] + poles[0] * poles[2] + poles[1] * poles[2];
    const cplx q3 = -poles[0] * poles[1] * poles[2];
    // p = q + Y q' + Y^2 q'' + Y^3 q''' (as unison.py builds it: the derivatives as they are)
    const cplx p2 = q1 + 3.0 * Y;
    const cplx p1 = q2 + 2.0 * q1 * Y + 6.0 * Y * Y;
    const cplx p0 = q3 + q2 * Y + 2.0 * q1 * Y * Y + 6.0 * Y * Y * Y;
    cubicRoots(p2, p1, p0, s);
}

/// The residuals of unison.py's fit: (Re s_i - mean Re s) / scale.
void residual(const cplx rel[3], double scale, const double y[2], double r[3])
{
    cplx s[3];
    stringsFor(rel, cplx(y[0], y[1]) * scale, s);
    const double mean = (s[0].real() + s[1].real() + s[2].real()) / 3.0;
    for (int i = 0; i < 3; ++i)
        r[i] = (s[i].real() - mean) / scale;
}

double cost(const double r[3]) { return 0.5 * (r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); }

/// Levenberg-Marquardt from one start: the best y and its cost.
double minimise(const cplx rel[3], double scale, double y[2])
{
    const double eps = std::sqrt(std::numeric_limits<double>::epsilon());
    double r[3], rt[3], J[3][2];
    residual(rel, scale, y, r);
    double c = cost(r);
    double mu = 1e-3;
    int nfev = 1;
    for (int iter = 0; iter < 200 && nfev < 600; ++iter) {
        for (int j = 0; j < 2; ++j) { // forward differences, as scipy's '2-point'
            double yh[2] = { y[0], y[1] };
            const double h = eps * std::max(1.0, std::abs(y[j]));
            yh[j] += h;
            residual(rel, scale, yh, rt);
            ++nfev;
            for (int i = 0; i < 3; ++i)
                J[i][j] = (rt[i] - r[i]) / h;
        }
        double a00 = 0, a01 = 0, a11 = 0, g0 = 0, g1 = 0;
        for (int i = 0; i < 3; ++i) {
            a00 += J[i][0] * J[i][0];
            a01 += J[i][0] * J[i][1];
            a11 += J[i][1] * J[i][1];
            g0 += J[i][0] * r[i];
            g1 += J[i][1] * r[i];
        }
        if (std::max(std::abs(g0), std::abs(g1)) <= 1e-15)
            break;
        bool accepted = false;
        while (!accepted && nfev < 600) {
            const double b00 = a00 * (1.0 + mu) + 1e-300, b11 = a11 * (1.0 + mu) + 1e-300;
            const double det = b00 * b11 - a01 * a01;
            if (!(std::abs(det) > 0.0) || !std::isfinite(det)) {
                mu *= 10.0;
                if (mu > 1e20)
                    return c;
                continue;
            }
            const double d0 = (-g0 * b11 + g1 * a01) / det, d1 = (-g1 * b00 + g0 * a01) / det;
            const double yt[2] = { y[0] + d0, y[1] + d1 };
            residual(rel, scale, yt, rt);
            ++nfev;
            const double ct = cost(rt);
            if (std::isfinite(ct) && ct < c) {
                const double gain = c - ct;
                y[0] = yt[0];
                y[1] = yt[1];
                std::copy(rt, rt + 3, r);
                c = ct;
                mu = std::max(mu / 3.0, 1e-12);
                accepted = true;
                const double step = std::sqrt(d0 * d0 + d1 * d1), size = std::sqrt(y[0] * y[0] + y[1] * y[1]);
                if (gain <= 1e-14 * (c + gain) || step <= 1e-12 * (1e-12 + size))
                    return c;
            } else {
                mu *= 10.0;
                const double step = std::sqrt(d0 * d0 + d1 * d1), size = std::sqrt(y[0] * y[0] + y[1] * y[1]);
                if (mu > 1e20 || step <= 1e-12 * (1e-12 + size))
                    return c;
            }
        }
        if (!accepted)
            break;
    }
    return c;
}

} // namespace

/// rel: three poles (re, im interleaved) against their mean; scale: their spread. From unison.py's six starting
/// points, writes each fit's y (Y = (y0 + i y1) scale; 6 x 2) and cost (0.5 |r|^2; 6, NaN when not finite): the
/// problem may have several exact solutions, of which unison.py keeps the first physical one by cost.
PF_EXPORT void pf_unison3_fits(const double* rel_in, double scale, double* y_out, double* cost_out)
{
    const cplx rel[3] = { { rel_in[0], rel_in[1] }, { rel_in[2], rel_in[3] }, { rel_in[4], rel_in[5] } };
    int i = 0;
    for (const double g0 : { -0.3, -1.0, -3.0 })
        for (const double b0 : { 0.0, 0.3 }) {
            double y[2] = { g0, b0 };
            const double c = minimise(rel, scale, y);
            y_out[2 * i] = y[0];
            y_out[2 * i + 1] = y[1];
            cost_out[i] = std::isfinite(c) ? c : std::numeric_limits<double>::quiet_NaN();
            ++i;
        }
}
