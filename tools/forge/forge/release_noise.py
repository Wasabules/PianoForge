"""`forge release-noise`: the knock of the key release, as an excitation of the soundboard resonances.

When the damper lands, the reference adds a low thump (centroid near 100 Hz, measured on 2026-11-06: 25 to 40 dB
under the note at the release on the damped notes, none on the undamped top section) that rings for about a
second. The release takes are bit-identical to the same note held until the key release, so their difference
holds the damping of the note and that noise; away from the note's partials (the bins the soundboard fit leaves
out, forge soundboard), only the noise is left (the body set ringing by the strike has died out on the longer
holds). The noise is fitted there, per note and velocity of the releases pass, on the shared soundboard
resonances (fit_note), from the release: the engine adds those amplitudes to the soundboard when a damper lands.

release-noise.json: notes, velocities, holds_used, amps [note][velocity] -> [[l re, l im, r re, r im] per
resonance] (complex amplitude at the release), level_db [note][velocity] (RMS of the fitted noise over 1 s, dBFS).
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

HOLDS_USED = (0.5, 2.0)   # after a short hold the strike's own body still rings, which the difference also holds
LEAD_S, LENGTH_S = 0.05, 1.5  # the fit window, around the release (the takes are identical before it)
WEIGHT_PER_S = 0.0        # a transient: every instant counts alike (the body fit weighs its tail)


def _fit_task(args) -> tuple[int, int, np.ndarray]:
    """(note, velocity, amplitudes channels x resonances at the release) from one release take."""
    release_path, held_path, off, note_modes, freqs, decays = args
    rel, fs = sf.read(release_path, dtype="float64", always_2d=True)
    held, _ = sf.read(held_path, dtype="float64", always_2d=True)
    lead, n = int(round(LEAD_S * fs)), int(round(LENGTH_S * fs))
    d = (rel[off - lead:off + n] - held[off - lead:off + n]).T
    lo, hi = soundboard.BAND
    centre = (lo + hi) / 2
    z, fs_d = BandExtractor(d, fs).baseband(centre, (hi - lo) / 2, (hi - lo) / 2 + 20)
    poles = np.exp((-np.asarray(decays) + 2j * np.pi * (np.asarray(freqs) - centre)) / fs_d)
    partials, half_width = soundboard.excluded(note_modes)
    amps = soundboard.fit_note(z, fs_d, poles, partials, half_width, weight_per_s=WEIGHT_PER_S)
    return note_modes["midi"], json.loads(Path(release_path).with_suffix(".json").read_text(encoding="utf-8"))["velocity"], \
        amps * poles[None, :] ** (LEAD_S * fs_d)  # from the window start to the release


def run(corpus: Path, jobs: int = 0) -> list[Path]:
    freqs, decays, _ = soundboard.load(corpus / f"soundboard-v{soundboard.POLES_VELOCITY:03d}.json")
    modes, tasks = {}, []
    for meta_path in sorted((corpus / "releases").glob("*.json")):
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        if round(meta["hold"], 3) not in HOLDS_USED:
            continue
        v, note = meta["velocity"], meta["note"]
        if v not in modes:
            modes[v] = {n["midi"]: n for n in json.loads((corpus / f"modes-v{v:03d}.json").read_text(encoding="utf-8"))["notes"]}
        held = next((corpus / "single-notes").glob(f"{note:03d}-*_v{v:03d}_h*.flac"), None)
        if held is None or note not in modes[v]:
            continue
        tasks.append((str(meta_path.with_suffix(".flac")), str(held), meta["note_off_sample"], modes[v][note],
                      freqs.tolist(), decays.tolist()))
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        fitted = list(pool.map(_fit_task, tasks))
    by_key: dict[tuple[int, int], list[np.ndarray]] = defaultdict(list)
    for note, v, amps in fitted:
        by_key[(note, v)].append(amps)
    notes = sorted({k[0] for k in by_key})
    velocities = sorted({k[1] for k in by_key})
    t = np.arange(int(44100 * 1.0)) / 44100.0
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
            body = soundboard.synthesize_body(freqs, decays, amps, len(t), 44100.0)
            row_amps.append(amps)
            row_levels.append(10 * np.log10(float(np.mean(body ** 2)) + 1e-30))
        out_amps.append(row_amps)
        levels.append(row_levels)
        print(f"  note {note:3d}: release noise " + "  ".join(f"v{v}: {lv:6.1f} dBFS" for v, lv in zip(velocities, row_levels)))
    out = corpus / "release-noise.json"
    out.write_text(json.dumps({
        "source": str(corpus), "notes": notes, "velocities": velocities, "holds_used": HOLDS_USED,
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
