# SPDX-License-Identifier: AGPL-3.0-or-later

"""Is a sympathetic calibration stable? The same fit on two halves of the pairs (even and odd notes), compared.

forge sympathetic fits, per third-octave band, an extra decay (a grid) and a coupling gain (the median over takes and
windows of the library's resonance power against the model's). With the resonance measured by power (two strikes per
pair: Piano in 162), each measure is noisy; this study fits both halves of the keyboard apart and reports how far their
extra decays and gains are, band by band. Close halves: the calibration is measured, not noise.

Usage: .venv/Scripts/python studies/sympathetic_split_half.py [state]   (default: in162-close)
"""
import json
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

from forge import sympathetic as s

STATE = sys.argv[1] if len(sys.argv) > 1 else "in162-close"
CORPUS = Path("corpus/reference") / STATE
INTERACTIONS = Path("corpus/interactions") / STATE


def main() -> None:
    res_by_note = s.strings(json.loads((CORPUS / f"modes-v{s.SYMPATHETIC_VELOCITY:03d}.json").read_text(encoding="utf-8")))
    pairs = [p for d in sorted((INTERACTIONS / "pedal-down").glob("*.flac")) if (p := s._pair(d))]
    nb = len(s.BANDS) - 1
    configs = [(np.full(nb, e), 0.0, 0.0) for e in s.EXTRA_DECAYS]
    with ProcessPoolExecutor() as pool:
        measures = list(pool.map(s._measure_task, [(str(d), str(u), res_by_note, configs) for d, u in pairs]))
    methods = sorted({m["method"] for m in measures})
    print(f"{len(measures)} pairs of {STATE}, resonance by {', '.join(methods)}")
    halves = {"even": [m for m in measures if m["note"] % 2 == 0], "odd": [m for m in measures if m["note"] % 2 == 1]}
    extra = {k: s.fit_decays(v) for k, v in halves.items()}
    gains = {}
    for k, v in halves.items():
        row = []
        for b in range(nb):
            i = s.EXTRA_DECAYS.index(min(s.EXTRA_DECAYS, key=lambda e: abs(e - extra["even"][b])))  # the same decay
            f = s._band_fit(v, i, b)
            row.append(f[0] if f else np.nan)
        gains[k] = np.array(row)
    print(f"{'band (Hz)':>12} {'extra decay even/odd (1/s)':>28} {'gain even/odd (dB)':>22} {'difference':>11}")
    diffs = []
    for b in range(nb):
        d = gains["even"][b] - gains["odd"][b]
        if np.isfinite(d):
            diffs.append(d)
        print(f"{s.BANDS[b]:6.0f}-{s.BANDS[b + 1]:<5.0f} {extra['even'][b]:12.2f} / {extra['odd'][b]:<12.2f} "
              f"{gains['even'][b]:9.1f} / {gains['odd'][b]:<9.1f} {d:+9.1f}")
    a = np.abs(diffs)
    print(f"gain difference between the halves: median {np.median(a):.1f} dB, 90 % within {np.percentile(a, 90):.1f} dB "
          f"({len(a)} bands)")


if __name__ == "__main__":
    main()
