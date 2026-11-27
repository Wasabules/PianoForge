# SPDX-License-Identifier: AGPL-3.0-or-later

"""Forge command line."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]


def cmd_release_curves(args) -> int:
    from forge.release_curves import write

    corpus = Path(args.corpus)
    print(f"Measuring damper curves in {corpus}")
    out = write(corpus)
    print(f"Written: {out}")
    return 0


def cmd_half_pedal(args) -> int:
    from forge.half_pedal import write

    out = write(Path(args.corpus), Path(args.interactions) if args.interactions else None)
    print(f"Written: {out}")
    return 0


def cmd_analyze(args) -> int:
    from forge.analysis import run

    for path in run(Path(args.corpus), args.velocity):
        print(f"Written: {path}")
    return 0


def cmd_modes(args) -> int:
    from forge.note_modes import run

    lo, _, hi = args.notes.partition("-")
    for path in run(Path(args.corpus), args.velocity, (int(lo), int(hi or lo)), args.jobs):
        print(f"Written: {path}")
    return 0


def cmd_soundboard(args) -> int:
    from forge.soundboard import run

    for path in run(Path(args.corpus), args.velocity, args.jobs):
        print(f"Written: {path}")
    return 0


def cmd_release_noise(args) -> int:
    from forge.release_noise import run

    for path in run(Path(args.corpus), args.jobs):
        print(f"Written: {path}")
    return 0


def cmd_sympathetic(args) -> int:
    from forge.sympathetic import run

    for path in run(Path(args.corpus), Path(args.interactions) if args.interactions else None, args.jobs,
                    Path(args.decays_from) if args.decays_from else None,
                    Path(args.calibration_from) if args.calibration_from else None):
        print(f"Written: {path}")
    return 0


def cmd_export(args) -> int:
    from forge.export import run

    for path in run(Path(args.corpus), args.jobs):
        print(f"Written: {path}")
    return 0


def cmd_pipeline(args) -> int:
    from forge.pipeline import LAYERS, STEPS, run

    layers = [int(v) for v in args.layers.split(",")] if args.layers else list(LAYERS)
    skip = {s.strip() for s in args.skip.split(",")} if args.skip else set()
    unknown = skip - set(STEPS)
    if unknown:
        raise SystemExit(f"unknown step(s): {', '.join(sorted(unknown))}; steps: {', '.join(STEPS)}")
    return run(Path(args.corpus), layers, skip, args.jobs, args.resume,
               Path(args.sympathetic_from) if args.sympathetic_from else None)


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser(prog="forge", description="PianoForge analysis and calibration tools")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("release-curves", help="measure the damper curves of a rendered corpus state")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"),
                   help="folder of one corpus state, with releases/ and single-notes/")
    p.set_defaults(func=cmd_release_curves)
    p = sub.add_parser("half-pedal", help="measure the damper lift per CC64 value on the half-pedal ramps of the "
                                          "interactions session (needs release-curves)")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--interactions", help="the same state in the interactions session (default: "
                                          "corpus/interactions/<state>)")
    p.set_defaults(func=cmd_half_pedal)
    p = sub.add_parser("analyze", help="analyze every note: f0, inharmonicity, tuning, partial decays; write the profile")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--velocity", type=int, default=64)
    p.set_defaults(func=cmd_analyze)
    p = sub.add_parser("modes", help="resolve every partial into its modes (unison strings, polarisations)")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--velocity", type=int, default=64)
    p.add_argument("--notes", default="21-108", help="MIDI range, e.g. 21-108 or 60")
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.set_defaults(func=cmd_modes)
    p = sub.add_parser("soundboard", help="fit the soundboard body: shared resonances, amplitudes per note "
                                          "(needs forge modes first)")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--velocity", type=int, default=64)
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.set_defaults(func=cmd_soundboard)
    p = sub.add_parser("release-noise", help="fit the knock of the key release on the soundboard resonances "
                                             "(needs forge soundboard and the releases pass)")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.set_defaults(func=cmd_release_noise)
    p = sub.add_parser("sympathetic", help="calibrate the sympathetic resonance of the free strings on the pedal "
                                           "pairs of the interactions session (needs forge modes at velocity 64)")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--interactions", help="the same state in the interactions session (default: "
                                          "corpus/interactions/<state>)")
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.add_argument("--decays-from", help="keep the extra decays of this sympathetic.json (skips the first, longest "
                                         "stage)")
    p.add_argument("--calibration-from", help="a bank with no pedal-down recording: take this sympathetic.json's "
                                              "calibration, on this bank's own strings")
    p.set_defaults(func=cmd_sympathetic)
    p = sub.add_parser("pipeline", help="every step of a state's profile in order, with its progress (progress.json, "
                                        "read by the forge monitor) and log (forge-log.txt)")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--layers", help="velocity layers to measure, e.g. 64,32,80,96,127 (default: the 16 of the "
                                    "reference session)")
    p.add_argument("--skip", help="steps left out, e.g. sympathetic,half-pedal")
    p.add_argument("--resume", action="store_true", help="keep the modes and soundboard layers already measured "
                                                         "since the last render")
    p.add_argument("--sympathetic-from", help="a bank with no pedal-down recording: the sympathetic.json whose "
                                              "calibration it takes")
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.set_defaults(func=cmd_pipeline)
    p = sub.add_parser("export", help="write the modal profile played by the engine (modal-profile.bin), from the "
                                      "modes and soundboard of every measured velocity layer")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"))
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.set_defaults(func=cmd_export)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
