"""`forge sympathetic`: the free strings (sustain pedal down, keys held) set ringing by the notes played.

With its damper up, a string is driven through the bridge by everything else that sounds. The strings whose
partials fall on the played note's (octaves, fifths: A3 sets A2, A4 and E5 ringing) answer loudly and for long; the
others barely do. The engine models every string as resonators, one per measured mode, driven by the voices of the
other notes. This module measures the coupling against the reference library, on the pairs of the interactions
session: same note and velocity, key held over [0, 4] s, pedal down or up. Their difference is the library's
sympathetic resonance (measured on 2026-11-06: -40 dB under the note over the first 100 ms, -15 to -1 dB after 1.5 s).

Model, per resonator k (a mode of a free string: frequency f_k, decay sigma_k):
    u_k <- p_k u_k + (b_k / fs) x,    out_c += Re(w_ck u_k)
  - x: the drive, the mono sum of the other notes' voices (their strings and attack residual; the body is shared,
    not a voice). Here, the pedal-up take of the note: the note as heard;
  - w_ck: the measured complex amplitude of the mode on channel c, over the RMS amplitude of its partial (the
    share of each unison mode, and the stereo image);
  - b_k: the coupling, one gain per third octave. Dividing by fs keeps the response independent of the sample
    rate.
  - an extra decay per third octave, added to the string's own: the library's resonance dies within a few
    seconds, where the measured string modes would ring for tens of seconds. Each band's extra decay is picked on
    a grid, with the gain refitted for it, as the one whose response follows the library's best over the time
    windows (median over the takes, in dB).
  - three traits of the library, not of a piano, fitted next: its resonance builds up later than the strings would
    answer (the drive of each voice rises as 1 - exp(-t / onset) from the note-on; it also keeps the hammer's knock
    from setting every string ringing at once); it may fade on its own (the drive also falls as exp(-t / decay));
    and it is weaker at low velocity than the note itself is (the drive scales as (velocity /
    REFERENCE_VELOCITY) ^ exponent, from the v40 / v100 pairs). The shape (onset, decay) is picked on a grid per
    register of the exciting note (REGISTERS): one shape for all left the bass resonance 5 to 6 dB too weak over
    the first 0.2 s and 4 to 13 dB too strong after 1.5 s (long bass notes drive the strings for as long as they
    sound, the library's resonance fades); the bass keeps a fast rise and a 4 s decay, the rest a slower rise and
    no decay. The combination kept is the one whose check (model against library, total power per take and
    window) has the smallest 90th percentile of the absolute error. (Driving the fit with the modes alone instead does worse: back-extrapolated to the
    note-on, they overstate the first instants, which the bass strings integrate for seconds.)

Last, per exciting note (2026-11-06, the pairs of every note): its own drive shape, gain and velocity exponent,
on top of the band gains and the registers' fit. Each note's resonance depends on its own coincidences with the
other strings, which neither the bands nor the registers see: with one note in six measured, a correction per note
did not carry over to its neighbours (left out, interpolated from them: worse than the registers); with all of them
measured, it needs no interpolation. Per note, on its two takes (v40, v100) over the five windows: for each shape of
the grid, the gain and the exponent's change by least squares on the dB errors (the exponent's change held back a
little: two velocities only); the shape kept is the one with the smallest mean absolute error.

The strings are those of the layer nearest SYMPATHETIC_VELOCITY: a free string rings with its own modes, whatever
the strike that measured them.

sympathetic.json: bands_hz (edges), gains, extra_decays, registers [{first, onset_s, drive_decay_s (0: none), gain_db,
velocity_exponent}] (one per exciting note measured: the note's own fit; and
onset_s, drive_decay_s: the first register's), velocity_exponent, reference_velocity, f_max_hz,
windows_s, fit (per take and window: model and library power, dB), notes[]: midi, resonators [[freq Hz, decay 1/s (extra decay included), gain, wl re, wl im,
wr re, wr im]].
"""

