"""Are the modes of a note the same at every velocity? The answer fixes the engine's data format.

Physically, the poles of a partial (frequency and decay of each mode) belong to the coupled strings and the bridge,
not to the blow: a harder blow should only change how much each mode is excited (its complex amplitude). If that
holds for the reference, the engine stores one set of poles per note and amplitudes per velocity, and a velocity
between two layers is an interpolation of amplitudes on the same resonators. If not, it needs poles per layer.

For a few notes, the first partials of every velocity layer are analysed, each layer over its own window (a soft
take reaches its noise floor sooner than a loud one; a common window would judge it in its noise):
  independent   each layer fitted alone (`forge modes`): the reference error;
  fixed         the poles measured at velocity 64, only the amplitudes solved per layer;
  shared        poles optimised jointly over every layer, amplitudes per layer (variable projection, each layer
                normalised to the same weight); started from the poles of v064, of the layer with the most modes,
                and of v127, the best kept;
  tracked       each layer refined alone from the shared poles: its own values, its modes paired with the others'.
Then, leave one out, an inner layer predicted from its two neighbours (what a velocity between layers would get):
  interp_poles  poles interpolated, amplitudes solved on the layer itself (the poles alone);
  interp_all    poles and complex amplitudes interpolated (log magnitude, phase along the shorter arc);
  nearest_gain  a neighbour as it is, with each partial's level interpolated between the neighbours;
  interp_mag    the modes' magnitudes interpolated, with a neighbour's phases.
The metric is the envelope error of `forge modes` (RMS dB over each layer's window; beating shows here).

Result (5 notes, pre-analysis section 8): the poles do move with the velocity (fixed: up to +6 to +8 dB at the
extremes), per-layer values cost nothing (tracked ~ independent), and nothing predicts a neighbouring layer: each
layer is a separate strike with its own beating phases. Hence the engine format: the modes of every layer as
measured; between two layers, the nearest one is played with its partials' levels interpolated.

Usage: .venv/Scripts/python studies/velocity_linearity.py [midi ...]
Writes corpus/spikes/velocity-linearity/report.json and .png.
"""
import json
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge.modes import (BandExtractor, _amplitudes, _cap_decays, _fit_orders, _model, envelope_error_db, fit_shared,
                         partial_window)
from forge.partials import discover_partials

CORPUS = Path("corpus/reference/noire-dry")
OUT = Path("corpus/spikes/velocity-linearity")
VELOCITIES = [8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127]
PARTIALS = 12
KEYS = ("independent", "fixed", "shared", "tracked", "interp_poles", "interp_all", "nearest_gain", "interp_mag")


def take(midi: int, velocity: int) -> Path:
    return next((CORPUS / "single-notes").glob(f"{midi:03d}-*_v{velocity:03d}_h*.flac"))


def load(path: Path) -> tuple[np.ndarray, int]:
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    return audio[meta["note_on_sample"]:meta["note_off_sample"]].T, fs


def layer_task(args) -> dict:
    """Windows and independent fits of the first partials of one layer, at the reference's partial frequencies."""
    midi, velocity, f0, centres = args
    x, fs = load(take(midi, velocity))
    extractor = BandExtractor(x, fs)
    pass_hz, stop_hz = min(0.33 * f0, 30.0), min(0.66 * f0, 60.0)
    peak = max(float(np.max(np.abs(extractor.baseband(c, pass_hz, stop_hz)[0]))) for c in list(centres.values())[:8])
    out = {}
    for n, centre in centres.items():
        w = partial_window(extractor, centre, f0, audible_floor=peak * 1e-4)
        if w is None:
            continue
        (order, decay, offset, amps, res, error, _), _ = _fit_orders(w.seg, w.weight, w.fs_d, 6, w.pass_hz, 0.5, 0.7)
        capped = _cap_decays(decay, w.length_s)
        if np.any(capped != decay):
            decay = capped
            amps = _amplitudes(w, decay, offset)
            error = envelope_error_db(w.seg, _model(w, decay, offset, amps), w.fs_d)
        out[n] = {"window": w, "decay": decay, "offset": offset, "error": error}
    return {"midi": midi, "velocity": velocity, "partials": out}


