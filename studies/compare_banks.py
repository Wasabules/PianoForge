"""Compares the reference banks, measure for measure: the forge's analyses of each corpus state side by side.

PianoForge was studied on one commercial library (Noire, rendered through Kontakt); free banks (the Salamander Grand
Piano, a Yamaha C5, CC-BY 3.0; the Piano in 162, a Steinway B) are rendered by the bench's SFZ player through the same
sessions and analysed by the same forge. This study puts their measures side by side, note by note:
  - tuning: the stretch, cents against equal temperament (forge analyze, velocity 64);
  - inharmonicity B (forge analyze);
  - sustain: the early decay of the fundamental, dB/s (forge analyze);
  - brightness: the partials 2 to 8 against the fundamental, dB (forge analyze);
  - dynamics: the peak level per velocity against velocity 127, median over the notes (single-notes takes);
  - dampers: the level 0.2 s and 1 s after the key release (a 2 s hold) against the note held (forge release-curves).
And a table per register (medians), to read the differences in numbers.

Usage: .venv/Scripts/python studies/compare_banks.py [state ...]   (default: noire-dry salamander-dry in162-close)
Writes corpus/spikes/banks/comparison.png and comparison.md.
"""
import csv
import json
import sys
from pathlib import Path

import numpy as np

CORPUS = Path("corpus/reference")
OUT = Path("corpus/spikes/banks")
LABELS = {"noire-dry": "Noire (Yamaha CFX)", "salamander-dry": "Salamander (Yamaha C5)",
          "in162-close": "Piano in 162 (Steinway B)"}
# The reference palette's first three categorical slots (light mode): distinct in every pair, colour blindness
# included. Text stays in the text inks.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]
COLOURS = {"noire-dry": SERIES[0], "salamander-dry": SERIES[1], "in162-close": SERIES[2]}  # a bank keeps its colour
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
REGISTERS = [(21, 35, "A0-B1"), (36, 47, "C2-B2"), (48, 59, "C3-B3"), (60, 71, "C4-B4"), (72, 83, "C5-B5"),
             (84, 95, "C6-B6"), (96, 108, "C7-C8")]


def note_measures(state: str) -> dict[int, dict]:
    """Per note: tuning, B, the fundamental's early decay, brightness (forge analyze, velocity 64)."""
    analysis = json.loads((CORPUS / state / "analysis.json").read_text(encoding="utf-8"))
    out = {}
    for n in analysis["notes"]:
        partials = {p["n"]: p for p in n["partials"]}
        first = partials.get(1)
        upper = [10 ** (p["amp_db"] / 10) for k, p in partials.items() if 2 <= k <= 8]
        out[n["midi"]] = {
            "cents": n["cents_vs_et"],
            "b": n["inharmonicity"],
            "decay": -first["early_db_s"] if first and first.get("early_db_s") is not None else np.nan,
            "bright": 10 * np.log10(sum(upper)) - first["amp_db"] if first and upper else np.nan,
        }
    return out


def dynamics(state: str) -> dict[int, float]:
    """Median over the notes of the peak level per velocity, against velocity 127 (dB)."""
    rows = list(csv.DictReader(open(CORPUS / state / "index.csv", encoding="utf-8")))
    peaks: dict[int, dict[int, float]] = {}
    for r in rows:
        if r["pass"] == "single-notes" and r["peak_dbfs"]:
            peaks.setdefault(int(r["note"]), {})[int(r["velocity"])] = float(r["peak_dbfs"])
    velocities = sorted({v for p in peaks.values() for v in p})
    return {v: float(np.median([p[v] - p[127] for p in peaks.values() if v in p and 127 in p])) for v in velocities}


def dampers(state: str) -> dict[int, tuple[float, float]]:
    """Per note: the level 0.2 s and 1 s after the release of a 2 s hold, against the note held (dB)."""
    path = CORPUS / state / "release-curves.json"
    if not path.exists():
        return {}
    c = json.loads(path.read_text(encoding="utf-8"))
    holds = list(c["holds_s"])
    h = int(np.argmin([abs(x - 2.0) for x in holds]))
    db, step = np.asarray(c["curves_db"]), c["step_s"]
    at = lambda s: min(db.shape[2] - 1, int(round(s / step)))
    return {m: (float(db[i, h, at(0.2)]), float(db[i, h, at(1.0)])) for i, m in enumerate(c["notes"])}


def running_median(values: dict[int, float], width: int = 5) -> dict[int, float]:
    """The median of each note and its neighbours (width notes), over the valid values: the trend, without the notes
    whose measure failed."""
    notes = sorted(m for m, v in values.items() if np.isfinite(v))
    half = width // 2
    return {m: float(np.median([values[n] for n in notes if abs(n - m) <= half])) for m in notes}


def median_in(values: dict[int, float], lo: int, hi: int) -> float:
    xs = [v for m, v in values.items() if lo <= m <= hi and np.isfinite(v)]
    return float(np.median(xs)) if xs else float("nan")