from __future__ import annotations

import itertools
import json
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import decimate, lfilter

from forge.note_modes import clean
from forge import progress

SYMPATHETIC_VELOCITY = 64
F_MAX = 25.0 * 2 ** (22 / 3)  # 4032 Hz: the library's resonance holds no energy above 4 kHz
BANDS = 25.0 * 2 ** (np.arange(0, 23) / 3)  # third octaves, 25 Hz to F_MAX
WINDOWS_S = ((0.03, 0.2), (0.2, 0.5), (0.5, 1.5), (1.5, 4.0), (4.0, 9.0))  # after the note-on
DRIVE_S = 9.5             # the key is held (pedal-up take) or the pedal holds the note (pedal-down) until 10 s
DECIMATIONS = (4, 8, 16, 32)  # each band is computed at the lowest rate that holds it (2.6 x its upper edge)
EXTRA_DECAYS = (0.0, 0.25, 0.5, 1.0, 2.0, 4.0)  # 1/s, the grid of the extra decay
ONSETS_S = (0.05, 0.1, 0.2, 0.3, 0.5)             # the grid of the drive's rise time
DRIVE_DECAYS_S = (0.0, 4.0, 2.0, 1.0, 0.5)         # the grid of the drive's fading time constant (0: none)
REGISTERS = (0, 45, 69)   # first note of each register of the exciting note, which has its own drive shape
REFERENCE_VELOCITY = 64                            # where the velocity factor is 1
NOTE_EXPONENT_RIDGE = 50.0  # how much the per-note fit holds the exponent's change back (dB^2 per unit)
MIN_SHARE = 1e-3          # a mode carrying less of its partial's energy than this is left out (CPU)
FLOOR = 1e-15             # library band powers below this (it is denoised) carry no measurement
MIN_SAMPLES = 6           # takes x windows a band needs to be fitted on its own


def strings(modes: dict) -> dict[int, np.ndarray]:
    """midi -> resonators (k x 6: freq, decay, wl re, wl im, wr re, wr im), from one modes-vNNN.json document."""
    out = {}
    for note in modes["notes"]:
        rows = []
        for p in clean(note)[0]["partials"]:
            m = np.asarray([x for x in p["modes"] if x[0] < F_MAX], dtype=np.float64)
            if len(m) == 0:
                continue
            a = m[:, 2:6]
            energy = 0.5 * np.sum(a ** 2, axis=1)
            norm = np.sqrt(np.sum(energy))
            if norm <= 0:
                continue
            keep = energy / np.sum(energy) >= MIN_SHARE
            rows.append(np.column_stack([m[keep, 0], m[keep, 1], a[keep] / norm]))
        if rows:
            out[note["midi"]] = np.concatenate(rows)
    return out


def band_of(freqs: np.ndarray) -> np.ndarray:
    return np.clip(np.searchsorted(BANDS, freqs, side="right") - 1, 0, len(BANDS) - 2)


def decimation_of(band: int, fs: float) -> int:
    return max([d for d in DECIMATIONS if fs / d > 2.6 * BANDS[band + 1]], default=1)


def response(x: np.ndarray, fs: float, res: np.ndarray, gains=None, extra=None) -> np.ndarray:
    """Stereo response (2 x samples) of the resonators `res` (k x 6) to the mono drive x at the rate fs. Unit
    coupling (b = 1 per second) and no extra decay when `gains` / `extra` are None, otherwise per band."""
    bands = band_of(res[:, 0])
    b = np.ones(len(res)) if gains is None else np.asarray(gains, dtype=np.float64)[bands]
    e = np.zeros(len(res)) if extra is None else np.asarray(extra, dtype=np.float64)[bands]
    from forge import native

    if native.available:  # the same recurrences in C++ (tools/forge/native/resonators.cpp), 50 times faster
        return native.resonator_bank(x, fs, res, b, e)
    return _response_python(x, fs, res, b, e)