def shared_task(args) -> dict:
    """The fits of one partial over every layer where it was analysed."""
    midi, n, layers = args  # layers: {velocity: layer entry}
    velocities = sorted(layers)
    windows = [layers[v]["window"] for v in velocities]
    result = {"midi": midi, "n": n, "velocities": velocities,
              "independent": [float(layers[v]["error"]) for v in velocities],
              "orders": [len(layers[v]["decay"]) for v in velocities],
              "t1": [round(layers[v]["window"].i1 / layers[v]["window"].fs_d, 2) for v in velocities]}
    ref = 64 if 64 in layers else velocities[len(velocities) // 2]
    result["fixed"] = [float(e) for e in fit_shared(windows, layers[ref]["decay"], layers[ref]["offset"], refine=False)[3]]
    most = max(velocities, key=lambda v: (len(layers[v]["decay"]), v))
    best = None
    for v in dict.fromkeys([ref, most, velocities[-1]]):
        decay, offset, _, errors = fit_shared(windows, layers[v]["decay"], layers[v]["offset"])
        if best is None or np.mean(errors) < np.mean(best[2]):
            best = (decay, offset, errors, v)
    result["shared"] = [float(e) for e in best[2]]
    result["shared_order"], result["shared_start"] = len(best[0]), best[3]
    result["shared_modes"] = [[round(float(f), 4), round(float(d), 4)] for f, d in zip(best[1], best[0])]
    # Tracked: each layer refined alone from the shared poles, so that its modes stay paired with the other layers'.
    tracked = [fit_shared([w], best[0], best[1]) for w in windows]
    result["tracked"] = [float(t[3][0]) for t in tracked]
    result["tracked_modes"] = [[[round(float(f), 4), round(float(d), 4)] for f, d in zip(t[1], t[0])] for t in tracked]
    # Leave one out: an inner layer predicted from its two neighbours, the poles interpolated (log decay, linear
    # frequency), then either the amplitudes solved (interp_poles) or interpolated as well (interp_all: log
    # magnitude and phase along the shorter arc). This is what a velocity between two layers would get.
    nan = float("nan")
    for key in ("interp_poles", "interp_all", "nearest_gain", "interp_mag"):
        result[key] = [nan] * len(windows)
    for i in range(1, len(windows) - 1):
        (da, oa, aa, _), (db, ob, ab, _) = tracked[i - 1], tracked[i + 1]
        a = (velocities[i] - velocities[i - 1]) / (velocities[i + 1] - velocities[i - 1])
        d, o, w = np.exp((1 - a) * np.log(da) + a * np.log(db)), (1 - a) * oa + a * ob, windows[i]
        result["interp_poles"][i] = float(envelope_error_db(w.seg, _model(w, d, o, _amplitudes(w, d, o)), w.fs_d))
        aa, ab = aa[0], ab[0]
        amps0 = np.exp((1 - a) * np.log(np.abs(aa) + 1e-300) + a * np.log(np.abs(ab) + 1e-300)
                       + 1j * (np.angle(aa) + a * np.angle(ab / np.where(aa == 0, 1, aa))))
        amps = amps0 * np.exp((-d + 2j * np.pi * o) * w.i0 / w.fs_d)[None, :]
        result["interp_all"][i] = float(envelope_error_db(w.seg, _model(w, d, o, amps), w.fs_d))
        # A neighbouring layer as it is (its own strike: poles, amplitudes and beating phases), with one gain so
        # that the partial's level is the one interpolated between the neighbours (energy at the note-on);
        # and the modes' magnitudes interpolated with the neighbour's phases. Averaged over both neighbours.
        ea, eb = np.sum(np.abs(aa) ** 2), np.sum(np.abs(ab) ** 2)
        level = np.exp((1 - a) * np.log(ea + 1e-300) + a * np.log(eb + 1e-300))
        magnitude = np.exp((1 - a) * np.log(np.abs(aa) + 1e-300) + a * np.log(np.abs(ab) + 1e-300))
        near, mag = [], []
        for (dn, on, an, _), en in ((tracked[i - 1][:3] + (None,), ea), (tracked[i + 1][:3] + (None,), eb)):
            an = an[0]
            shift = np.exp((-dn + 2j * np.pi * on) * w.i0 / w.fs_d)[None, :]
            near.append(envelope_error_db(w.seg, _model(w, dn, on, an * np.sqrt(level / en) * shift), w.fs_d))
            mag.append(envelope_error_db(w.seg, _model(w, dn, on, magnitude * np.exp(1j * np.angle(an)) * shift),
                                         w.fs_d))
        result["nearest_gain"][i], result["interp_mag"][i] = float(np.mean(near)), float(np.mean(mag))
    return result


def summarise(results: list[dict], midi: int) -> dict:
    rows = [r for r in results if r["midi"] == midi]
    out = {}
    for key in KEYS:
        per_velocity = {}
        for r in rows:
            for v, e in zip(r["velocities"], r[key]):
                if np.isfinite(e):
                    per_velocity.setdefault(v, []).append(e)
        out[key] = {v: round(float(np.median(e)), 2) for v, e in sorted(per_velocity.items())}
        if key != "independent":
            cost = [e - i for r in rows for e, i in zip(r[key], r["independent"]) if np.isfinite(e)]
            out[f"{key}_cost_db"] = {"median": round(float(np.median(cost)), 2),
                                     "p90": round(float(np.percentile(cost, 90)), 2)}
    return out


def plot(summary: dict, path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, len(summary), figsize=(4 * len(summary), 3.6), sharey=True, squeeze=False)
    for ax, (name, s) in zip(axes[0], summary.items()):
        for key, style in zip(KEYS, ("o-", "s--", "^-", "v-", "x:", "+:", "d-", "*:")):
            v = sorted(int(k) for k in s[key])
            ax.plot(v, [s[key][k] for k in v], style, ms=3, label=key)
        ax.set_title(name)
        ax.set_xlabel("velocity")
        ax.grid(alpha=0.3)
    axes[0][0].set_ylabel("envelope error, median over partials (dB)")
    axes[0][0].legend()
    fig.tight_layout()
    fig.savefig(path, dpi=90)


def main(midis: list[int]) -> None:
    start = time.time()
    notes = {}
    for midi in midis:
        x, fs = load(take(midi, 64))
        f0, _, partials = discover_partials(x.mean(axis=0), fs, midi, 0)
        notes[midi] = (f0, {p.n: p.freq for p in partials[:PARTIALS]})
    tasks = [(m, v, notes[m][0], notes[m][1]) for m in midis for v in VELOCITIES]
    layers: dict[int, dict[int, dict]] = {}
    with ProcessPoolExecutor() as pool:
        for r in pool.map(layer_task, tasks):
            layers.setdefault(r["midi"], {})[r["velocity"]] = r["partials"]
        print(f"  independent fits: {len(tasks)} layers ({time.time() - start:.0f} s)")
        jobs = []
        for m in midis:
            for n in notes[m][1]:
                per = {v: layers[m][v][n] for v in VELOCITIES if n in layers[m][v]}
                if len(per) >= 2:
                    jobs.append((m, n, per))
        results = list(pool.map(shared_task, jobs))
        print(f"  shared fits: {len(jobs)} partials ({time.time() - start:.0f} s)")
    names = {m: take(m, 64).name.split("_")[0].split("-")[1] for m in midis}
    summary = {names[m]: summarise(results, m) for m in midis}
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "report.json").write_text(json.dumps({"summary": summary, "partials": results}, indent=1), encoding="utf-8")
    plot(summary, OUT / "report.png")
    for name, s in summary.items():
        print(f"{name:>4}  cost against independent (median / p90 dB): " + "  ".join(
            f"{k} {s[k + '_cost_db']['median']:+.2f}/{s[k + '_cost_db']['p90']:+.2f}" for k in KEYS[1:]))
        for key in KEYS:
            print(f"      {key:>12}: " + " ".join(f"{s[key].get(v, float('nan')):4.1f}" for v in VELOCITIES))
    print(f"Files in {OUT}")


if __name__ == "__main__":
    main([int(a) for a in sys.argv[1:]] or [21, 33, 60, 84, 96])
