"""`forge modes`: the modes of every partial of every note of a corpus state, at one velocity.

For each single-notes take, the partials are found (stiff-string law, partials.py) and each one is resolved into
its modes (modes.py): the unison strings and their polarisations, with frequency, decay and complex amplitude per
channel. The result is written as JSON, the measured ground truth for the coupled-string model and the engine.

modes-v064.json:
  notes[]: midi, name, f0, inharmonicity, note_on_s, hold_s,
           partials[]: n, centre, t0, t1 (analysis window), envelope_error_db, noise_db,
                       modes[]: [freq Hz, decay 1/s, re0, im0, re1, im1]  (complex amplitude per channel at t = 0)
  convention: x_c(t) = Re(a_c exp((-decay + 2 pi i freq) t)), t in seconds from the note-on.
"""

from __future__ import annotations

import json
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge.modes import BandExtractor, Mode, PartialModes, analyze_partial
from forge.partials import discover_partials
from forge.takes import held_end
from forge import progress

CONVENTION = "x_c(t) = Re(a_c exp((-decay + 2 pi i freq) t)), t in seconds from the note-on"


def analyze_take(path: Path, max_partials: int = 80, floor_db: float = -80.0) -> dict:
    """Modes of one single-notes take, as a JSON-ready dict. `floor_db`: partials and tails quieter than the
    loudest partial by more than this are not analysed (inaudible next to it)."""
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    on, off = meta["note_on_sample"], held_end(meta)
    x = audio[on:off].T
    midi = meta["note"]
    f0, B, partials = discover_partials(x.mean(axis=0), fs, midi, 0)
    extractor = BandExtractor(x, fs)
    pass_hz, stop_hz = min(0.33 * f0, 30.0), min(0.66 * f0, 60.0)
    peak = max(float(np.max(np.abs(extractor.baseband(p.freq, pass_hz, stop_hz)[0]))) for p in partials[:8])
    result = {"midi": midi, "name": meta["note_name"], "velocity": meta["velocity"], "f0": f0, "inharmonicity": B,
              "note_on_s": on / fs, "hold_s": (off - on) / fs, "partials": []}
    for p in partials[:max_partials]:
        modes = analyze_partial(extractor, p.n, p.freq, f0, audible_floor=peak * 10 ** (floor_db / 20))
        if modes is None:
            continue
        result["partials"].append({
            "n": p.n, "centre": round(p.freq, 4), "t0": round(modes.t0, 3), "t1": round(modes.t1, 3),
            "envelope_error_db": round(modes.envelope_error_db, 2), "noise_db": round(modes.noise_db, 1),
            "modes": [[round(m.freq, 5), round(m.decay, 5)] + [float(f"{v:.6e}") for a in m.amps for v in (a.real, a.imag)]
                      for m in modes.modes]})
    return result