def _response_python(x: np.ndarray, fs: float, res: np.ndarray, b: np.ndarray, e: np.ndarray) -> np.ndarray:
    """response() in Python (the reference): one lfilter per resonator."""
    y = np.zeros((2, len(x)))
    for k, (f, d, lr, li, rr, ri) in enumerate(res):
        if b[k] == 0:
            continue
        u = lfilter([b[k] / fs], [1.0, -np.exp((-d - e[k] + 2j * np.pi * f) / fs)], x)
        y[0] += lr * u.real - li * u.imag
        y[1] += rr * u.real - ri * u.imag
    return y


def power(y: np.ndarray, fs: float, a: float, b: float) -> float:
    """Mean power (Hann window) of a stereo signal over [a, b] s, channels summed."""
    seg = y[:, int(a * fs):int(b * fs)]
    w = np.hanning(seg.shape[1])
    return float(np.sum(np.mean((seg * w) ** 2, axis=1)) / np.mean(w ** 2))


def band_powers(y: np.ndarray, fs: float, a: float, b: float) -> np.ndarray:
    """Mean power per band (Hann window) over [a, b] s, channels summed, consistent with power()."""
    seg = y[:, int(a * fs):int(b * fs)]
    n = seg.shape[1]
    w = np.hanning(n)
    spec = 2 * np.sum(np.abs(np.fft.rfft(seg * w, axis=1)) ** 2, axis=0) / (n * n * np.mean(w ** 2))
    f = np.fft.rfftfreq(n, 1 / fs)
    return np.array([spec[(f >= lo) & (f < hi)].sum() for lo, hi in zip(BANDS[:-1], BANDS[1:])])


def _pair(down: Path) -> tuple[Path, Path] | None:
    """The pedal-up take of the same note and velocity (its own hold)."""
    stem = down.stem.rsplit("_h", 1)[0]
    up = next((down.parent.parent / "pedal-up").glob(f"{stem}_h*.flac"), None)
    return (down, up) if up is not None else None


def _load_pair(down: Path, up: Path) -> tuple[np.ndarray, np.ndarray, float, int]:
    """(drive mono, library resonance stereo, rate, note) from the note-on, at the full rate."""
    md = json.loads(down.with_suffix(".json").read_text(encoding="utf-8"))
    mu = json.loads(up.with_suffix(".json").read_text(encoding="utf-8"))
    xd, fs = sf.read(down, dtype="float64", always_2d=True)
    xu, _ = sf.read(up, dtype="float64", always_2d=True)
    n = int(DRIVE_S * fs)
    d = xd[md["note_on_sample"]:md["note_on_sample"] + n].T
    u = xu[mu["note_on_sample"]:mu["note_on_sample"] + n].T
    return u.mean(axis=0), d - u, float(fs), md["note"]


def onset_envelope(n: int, fs: float, onset: float, decay: float = 0.0) -> np.ndarray:
    """The drive's envelope from the note-on: 1 - exp(-t / onset) (1 throughout for onset 0), times exp(-t / decay)
    (none for decay 0)."""
    t = np.arange(n) / fs
    rise = np.ones(n) if onset <= 0 else 1.0 - np.exp(-t / onset)
    return rise if decay <= 0 else rise * np.exp(-t / decay)


