# SPDX-License-Identifier: AGPL-3.0-or-later

"""`forge half-pedal`: how far the sustain pedal lifts the dampers, per CC64 value, measured on the reference.

The engine's damper model is the measured release curve (forge release-curves), run at a speed set by the pedal:
0 with the dampers lifted, 1 with them resting on the strings. In between, the felt touches the strings lightly
and the curve runs slower. The half-pedal takes of the interactions session (pedal down, key released, CC64
lifted from 127 to 0 in 32 steps over 4 s) give that speed: the level against the same note held without pedal,
mapped back through the note's release curve to "curve time", grows by speed x elapsed time over each range of
CC values. The speed is the median over the notes, made non-decreasing as the pedal comes up (a damper only
presses harder) and capped at 1. CC64 = 127 is the pedal down: lift 1.

Measured on 2026-11-06 (Noire): from 127 down to about 16 the dampers barely touch (2 to 20 % of the release
speed, -1 to -5 dB per second); at 12 and below they rest on the strings.

pedal-curve.json: lift[128] (1 lifted, 0 resting), ranges (CC range, median speed, per-note speeds).
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import soundfile as sf

RANGES = ((126, 96), (95, 64), (63, 32), (31, 16), (12, 9), (8, 5), (4, 0))  # the ramp's steps fall in each
EDGE_S = 0.04  # level window half width, and the margin kept from the CC steps


def _level_db(x: np.ndarray, fs: float, t: float) -> float:
    a, b = int((t - EDGE_S) * fs), int((t + EDGE_S) * fs)
    return 10 * np.log10(float(np.mean(x[a:b] ** 2)) + 1e-30)


def speeds(take: Path, reference: Path, curves: dict) -> list[float]:
    """Damper speed over each CC range of one half-pedal take (nan where the level is past the curve)."""
    meta = json.loads(take.with_suffix(".json").read_text(encoding="utf-8"))
    note, velocity = meta["note"], meta["velocity"]
    held = next((reference / "single-notes").glob(f"{note:03d}-*_v{velocity:03d}_h*.flac"), None)
    if held is None:
        raise SystemExit(f"no single-notes take for note {note} at velocity {velocity}")
    x, fs = sf.read(take, dtype="float64", always_2d=True)
    h, _ = sf.read(held, dtype="float64", always_2d=True)
    shift = json.loads(held.with_suffix(".json").read_text(encoding="utf-8"))["note_on_sample"] / fs \
        - meta["note_on_sample"] / fs
    steps = [(e[0] / fs, e[1][2]) for e in meta["events"] if e[1][0] & 0xF0 == 0xB0 and e[1][1] == 64][1:]
    period = steps[1][0] - steps[0][0]
    i = int(np.argmin(np.abs(np.asarray(curves["notes"]) - note)))
    j = int(np.argmin(np.abs(np.asarray(curves["holds_s"]) - 2.0)))
    c = np.asarray(curves["curves_db"][i][j])
    curve_time = lambda db: float(np.interp(-db, -c, np.arange(len(c)) * curves["step_s"]))
    against_held = lambda t: _level_db(x, fs, t) - _level_db(h, fs, t + shift)
    out = []
    for hi, lo in RANGES:
        times = [t for t, v in steps if lo <= v <= hi]
        if not times:
            out.append(float("nan"))
            continue
        t0, t1 = min(times) + EDGE_S, max(times) + period - EDGE_S
        d0, d1 = against_held(t0), against_held(t1)
        out.append((curve_time(d1) - curve_time(d0)) / (t1 - t0) if d1 > c[-1] + 3 else float("nan"))
    return out


def measure(reference: Path, interactions: Path) -> dict:
    curves = json.loads((reference / "release-curves.json").read_text(encoding="utf-8"))
    takes = sorted((interactions / "half-pedal").glob("*.flac"))
    if not takes:
        raise SystemExit(f"no half-pedal take in {interactions}")
    per_note = {t.stem: speeds(t, reference, curves) for t in takes}
    median = []
    for k in range(len(RANGES)):
        found = [s[k] for s in per_note.values() if np.isfinite(s[k])]
        median.append(float(np.median(found)) if found else 1.0)  # past every curve: resting on the strings
    # Non-decreasing as the pedal comes up (pool adjacent violators), capped at 1.
    values, sizes = [], []
    for v in median:
        values.append(min(float(v), 1.0)); sizes.append(1)
        while len(values) > 1 and values[-2] > values[-1]:
            w = sizes[-2] + sizes[-1]
            values[-2:], sizes[-2:] = [(values[-2] * sizes[-2] + values[-1] * sizes[-1]) / w], [w]
    speed = np.repeat(values, sizes)
    centres = [(hi + lo) / 2 for hi, lo in RANGES]
    lift = np.ones(128)
    cc = np.arange(127)  # 127 is the pedal fully down
    lift[:127] = 1.0 - np.interp(cc, centres[::-1], speed[::-1])
    for (hi, lo), s in zip(RANGES, speed):
        if s >= 1.0:
            lift[lo:hi + 1] = 0.0  # resting on the strings over the whole range
    return {"description": "Damper lift per CC64 value (1 lifted, 0 resting on the strings), from the half-pedal "
                           "ramps.", "source": str(interactions), "lift": [round(float(v), 4) for v in lift],
            "ranges": [{"cc": [hi, lo], "speed": round(float(s), 4),
                        "per_note": {k: round(v[i], 4) for k, v in per_note.items()}}
                       for i, ((hi, lo), s) in enumerate(zip(RANGES, speed))]}


def write(reference: Path, interactions: Path | None = None, log=print) -> Path:
    interactions = interactions or reference.parent.parent / "interactions" / reference.name
    data = measure(reference, interactions)
    for r in data["ranges"]:
        log(f"  CC {r['cc'][0]:3d}-{r['cc'][1]:3d}: damper speed {r['speed']:.3f}")
    out = reference / "pedal-curve.json"
    out.write_text(json.dumps(data, indent=1), encoding="utf-8")
    return out
