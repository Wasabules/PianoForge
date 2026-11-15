"""Analysis of the whole keyboard: one note analysis per key, a smoothed inharmonicity model, and the profile.

Outputs, next to the corpus state:
  analysis.json  every partial of every note (frequency, level, prompt and after-sound decay rates)
  physics.toml   the first instrument profile: per note f0, B, tuning, decay of the fundamental, damping law
  analysis.png   overview plots (inharmonicity, stretched tuning, decays, partial count)
"""

from __future__ import annotations

import json
import os
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict
from pathlib import Path

import numpy as np
import soundfile as sf

from forge.partials import NoteAnalysis, Partial, analyze_note
from forge.takes import held_end
from forge import progress

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MIN_PARTIALS_FOR_B = 4  # fewer partials cannot pin down the curvature of f_n / n


def note_name(midi: int) -> str:
    return f"{NAMES[midi % 12]}{midi // 12 - 1}"


def _analyze(args: tuple[str, int]) -> dict:
    path, velocity = args
    meta = json.loads(Path(path).with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    hold = (held_end(meta) - meta["note_on_sample"]) / fs  # to where the recording stops, if earlier
    result = analyze_note(audio.T, fs, meta["note"], meta["note_on_sample"], hold)
    out = asdict(result)
    out.update(velocity=velocity, file=Path(path).name, sample_rate=fs)
    return out


def _fit_log_b(midis: np.ndarray, b: np.ndarray) -> tuple[float, float, float, float, float]:
    """Two straight lines in log10(B) versus the note, joined at a break point (bass strings vs treble)."""
    lb = np.log10(b)
    best = None
    for k in range(int(midis.min()) + 6, int(midis.max()) - 6):
        lo, hi = midis <= k, midis > k
        if lo.sum() < 3 or hi.sum() < 3:
            continue
        p1, p2 = np.polyfit(midis[lo], lb[lo], 1), np.polyfit(midis[hi], lb[hi], 1)
        err = np.sum((lb[lo] - np.polyval(p1, midis[lo])) ** 2) + np.sum((lb[hi] - np.polyval(p2, midis[hi])) ** 2)
        if best is None or err < best[0]:
            best = (err, k, *p1, *p2)
    _, k, a1, c1, a2, c2 = best
    return float(k), float(a1), float(c1), float(a2), float(c2)


def b_model(midi: float, fit: tuple[float, float, float, float, float]) -> float:
    k, a1, c1, a2, c2 = fit
    return float(10 ** (a1 * midi + c1 if midi <= k else a2 * midi + c2))


def analyze_keyboard(corpus: Path, velocity: int = 64, log=print) -> dict:
    single = corpus / "single-notes"
    paths = []
    for midi in range(21, 109):
        found = sorted(single.glob(f"{midi:03d}-*_v{velocity:03d}_h*.flac"))
        if found:
            paths.append((str(found[0]), velocity))
    workers = max(1, (os.cpu_count() or 2) - 1)
    log(f"Analyzing {len(paths)} notes at velocity {velocity} on {workers} processes...")
    with ProcessPoolExecutor(max_workers=workers) as pool:
        notes = list(progress.track(pool.map(_analyze, paths), len(paths), "notes", "partials and decays"))

    reliable = [n for n in notes if len(n["partials"]) >= MIN_PARTIALS_FOR_B and n["inharmonicity"] > 0]
    fit = _fit_log_b(np.array([n["midi"] for n in reliable], float), np.array([n["inharmonicity"] for n in reliable]))
    for _ in range(3):  # robust fit: measures more than a factor 2.5 away from the model are outliers
        kept = [n for n in reliable if abs(np.log10(n["inharmonicity"]) - np.log10(b_model(n["midi"], fit))) < 0.4]
        if len(kept) == len(reliable) or len(kept) < 8:
            break
        reliable = kept
        fit = _fit_log_b(np.array([n["midi"] for n in reliable], float), np.array([n["inharmonicity"] for n in reliable]))
    for n in notes:
        n["inharmonicity_model"] = b_model(n["midi"], fit)
        n["inharmonicity_measured"] = n in reliable
    log(f"Inharmonicity model: break at {note_name(int(fit[0]))}; {len(reliable)} notes measured, "
        f"{len(notes) - len(reliable)} filled from the model")
    return {"source": str(corpus), "velocity": velocity, "b_model": {"break_midi": fit[0], "bass": fit[1:3],
            "treble": fit[3:5], "law": "log10(B) = slope * midi + intercept on each side of the break"},
            "notes": notes}


def _t60(rate_db_s: float) -> float | None:
    return round(60.0 / -rate_db_s, 2) if rate_db_s < 0 else None


def write_profile(data: dict, out: Path) -> None:
    lines = [
        "# PianoForge instrument profile (physics), first draft measured by the forge from the reference corpus.",
        "# Per note: fundamental, inharmonicity B (f_n = n f0 sqrt(1 + B n^2)), tuning of the first partial against",
        "# equal temperament at A4 = 440 Hz, decay of the whole note (prompt sound, and after-sound with its knee",
        "# when the decay is double), and the damping law sigma = b1 + b3 omega^2 fitted on the partials.",
        "",
        "[instrument]",
        'name = "concert-grand-noire-pure"',
        f'source = "{Path(data["source"]).as_posix()} (velocity {data["velocity"]})"',
        "reference_a4_hz = 440.0",
        "",
    ]
    for n in data["notes"]:
        p1 = n["partials"][0] if n["partials"] else None
        b = n["inharmonicity"] if n["inharmonicity_measured"] else n["inharmonicity_model"]
        lines += [
            "[[note]]",
            f"midi = {n['midi']}",
            f'name = "{note_name(n["midi"])}"',
            f"f0_hz = {n['f0']:.4f}",
            f"tuning_cents = {n['cents_vs_et']:.2f}",
            f"inharmonicity = {b:.4e}",
            f"inharmonicity_measured = {str(n['inharmonicity_measured']).lower()}",
            f"partials = {len(n['partials'])}",
        ]
        t_prompt, t_after = _t60(n["level_early_db_s"]), _t60(n["level_late_db_s"])  # whole note, broadband
        if t_prompt is not None:
            lines.append(f"t60_prompt_s = {t_prompt}")
        # An after-sound only exists when the late decay is gentler than the early one (double decay); a
        # steeper late slope is the note sinking into the noise floor.
        if t_after is not None and t_after < 1000 and n["level_late_db_s"] > n["level_early_db_s"]:
            lines.append(f"t60_aftersound_s = {t_after}")
            lines.append(f"decay_knee_s = {n['level_knee_s']:.2f}")
        lines += [f"damping_b1 = {n['b1']:.4f}", f"damping_b3 = {n['b3']:.4e}", ""]
    out.write_text("\n".join(lines), encoding="utf-8")


def plot(data: dict, out: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    notes = data["notes"]
    m = np.array([n["midi"] for n in notes])
    measured = np.array([n["inharmonicity_measured"] for n in notes])
    fig, ax = plt.subplots(2, 2, figsize=(14, 9))
    b = np.array([n["inharmonicity"] for n in notes])
    model = np.array([n["inharmonicity_model"] for n in notes])
    ax[0, 0].semilogy(m[measured], b[measured], "o", ms=4, label="measured")
    ax[0, 0].semilogy(m, model, "-", label="two-segment model")
    ax[0, 0].set_title("Inharmonicity B", loc="left")
    ax[0, 0].legend()
    cents = np.array([n["cents_vs_et"] for n in notes])
    ax[0, 1].plot(m, cents, "o-", ms=3)
    ax[0, 1].axhline(0, color="grey", lw=0.5)
    ax[0, 1].set_title("Tuning against equal temperament at A4 = 440 Hz (cents)", loc="left")
    t_prompt = [_t60(n["level_early_db_s"]) for n in notes]
    t_after = [_t60(n["level_late_db_s"]) if n["level_late_db_s"] > n["level_early_db_s"] else None for n in notes]
    ax[1, 0].semilogy(m, [t if t else np.nan for t in t_prompt], "o-", ms=3, label="prompt sound")
    ax[1, 0].semilogy(m, [t if t and t < 1000 else np.nan for t in t_after], "s-", ms=3, label="after-sound")
    ax[1, 0].set_title("T60 of the whole note, broadband level (s)", loc="left")
    ax[1, 0].legend()
    ax[1, 1].bar(m, [len(n["partials"]) for n in notes])
    ax[1, 1].set_title("Partials found (up to 16 kHz, 40 at most)", loc="left")
    for a in ax.flat:
        a.set_xticks(range(21, 109, 12))
        a.set_xticklabels([note_name(k) for k in range(21, 109, 12)])
        a.grid(alpha=0.2)
    fig.tight_layout()
    fig.savefig(out, dpi=100)


def run(corpus: Path, velocity: int = 64, log=print) -> list[Path]:
    data = analyze_keyboard(corpus, velocity, log)
    outputs = [corpus / "analysis.json", corpus / "physics.toml", corpus / "analysis.png"]
    outputs[0].write_text(json.dumps(data), encoding="utf-8")
    write_profile(data, outputs[1])
    try:
        plot(data, outputs[2])
    except ImportError:
        outputs.pop()
    return outputs
