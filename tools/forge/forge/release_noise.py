"""`forge release-noise`: the knock of the key release, as an excitation of the soundboard resonances.

When the damper lands, the reference adds a low thump (centroid near 100 Hz, measured on 2026-11-06: 25 to 40 dB
under the note at the release on the damped notes, none on the undamped top section) that rings for about a
second. The release takes are bit-identical to the same note held until the key release, so their difference
holds the damping of the note and that noise; away from the note's partials (the bins the soundboard fit leaves
out, forge soundboard), only the noise is left (the body set ringing by the strike has died out on the longer
holds). The noise is fitted there, per note and velocity of the releases pass, on the shared soundboard
resonances (fit_note), from the release: the engine adds those amplitudes to the soundboard when a damper lands.

The noise is weaker the longer the key was held (measured on 2026-11-06, against a 2 s hold: +6 dB after 0.5 s,
-3 to -12 dB after 10 s on the damped notes, about -10 dB per decade): its level is also measured per hold, against
the one kept, on the holds of the releases pass and on the 10 s holds of the interactions session (key released
after 10 s without the pedal, v40: the same recording as the single-notes take at v40 until the release); the engine
interpolates in log(hold). Without that, a pedal lifted after a long note knocked 7 to 10 dB too loud.

release-noise.json: notes, velocities, holds_used, amps [note][velocity] -> [[l re, l im, r re, r im] per
resonance] (complex amplitude at the release), level_db [note][velocity] (RMS of the fitted noise over 1 s, dBFS),
hold_s and hold_db (the level after each hold against the one kept, median over the notes and velocities).
"""

from __future__ import annotations

import json
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge import soundboard
from forge.modes import BandExtractor
from forge import progress
from forge.takes import sound_end

HOLDS_USED = (0.5, 2.0)   # after a short hold the strike's own body still rings, which the difference also holds
LONG_HOLD_S, LONG_HOLD_VELOCITY = 10.0, 40  # the interactions session's pedal-up takes (single-notes has v40)
LEAD_S, LENGTH_S = 0.05, 1.5  # the fit window, around the release (the takes are identical before it)
WEIGHT_PER_S = 0.0        # a transient: every instant counts alike (the body fit weighs its tail)


def _fit_task(args) -> tuple[int, int, float, np.ndarray]:
    """(note, velocity, hold, amplitudes channels x resonances at the release) from one release take; the held take
    is read from its own sample of the release (the takes may start apart)."""
    release_path, held_path, off, off_held, note_modes, freqs, decays, velocity, hold = args
    rel, fs = sf.read(release_path, dtype="float64", always_2d=True)
    held, _ = sf.read(held_path, dtype="float64", always_2d=True)
    lead, n = int(round(LEAD_S * fs)), int(round(LENGTH_S * fs))
    d = (rel[off - lead:off + n] - held[off_held - lead:off_held + n]).T
    lo, hi = soundboard.BAND
    centre = (lo + hi) / 2
    z, fs_d = BandExtractor(d, fs).baseband(centre, (hi - lo) / 2, (hi - lo) / 2 + 20)
    poles = np.exp((-np.asarray(decays) + 2j * np.pi * (np.asarray(freqs) - centre)) / fs_d)
    partials, half_width = soundboard.excluded(note_modes)
    amps = soundboard.fit_note(z, fs_d, poles, partials, half_width, weight_per_s=WEIGHT_PER_S)
    return note_modes["midi"], velocity, hold, amps * poles[None, :] ** (LEAD_S * fs_d)  # from the window start