def main(states: list[str]) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    states = [s for s in states if (CORPUS / s / "analysis.json").exists()]
    if not states:
        raise SystemExit("no state with an analysis.json (forge analyze) in corpus/reference")
    notes = {s: note_measures(s) for s in states}
    dyn = {s: dynamics(s) for s in states}
    damp = {s: dampers(s) for s in states}

    plt.rcParams.update({"font.size": 9, "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "xtick.color": INK_2,
                         "ytick.color": INK_2, "axes.titlecolor": INK, "axes.titleweight": "bold",
                         "axes.titlesize": 10, "axes.titlelocation": "left"})
    fig, axes = plt.subplots(2, 3, figsize=(15, 8.4), facecolor=SURFACE)
    valid_b = lambda s: {m: (v["b"] if v["b"] > 0 else np.nan) for m, v in notes[s].items()}  # 0: not measured
    panels = [
        ("Tuning: cents against equal temperament", "cents", lambda s: {m: v["cents"] for m, v in notes[s].items()}, False),
        ("Inharmonicity B (median of 5 notes)", "B (log)", lambda s: running_median(valid_b(s)), True),
        ("Sustain: early decay of the fundamental (median of 5)", "dB/s (higher: shorter)",
         lambda s: running_median({m: v["decay"] for m, v in notes[s].items()}), False),
        ("Brightness: partials 2-8 vs fundamental (median of 5)", "dB",
         lambda s: running_median({m: v["bright"] for m, v in notes[s].items()}), False),
    ]
    flat = axes.ravel()
    for ax, (title, unit, values, log) in zip(flat, panels):
        for k, s in enumerate(states):
            v = values(s)
            xs = sorted(v)
            ax.plot(xs, [v[m] for m in xs], color=COLOURS.get(s, SERIES[k % 3]), lw=2, label=LABELS.get(s, s))
        if log:
            ax.set_yscale("log")
        ax.set_title(title)
        ax.set_ylabel(unit)
        ax.set_xlabel("MIDI note")
    ax = flat[4]
    for k, s in enumerate(states):
        vs = sorted(dyn[s])
        ax.plot(vs, [dyn[s][v] for v in vs], color=COLOURS.get(s, SERIES[k % 3]), lw=2, marker="o", ms=4)
    ax.set_title("Dynamics: peak level against velocity 127")
    ax.set_ylabel("dB (median over the notes)")
    ax.set_xlabel("velocity")
    ax = flat[5]
    for k, s in enumerate(states):
        ms = sorted(damp[s])
        if ms:
            ax.plot(ms, [damp[s][m][0] for m in ms], color=COLOURS.get(s, SERIES[k % 3]), lw=2)
            ax.plot(ms, [damp[s][m][1] for m in ms], color=COLOURS.get(s, SERIES[k % 3]), lw=1.2, ls=(0, (3, 2)))
    ax.set_title("Dampers: 0.2 s (solid), 1 s (dashed) after release")
    ax.set_ylabel("dB against the note held")
    ax.set_xlabel("MIDI note")
    for ax in flat:
        ax.set_facecolor(SURFACE)
        ax.grid(True, color=GRID, lw=0.6)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
    handles, labels = flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=len(states), frameon=False, fontsize=10,
               labelcolor=INK, bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / "comparison.png", dpi=130, facecolor=SURFACE)

    # The table: medians per register.
    lines = ["# Reference banks compared", "", "Medians per register (forge analyze at velocity 64; dampers after a "
             "2 s hold).", ""]
    measures = [("cents", "Tuning (cents vs ET)", "{:+.1f}"), ("b", "Inharmonicity B (x 1e-4)", "{:.2f}"),
                ("decay", "Early decay of the fundamental (dB/s)", "{:.1f}"),
                ("bright", "Partials 2-8 vs fundamental (dB)", "{:+.1f}")]
    header = "| Register | " + " | ".join(LABELS.get(s, s) for s in states) + " |"
    rule = "|---|" + "---|" * len(states)
    for key, title, fmt in measures:
        lines += [f"## {title}", "", header, rule]
        for lo, hi, name in REGISTERS:
            cells = []
            for s in states:
                v = median_in({m: x[key] for m, x in notes[s].items()}, lo, hi)
                cells.append(fmt.format(v * 1e4 if key == "b" else v) if np.isfinite(v) else "-")
            lines.append(f"| {name} | " + " | ".join(cells) + " |")
        lines.append("")
    lines += ["## Dampers: dB 0.2 s / 1 s after the release", "", header, rule]
    for lo, hi, name in REGISTERS:
        cells = []
        for s in states:
            a = median_in({m: x[0] for m, x in damp[s].items()}, lo, hi)
            b = median_in({m: x[1] for m, x in damp[s].items()}, lo, hi)
            cells.append(f"{a:+.1f} / {b:+.1f}" if np.isfinite(a) else "-")
        lines.append(f"| {name} | " + " | ".join(cells) + " |")
    lines += ["", "## Dynamics: peak level against velocity 127 (dB, median over the notes)", "",
              "| Velocity | " + " | ".join(LABELS.get(s, s) for s in states) + " |", rule]
    for v in sorted({v for s in states for v in dyn[s]}):
        lines.append(f"| {v} | " + " | ".join(f"{dyn[s][v]:+.1f}" if v in dyn[s] else "-" for s in states) + " |")
    (OUT / "comparison.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Written: {OUT / 'comparison.png'}, {OUT / 'comparison.md'}")


if __name__ == "__main__":
    main(sys.argv[1:] or ["noire-dry", "salamander-dry", "in162-close"])
