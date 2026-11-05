"""Partial analysis of a single piano note: fundamental, inharmonicity, partial amplitudes and decay rates.

Stiff-string model: f_n = n f0 sqrt(1 + B n^2), hence (f_n / n)^2 = f0^2 + f0^2 B n^2, a straight line in n^2
whose intercept gives f0 and slope gives B. Partials are found iteratively from the low ones (where B hardly
matters) upwards, with parabolic interpolation of the spectral peaks.

Decay: each partial is demodulated (complex heterodyne, then a moving average shorter than the partial
spacing) to get its amplitude envelope; a two-segment line fitted in dB gives the prompt (early) and
after-sound (late) decay rates, the double decay of piano tones.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

DB_PER_NEPER = 20 / np.log(10)


@dataclass
class Partial:
    n: int
    freq: float
    amp_db: float           # spectral peak level in the analysis segment
    early_db_s: float = 0.0  # decay rate of the prompt sound (dB/s, negative)
    late_db_s: float = 0.0   # decay rate of the after-sound (dB/s, negative)
    knee_s: float = 0.0      # time of the change of slope after the analysis start
    fit_rms_db: float = 0.0  # residual of the two-segment fit (beating shows here)


@dataclass
class NoteAnalysis:
    midi: int
    f0: float
    inharmonicity: float
    cents_vs_et: float  # tuning of the first partial (what is heard), against equal temperament
    partials: list[Partial] = field(default_factory=list)
    b1: float = 0.0  # frequency-independent damping (1/s), sigma = b1 + b3 omega^2
    b3: float = 0.0  # frequency-dependent damping (s)
    # Broadband level of the whole note, more robust than any single partial (beating between strings).
    level_early_db_s: float = 0.0
    level_late_db_s: float = 0.0
    level_knee_s: float = 0.0


def et_frequency(midi: int, a4: float = 440.0) -> float:
    return a4 * 2 ** ((midi - 69) / 12)


def _peak(spec_db: np.ndarray, freqs: np.ndarray, lo: float, hi: float) -> tuple[float, float] | None:
    i0, i1 = np.searchsorted(freqs, lo), np.searchsorted(freqs, hi)
    if i1 - i0 < 3:
        return None
    k = i0 + int(np.argmax(spec_db[i0:i1]))
    if k <= 0 or k >= len(spec_db) - 1:
        return None
    a, b, c = spec_db[k - 1], spec_db[k], spec_db[k + 1]
    denom = a - 2 * b + c
    delta = 0.5 * (a - c) / denom if denom != 0 else 0.0  # parabolic interpolation
    df = freqs[1] - freqs[0]
    return freqs[k] + delta * df, b - 0.25 * (a - c) * delta


def find_partials(mono: np.ndarray, fs: int, midi: int, a4: float = 440.0, max_freq: float = 16000.0,
                  min_snr_db: float = 15.0) -> tuple[float, float, list[Partial]]:
    """Returns (f0, B, partials) from a segment of a sustained note."""
    n_fft = 1 << int(np.ceil(np.log2(len(mono) * 8)))
    spec = np.abs(np.fft.rfft(mono * np.hanning(len(mono)), n_fft))
    spec_db = 20 * np.log10(np.maximum(spec, 1e-12))
    freqs = np.fft.rfftfreq(n_fft, 1 / fs)
    floor = np.median(spec_db)
    f0, B = et_frequency(midi, a4), 1e-4
    found: dict[int, Partial] = {}
    misses = 0
    n = 1
    while True:
        pred = n * f0 * np.sqrt(1 + B * n * n)
        if pred > min(max_freq, 0.45 * fs):
            break
        half = min(0.25 * f0, 0.04 * pred)
        peak = _peak(spec_db, freqs, pred - half, pred + half)
        if peak is not None and peak[1] - floor > min_snr_db:
            found[n] = Partial(n, peak[0], peak[1] - spec_db.max())
            misses = 0
        else:
            misses += 1
            if misses >= 4 and n > 8:
                break
        if len(found) >= 3:  # refit f0 and B on the partials found so far (weighted by level)
            ns = np.array(list(found))
            fn = np.array([found[k].freq for k in ns])
            w = 10 ** (np.array([found[k].amp_db for k in ns]) / 20)
            A = np.vstack([np.ones_like(ns, dtype=float), ns.astype(float) ** 2]).T
            y = (fn / ns) ** 2
            coef, *_ = np.linalg.lstsq(A * w[:, None], y * w, rcond=None)
            if coef[0] > 0:
                f0 = float(np.sqrt(coef[0]))
                B = float(max(coef[1] / coef[0], 0.0))
        n += 1
    return f0, B, [found[k] for k in sorted(found)]


def partial_envelope(mono: np.ndarray, fs: int, freq: float, spacing: float, hop_s: float = 0.01) -> np.ndarray:
    """Amplitude envelope (dB) of one partial, sampled every hop_s."""
    t = np.arange(len(mono)) / fs
    base = mono * np.exp(-2j * np.pi * freq * t)
    width = max(int(round(fs * 2.0 / spacing)), int(0.02 * fs))  # null at the neighbouring partials
    c = np.cumsum(np.concatenate([[0], base]))
    avg = (c[width:] - c[:-width]) / width
    hop = int(round(hop_s * fs))
    return 20 * np.log10(np.maximum(2 * np.abs(avg[::hop]), 1e-12))


def two_slope_fit(env_db: np.ndarray, hop_s: float) -> tuple[float, float, float, float]:
    """Fits a two-segment line to a dB envelope: (early slope, late slope, knee time, rms residual)."""
    t = np.arange(len(env_db)) * hop_s
    best = None
    for k in range(5, len(env_db) - 5, max(1, len(env_db) // 60)):
        p1 = np.polyfit(t[:k], env_db[:k], 1)
        p2 = np.polyfit(t[k:], env_db[k:], 1)
        res = np.concatenate([env_db[:k] - np.polyval(p1, t[:k]), env_db[k:] - np.polyval(p2, t[k:])])
        err = float(np.mean(res ** 2))
        if best is None or err < best[0]:
            best = (err, p1[0], p2[0], t[k])
    if best is None:
        p = np.polyfit(t, env_db, 1)
        return float(p[0]), float(p[0]), 0.0, 0.0
    return float(best[1]), float(best[2]), float(best[3]), float(np.sqrt(best[0]))


def analyze_note(audio: np.ndarray, fs: int, midi: int, note_on: int, hold_s: float, a4: float = 440.0,
                 max_partials: int = 40) -> NoteAnalysis:
    mono = audio.mean(axis=0)
    f_et = et_frequency(midi, a4)
    # Frequency analysis: skip the attack, long enough to separate the partials.
    seg_len = float(np.clip(40.0 / f_et, 0.4, 3.0))
    start = note_on + int(0.05 * fs)
    seg = mono[start: start + int(seg_len * fs)]
    f0, B, partials = find_partials(seg, fs, midi, a4)
    if len(partials) < 3:
        # Top notes: too few partials to fit f0 and B. The pitch is the strongest peak within a semitone of the
        # nominal note; B stays unknown here (the keyboard-wide model fills it in).
        n_fft = 1 << int(np.ceil(np.log2(len(seg) * 8)))
        spec_db = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n_fft)), 1e-12))
        peak = _peak(spec_db, np.fft.rfftfreq(n_fft, 1 / fs), f_et * 2 ** (-1 / 12), f_et * 2 ** (1 / 12))
        if peak is not None:
            f0, B = peak[0], 0.0
            partials = [Partial(1, peak[0], 0.0)] + [p for p in partials if p.n > 1]
    first = partials[0].freq if partials and partials[0].n == 1 else f0 * np.sqrt(1 + B)
    result = NoteAnalysis(midi, f0, B, 1200 * np.log2(first / f_et), partials[:max_partials])

    # Decay analysis over the hold (key down), until the partial sinks into the noise floor.
    body = mono[start: note_on + int(hold_s * fs)]
    hop = int(0.01 * fs)
    count = len(body) // hop
    level = 10 * np.log10(np.maximum(np.mean(body[: count * hop].reshape(count, hop) ** 2, axis=1), 1e-24))
    level = level[: np.flatnonzero(level > level.max() - 60)[-1] + 1]
    if len(level) > 20:
        result.level_early_db_s, result.level_late_db_s, result.level_knee_s, _ = two_slope_fit(level, 0.01)
    for p in result.partials:
        env = partial_envelope(body, fs, p.freq, f0)
        usable = np.flatnonzero(env > env.max() - 50)
        if len(usable) < 20:
            continue
        env = env[: usable[-1] + 1]
        p.early_db_s, p.late_db_s, p.knee_s, p.fit_rms_db = two_slope_fit(env, 0.01)

    # Damping law sigma = b1 + b3 omega^2 on the prompt-sound decay rates (Chaigne & Askenfelt).
    rated = [p for p in result.partials if p.early_db_s < 0]
    if len(rated) >= 3:
        omega2 = np.array([(2 * np.pi * p.freq) ** 2 for p in rated])
        sigma = np.array([-p.early_db_s / DB_PER_NEPER for p in rated])
        A = np.vstack([np.ones_like(omega2), omega2]).T
        (b1, b3), *_ = np.linalg.lstsq(A, sigma, rcond=None)
        result.b1, result.b3 = float(b1), float(b3)
    return result
