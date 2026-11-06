"""The coupled strings of a unison (Weinreich, 1977), fitted to the measured modes of each partial.

The 2 or 3 strings of a note are tuned almost alike and all pull on the bridge, which moves and so couples them.
Per partial, with one polarisation, the strings obey du/dt = A u with A = diag(s_i) + Y J: s_i = -sigma + 2 pi i f_i,
each string's own pole (a common intrinsic damping sigma, detuned frequencies f_i), Y the bridge's admittance seen
by the strings (its real part, negative, takes energy away: the bridge damps the motion where the strings move
together), J the matrix of ones (every string drives the bridge, the bridge drives every string). A hammer striking
the strings with weights h, the bridge summing them (weights 1), the partial is the sum over the eigenmodes k of
    r_k exp(lambda_k t),   r_k = (sum_i v_ki) (sum_i v_ki h_i) / (sum_i v_ki^2),   v_ki = 1 / (lambda_k - s_i)
(A is complex symmetric: its left eigenvectors are its right ones transposed). Struck alike, the strings mostly move
together: a fast decay (the prompt sound); their detuning leaks energy into the motions where they oppose, which
hardly move the bridge and die slowly (the after-sound), and beat against the first.

Fit: the poles of the N strongest measured modes of the partial (N = the strings of the note) are the eigenvalues
lambda_k; given Y, the s_i follow (the characteristic polynomial of A is p - Y p', p = prod (lambda - s_i), so p =
q + Y q' + Y^2 q'' + ..., q = prod (lambda - lambda_k)); Y is the one that gives the strings a common damping.
  - 2 strings: one condition for two unknowns. Y = -sqrt((lambda_1 - lambda_2)^2 / 4 + delta^2 / 4) for a string
    detuning delta (rad/s); the delta kept is the one that makes the bridge the most resistive (smallest imaginary
    part of Y) with a positive intrinsic damping.
  - 3 strings: two conditions, two unknowns (least squares from a few starting points). Kept when the strings'
    dampings agree within 20 % of the poles' spread, the bridge takes energy (Re Y < 0) and the damping is >= 0.
What is kept per partial: the indices of the N modes (by ascending frequency), the strings' detunings f_i - mean
(Hz) and Y. The engine plays the measured modes as they are, and moves them by what the model gives for another
strike (the soft pedal: the hammer misses one string) or another unison tuning (the detunings scaled).

Measured on Noire (layer v64, a third of the notes, first 10 partials; 2026-11-06): trichords solved for 30 of 117
partials exactly, intrinsic damping 1.2 /s and bridge 1.8 /s (medians), detuning spread 1.3 cents (0.5 to 5.8):
a well tuned unison.
"""

from __future__ import annotations

import numpy as np
from scipy.optimize import least_squares

# A concert grand's stringing: the lowest 10 keys have one string, the next 18 two, the top 60 three (Steinway D).
SINGLE_LAST, DOUBLE_LAST = 30, 48
MAX_SPREAD_TOLERANCE = 0.2   # 3 strings: the dampings' disagreement allowed, against the poles' spread
MAX_DETUNE_HZ = 8.0          # 2 strings: the detunings scanned (each string +- half of it)


def strings_of(midi: int) -> int:
    return 1 if midi <= SINGLE_LAST else 2 if midi <= DOUBLE_LAST else 3


def _strings_for(poles: np.ndarray, Y: complex) -> np.ndarray:
    """The s_i for which diag(s) + Y J has the eigenvalues `poles`."""
    q = np.poly(poles)
    p = np.zeros(len(q), dtype=complex)
    d, m = q, 0
    while len(d) > 0:
        p[len(q) - len(d):] += Y ** m * d
        d, m = np.polyder(d) if len(d) > 1 else np.array([]), m + 1
    return np.roots(p)


def _pick(modes: list, n: int) -> np.ndarray | None:
    """Indices of the n decaying modes carrying the most energy (|a|^2 / 2 sigma), by ascending frequency."""
    m = np.asarray(modes, dtype=np.float64)
    if len(m) < n:
        return None
    live = m[:, 1] > 0
    if np.count_nonzero(live) < n:
        return None
    energy = np.where(live, np.sum(m[:, 2:6] ** 2, axis=1) / (2 * np.maximum(m[:, 1], 1e-9)), -1.0)
    idx = np.argsort(energy)[::-1][:n]
    return idx[np.argsort(m[idx, 0])]