def _measure_task(args) -> dict:
    """For one pair: the library's power per window and band, and the model's for each configuration (extra decay
    per band, onset, drive decay), with unit coupling, each band alone at its own rate."""
    down, up, res_by_note, configs = Path(args[0]), Path(args[1]), args[2], args[3]
    drive, diff, fs, note = _load_pair(down, up)
    meta = json.loads(down.with_suffix(".json").read_text(encoding="utf-8"))
    d4 = decimate(diff, 4, ftype="fir", zero_phase=True, axis=1)
    library = [band_powers(d4, fs / 4, a, b) for a, b in WINDOWS_S]
    others = np.concatenate([r for m, r in res_by_note.items() if m != note])  # the struck string is the voice
    bands = band_of(others[:, 0])
    model = np.zeros((len(configs), len(WINDOWS_S), len(BANDS) - 1))
    for shape in sorted({(c[1], c[2]) for c in configs}):
        drives = {1: drive * onset_envelope(len(drive), fs, *shape)}
        for d in DECIMATIONS:  # successive halvings from the nearest rate already computed
            prev = max(k for k in drives if d % k == 0)
            drives[d] = decimate(drives[prev], d // prev, ftype="fir", zero_phase=True)
        for b in np.unique(bands):
            dec = decimation_of(int(b), fs)
            for i, (extra, o, fade) in enumerate(configs):
                if (o, fade) != shape:
                    continue
                y = response(drives[dec], fs / dec, others[bands == b], extra=extra)
                model[i, :, b] = [power(y, fs / dec, a, c) for a, c in WINDOWS_S]
    return {"take": down.stem, "note": note, "velocity": meta["velocity"], "model": model,
            "library": np.asarray(library)}


def _band_fit(measures: list[dict], i: int, b: int, scale: dict | None = None):
    """(gain dB, mean absolute deviation dB, samples) of band b under configuration i, or None; `scale`: power
    factor per take (the velocity factor)."""
    scale = scale or {}
    r = [10 * np.log10(m["library"][w, b] / (m["model"][i, w, b] * scale.get(m["take"], 1.0)))
         for m in measures for w in range(len(WINDOWS_S))
         if m["model"][i, w, b] > 0 and m["library"][w, b] > FLOOR]
    if len(r) < MIN_SAMPLES:
        return None
    g = float(np.median(r))
    return g, float(np.mean(np.abs(np.asarray(r) - g))), len(r)


def _fill_smooth(values: np.ndarray) -> np.ndarray:
    """Bands without a value take their neighbours'; then a median over three bands."""
    nb = len(values)
    known = np.flatnonzero(np.isfinite(values))
    if len(known) == 0:
        raise SystemExit("no band of the library's resonance could be measured")
    v = np.interp(np.arange(nb), known, values[known])
    return np.array([np.median(v[max(0, b - 1):b + 2]) for b in range(nb)])


def fit_decays(measures: list[dict]) -> np.ndarray:
    """Per band, the extra decay of the grid whose ratios (library over unit response, dB, over takes and windows)
    scatter least around their median (configuration i: EXTRA_DECAYS[i], no onset)."""
    extra = np.full(len(BANDS) - 1, np.nan)
    for b in range(len(extra)):
        fits = [(f[1], EXTRA_DECAYS[i]) for i in range(len(EXTRA_DECAYS)) if (f := _band_fit(measures, i, b))]
        if fits:
            extra[b] = min(fits)[1]
    return _fill_smooth(extra)


def velocity_scale(measures: list[dict], exponent: float) -> dict[str, float]:
    """Power factor of the velocity law, per take."""
    return {m["take"]: (m["velocity"] / REFERENCE_VELOCITY) ** (2 * exponent) for m in measures}


def fit_exponent(measures: list[dict], i: int) -> float:
    """The velocity exponent: how much weaker than the unit response the library is at the lower velocity of each
    note's pair, per window and band (median, dB), against the velocity ratio (dB)."""
    by_note: dict[int, list[dict]] = {}
    for m in measures:
        by_note.setdefault(m["note"], []).append(m)
    slopes = []
    for pair in by_note.values():
        if len(pair) != 2:
            continue
        lo, hi = sorted(pair, key=lambda m: m["velocity"])
        span = 20 * np.log10(lo["velocity"] / hi["velocity"])
        for w in range(len(WINDOWS_S)):
            for b in range(len(BANDS) - 1):
                models = (lo["model"][i, w, b], hi["model"][i, w, b])
                libraries = (lo["library"][w, b], hi["library"][w, b])
                if min(models) > 0 and min(libraries) > FLOOR:
                    d = (10 * np.log10(libraries[0] / models[0])) - (10 * np.log10(libraries[1] / models[1]))
                    slopes.append(d / span)
    return float(np.clip(np.median(slopes), 0.0, 2.0)) if slopes else 0.0


def fit_gains(measures: list[dict], i: int, scale: dict[str, float]) -> tuple[np.ndarray, float]:
    """Per band gain (amplitude) under configuration i and the velocity factor, and the mean scatter (dB)."""
    fits = [_band_fit(measures, i, b, scale) for b in range(len(BANDS) - 1)]
    gains_db = np.array([f[0] if f else np.nan for f in fits])
    found = [f for f in fits if f]
    spread = float(np.average([f[1] for f in found], weights=[f[2] for f in found]))
    return 10 ** (_fill_smooth(gains_db) / 20), spread


def run(corpus: Path, interactions: Path | None = None, jobs: int = 0, decays_from: Path | None = None) -> list[Path]:
    """corpus: the reference state folder (modes); interactions: the same state in the interactions session;
    decays_from: a sympathetic.json whose extra decays are kept (the first, longest stage skipped)."""
    interactions = interactions or corpus.parent.parent / "interactions" / corpus.name
    start = time.time()
    modes = json.loads((corpus / f"modes-v{SYMPATHETIC_VELOCITY:03d}.json").read_text(encoding="utf-8"))
    res_by_note = strings(modes)
    total = sum(len(r) for r in res_by_note.values())
    print(f"  {total} resonators on {len(res_by_note)} strings (modes under {F_MAX:.0f} Hz)")
    pairs = [p for d in sorted((interactions / "pedal-down").glob("*.flac")) if (p := _pair(d))]
    if not pairs:
        raise SystemExit(f"no pedal-down / pedal-up pair in {interactions}")
    nb = len(BANDS) - 1

    def measure(configs):
        with ProcessPoolExecutor(max_workers=jobs or None) as pool:
            return list(progress.track(pool.map(_measure_task, [(str(d), str(u), res_by_note, configs)
                                                                for d, u in pairs]), len(pairs), "pairs",
                                       "responses of the free strings"))

    # 1. The extra decay per band (no onset, no drive decay).
    if decays_from is not None:
        extra = np.asarray(json.loads(decays_from.read_text(encoding="utf-8"))["extra_decays"], dtype=np.float64)
        print("  extra decay per band (1/s), from " + decays_from.name + ": " + " ".join(f"{e:g}" for e in extra))
    else:
        measures = measure([(np.full(nb, e), 0.0, 0.0) for e in EXTRA_DECAYS])
        extra = fit_decays(measures)
        print("  extra decay per band (1/s): " + " ".join(f"{e:g}" for e in extra) + f" ({time.time() - start:.0f} s)")
    # 2. The drive's shape per register of the exciting note, with those decays.
    shapes = [(o, fade) for o in ONSETS_S if o > 0 for fade in DRIVE_DECAYS_S]
    measures = measure([(extra, o, fade) for o, fade in shapes])
    print(f"  responses for {len(shapes)} drive shapes ({time.time() - start:.0f} s)")
    out = write(corpus, interactions, res_by_note, extra, shapes, measures)
    print(f"  ({time.time() - start:.0f} s)")
    return [out]


def register_of(note: int) -> int:
    return max(g for g, first in enumerate(REGISTERS) if note >= first)


def assess(measures: list[dict], shapes_of: list[int]) -> dict:
    """The model with one shape per register (indices into the measured configurations): velocity exponent and band
    gains refitted, then the check against the library (total power per take and window)."""
    chosen = [{**m, "model": m["model"][[shapes_of[register_of(m["note"])]]]} for m in measures]
    exponent = fit_exponent(chosen, 0)
    scale = velocity_scale(chosen, exponent)
    gains, spread = fit_gains(chosen, 0, scale)
    report, errors, by_register = [], [], [[] for _ in REGISTERS]
    for m in chosen:
        rows = []
        for w in range(len(WINDOWS_S)):
            model = float(np.sum(m["model"][0, w] * gains ** 2) * scale[m["take"]])
            library = float(np.sum(m["library"][w]))
            rows.append([round(10 * np.log10(model + 1e-30), 2), round(10 * np.log10(library + 1e-30), 2)])
            if library > FLOOR:
                errors.append(rows[-1][0] - rows[-1][1])
                by_register[register_of(m["note"])].append((w, errors[-1]))
        report.append({"take": m["take"], "windows": rows})
    a = np.abs(np.asarray(errors))
    return {"exponent": exponent, "gains": gains, "spread": spread, "report": report, "errors": np.asarray(errors),
            "median": float(np.median(a)), "p90": float(np.percentile(a, 90)), "by_register": by_register}


def choose(measures: list[dict], shapes: list[tuple[float, float]]) -> tuple[list[int], dict]:
    """The shape of each register: every combination, the one whose check has the smallest 90th percentile of the
    absolute error (then median). The gain scatter, used before, follows the check poorly."""
    best = None
    for combo in itertools.product(range(len(shapes)), repeat=len(REGISTERS)):
        r = assess(measures, list(combo))
        if best is None or (r["p90"], r["median"]) < (best[1]["p90"], best[1]["median"]):
            best = (list(combo), r)
    return best


def fit_notes(measures: list[dict], shapes: list, gains: np.ndarray, exponent: float) -> dict[int, dict]:
    """Per exciting note: the drive shape (index into shapes), gain (dB) and velocity exponent that fit its takes
    best on top of the band gains and the global exponent, and the errors (dB) per take and window."""
    by_note: dict[int, list[dict]] = {}
    for m in measures:
        by_note.setdefault(m["note"], []).append(m)
    out = {}
    for note, takes in sorted(by_note.items()):
        best = None
        for i in range(len(shapes)):
            rows, rhs = [], []
            for m in takes:
                x = 20 * np.log10(m["velocity"] / REFERENCE_VELOCITY)
                scale = (m["velocity"] / REFERENCE_VELOCITY) ** (2 * exponent)
                for w in range(len(WINDOWS_S)):
                    library = float(np.sum(m["library"][w]))
                    model = float(np.sum(m["model"][i, w] * gains ** 2) * scale)
                    if library > FLOOR and model > 0:
                        rows.append([1.0, x])
                        rhs.append(10 * np.log10(library) - 10 * np.log10(model))
            if len(rows) < 3:
                continue
            A, b = np.asarray(rows), np.asarray(rhs)
            sol = np.linalg.solve(A.T @ A + np.diag([0.0, NOTE_EXPONENT_RIDGE]), A.T @ b)
            err = A @ sol - b
            if best is None or np.mean(np.abs(err)) < best[0]:
                best = (float(np.mean(np.abs(err))), i, sol, err)
        if best is not None:
            out[note] = {"shape": best[1], "gain_db": float(best[2][0]), "velocity_exponent": float(exponent + best[2][1]),
                         "errors": best[3]}
    return out


def write(corpus: Path, interactions: Path, res_by_note: dict, extra: np.ndarray, shapes: list, measures: list) -> Path:
    """Chooses the shapes, reports the fit and writes sympathetic.json."""
    uniform = assess(measures, [shapes.index((0.3, 0.0))] * len(REGISTERS)) if (0.3, 0.0) in shapes else None
    combo, r = choose(measures, shapes)
    gains, exponent = r["gains"], r["exponent"]
    for g, first in enumerate(REGISTERS):
        cells = []
        for w in range(len(WINDOWS_S)):
            e = [x for k, x in r["by_register"][g] if k == w]
            cells.append(f"{np.median(e):+.1f}" if e else "-")
        onset, fade = shapes[combo[g]]
        print(f"  register from note {first}: onset {onset:.2f} s, drive decay {fade:.1f} s "
              f"(signed median per window, dB: {' '.join(cells)})")
    if uniform is not None:
        print(f"  one shape for all (onset 0.3 s, no decay): median |error| {uniform['median']:.1f} dB, "
              f"90 % within {uniform['p90']:.1f} dB")
    e = r["errors"]
    print(f"  model against the library, per take and window: median {np.median(e):+.1f} dB, "
          f"half within {r['median']:.1f} dB, 90 % within {r['p90']:.1f} dB (velocity exponent {exponent:.2f})")
    per_note = fit_notes(measures, shapes, gains, exponent)
    if per_note:
        a = np.abs(np.concatenate([f["errors"] for f in per_note.values()]))
        print(f"  with a fit per exciting note ({len(per_note)} notes): half within {np.median(a):.1f} dB, "
              f"90 % within {np.percentile(a, 90):.1f} dB")
    out = corpus / "sympathetic.json"
    notes = []
    for m, res in sorted(res_by_note.items()):
        bands = band_of(res[:, 0])
        rows = np.column_stack([res[:, 0], res[:, 1] + extra[bands], gains[bands], res[:, 2:6]])
        notes.append({"midi": m, "resonators": [[round(float(v[0]), 5), round(float(v[1]), 5),
                                                 *(float(f"{x:.6e}") for x in v[2:])] for v in rows]})
    registers = [{"first": first, "onset_s": shapes[combo[g]][0], "drive_decay_s": shapes[combo[g]][1], "gain_db": 0.0,
                  "velocity_exponent": exponent} for g, first in enumerate(REGISTERS)]
    if per_note:  # one register per note measured, from that note up
        registers = [{"first": n, "onset_s": shapes[f["shape"]][0], "drive_decay_s": shapes[f["shape"]][1],
                      "gain_db": round(f["gain_db"], 3), "velocity_exponent": round(f["velocity_exponent"], 4)}
                     for n, f in sorted(per_note.items())]
        registers[0]["first"] = 0
    out.write_text(json.dumps({
        "source": str(interactions), "velocity": SYMPATHETIC_VELOCITY, "f_max_hz": F_MAX,
        "bands_hz": [round(float(b), 3) for b in BANDS], "gains": [float(f"{g:.6e}") for g in gains],
        "extra_decays": [float(x) for x in extra], "onset_s": registers[0]["onset_s"],
        "drive_decay_s": registers[0]["drive_decay_s"], "registers": registers,
        "velocity_exponent": round(exponent, 4),
        "reference_velocity": REFERENCE_VELOCITY, "windows_s": WINDOWS_S, "fit": r["report"], "notes": notes},
        separators=(",", ":")), encoding="utf-8")
    return out


def load(path: Path) -> tuple[dict[int, np.ndarray], dict]:
    """(midi -> resonators (k x 7: freq, decay, gain, wl re, wl im, wr re, wr im), the drive's shape: onset_s,
    drive_decay_s, velocity_exponent, reference_velocity, registers [(first note, onset s, decay s, gain dB,
    velocity exponent)])."""
    data = json.loads(path.read_text(encoding="utf-8"))
    shape = {k: float(data.get(k, d)) for k, d in (("onset_s", 0.0), ("drive_decay_s", 0.0), ("velocity_exponent", 0.0),
                                                   ("reference_velocity", REFERENCE_VELOCITY))}
    shape["registers"] = [(int(r["first"]), float(r["onset_s"]), float(r["drive_decay_s"]), float(r.get("gain_db", 0.0)),
                           float(r.get("velocity_exponent", shape["velocity_exponent"]))) for r in data.get("registers", [])] \
        or [(0, shape["onset_s"], shape["drive_decay_s"], 0.0, shape["velocity_exponent"])]
    return {n["midi"]: np.asarray(n["resonators"], dtype=np.float64) for n in data["notes"]}, shape
