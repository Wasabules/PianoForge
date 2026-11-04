"""Automatic checks of a rendered take. Independent of the instrument: NumPy only."""

from __future__ import annotations

import math

import numpy as np

SILENT_DBFS = -90.0
ATTACK_THRESHOLD_DB = -30.0  # below the peak: first crossing = start of the attack
ATTACK_WINDOW_S = 0.2  # the attack must appear within 200 ms after the note-on
TAIL_MAX_DB = -60.0  # the end of the take must be at least 60 dB below the peak
TAIL_LENGTH_S = 0.1

# An unplayed onset counts if its spectral flux exceeds 50 % of the played attack's. Measured on Noire
# (smoke session): the normal evolution of a note (end of attack, beating) reaches 33 % in the soft bass
# and stays under 10 % in the middle and treble. This guard therefore only targets clear anomalies.
SPURIOUS_RATIO = 0.5
EXPECTED_WINDOWS_S = {"note_on": (-0.01, 0.15), "other": (-0.02, 0.30)}  # around MIDI events


def db(x: float) -> float:
    return 20.0 * math.log10(x) if x > 0 else -math.inf


def peak_dbfs(audio: np.ndarray) -> float:
    return db(float(np.max(np.abs(audio)))) if audio.size else -math.inf


def spectral_flux(audio: np.ndarray, fs: int, frame: int = 1024, hop: int = 256) -> tuple[np.ndarray, np.ndarray]:
    """Onset detection function: sum of the rises of the log-compressed spectrum, frame by frame.
    Returns the flux and the time of each frame, dated by its end: an onset only becomes visible once
    it has entered the frame."""
    mono = audio.mean(axis=0).astype(np.float64)
    if mono.size < frame:
        return np.zeros(0), np.zeros(0)
    n = 1 + (mono.size - frame) // hop
    frames = np.lib.stride_tricks.as_strided(mono, (n, frame), (mono.strides[0] * hop, mono.strides[0]))
    spectrum = np.log1p(1000.0 * np.abs(np.fft.rfft(frames * np.hanning(frame), axis=1)))
    rises = np.maximum(np.diff(spectrum, axis=0), 0.0).sum(axis=1)
    return np.concatenate([[0.0], rises]), (np.arange(n) * hop + frame) / fs


def spurious_onsets(audio: np.ndarray, fs: int, events) -> list[float]:
    """Times (s) of onsets that match no MIDI event of the take.
    The reference is the strongest played attack: without a played note there is nothing to compare to."""
    is_note_on = lambda b: b[0] & 0xF0 == 0x90 and b[2] > 0
    if not any(is_note_on(b) for _, b in events):
        return []
    flux, times = spectral_flux(audio, fs)
    if flux.size < 3:
        return []
    windows = []
    for t, b in events:
        lo, hi = EXPECTED_WINDOWS_S["note_on" if is_note_on(b) else "other"]
        windows.append((t / fs + lo, t / fs + hi))
    expected = lambda s: any(lo <= s <= hi for lo, hi in windows)
    peaks = [i for i in range(1, flux.size - 1) if flux[i] >= flux[i - 1] and flux[i] > flux[i + 1]]
    reference = max((flux[i] for i in peaks if expected(times[i])), default=0.0)
    if reference == 0.0:
        return []
    return [round(float(times[i]), 3) for i in peaks if not expected(times[i]) and flux[i] > SPURIOUS_RATIO * reference]


def decay_time(audio: np.ndarray, fs: int, start: int, drop_db: float = 40.0) -> float | None:
    """Time (s) for the RMS level (10 ms windows) to drop by drop_db after sample `start`."""
    win = max(1, round(0.01 * fs))
    segment = audio[:, start:].astype(np.float64)
    n = segment.shape[1] // win
    if n < 2:
        return None
    rms = np.sqrt(np.mean(segment[:, : n * win].reshape(segment.shape[0], n, win) ** 2, axis=(0, 2)))
    if rms[0] <= 0:
        return None
    below = np.flatnonzero(20 * np.log10(np.maximum(rms, 1e-12) / rms[0]) < -drop_db)
    return round(below[0] * win / fs, 3) if below.size else None


def analyze(audio: np.ndarray, fs: int, note_on_sample: int | None = None, events=(),
            note_off_sample: int | None = None) -> dict:
    """audio: array (channels, samples). note_on_sample, note_off_sample: first note-on and its note-off.
    events: the take's (time, bytes) list, used to detect unplayed onsets."""
    envelope = np.max(np.abs(audio), axis=0)
    peak = float(envelope.max()) if envelope.size else 0.0
    res = {"peak_dbfs": round(db(peak), 2) if peak > 0 else None, "defects": []}  # None: digital silence
    if db(peak) < SILENT_DBFS:
        if note_on_sample is not None:
            res["defects"].append("silent")
        return res
    if peak >= 0.999:
        res["defects"].append("clipping")

    n_tail = max(1, round(TAIL_LENGTH_S * fs))
    tail_rms = float(np.sqrt(np.mean(audio[:, -n_tail:].astype(np.float64) ** 2)))
    res["tail_db"] = round(db(tail_rms) - db(peak), 1)
    if note_on_sample is not None and res["tail_db"] > TAIL_MAX_DB:
        res["defects"].append("tail_too_short")

    if note_on_sample is not None:
        threshold = peak * 10 ** (ATTACK_THRESHOLD_DB / 20)
        before = envelope[:note_on_sample]
        res["pre_note_db"] = round(db(float(before.max())) - db(peak), 1) if before.size else None
        window = envelope[note_on_sample: note_on_sample + round(ATTACK_WINDOW_S * fs)]
        above = np.flatnonzero(window > threshold)
        if above.size:
            res["attack_delay_ms"] = round(1000.0 * above[0] / fs, 2)
        else:
            res["defects"].append("missing_attack")

    spurious = spurious_onsets(audio, fs, list(events))
    if spurious:
        res["spurious_onsets_s"] = spurious[:20]
        res["defects"].append("spurious_onsets")
    if note_off_sample is not None:
        res["release_decay_40db_s"] = decay_time(audio, fs, note_off_sample)
    for key in ("tail_db", "pre_note_db"):  # -inf (digital silence) is not valid JSON
        if key in res and res[key] is not None and not math.isfinite(res[key]):
            res[key] = None
    return res


def max_diff_dbfs(reference: np.ndarray, other: np.ndarray) -> float:
    """Largest absolute difference between two renders, in dBFS (-inf: identical)."""
    if reference.shape != other.shape:
        return math.inf
    return db(float(np.max(np.abs(reference.astype(np.float64) - other.astype(np.float64)))))


def max_diff_db(reference: np.ndarray, other: np.ndarray) -> float:
    """Largest difference between two renders, in dB below the reference's peak (-inf: identical)."""
    if reference.shape != other.shape:
        return math.inf
    peak = float(np.max(np.abs(reference)))
    diff = float(np.max(np.abs(reference.astype(np.float64) - other.astype(np.float64))))
    if diff == 0.0:
        return -math.inf
    return db(diff) - db(peak) if peak > 0 else math.inf
