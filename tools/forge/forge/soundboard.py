"""`forge soundboard`: the soundboard body, as resonances shared by every note.

What the strings leave over (recording minus the measured modes, `forge modes`) holds, under the fundamental, a
broadband body that dies away in about a second: the soundboard and the case, excited by the hammer blow through
the bridge. Physics says the resonances belong to the instrument and only their excitation depends on the note
(where the bridge is driven). The measurement agrees: poles estimated on half of the treble notes explain the
other half as well (spike B v2 study, pre-analysis section 8).

Method:
  1. Residual of each note, brought to baseband over the body band (30-700 Hz), from 30 ms after the note-on
     (the hammer has left the strings) to 2.5 s.
  2. Shared poles, by multichannel ESPRIT over the treble notes (whose partials all lie above the band): 160 on
     the whole window, plus 40 on its tail, where the slower resonances stand out.
  3. Per note, the complex amplitude of each resonance per channel, by least squares in the frequency domain,
     leaving out the bins around the note's partials (the body cannot be measured under a partial). The fit is
     weighted towards the tail (exp(2 t)) so that the body's decay counts, not only its start.

soundboard-v064.json: band, poles [[freq Hz, decay 1/s]], notes[]: midi, amps [[re0, im0, re1, im1] per pole]
(complex amplitude per channel at t = 0, the note-on; same convention as the modes).
"""

from __future__ import annotations

import json
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge.modes import BandExtractor, _hankel_cov, esprit_poles, synthesize
from forge.note_modes import clean, partial_modes

BAND = (30.0, 700.0)
T0, T1 = 0.03, 2.5
TAIL_S = 0.4
EARLY_POLES, TAIL_POLES = 160, 40
WEIGHT_PER_S = 2.0
POLE_NOTES = (81, 104)  # partials all above the band: the residual there is body (and noise) only


def residual_baseband(path: Path, note: dict, seconds: float | None = None) -> tuple[np.ndarray, float]:
    """Body band of (recording - measured modes), at baseband, over [T0, T1] (channels x samples). `seconds` limits
    the part of the hold that is read (it changes the baseband rate, which depends on the length)."""
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    on, off = meta["note_on_sample"], meta["note_off_sample"]
    if seconds is not None:
        off = min(off, on + int(round(seconds * fs)))
    x = audio[on:off].T
    r = x - synthesize(partial_modes(clean(note)[0]), x.shape[0], x.shape[1], fs)  # failed fits left out
    lo, hi = BAND
    z, fs_d = BandExtractor(r, fs).baseband((lo + hi) / 2, (hi - lo) / 2, (hi - lo) / 2 + 20)
    i0, i1 = int(round(T0 * fs_d)), min(int(round(T1 * fs_d)), z.shape[1])
    return z[:, i0:i1], fs_d


def shared_poles(segments: list[np.ndarray], order: int, rows: int) -> np.ndarray:
    """Poles common to several multichannel signals (each channel normalised to the same weight)."""
    z = np.concatenate(segments, axis=0)
    z = z / np.sqrt(np.mean(np.abs(z) ** 2, axis=1, keepdims=True))
    w, u = np.linalg.eigh(_hankel_cov(z, rows))
    return esprit_poles(z, order, basis=u[:, ::-1])


def fit_note(z: np.ndarray, fs_d: float, poles: np.ndarray, partial_freqs: list[float], half_width: float,
             ridge: float = 1e-3) -> np.ndarray:
    """Amplitudes (channels x poles) at the window start, by weighted least squares in the frequency domain on the
    bins farther than `half_width` from every partial. A resonance whose own frequency lies in an excluded band
    cannot be measured (the fit would trade it against its neighbours, with huge amplitudes cancelling each other
    in the bins kept): it is left at zero, the body being masked by the partial there anyway. A light ridge
    (`ridge` times the mean diagonal of the normal matrix) keeps close resonances from cancelling each other."""
    n = z.shape[1]
    m = np.arange(n)
    # Weight towards the tail, times a Tukey taper: the resonances get the same window in the data and in the
    # model (no bias), while what is not modelled (the partials' leftovers) stops leaking into the bins kept.
    taper = np.ones(n)
    edge = max(1, int(0.1 * n))
    ramp = 0.5 * (1 - np.cos(np.pi * np.arange(edge) / edge))
    taper[:edge], taper[-edge:] = ramp, ramp[::-1]
    weight = np.exp(WEIGHT_PER_S * m / fs_d) * taper
    lo, hi = BAND
    pole_freqs = (lo + hi) / 2 + np.angle(poles) * fs_d / (2 * np.pi)
    measurable = np.ones(len(poles), dtype=bool)
    for f in partial_freqs:
        measurable &= np.abs(pole_freqs - f) > half_width
    amps = np.zeros((z.shape[0], len(poles)), dtype=np.complex128)
    if not np.any(measurable):
        return amps
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    basis = np.fft.fft((poles[None, measurable] ** m[:, None]) * weight[:, None], nfft, axis=0)
    data = np.fft.fft(z.T * weight[:, None], nfft, axis=0)
    freqs = (lo + hi) / 2 + np.fft.fftfreq(nfft, 1 / fs_d)
    keep = (freqs >= lo) & (freqs <= hi)
    for f in partial_freqs:
        keep &= np.abs(freqs - f) > half_width
    b = basis[keep]
    normal = b.conj().T @ b
    normal += ridge * np.real(np.trace(normal)) / len(normal) * np.eye(len(normal))
    amps[:, measurable] = np.linalg.solve(normal, b.conj().T @ data[keep]).T
    return amps


