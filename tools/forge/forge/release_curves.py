# SPDX-License-Identifier: AGPL-3.0-or-later

"""Damper curves measured on the reference corpus.

For every take of the 'releases' pass (one note in four, three velocities, holds of 0.1, 0.5 and 2 s), the
same note and velocity in the 'single-notes' pass is the same recording held longer: both are bit-identical
until the key release. After it, the power ratio between the two is exactly what the damper does:

    D(t) = 10 log10( P_release(t_off + t) / P_held(t_off + t) )

Beating makes a single ratio irregular, so the ratios are averaged over the three velocities (in power),
then smoothed and made non-increasing (isotonic regression): a damper only ever removes energy.

The damper does not take every velocity alike, though: against that average, the loud strikes die much faster
(v120 after a 2 s hold: 7 dB under it at 0.3 s, 11 dB at 0.6 s; v32: 1 to 3 dB over it), their energy being in
the high partials the felt stops first. The same curves are also measured per velocity (each smoothed and made
non-increasing alike), and the engine plays the one of the note's velocity, interpolated in dB between the
measured ones; the average stays for what has no velocity (the dampers of the free strings).

The felt does not damp every partial alike: the high partials die faster (A1 0.4 s after the release: -18 dB
for the fundamental, -35 dB around 880 Hz). The same ratio is measured per group of partials (numbers 1, 2-3,
4-7, 8-15, 16-31, 32 and up: the bands around each partial, BandExtractor) and kept relative to the overall
curve; the engine applies it to the partials' poles. Where a group is too far under its own level at the
release (the take there is noise or the library's denoised silence), or louder than the note held (a damper
only removes energy: that is the release noise, in the band of a weak partial), it keeps its last measured
value.
"""

from __future__ import annotations

import json
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge.modes import BandExtractor
from forge.takes import sound_end
from forge import progress

STEP_S = 0.01      # curve resolution
WINDOW_S = 0.02    # power window
LENGTH_S = 2.9     # curve length (the release takes keep 3 s after the key release)
FLOOR_DB = -100.0
GROUP_FIRST = (1, 2, 4, 8, 16, 32)  # first partial number of each group (octaves of partial numbers)
GROUP_FLOOR_DB = 50.0               # a group this far under its level at the release carries no measurement
GROUP_LIMITS_DB = (-60.0, 12.0)     # bounds of a group's level against the overall curve


def group_of(n: int) -> int:
    return max(i for i, first in enumerate(GROUP_FIRST) if n >= first)


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


def _group_powers(audio: np.ndarray, off: int, fs: int, count: int, partials: list[tuple[int, float]],
                  f0: float) -> np.ndarray:
    """Power of each group of partials (groups x count): the bands around its partials, 20 ms windows every 10 ms
    from the release (as _powers)."""
    start = max(0, off - round(0.1 * fs))
    seg = audio[:, start:off + round((LENGTH_S + 0.2) * fs)]
    extractor = BandExtractor(seg, fs)
    pass_hz = min(0.33 * f0, 30.0)
    stop_hz = min(0.66 * f0, 2 * pass_hz)
    out = np.zeros((len(GROUP_FIRST), count))
    for n, centre in partials:
        if centre + stop_hz >= fs / 2:
            continue
        z, fs_d = extractor.baseband(centre, pass_hz, stop_hz)
        cumulative = np.concatenate([[0.0], np.cumsum(np.mean(np.abs(z) ** 2, axis=0))])
        t = (off - start) / fs + np.arange(count) * STEP_S
        a = np.clip((t * fs_d).astype(int), 0, len(cumulative) - 1)
        b = np.clip(((t + WINDOW_S) * fs_d).astype(int), 0, len(cumulative) - 1)
        out[group_of(n)] += (cumulative[b] - cumulative[a]) / np.maximum(b - a, 1)
    return out


def _group_task(args) -> tuple[int, float, dict[int, tuple[np.ndarray, np.ndarray]]]:
    """Group powers of the takes of one (note, hold): (note, hold, velocity -> (release, held), groups x count)."""
    note, hold, takes, count = args
    out = {}
    for velocity, release_path, held_path, off, partials, f0 in takes:
        rel, fs = _load(Path(release_path))
        held, _ = _load(Path(held_path))
        out[velocity] = (_group_powers(rel, off, fs, count, partials, f0), _group_powers(held, off, fs, count, partials, f0))
    return note, hold, out


def group_curves(rel: np.ndarray, held: np.ndarray, overall: np.ndarray) -> np.ndarray:
    """Per group, the level after the release against the overall curve `overall` (count + 1 points, 0 at the
    release), dB: smoothed over 50 ms, held at its last measured value where the group is under its floor."""
    count = rel.shape[1]
    out = np.zeros((rel.shape[0], count + 1))
    for g in range(rel.shape[0]):
        if held[g, 0] <= 0:
            continue
        d = 10 * np.log10(np.maximum(rel[g], 1e-30) / np.maximum(held[g], 1e-30))
        d = np.convolve(np.pad(d, 2, mode="edge"), np.ones(5) / 5, mode="valid")
        valid = (held[g] > held[g, 0] * 10 ** (-GROUP_FLOOR_DB / 10)) & (rel[g] > 1e-14) & (d < 1.0)
        relative = np.clip(d - overall[1:], *GROUP_LIMITS_DB)
        last = 0.0
        for k in range(count):
            if valid[k] and (k < 2 or valid[k - 2]) and (k + 2 >= count or valid[k + 2]):
                last = relative[k]
            out[g, k + 1] = last
    return out