def run(corpus: Path, jobs: int = 0, interactions: Path | None = None) -> list[Path]:
    """corpus: the reference state folder; interactions: the same state in the interactions session (its 10 s
    holds; default corpus/interactions/<state>, optional)."""
    interactions = interactions or corpus.parent.parent / "interactions" / corpus.name
    freqs, decays, _ = soundboard.load(corpus / f"soundboard-v{soundboard.POLES_VELOCITY:03d}.json")
    modes, tasks = {}, []

    measured = sorted(int(p.stem.split("-v")[1]) for p in corpus.glob("modes-v*.json"))
    skipped = 0

    def usable(meta: dict, held_meta: dict, at: int, at_held: int, fs: int) -> bool:
        """Both takes hold the note through the fit window: the held one is no shorter than it, and neither
        recording stops inside it (an SFZ sample cut short: the difference would be the whole note, not the knock)."""
        end = int(round(LENGTH_S * fs))
        length = int(round(held_meta.get("duration_s", 0.0) * fs)) or None
        if length is not None and at_held + end > length:
            return False
        for m, a in ((meta, at), (held_meta, at_held)):
            stop = sound_end(m)
            if stop is not None and stop < a + end:
                return False
        return True

    def modes_of(v: int) -> dict:
        """The modes at velocity v, or at the nearest velocity measured (a bank measured on fewer layers: the
        partials sit at the same frequencies, which is what the body fit needs to leave out)."""
        if v not in modes:
            nearest = min(measured, key=lambda m: abs(m - v)) if measured else v
            modes[v] = {n["midi"]: n for n in json.loads((corpus / f"modes-v{nearest:03d}.json").read_text(encoding="utf-8"))["notes"]}
        return modes[v]

    for meta_path in sorted((corpus / "releases").glob("*.json")):
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        hold = round(meta["hold"], 3)
        if hold not in HOLDS_USED:
            continue
        v, note = meta["velocity"], meta["note"]
        held = next((corpus / "single-notes").glob(f"{note:03d}-*_v{v:03d}_h*.flac"), None)
        if held is None or note not in modes_of(v):
            continue
        held_meta = json.loads(held.with_suffix(".json").read_text(encoding="utf-8"))
        if not usable(meta, held_meta, meta["note_off_sample"], meta["note_off_sample"], sf.info(str(held)).samplerate):
            skipped += 1
            continue
        tasks.append((str(meta_path.with_suffix(".flac")), str(held), meta["note_off_sample"], meta["note_off_sample"],
                      modes_of(v)[note], freqs.tolist(), decays.tolist(), v, hold))
    # The long holds: key released after 10 s without the pedal, against the single-notes take of the same velocity
    # (aligned on the note-on).
    for take in sorted((interactions / "pedal-up").glob(f"*_v{LONG_HOLD_VELOCITY:03d}_*.flac")):
        meta = json.loads(take.with_suffix(".json").read_text(encoding="utf-8"))
        note = meta["note"]
        held = next((corpus / "single-notes").glob(f"{note:03d}-*_v{LONG_HOLD_VELOCITY:03d}_h*.flac"), None)
        if held is None or note not in modes_of(LONG_HOLD_VELOCITY) or round(meta["hold"], 1) != LONG_HOLD_S:
            continue
        held_meta = json.loads(held.with_suffix(".json").read_text(encoding="utf-8"))
        on_held = held_meta["note_on_sample"]
        off = meta["note_off_sample"]
        if not usable(meta, held_meta, off, off - meta["note_on_sample"] + on_held, sf.info(str(held)).samplerate):
            skipped += 1
            continue
        tasks.append((str(take), str(held), off, off - meta["note_on_sample"] + on_held, modes_of(LONG_HOLD_VELOCITY)[note],
                      freqs.tolist(), decays.tolist(), LONG_HOLD_VELOCITY, LONG_HOLD_S))
    if skipped:
        print(f"  {skipped} takes left out: a recording stops before the end of the fit window")
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        fitted = list(progress.track(pool.map(_fit_task, tasks), len(tasks), "takes", "release knocks"))
    t = np.arange(int(44100 * 1.0)) / 44100.0

    def level(amps: np.ndarray) -> float:
        body = soundboard.synthesize_body(freqs, decays, amps, len(t), 44100.0)
        return 10 * np.log10(float(np.mean(body ** 2)) + 1e-30)

    by_key: dict[tuple[int, int], list[np.ndarray]] = defaultdict(list)
    level_at: dict[tuple[int, int], dict[float, float]] = defaultdict(dict)  # (note, velocity) -> hold -> dBFS
    for note, v, hold, amps in fitted:
        level_at[(note, v)][hold] = level(amps)
        if hold in HOLDS_USED:
            by_key[(note, v)].append(amps)
    notes = sorted({k[0] for k in by_key})
    velocities = sorted({k[1] for k in by_key})
    out_amps, levels = [], []
    for note in notes:
        row_amps, row_levels = [], []
        for v in velocities:
            group = by_key.get((note, v), [])
            if not group:
                row_amps.append(np.zeros((2, len(freqs)), dtype=np.complex128))
                row_levels.append(-300.0)
                continue
            # The holds give two estimates of one noise: the one of median energy (a failed fit is an outlier).
            energies = [float(np.sum(np.abs(a) ** 2)) for a in group]
            amps = group[int(np.argsort(energies)[len(energies) // 2])]
            row_amps.append(amps)
            row_levels.append(level(amps))
        out_amps.append(row_amps)
        levels.append(row_levels)
        print(f"  note {note:3d}: release noise " + "  ".join(f"v{v}: {lv:6.1f} dBFS" for v, lv in zip(velocities, row_levels)))

    # The level against the hold: per hold, against the level kept (at the long hold's velocity, interpolated in dB
    # between the measured ones), median over the notes and velocities where the noise is measurable.
    kept = {(n, v): levels[i][j] for i, n in enumerate(notes) for j, v in enumerate(velocities)}

    def kept_at(note: int, v: int) -> float | None:
        if (note, v) in kept:
            return kept[(note, v)]
        lo = max((u for u in velocities if u <= v), default=None)
        hi = min((u for u in velocities if u >= v), default=None)
        if lo is None or hi is None or (note, lo) not in kept or (note, hi) not in kept:
            return None
        return kept[(note, lo)] + (kept[(note, hi)] - kept[(note, lo)]) * (v - lo) / max(hi - lo, 1)

    hold_s, hold_db = [], []
    for hold in sorted({h for byh in level_at.values() for h in byh}):
        d = [byh[hold] - ref for (note, v), byh in level_at.items()
             if hold in byh and (ref := kept_at(note, v)) is not None and ref > -120 and byh[hold] > -120]
        if len(d) >= 3:
            hold_s.append(hold)
            hold_db.append(round(float(np.median(d)), 2))
            print(f"  after a {hold:g} s hold: {hold_db[-1]:+.1f} dB against the level kept ({len(d)} notes x velocities)")
    out = corpus / "release-noise.json"
    out.write_text(json.dumps({
        "source": str(corpus), "notes": notes, "velocities": velocities, "holds_used": HOLDS_USED,
        "hold_s": hold_s, "hold_db": hold_db,
        "resonances": len(freqs), "level_db": [[round(x, 2) for x in r] for r in levels],
        "amps": [[[[float(f"{a[0, k].real:.6e}"), float(f"{a[0, k].imag:.6e}"),
                    float(f"{a[1, k].real:.6e}"), float(f"{a[1, k].imag:.6e}")] for k in range(a.shape[1])]
                  for a in row] for row in out_amps]}, separators=(",", ":")),
        encoding="utf-8")
    return [out]


def load(path: Path) -> tuple[list[int], list[int], np.ndarray]:
    """(notes, velocities, amplitudes notes x velocities x resonances x 4: l re, l im, r re, r im)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return data["notes"], data["velocities"], np.asarray(data["amps"], dtype=np.float64)