def partial_modes(note: dict) -> list[PartialModes]:
    """The partials of one note of a modes JSON, back as PartialModes (for synthesis)."""
    out = []
    for p in note["partials"]:
        modes = [Mode(m[0], m[1], [complex(m[2 + 2 * c], m[3 + 2 * c]) for c in range((len(m) - 2) // 2)])
                 for m in p["modes"]]
        out.append(PartialModes(p["n"], p["centre"], modes, p["t0"], p["t1"], envelope_error_db=p["envelope_error_db"]))
    return out


MAX_EXTRAPOLATION = 100.0   # a mode at t = 0 at most 40 dB above its level at the start of its analysis window
MAX_ENVELOPE_ERROR_DB = 20.0


def clean(note: dict) -> tuple[dict, int, int]:
    """The note without its failed fits: modes so fast that their amplitude at the note-on is extrapolated by
    more than MAX_EXTRAPOLATION from their analysis window (they would ring on, absurdly loud, after the attack
    residual), and partials whose envelope error shows that the fit diverged. Returns (note, modes dropped,
    partials dropped). About 0.1 % of the modes and 1 % of the partials of the reference corpus."""
    partials, modes_dropped, partials_dropped = [], 0, 0
    for p in note["partials"]:
        if p["envelope_error_db"] > MAX_ENVELOPE_ERROR_DB:
            partials_dropped += 1
            continue
        kept = [m for m in p["modes"] if np.exp(m[1] * p["t0"]) <= MAX_EXTRAPOLATION]
        modes_dropped += len(p["modes"]) - len(kept)
        if kept:
            partials.append({**p, "modes": kept})
        else:
            partials_dropped += 1
    return {**note, "partials": partials}, modes_dropped, partials_dropped


def _task(path: str) -> dict:
    return analyze_take(Path(path))


def run(corpus: Path, velocity: int = 64, notes: tuple[int, int] = (21, 108), jobs: int = 0) -> list[Path]:
    takes = sorted(p for p in (corpus / "single-notes").glob(f"*_v{velocity:03d}_h*.flac")
                   if notes[0] <= int(p.name[:3]) <= notes[1])
    if not takes:
        raise SystemExit(f"No single-notes take at velocity {velocity} in {corpus}")
    start = time.time()
    results = []
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        for result in progress.track(pool.map(_task, [str(p) for p in takes]), len(takes), "notes",
                                     f"modes at velocity {velocity}"):
            errors = [p["envelope_error_db"] for p in result["partials"]]
            counts = [len(p["modes"]) for p in result["partials"]]
            print(f"  {result['name']:>4}: {len(errors):2d} partials, envelope error median "
                  f"{np.median(errors) if errors else float('nan'):4.1f} dB, modes per partial "
                  f"{np.mean(counts) if counts else 0:.1f}  ({time.time() - start:.0f} s)")
            results.append(result)
    out = corpus / f"modes-v{velocity:03d}.json"
    out.write_text(json.dumps({"source": str(corpus), "velocity": velocity, "convention": CONVENTION,
                               "notes": results}, separators=(",", ":")), encoding="utf-8")
    png = corpus / f"modes-v{velocity:03d}.png"
    plot(results, png)
    return [out, png]


def plot(results: list[dict], path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    midis = np.array([r["midi"] for r in results])
    fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
    med = [np.median([p["envelope_error_db"] for p in r["partials"]]) if r["partials"] else np.nan for r in results]
    p90 = [np.percentile([p["envelope_error_db"] for p in r["partials"]], 90) if r["partials"] else np.nan for r in results]
    axes[0].plot(midis, med, "o-", ms=3, label="median over the partials")
    axes[0].plot(midis, p90, "o-", ms=3, alpha=0.6, label="90th percentile")
    axes[0].set_ylabel("envelope error (dB)")
    axes[0].legend()
    axes[0].set_title("Measured modes: how well they reproduce each partial's envelope (beating included)")
    for r in results:
        for p in r["partials"][:12]:
            shade = min(p["n"], 12) / 12
            axes[1].scatter(r["midi"] + 0.04 * (min(p["n"], 12) - 6), len(p["modes"]), s=6, color=(shade, 0.3, 1 - shade))
    axes[1].set_ylabel("modes per partial\n(first 12 partials)")
    for r in results:
        if not r["partials"] or r["partials"][0]["n"] != 1:
            continue
        p = r["partials"][0]
        for m in p["modes"]:
            level = np.sqrt(np.mean([m[2 + 2 * c] ** 2 + m[3 + 2 * c] ** 2 for c in range((len(m) - 2) // 2)]))
            cents = 1200 * np.log2(m[0] / p["centre"])
            axes[2].scatter(r["midi"], cents, s=4 + 40 * level / max(1e-12, max(
                np.sqrt(np.mean([q[2 + 2 * c] ** 2 + q[3 + 2 * c] ** 2 for c in range((len(q) - 2) // 2)])) for q in p["modes"])),
                color=(min(1.0, m[1] / 3.0), 0.2, 1 - min(1.0, m[1] / 3.0)))
    axes[2].set_ylabel("first partial: mode offsets (cents)\nred = fast decay, blue = slow")
    axes[2].set_ylim(-6, 6)
    axes[2].set_xlabel("MIDI note")
    for ax in axes:
        ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=90)
