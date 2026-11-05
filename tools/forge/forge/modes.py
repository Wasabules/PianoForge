"""High-resolution analysis of the modes inside one partial.

A piano partial is not one damped sinusoid. The 1 to 3 strings of a unison, each with two polarisations, are
coupled through the bridge: a partial is the sum of up to six damped sinusoids, a fraction of a hertz to a few
hertz apart. Their sum is what the ear hears as beating, and as the double decay (prompt sound, after-sound).
A two-slope envelope fit, as in partials.py, only describes the result; this module measures the modes.

Method:
  1. The partial is brought to baseband in the frequency domain: the band around it is cut out of one long FFT
     with a flat passband and a raised-cosine edge, then inverse-transformed at a low rate. This is a zero-phase
     filter and a decimation in one step; inside the passband, amplitudes and phases are untouched.
  2. The decay is flattened by an exponential weight (a sum of exponentials stays a sum of exponentials, with
     every pole scaled by the same factor), so that the quiet after-sound weighs as much as the loud start.
  3. ESPRIT estimates the poles shared by both channels, then variable projection refines them (Levenberg-
     Marquardt on the poles, exact least squares for each mode's complex amplitude per channel). The order is the
     smallest that reproduces the partial's envelope, beating included (see `_fit_orders`).

Conventions: time 0 is the note-on; a mode is x_c(t) = Re(a_c exp((-decay + 2 pi i freq) t)).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


@dataclass
class Mode:
    freq: float               # Hz
    decay: float              # sigma (1/s): the amplitude falls as exp(-sigma t); T60 = 6.91 / sigma
    amps: list[complex]       # complex amplitude per channel at t = 0

    @property
    def level(self) -> float:
        return float(np.sqrt(np.mean(np.abs(np.asarray(self.amps)) ** 2)))


@dataclass
class PartialModes:
    n: int                    # partial number
    centre: float             # Hz, the frequency it was demodulated at
    modes: list[Mode] = field(default_factory=list)
    t0: float = 0.0           # analysis window (s after the note-on)
    t1: float = 0.0
    residual_db: float = 0.0  # weighted energy left by the model over the window, against the partial's
    orders_db: list = field(default_factory=list)  # (order, weighted residual dB, envelope error dB) per try
    noise_db: float = 0.0     # noise floor in the partial's band, against the partial's maximum
    envelope_error_db: float = 0.0  # RMS dB error of the model's envelope over the window (beating shows here)


class BandExtractor:
    """One FFT of a multichannel signal; any band can then be cut out and brought to baseband cheaply."""

    def __init__(self, x: np.ndarray, fs: float):
        x = np.atleast_2d(np.asarray(x, dtype=np.float64))
        self.fs, self.length = float(fs), x.shape[1]
        self.nfft = 1 << int(np.ceil(np.log2(2 * self.length)))  # zero padding: no circular wrap of the signal
        self.spectrum = np.fft.rfft(x, self.nfft, axis=1)
        self.df = self.fs / self.nfft

    def baseband(self, centre: float, pass_hz: float, stop_hz: float) -> tuple[np.ndarray, float]:
        """Complex signal z_c(t) with x_c(t) = Re(z_c(t) exp(2 pi i centre t)) for the band's content; flat for
        |f - centre| <= pass_hz, zero beyond stop_hz. Returns (z, fs_d); z[:, m] is at t = m / fs_d."""
        j = int(np.ceil(stop_hz / self.df))
        m_len = 1 << int(np.ceil(np.log2(2 * j + 2)))
        k_c = int(round(centre / self.df))
        ks = np.arange(k_c - j, k_c + j + 1)
        ks = ks[(ks >= 0) & (ks < self.spectrum.shape[1])]
        offset = ks * self.df - centre
        mask = np.ones_like(offset)
        edge = (np.abs(offset) > pass_hz)
        mask[edge] = 0.5 * (1 + np.cos(np.pi * (np.abs(offset[edge]) - pass_hz) / (stop_hz - pass_hz)))
        mask[np.abs(offset) >= stop_hz] = 0.0
        spec = np.zeros((self.spectrum.shape[0], m_len), dtype=np.complex128)
        spec[:, (ks - k_c) % m_len] = self.spectrum[:, ks] * mask
        z = np.fft.ifft(spec, axis=1) * (2.0 * m_len / self.nfft)
        fs_d = m_len * self.df
        t = np.arange(m_len) / fs_d
        z *= np.exp(2j * np.pi * (k_c * self.df - centre) * t)  # from the bin grid to the exact centre
        valid = int(self.length / self.fs * fs_d)
        return z[:, :valid], fs_d


def envelope_error_db(z: np.ndarray, model: np.ndarray, fs_d: float, smooth_s: float = 0.05) -> float:
    """RMS difference in dB between the envelopes of a measured baseband signal and its model (channels x samples
    for z, samples x channels for model), smoothed over `smooth_s`: phase-insensitive, and sensitive to beating."""
    k = max(1, int(round(smooth_s * fs_d)))
    kernel = np.ones(k) / k
    errors = []
    for c in range(z.shape[0]):
        a = np.convolve(np.abs(z[c]), kernel, mode="valid")
        b = np.convolve(np.abs(model[:, c]), kernel, mode="valid")
        errors.append(np.mean((20 * np.log10(np.maximum(a, 1e-30) / np.maximum(b, 1e-30))) ** 2))
    return float(np.sqrt(np.mean(errors)))


def _hankel_cov(z: np.ndarray, rows: int) -> np.ndarray:
    """R = H H^H for the Hankel matrices of all channels stacked side by side (shared signal subspace)."""
    r = np.zeros((rows, rows), dtype=np.complex128)
    for zc in z:
        cols = len(zc) - rows + 1
        h = np.lib.stride_tricks.sliding_window_view(zc, cols)  # rows x cols, h[i, j] = zc[i + j]
        r += h @ h.conj().T
    return r


def esprit_poles(z: np.ndarray, order: int, rows: int | None = None, basis: np.ndarray | None = None) -> np.ndarray:
    """Poles (per sample) of the `order` strongest damped exponentials in z (channels x samples)."""
    if basis is None:
        n = z.shape[1]
        rows = rows or max(order + 1, min(n // 3, 200))
        w, u = np.linalg.eigh(_hankel_cov(z, rows))
        basis = u[:, ::-1]
    us = basis[:, :order]
    phi, *_ = np.linalg.lstsq(us[:-1], us[1:], rcond=None)
    return np.linalg.eigvals(phi)


def fit_amplitudes(z: np.ndarray, poles: np.ndarray, m0: int = 0) -> tuple[np.ndarray, float]:
    """Least squares complex amplitudes (channels x modes) at sample m0 and the relative residual energy."""
    m = np.arange(z.shape[1])
    v = poles[None, :] ** m[:, None]
    amps, *_ = np.linalg.lstsq(v, z.T, rcond=None)
    model = (v @ amps).T
    energy = float(np.sum(np.abs(z) ** 2)) + 1e-300
    return amps.T, float(np.sum(np.abs(z - model) ** 2)) / energy


def _exponents(poles: np.ndarray, fs_d: float) -> tuple[np.ndarray, np.ndarray]:
    """(decay sigma in 1/s, offset in Hz) of poles per sample; decays are kept positive (a string only loses energy)."""
    decay = np.maximum(-np.log(np.maximum(np.abs(poles), 1e-300)) * fs_d, 1e-3)
    return decay, np.angle(poles) * fs_d / (2 * np.pi)


def _refine(seg: np.ndarray, weight: np.ndarray, fs_d: float, decay: np.ndarray, offset: np.ndarray):
    """Variable projection: the poles are optimised by Levenberg-Marquardt, the amplitudes solved exactly at each
    step (weighted least squares). Decays are parametrised by their logarithm, so they stay positive.
    Returns (decay, offset, amplitudes at the window start (channels x modes), weighted relative residual)."""
    from scipy.optimize import least_squares

    m = np.arange(seg.shape[1], dtype=np.float64)
    target = (seg * weight).T
    k = len(decay)

    def basis(theta):
        s = (-np.exp(np.clip(theta[:k], -12.0, 8.0)) + 2j * np.pi * theta[k:]) / fs_d
        return np.exp(np.outer(m, s)) * weight[:, None]

    def residual(theta):
        vw = basis(theta)
        amps, *_ = np.linalg.lstsq(vw, target, rcond=None)
        e = (target - vw @ amps).ravel()
        return np.concatenate([e.real, e.imag])

    theta0 = np.concatenate([np.log(np.maximum(decay, 1e-5)), offset])
    try:
        theta = least_squares(residual, theta0, method="lm", max_nfev=200).x
    except (ValueError, np.linalg.LinAlgError):
        theta = theta0
    vw = basis(theta)
    amps, *_ = np.linalg.lstsq(vw, target, rcond=None)
    res = float(np.sum(np.abs(target - vw @ amps) ** 2)) / (float(np.sum(np.abs(target) ** 2)) + 1e-300)
    return np.exp(np.clip(theta[:k], -12.0, 8.0)), theta[k:], amps.T, res


def _fit_orders(seg: np.ndarray, weight: np.ndarray, fs_d: float, max_order: int, pass_hz: float,
                tolerance_db: float, good_db: float):
    """Fits 1 to `max_order` modes and keeps the smallest order whose envelope error is within `tolerance_db` of
    the best one. Each order starts from ESPRIT (on the weighted signal) and from the previous order plus one mode,
    and keeps the better refinement. Fits with modes outside the passband (a neighbour's), or with strong modes
    cancelling each other (ill-conditioned), are refused. Stops early once the envelope error is under `good_db`."""
    flat = seg * weight
    rows = max(max_order + 1, min(flat.shape[1] // 3, 200))
    w, u = np.linalg.eigh(_hankel_cov(flat, rows))
    basis = u[:, ::-1]
    alpha = float(np.log(weight[1] / weight[0])) if len(weight) > 1 else 0.0
    peak = float(np.max(np.abs(seg)))
    m = np.arange(seg.shape[1])
    results, previous = [], None
    for order in range(1, max_order + 1):
        starts = [_exponents(esprit_poles(flat, order, basis=basis) * np.exp(-alpha), fs_d)]
        if previous is not None:  # warm start: the previous modes, plus one at the strongest remaining frequency
            model = (np.exp(np.outer(m, (-previous[0] + 2j * np.pi * previous[1]) / fs_d)) @ previous[2].T).T
            spec = np.abs(np.fft.fft((seg - model) * weight, 8 * seg.shape[1], axis=1)).sum(axis=0)
            freqs = np.fft.fftfreq(len(spec), 1 / fs_d)
            inside = np.abs(freqs) <= pass_hz
            extra = freqs[inside][np.argmax(spec[inside])]
            starts.append((np.append(previous[0], 0.5), np.append(previous[1], extra)))
        best = None
        for decay, offset in starts:
            fit = _refine(seg, weight, fs_d, decay, offset)
            if best is None or fit[3] < best[3]:
                best = fit
        decay, offset, amps, res = best
        model = np.exp(np.outer(m, (-decay + 2j * np.pi * offset) / fs_d)) @ amps.T
        level = np.sqrt(np.mean(np.abs(amps) ** 2, axis=0))
        ok = bool(np.all(np.abs(offset) <= pass_hz)) and bool(np.max(level) < 4.0 * peak)
        error = envelope_error_db(seg, model, fs_d)
        results.append((order, decay, offset, amps, res, error, ok))
        previous = (decay, offset, amps)
        if ok and error < good_db:
            break
    acceptable = [r for r in results if r[6]] or results[:1]
    best_error = min(r[5] for r in acceptable)
    chosen = next(r for r in acceptable if r[5] <= best_error + tolerance_db)
    return chosen, [(r[0], round(10 * np.log10(max(r[4], 1e-30)), 1), round(r[5], 2)) for r in results]


def analyze_partial(extractor: BandExtractor, n: int, centre: float, spacing: float, *, t_start: float = 0.08,
                    t_end: float | None = None, max_order: int = 6, dynamic_db: float = 50.0, snr_db: float = 15.0,
                    audible_floor: float = 0.0, flatten: float = 0.5, tolerance_db: float = 0.5,
                    good_db: float = 0.7) -> PartialModes | None:
    """Modes of one partial. `spacing` is the distance to the neighbouring partials (about f0); the passband is
    a third of it, at most 30 Hz. The window runs from `t_start` (after the attack and the filter's smear) to the
    end of the hold, and stops earlier when the partial comes within `snr_db` of the noise floor (measured in an
    equal band halfway to the next partial), falls `dynamic_db` under its maximum, or under `audible_floor` (an
    absolute amplitude, typically the note's loudest partial minus 80 dB). Sample libraries are often denoised:
    partials then vanish abruptly near the floor, and the noise left around them is not a decay to be modelled.
    `flatten` (0..1) is the share of the overall decay compensated by the weight of the fit."""
    pass_hz = min(0.33 * spacing, 30.0)
    stop_hz = min(0.66 * spacing, 2 * pass_hz)
    z, fs_d = extractor.baseband(centre, pass_hz, stop_hz)
    smear = 1.5 / (stop_hz - pass_hz)
    i0 = int(np.ceil(max(t_start, smear) * fs_d))
    i1 = z.shape[1] - int(np.ceil(smear * fs_d))
    if t_end is not None:
        i1 = min(i1, int(t_end * fs_d))
    if i1 - i0 < 24:
        return None
    env = np.sqrt(np.mean(np.abs(z[:, i0:i1]) ** 2, axis=0))
    smooth = np.convolve(env, np.ones(5) / 5, mode="same")
    gap, _ = extractor.baseband(centre + 0.5 * spacing, pass_hz, stop_hz)
    gap_env = np.sqrt(np.mean(np.abs(gap[:, i0:i1]) ** 2, axis=0))
    noise = float(np.median(gap_env[len(gap_env) // 2:]))
    floor = max(smooth.max() * 10 ** (-dynamic_db / 20), noise * 10 ** (snr_db / 20), audible_floor)
    above = np.flatnonzero(smooth > floor)
    if len(above) == 0:
        return None
    i1 = i0 + int(above[-1]) + 1
    if i1 - i0 < 24:
        return None
    seg = z[:, i0:i1]
    # Weight: compensates part of the overall decay (fitted on the log envelope), so that the after-sound counts.
    t = np.arange(i1 - i0)
    slope = np.polyfit(t, np.log(np.maximum(env[: i1 - i0], 1e-30)), 1)[0]
    weight = np.exp(-flatten * min(slope, 0.0) * t)
    (order, decay, offset, amps, res, error, _), orders = _fit_orders(seg, weight, fs_d, max_order, pass_hz,
                                                                      tolerance_db, good_db)
    # A decay much slower than the window is long is not measured, only extrapolated: T60 is capped at ten times
    # the window, and the amplitudes are solved again for the capped poles.
    min_decay = 6.91 / (10.0 * (i1 - i0) / fs_d)
    if np.any(decay < min_decay):
        decay = np.maximum(decay, min_decay)
        vw = np.exp(np.outer(t, (-decay + 2j * np.pi * offset) / fs_d)) * weight[:, None]
        amps = np.linalg.lstsq(vw, (seg * weight).T, rcond=None)[0].T
    amps0 = amps * np.exp((decay - 2j * np.pi * offset) * i0 / fs_d)[None, :]  # from the window start to t = 0
    out = PartialModes(n, centre, t0=i0 / fs_d, t1=i1 / fs_d, residual_db=10 * np.log10(max(res, 1e-30)),
                       orders_db=orders, envelope_error_db=error)
    out.noise_db = 20 * np.log10(max(noise, 1e-30) / max(smooth.max(), 1e-30))
    for k in range(len(decay)):
        out.modes.append(Mode(float(centre + offset[k]), float(decay[k]), [complex(a) for a in amps0[:, k]]))
    out.modes.sort(key=lambda mode: -mode.level)
    return out


def synthesize(partials: list[PartialModes], channels: int, length: int, fs: float,
               start: int = 0) -> np.ndarray:
    """Sum of every mode, from sample `start` (t = start / fs) over `length` samples (channels x length)."""
    t = (start + np.arange(length)) / fs
    out = np.zeros((channels, length))
    for p in partials:
        for m in p.modes:
            carrier = np.exp((-m.decay + 2j * np.pi * m.freq) * t)
            for c in range(channels):
                out[c] += np.real(m.amps[c] * carrier)
    return out