def fit_partial(modes: list, n: int) -> dict | None:
    """The unison model of one partial (rows [freq, decay, re, im, re, im]) for n strings, or None."""
    if n < 2:
        return None
    idx = _pick(modes, n)
    if idx is None:
        return None
    m = np.asarray(modes, dtype=np.float64)[idx]
    poles = -m[:, 1] + 2j * np.pi * m[:, 0]
    centre = np.mean(poles)
    rel = poles - centre
    if n == 2:
        D = rel[0] - rel[1]
        best = None
        for delta in np.linspace(0.0, 2 * np.pi * MAX_DETUNE_HZ, 801):
            Y = np.sqrt(D * D / 4 + delta * delta / 4)
            Y = -Y if Y.real > 0 else Y
            sigma = -(centre.real - Y.real)          # the strings' own damping: Re(s) = Re(mean) - Re(Y)
            if Y.real >= 0 or sigma < 0:
                continue
            if best is None or abs(Y.imag) < abs(best[1].imag):
                best = (delta, Y)
        if best is None:
            return None
        delta, Y = best
        detune = np.array([-delta / 2, delta / 2]) / (2 * np.pi)
        # Which string is which pole: the eigenvalues of the model, by ascending frequency, match the modes'.
        return {"modes": [int(i) for i in idx], "detune_hz": [float(x) for x in detune], "y": [float(Y.real), float(Y.imag)]}
    scale = max(float(np.max(np.abs(rel))), 1e-3)

    def residual(y):
        s = _strings_for(rel, (y[0] + 1j * y[1]) * scale)
        return (s.real - s.real.mean()) / scale

    best = None
    for g0 in (-0.3, -1.0, -3.0):
        for b0 in (0.0, 0.3):
            try:
                r = least_squares(residual, [g0, b0], method="lm")
            except (ValueError, np.linalg.LinAlgError):
                continue
            if best is None or r.cost < best.cost:
                best = r
    if best is None:
        return None
    Y = (best.x[0] + 1j * best.x[1]) * scale
    s = _strings_for(rel, Y) + centre
    if np.max(np.abs(residual(best.x))) > MAX_SPREAD_TOLERANCE or Y.real >= 0 or s.real.mean() > 0:
        return None
    detune = (s.imag - s.imag.mean()) / (2 * np.pi)
    return {"modes": [int(i) for i in idx], "detune_hz": [float(x) for x in np.sort(detune)], "y": [float(Y.real), float(Y.imag)]}


def model(detune_hz: np.ndarray, Y: complex, spread: float = 1.0, h: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """(eigenvalues relative to the strings' mean pole, residues) for the detunings scaled by `spread` and the
    strike weights h (ones: all the strings struck alike), eigenvalues by ascending imaginary part."""
    s = 2j * np.pi * spread * np.asarray(detune_hz, dtype=np.float64)
    n = len(s)
    h = np.ones(n) if h is None else np.asarray(h, dtype=np.float64)
    lam = np.linalg.eigvals(np.diag(s) + Y * np.ones((n, n)))
    lam = lam[np.argsort(lam.imag)]
    res = []
    for l in lam:
        d = l - s
        v = np.where(np.abs(d) > 1e-12, 1.0 / np.where(np.abs(d) > 1e-12, d, 1.0), 1e12)
        res.append(np.sum(v) * np.sum(v * h) / np.sum(v * v))
    return lam, np.asarray(res)


def layer_table(note: dict) -> list[dict | None]:
    """The model of each partial of one layer that has modes (the partials the profile holds, in their order)."""
    n = strings_of(int(note["midi"]))
    return [fit_partial(p["modes"], n) for p in note["partials"] if p["modes"]]


def layer_bytes(velocity: int, table: list[dict | None]) -> bytes:
    """u32 velocity, u32 partials, then per partial: u8 strings (0: no model), u8 x 3 mode indices (by ascending
    frequency, within the partial), f32 x 3 detunings (Hz, against the strings' mean), f32 Y re, f32 Y im (1/s)."""
    import struct

    out = [struct.pack("<2I", velocity, len(table))]
    for t in table:
        if t is None:
            out.append(struct.pack("<4B5f", 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0))
            continue
        idx = list(t["modes"]) + [0] * (3 - len(t["modes"]))
        det = list(t["detune_hz"]) + [0.0] * (3 - len(t["detune_hz"]))
        out.append(struct.pack("<4B5f", len(t["modes"]), *idx, *det, *t["y"]))
    return b"".join(out)