def _curve(ratio: np.ndarray) -> np.ndarray:
    """dB curve (0 at the release, then count points) from a power ratio: smoothed over 50 ms, non-increasing."""
    d = 10 * np.log10(np.maximum(ratio, 10 ** (FLOOR_DB / 10)))
    d = np.convolve(np.pad(d, 2, mode="edge"), np.ones(5) / 5, mode="valid")
    d = np.clip(_isotonic_decreasing(np.minimum(d, 0.0)), FLOOR_DB, 0.0)
    return np.concatenate([[0.0], d])


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
    group_db = np.zeros((len(notes), len(holds), len(GROUP_FIRST), count + 1))
    velocities = sorted({json.loads(p.read_text(encoding="utf-8"))["velocity"] for ms in groups.values() for p in ms})
    by_velocity = np.zeros((len(velocities), len(notes), len(holds), count + 1))
    group_by_velocity = np.zeros((len(velocities), len(notes), len(holds), len(GROUP_FIRST), count + 1))
    modes = {}  # velocity -> midi -> partials (n, centre) and f0, from forge modes
    group_tasks = []
    for (note, hold), metas in sorted(groups.items()):
        ratios, ratio_of = [], {}
        for meta_path in metas:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            held_path = next(single.glob(f"{note:03d}-*_v{meta['velocity']:03d}_h*.flac"), None)
            if held_path is None:
                continue
            rel, fs = _load(releases / Path(meta["file"]).name)
            held, _ = _load(held_path)
            off = meta["note_off_sample"]
            p_rel, p_held = _powers(rel, off, fs, count), _powers(held, off, fs, count)
            ratio = np.where(p_held > 1e-20, p_rel / np.maximum(p_held, 1e-30), 1.0)
            # Where a sample cuts the held note short (an SFZ bank), the ratio past the cut compares silences: it
            # keeps its last measured value.
            ends = [e for e in (sound_end(json.loads(held_path.with_suffix(".json").read_text(encoding="utf-8"))),
                                sound_end(meta)) if e is not None]
            if ends:
                valid = max(1, (min(ends) - off - round(WINDOW_S * fs)) // round(STEP_S * fs) + 1)
                if valid < count:
                    ratio[valid:] = ratio[valid - 1]
            ratios.append(ratio)
            v = meta["velocity"]
            ratio_of[v] = ratios[-1]
            if v not in modes and (corpus / f"modes-v{v:03d}.json").exists():
                doc = json.loads((corpus / f"modes-v{v:03d}.json").read_text(encoding="utf-8"))
                modes[v] = {n["midi"]: ([(p["n"], p["centre"]) for p in n["partials"]], n["f0"]) for n in doc["notes"]}
            if note in modes.get(v, {}):
                partials, f0 = modes[v][note]
                group_tasks.append(((note, hold), (v, str(releases / Path(meta["file"]).name), str(held_path), off,
                                                   partials, f0)))
        if not ratios:
            continue
        i, j = notes.index(note), holds.index(hold)
        curves[i, j] = d = _curve(np.mean(ratios, axis=0))  # 0 dB at the release
        for k, v in enumerate(velocities):  # a velocity not measured here takes the average
            by_velocity[k, i, j] = _curve(ratio_of[v]) if v in ratio_of else d
        log(f"  note {note:3d} hold {hold:4.1f} s: -{-d[10]:.1f} dB at 0.1 s, -{-d[30]:.1f} dB at 0.3 s, "
            f"-{-d[min(100, count)]:.1f} dB at 1 s ({len(ratios)} velocities; at 0.6 s per velocity "
            + ", ".join(f"v{v} {by_velocity[k, i, j, 60]:.1f}" for k, v in enumerate(velocities)) + ")")

    # Per group of partials, against the overall curve (the bands of every partial: in parallel).
    by_key: dict[tuple[int, float], list] = defaultdict(list)
    for key, take in group_tasks:
        by_key[key].append(take)
    with ProcessPoolExecutor() as pool:
        group_tasks_list = [(k[0], k[1], t, count) for k, t in sorted(by_key.items())]
        for note, hold, powers in progress.track(pool.map(_group_task, group_tasks_list), len(group_tasks_list),
                                                 "notes", "partial groups"):
            i, j = notes.index(note), holds.index(hold)
            group_db[i, j] = group_curves(sum(p[0] for p in powers.values()), sum(p[1] for p in powers.values()),
                                          curves[i, j])
            for k, v in enumerate(velocities):  # against that velocity's own overall curve
                group_by_velocity[k, i, j] = (group_curves(*powers[v], by_velocity[k, i, j]) if v in powers
                                              else group_db[i, j])
    at = round(0.3 / STEP_S)
    for i, note in enumerate(notes):
        log(f"  note {note:3d}, against the overall curve at 0.3 s (hold {holds[-1]:.1f} s), groups "
            + " ".join(f"{GROUP_FIRST[g]}+: {group_db[i, -1, g, at]:+.1f}" for g in range(len(GROUP_FIRST))))

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
            "curves_db": np.round(curves[keep], 2).tolist(),
            "group_first": list(GROUP_FIRST), "group_db": np.round(group_db[keep], 2).tolist(),
            "velocities": velocities, "curves_db_by_velocity": np.round(by_velocity[:, keep], 2).tolist(),
            "group_db_by_velocity": np.round(group_by_velocity[:, keep], 2).tolist()}


def write(corpus: Path, log=print) -> Path:
    data = measure(corpus, log)
    out = corpus / "release-curves.json"
    out.write_text(json.dumps(data), encoding="utf-8")
    return out
