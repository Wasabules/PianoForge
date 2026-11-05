"""`forge export`: the modal profile played by the C++ engine, from the measurements of every velocity layer.

Inputs, in the corpus state folder: modes-vNNN.json (`forge modes`) and soundboard-vNNN.json (`forge soundboard`)
for each velocity layer, release-curves.json (`forge release-curves`), and the single-notes takes themselves for
the attack residuals.

Per note and per layer, the profile holds:
  - the modes of every partial (frequency, decay, complex amplitude per channel at the note-on);
  - the soundboard body: the complex amplitude of each shared resonance;
  - the attack residual: what neither the strings nor the body explain over the first RESIDUAL_S (the hammer,
    the knock, the first instants before the modes settle), faded out over its last RESIDUAL_FADE_S.
The layers are kept as they were measured: each one is a real strike, with its own beating phases. Interpolating
amplitudes between two strikes gives neither (studies/velocity_linearity.py); the engine plays the nearest layer
with the levels interpolated per partial.

Binary format (little endian), modal-profile.bin:
  header      char[8] "PFMODAL1", u32 version (1), f64 residual sample rate, u32 channels (2)
  body        u32 P, then P x (f64 freq Hz, f64 decay 1/s)
  dampers     f64 step s, u32 notes, u32 holds, u32 points, notes x i32 midi, holds x f64 hold s,
              notes x holds x points x f32 dB (level after the key release against the note held)
  notes       u32 count, then per note:
                u32 midi, u32 layers, f64 f0, then per layer:
                  u32 velocity, u32 partials, u32 modes, u32 residual frames,
                  partials x (u32 partial number, u32 modes of this partial),
                  modes x (f64 freq Hz, f64 decay 1/s, f32 left re, f32 left im, f32 right re, f32 right im),
                  P x (f32 left re, f32 left im, f32 right re, f32 right im)   body amplitudes,
                  f32 residual scale, frames x channels x i16 (interleaved; sample = i16 x scale)
Convention: x_c(t) = Re(a_c exp((-decay + 2 pi i freq) t)), t in seconds from the note-on.
The profile is calibrated on a commercial library: it stays in the corpus folder and is never published.
"""

from __future__ import annotations

import json
import struct
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge import soundboard
from forge.modes import synthesize
from forge.note_modes import clean, partial_modes

MAGIC, VERSION = b"PFMODAL1", 1
RESIDUAL_S, RESIDUAL_FADE_S = 0.10, 0.04


def velocities(corpus: Path) -> list[int]:
    """The velocity layers for which both the modes and the soundboard have been measured."""
    found = []
    for path in sorted(corpus.glob("modes-v*.json")):
        v = int(path.stem.split("-v")[1])
        if (corpus / f"soundboard-v{v:03d}.json").exists():
            found.append(v)
    return found


def residual(path: Path, note: dict, body_freqs, body_decays, body_amps) -> tuple[np.ndarray, int]:
    """Attack residual of one take (frames x channels), faded out, at the take's sample rate."""
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    n = int(round(RESIDUAL_S * fs))
    on = meta["note_on_sample"]
    x = audio[on:on + n].T
    model = synthesize(partial_modes(note), x.shape[0], n, fs)
    model += soundboard.synthesize_body(body_freqs, body_decays, body_amps, n, fs)
    r = x - model
    fade = int(round(RESIDUAL_FADE_S * fs))
    r[:, n - fade:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, fade)))
    return r.T, fs


def _layer_task(args) -> dict:
    path, note, body = args
    freqs, decays, amps = body
    r, fs = residual(Path(path), note, np.asarray(freqs), np.asarray(decays), np.asarray(amps))
    return {"midi": note["midi"], "velocity": note["velocity"], "residual": r, "fs": fs}


def _layer_bytes(note: dict, body_amps: np.ndarray, r: np.ndarray) -> bytes:
    partials = [p for p in note["partials"] if p["modes"]]
    modes = [m for p in partials for m in p["modes"]]
    out = [struct.pack("<4I", note["velocity"], len(partials), len(modes), r.shape[0])]
    out.append(np.array([(p["n"], len(p["modes"])) for p in partials], dtype="<u4").tobytes())
    rec = np.zeros(len(modes), dtype=[("f", "<f8"), ("d", "<f8"), ("a", "<f4", (4,))])
    for i, m in enumerate(modes):
        rec[i] = (m[0], m[1], m[2:6])
    out.append(rec.tobytes())
    out.append(np.stack([body_amps.real[0], body_amps.imag[0], body_amps.real[1], body_amps.imag[1]],
                        axis=1).astype("<f4").tobytes())
    # Full scale is 1: anything far above it is a failed fit upstream, not a piano.
    largest = max([float(np.max(np.abs(rec["a"]))) if len(rec) else 0.0, float(np.max(np.abs(body_amps)))])
    peak = float(np.max(np.abs(r))) or 1.0
    if not np.isfinite(peak) or peak > 10.0 or not np.isfinite(largest) or largest > 10.0:
        raise SystemExit(f"note {note['midi']} velocity {note['velocity']}: absurd amplitudes (modes or body "
                         f"{largest:.3g}, residual {peak:.3g}); run forge soundboard again for this velocity")
    out.append(struct.pack("<f", peak / 32767.0))
    out.append(np.round(r / peak * 32767.0).clip(-32767, 32767).astype("<i2").tobytes())
    return b"".join(out)


BODY_OUTLIER = 15.0  # a body this many times louder (against its strings) than the note's median layer is a failed fit


