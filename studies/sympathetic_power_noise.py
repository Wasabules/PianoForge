# SPDX-License-Identifier: AGPL-3.0-or-later

"""How much does a second strike disturb the power estimate of the sympathetic resonance (Piano in 162)?

studies/sympathetic_power_check.py shows that P(down) - g P(up) finds the resonance when both takes hold the same
strike. A bank recorded with the pedal up and down holds two strikes: their differences (decay, beating) remain in the
estimate. Piano in 162 has two strikes per note and dynamic with the pedal up (its round robins), neither of them
resonating: the estimate between them, which should be zero, is the method's noise. This study measures it (as a share
of the take's power, per window and band) and sets it against the estimate on the real pairs of the interactions
session (pedal down against pedal up): the resonance is measurable where it stands clear of that noise.

Usage: .venv/Scripts/python studies/sympathetic_power_noise.py
"""
import json
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import decimate

from forge.sympathetic import BANDS, DRIVE_S, WINDOWS_S, _pair, band_powers

SAMPLES = Path("corpus/sources/piano-in-162/Ivy Audio - Piano in 162 sfz/Piano in 162 Samples/Close/PedalOffClose")
INTERACTIONS = Path("corpus/interactions/in162-close")
LAYERS = {"Piano": 40, "MezzoForte": 100}  # the dynamics the session's v40 and v100 play


def aligned(path: Path, fs_wanted: int | None = None) -> tuple[np.ndarray, int]:
    """A sample from 3 ms before its attack (its peak - 30 dB), stereo."""
    x, fs = sf.read(path, dtype="float64", always_2d=True)
    env = np.max(np.abs(x), axis=1)
    start = max(0, int(np.flatnonzero(env > env.max() * 10 ** (-30 / 20))[0]) - int(0.003 * fs))
    return x[start:].T, fs


def shares(a: np.ndarray, b: np.ndarray, fs: float) -> np.ndarray:
    """(P(a) - g P(b)) / P(a) per window and band (nan where a window runs past either recording)."""
    n = min(a.shape[1], b.shape[1], int(DRIVE_S * fs))
    a, b = a[:, :n], b[:, :n]
    dec = lambda x: decimate(x, 4, ftype="fir", zero_phase=True, axis=1)
    rows_a, rows_b = [], []
    for lo, hi in WINDOWS_S:
        if hi * fs > n:
            rows_a.append(np.full(len(BANDS) - 1, np.nan))
            rows_b.append(np.full(len(BANDS) - 1, np.nan))
            continue
        rows_a.append(band_powers(dec(a), fs / 4, lo, hi))
        rows_b.append(band_powers(dec(b), fs / 4, lo, hi))
    pa, pb = np.array(rows_a), np.array(rows_b)
    g = np.where(pb[0] > 0, pa[0] / np.maximum(pb[0], 1e-300), 1.0)
    with np.errstate(invalid="ignore", divide="ignore"):
        return (pa - g[None, :] * pb) / pa


def noise_task(args) -> np.ndarray:
    first, second = (aligned(Path(p)) for p in args)
    return shares(first[0], second[0], first[1])


def pair_task(args) -> np.ndarray:
    down, up = Path(args[0]), Path(args[1])
    out = []
    for path in (down, up):
        meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
        x, fs = sf.read(path, dtype="float64", always_2d=True)
        end = meta.get("sound_end_sample", x.shape[0])
        out.append(x[meta["note_on_sample"]:end].T)
    return shares(out[0], out[1], fs)


def report(name: str, values: list[np.ndarray]) -> None:
    v = np.array(values)  # takes x windows x bands
    print(name)
    for w, (lo, hi) in enumerate(WINDOWS_S):
        x = v[:, w, :].ravel()
        x = x[np.isfinite(x)]
        if len(x) == 0:
            print(f"  {lo:4.2f}-{hi:4.2f} s: no take this long")
            continue
        print(f"  {lo:4.2f}-{hi:4.2f} s: share of the take, median {np.median(x):+.3f}, |share| 90 % "
              f"{np.percentile(np.abs(x), 90):.3f} ({len(x)} band measures)")


if __name__ == "__main__":
    tasks = []
    for name in LAYERS:
        for one in sorted(SAMPLES.glob(f"*-PedalOff{name}1Close.flac")):
            two = one.with_name(one.name.replace(f"{name}1Close", f"{name}2Close"))
            if two.exists():
                tasks.append((str(one), str(two)))
    pairs = [p for d in sorted((INTERACTIONS / "pedal-down").glob("*.flac")) if (p := _pair(d))]
    with ProcessPoolExecutor() as pool:
        noise = list(pool.map(noise_task, tasks))
        real = list(pool.map(pair_task, [(str(d), str(u)) for d, u in pairs]))
    report(f"Noise: two strikes, pedal up ({len(noise)} pairs of round robins)", noise)
    report(f"Signal: pedal down against pedal up ({len(real)} pairs of the interactions session)", real)