def excluded(note: dict) -> tuple[list[float], float]:
    """The frequencies whose neighbourhood the body fit leaves out, and the half width of each: the partials
    measured, and those too quiet to be analysed, which still sit on the stiff-string law."""
    f0 = note["f0"]
    centres = [p["centre"] for p in note["partials"]]
    n_max = int(BAND[1] / max(f0, 1.0)) + 2
    law = [n * f0 * np.sqrt(1 + note["inharmonicity"] * n * n) for n in range(1, n_max + 1)]
    return centres + law, min(0.33 * f0, 30.0)


def _fit_task(args) -> dict:
    path, note, freqs, decays = args
    z, fs_d = residual_baseband(Path(path), note)
    lo, hi = BAND  # the poles at this take's own baseband rate (it depends on the take's length)
    poles = np.exp((-np.asarray(decays) + 2j * np.pi * (np.asarray(freqs) - (lo + hi) / 2)) / fs_d)
    partials, half_width = excluded(note)
    amps = fit_note(z, fs_d, poles, partials, half_width=half_width)
    i0 = int(round(T0 * fs_d))
    amps0 = amps / (poles[None, :] ** i0)  # back to t = 0 (the note-on)
    return {"midi": note["midi"], "name": note["name"],
            "amps": [[float(f"{v:.6e}") for c in range(amps0.shape[0]) for v in (amps0[c, k].real, amps0[c, k].imag)]
                     for k in range(amps0.shape[1])]}


POLES_VELOCITY = 64  # the resonances are measured once, at this velocity; other velocities only fit amplitudes


def estimate_poles(notes: dict, takes: dict) -> tuple[np.ndarray, float]:
    """Shared resonances (poles per baseband sample) and the baseband rate, from the treble notes' residuals."""
    segments, fs_d = [], 0.0
    for midi in range(POLE_NOTES[0], POLE_NOTES[1] + 1):
        if midi in notes and midi in takes:
            z, fs_d = residual_baseband(takes[midi], notes[midi])
            segments.append(z)
    k_tail = int(round((TAIL_S - T0) * fs_d))
    length = min(s.shape[1] for s in segments)
    segments = [s[:, :length] for s in segments]
    poles = np.concatenate([shared_poles(segments, EARLY_POLES, 600),
                            shared_poles([s[:, k_tail:] for s in segments], TAIL_POLES, 500)])
    poles = poles[np.abs(poles) < 1.0]
    lo, hi = BAND
    freqs = (lo + hi) / 2 + np.angle(poles) * fs_d / (2 * np.pi)
    inside = (freqs >= lo - 10) & (freqs <= hi + 10)
    return poles[inside], fs_d


def run(corpus: Path, velocity: int = 64, jobs: int = 0) -> list[Path]:
    """Amplitudes per note at `velocity`. The resonances belong to the instrument, not to the blow: they are
    estimated at POLES_VELOCITY (written first if needed) and reused as they are for every other velocity."""
    modes = json.loads((corpus / f"modes-v{velocity:03d}.json").read_text(encoding="utf-8"))
    notes = {n["midi"]: n for n in modes["notes"]}
    takes = {int(p.name[:3]): p for p in (corpus / "single-notes").glob(f"*_v{velocity:03d}_h*.flac")}
    start = time.time()
    lo, hi = BAND
    reference = corpus / f"soundboard-v{POLES_VELOCITY:03d}.json"
    if velocity != POLES_VELOCITY:
        if not reference.exists():
            run(corpus, POLES_VELOCITY, jobs)
        freqs, decays, _ = load(reference)
        print(f"  {len(freqs)} resonances from {reference.name}")
    else:
        poles, fs_d = estimate_poles(notes, takes)
        freqs = (lo + hi) / 2 + np.angle(poles) * fs_d / (2 * np.pi)
        decays = -np.log(np.abs(poles)) * fs_d
        print(f"  {len(poles)} shared resonances from the treble notes ({time.time() - start:.0f} s); "
              f"T60 median {np.median(6.91 / decays):.2f} s")
    tasks = [(str(takes[m]), notes[m], freqs.tolist(), decays.tolist()) for m in sorted(notes) if m in takes]
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        fitted = list(pool.map(_fit_task, tasks))
    print(f"  amplitudes of {len(fitted)} notes ({time.time() - start:.0f} s)")
    out = corpus / f"soundboard-v{velocity:03d}.json"
    out.write_text(json.dumps({
        "source": str(corpus), "velocity": velocity, "band_hz": list(BAND), "window_s": [T0, T1],
        "convention": modes["convention"],
        "poles": [[round(float(f), 4), round(float(d), 5)] for f, d in zip(freqs, decays)],
        "notes": fitted}, separators=(",", ":")), encoding="utf-8")
    return [out]


def load(path: Path) -> tuple[np.ndarray, np.ndarray, dict[int, np.ndarray]]:
    """(freqs, decays, {midi: amplitudes channels x poles})."""
    data = json.loads(path.read_text(encoding="utf-8"))
    freqs = np.array([p[0] for p in data["poles"]])
    decays = np.array([p[1] for p in data["poles"]])
    notes = {}
    for note in data["notes"]:
        a = np.array(note["amps"])  # poles x (2 * channels)
        notes[note["midi"]] = (a[:, 0::2] + 1j * a[:, 1::2]).T
    return freqs, decays, notes


def synthesize_body(freqs: np.ndarray, decays: np.ndarray, amps: np.ndarray, length: int, fs: float) -> np.ndarray:
    """The body of one note (channels x length), from the note-on."""
    t = np.arange(length) / fs
    out = np.zeros((amps.shape[0], length))
    for k in range(len(freqs)):
        carrier = np.exp((-decays[k] + 2j * np.pi * freqs[k]) * t)
        for c in range(amps.shape[0]):
            out[c] += np.real(amps[c, k] * carrier)
    return out
