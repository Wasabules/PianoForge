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


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser(prog="forge", description="PianoForge analysis and calibration tools")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("release-curves", help="measure the damper curves of a rendered corpus state")
    p.add_argument("--corpus", default=str(PROJECT_ROOT / "corpus" / "reference" / "noire-dry"),
                   help="folder of one corpus state, with releases/ and single-notes/")
    p.set_defaults(func=cmd_release_curves)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
