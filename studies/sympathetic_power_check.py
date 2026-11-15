"""Can the sympathetic resonance be measured without subtracting the two takes of a pair?

forge sympathetic measures the library's resonance as the pedal-down take minus the pedal-up take: exact when both
hold the same strike (a deterministic library: Noire), meaningless when they are two recordings (Piano in 162's
PedalOn and PedalOff samples). Without the subtraction, the resonance's power per band and window can be estimated
from the takes' powers: P(down) - g P(up), g the ratio of the two strikes' powers in the band over the first window
(where the resonance is still 40 dB under the note). A term crosses the note and the resonance where they share a
frequency (the strings in octaves), which this estimate cannot remove.

On Noire, where the true resonance P(down - up) is known, this study compares both, per window and band, and says
how far apart they are where the resonance counts.

Usage: .venv/Scripts/python studies/sympathetic_power_check.py [state]   (default: noire-dry)
"""
import json
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import decimate

from forge.sympathetic import BANDS, DRIVE_S, FLOOR, WINDOWS_S, _pair, band_powers

STATE = sys.argv[1] if len(sys.argv) > 1 else "noire-dry"
INTERACTIONS = Path("corpus/interactions") / STATE


def measure(paths):
    """(note, true resonance power, its estimate, pedal-down power), windows x bands."""
    down, up = Path(paths[0]), Path(paths[1])
    md = json.loads(down.with_suffix(".json").read_text(encoding="utf-8"))
    mu = json.loads(up.with_suffix(".json").read_text(encoding="utf-8"))
    xd, fs = sf.read(down, dtype="float64", always_2d=True)
    xu, _ = sf.read(up, dtype="float64", always_2d=True)
    n = int(DRIVE_S * fs)
    d = xd[md["note_on_sample"]:md["note_on_sample"] + n].T
    u = xu[mu["note_on_sample"]:mu["note_on_sample"] + n].T
    dec = lambda x: decimate(x, 4, ftype="fir", zero_phase=True, axis=1)
    pu, pd, pr = (np.array([band_powers(dec(x), fs / 4, a, b) for a, b in WINDOWS_S]) for x in (u, d, d - u))
    g = np.where(pu[0] > 0, pd[0] / np.maximum(pu[0], 1e-300), 1.0)
    return md["note"], pr, pd - g[None, :] * pu, pd


if __name__ == "__main__":
    pairs = [p for d in sorted((INTERACTIONS / "pedal-down").glob("*.flac")) if (p := _pair(d))]
    with ProcessPoolExecutor() as pool:
        results = list(pool.map(measure, [(str(d), str(u)) for d, u in pairs]))
    print(f"{len(results)} pairs of {STATE}")
    for w, (a, b) in enumerate(WINDOWS_S):
        errors, negative, counted = [], 0, 0
        for note, true, est, down in results:
            for k in range(len(BANDS) - 1):
                # where the resonance counts: above the floor, and at least 1 % of the pedal-down take's power
                if true[w, k] <= FLOOR or true[w, k] < 0.01 * down[w, k]:
                    continue
                counted += 1
                if est[w, k] <= 0:
                    negative += 1
                    continue
                errors.append(10 * np.log10(est[w, k] / true[w, k]))
        e = np.abs(errors)
        print(f"  window {a:4.2f}-{b:4.2f} s: {counted:5d} band measures; estimate <= 0 in {negative:4d}; "
              f"|error| median {np.median(e) if len(e) else float('nan'):5.1f} dB, 90 % {np.percentile(e, 90) if len(e) else float('nan'):5.1f} dB, "
              f"bias {np.median(errors) if errors else float('nan'):+5.1f} dB")