def string_level(note: dict) -> float:
    """RMS amplitude of the note's modes over the first second, cross terms ignored (as the engine computes it)."""
    e = 0.0
    for p in note["partials"]:
        for m in p["modes"]:
            a2 = 0.5 * sum(v * v for v in m[2:6])
            x = 2.0 * max(m[1], 1e-6)
            e += 0.5 * a2 * (1.0 - np.exp(-x)) / x
    return float(np.sqrt(e))


def repair_bodies(modes: dict, boards: dict, layers: list[int]) -> list[tuple[int, int]]:
    """The soundboard fit fails on a few takes (A0 at velocity 40: 130 times too loud), where the partials leave
    almost no bin to measure the body in. Against the strings of the same take, the body is about as loud from one
    layer to the next; a layer BODY_OUTLIER times above the note's median gets the body of the median layer instead,
    scaled to its own strings. Returns the (note, velocity) repaired."""
    repaired = []
    for m in sorted({m for v in layers for m in boards[v][2]}):
        ratio = {}
        for v in layers:
            if m in boards[v][2] and m in modes[v] and string_level(modes[v][m]) > 0:
                ratio[v] = float(np.sqrt(np.mean(np.abs(boards[v][2][m]) ** 2))) / string_level(modes[v][m])
        if len(ratio) < 3:
            continue
        median = float(np.median(list(ratio.values())))
        ref = min(ratio, key=lambda v: abs(ratio[v] - median))
        for v, r in ratio.items():
            if r > BODY_OUTLIER * median:
                boards[v][2][m] = boards[ref][2][m] * (string_level(modes[v][m]) / string_level(modes[ref][m]))
                repaired.append((m, v))
    return repaired


def run(corpus: Path, jobs: int = 0) -> list[Path]:
    layers = velocities(corpus)
    if not layers:
        raise SystemExit(f"No velocity layer with both modes-vNNN.json and soundboard-vNNN.json in {corpus}")
    start = time.time()
    modes, dropped_modes, dropped_partials = {}, 0, 0
    for v in layers:
        modes[v] = {}
        for n in json.loads((corpus / f"modes-v{v:03d}.json").read_text(encoding="utf-8"))["notes"]:
            modes[v][n["midi"]], dm, dp = clean(n)
            dropped_modes, dropped_partials = dropped_modes + dm, dropped_partials + dp
    print(f"  failed fits left out: {dropped_modes} modes, {dropped_partials} partials")
    boards = {v: soundboard.load(corpus / f"soundboard-v{v:03d}.json") for v in layers}
    freqs, decays = boards[layers[0]][0], boards[layers[0]][1]
    for v in layers[1:]:
        if len(boards[v][0]) != len(freqs) or np.max(np.abs(boards[v][0] - freqs)) > 1e-3:
            raise SystemExit(f"soundboard-v{v:03d}.json does not share the resonances of the other layers")
    if repaired := repair_bodies(modes, boards, layers):
        print(f"  soundboard fits replaced by the note's median layer: {repaired}")
    takes = {}
    for v in layers:
        for p in (corpus / "single-notes").glob(f"*_v{v:03d}_h*.flac"):
            takes[(int(p.name[:3]), v)] = p
    tasks = [(str(takes[(m, v)]), modes[v][m], (freqs, decays, boards[v][2][m]))
             for v in layers for m in sorted(modes[v]) if (m, v) in takes and m in boards[v][2]]
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        residuals = {(r["midi"], r["velocity"]): r for r in pool.map(_layer_task, tasks)}
    print(f"  residuals of {len(residuals)} takes ({time.time() - start:.0f} s)")
    rates = {r["fs"] for r in residuals.values()}
    if len(rates) != 1:
        raise SystemExit(f"Takes at several sample rates: {sorted(rates)}")

    curves = json.loads((corpus / "release-curves.json").read_text(encoding="utf-8"))
    db = np.asarray(curves["curves_db"], dtype="<f4")  # notes x holds x points
    out = [MAGIC, struct.pack("<IdI", VERSION, float(rates.pop()), 2)]
    out.append(struct.pack("<I", len(freqs)))
    out.append(np.stack([freqs, decays], axis=1).astype("<f8").tobytes())
    out.append(struct.pack("<d3I", curves["step_s"], db.shape[0], db.shape[1], db.shape[2]))
    out.append(np.asarray(curves["notes"], dtype="<i4").tobytes())
    out.append(np.asarray(curves["holds_s"], dtype="<f8").tobytes())
    out.append(db.tobytes())
    # A layer whose partials were all too quiet to analyse would play as silence: it is left out.
    notes = {}
    for m in sorted({m for (m, _) in residuals}):
        present = [v for v in layers if (m, v) in residuals and any(p["modes"] for p in modes[v][m]["partials"])]
        skipped = [v for v in layers if (m, v) in residuals and v not in present]
        if skipped:
            print(f"  note {m}: layers {skipped} left out (no partial analysed)")
        if present:
            notes[m] = present
    midis = list(notes)
    out.append(struct.pack("<I", len(midis)))
    total_modes = 0
    for m, present in notes.items():
        out.append(struct.pack("<2Id", m, len(present), modes[present[0]][m]["f0"]))
        for v in present:
            note = modes[v][m]
            out.append(_layer_bytes(note, boards[v][2][m], residuals[(m, v)]["residual"]))
            total_modes += sum(len(p["modes"]) for p in note["partials"])
    path = corpus / "modal-profile.bin"
    path.write_bytes(b"".join(out))
    print(f"  {len(midis)} notes x {len(layers)} layers, {total_modes} modes, "
          f"{path.stat().st_size / 1e6:.1f} MB ({time.time() - start:.0f} s)")
    return [path]
