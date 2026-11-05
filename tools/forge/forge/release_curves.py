"""Damper curves measured on the reference corpus.

For every take of the 'releases' pass (one note in four, three velocities, holds of 0.1, 0.5 and 2 s), the
same note and velocity in the 'single-notes' pass is the same recording held longer: both are bit-identical
until the key release. After it, the power ratio between the two is exactly what the damper does:

    D(t) = 10 log10( P_release(t_off + t) / P_held(t_off + t) )

Beating makes a single ratio irregular, so the ratios are averaged over the three velocities (in power),
then smoothed and made non-increasing (isotonic regression): a damper only ever removes energy.
"""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

import numpy as np
import soundfile as sf

STEP_S = 0.01      # curve resolution
WINDOW_S = 0.02    # power window
LENGTH_S = 2.9     # curve length (the release takes keep 3 s after the key release)
FLOOR_DB = -100.0


def _isotonic_decreasing(y: np.ndarray) -> np.ndarray:
    """Least-squares non-increasing fit (pool adjacent violators)."""
    values, weights, sizes = [], [], []
    for v in y:
        values.append(float(v)); weights.append(1.0); sizes.append(1)
        while len(values) > 1 and values[-2] < values[-1]:  # violation: merge the last two blocks
            w = weights[-2] + weights[-1]
            v_merged = (values[-2] * weights[-2] + values[-1] * weights[-1]) / w
            s = sizes[-2] + sizes[-1]
            values[-2:], weights[-2:], sizes[-2:] = [v_merged], [w], [s]
    return np.repeat(values, sizes)


def _powers(audio: np.ndarray, start: int, fs: int, count: int) -> np.ndarray:
    hop, win = round(STEP_S * fs), round(WINDOW_S * fs)
    out = np.empty(count)
    for k in range(count):
        seg = audio[:, start + k * hop: start + k * hop + win]
        out[k] = float(np.mean(seg ** 2)) if seg.size else 0.0
    return out


def _load(path: Path) -> tuple[np.ndarray, int]:
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    return audio.T, fs


def measure(corpus: Path, log=print) -> dict:
    """corpus: folder of one state, e.g. corpus/reference/noire-dry (with releases/ and single-notes/)."""
    releases, single = corpus / "releases", corpus / "single-notes"
    groups: dict[tuple[int, float], list[Path]] = defaultdict(list)
    for meta_path in sorted(releases.glob("*.json")):
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        groups[(meta["note"], round(meta["hold"], 3))].append(meta_path)
    if not groups:
        raise SystemExit(f"no take in {releases}")

    count = round(LENGTH_S / STEP_S)
    notes = sorted({n for n, _ in groups})
    holds = sorted({h for _, h in groups})
    curves = np.zeros((len(notes), len(holds), count + 1))
    for (note, hold), metas in sorted(groups.items()):
        ratios = []
        for meta_path in metas:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            held_path = next(single.glob(f"{note:03d}-*_v{meta['velocity']:03d}_h*.flac"), None)
            if held_path is None:
                continue
            rel, fs = _load(releases / Path(meta["file"]).name)
            held, _ = _load(held_path)
            off = meta["note_off_sample"]
            p_rel, p_held = _powers(rel, off, fs, count), _powers(held, off, fs, count)
            ratios.append(np.where(p_held > 1e-20, p_rel / np.maximum(p_held, 1e-30), 1.0))
        if not ratios:
            continue
        d = 10 * np.log10(np.maximum(np.mean(ratios, axis=0), 10 ** (FLOOR_DB / 10)))
        d = np.convolve(np.pad(d, 2, mode="edge"), np.ones(5) / 5, mode="valid")  # 50 ms smoothing
        d = np.clip(_isotonic_decreasing(np.minimum(d, 0.0)), FLOOR_DB, 0.0)
        curves[notes.index(note), holds.index(hold)] = np.concatenate([[0.0], d])  # 0 dB at the release
        log(f"  note {note:3d} hold {hold:4.1f} s: -{-d[9]:.1f} dB at 0.1 s, -{-d[29]:.1f} dB at 0.3 s, "
            f"-{-d[min(99, count - 1)]:.1f} dB at 1 s ({len(ratios)} velocities)")

    # A real piano has dampers on every key up to the undamped top section. An undamped note between two
    # damped neighbours is a quirk of the library (Noire's C#6), not physics: drop it, so that the
    # interpolation between measured notes stays meaningful.
    damped = [bool(np.mean(curves[i, :, round(0.3 / STEP_S)]) < -6.0) for i in range(len(notes))]
    keep = [i for i in range(len(notes))
            if damped[i] or i == 0 or i == len(notes) - 1 or not (damped[i - 1] and damped[i + 1])]
    for i in sorted(set(range(len(notes))) - set(keep)):
        log(f"  WARNING: note {notes[i]} is undamped between two damped neighbours: dropped as an outlier")
    return {"description": "Damper curves: level after the key release relative to the same note held, in dB.",
            "source": str(corpus), "step_s": STEP_S, "notes": [notes[i] for i in keep], "holds_s": holds,
            "dropped_notes": [notes[i] for i in sorted(set(range(len(notes))) - set(keep))],
            "curves_db": np.round(curves[keep], 2).tolist()}


def write(corpus: Path, log=print) -> Path:
    data = measure(corpus, log)
    out = corpus / "release-curves.json"
    out.write_text(json.dumps(data), encoding="utf-8")
    return out
