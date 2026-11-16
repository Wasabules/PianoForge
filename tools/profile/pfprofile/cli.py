"""pianoforge-profile: a sample bank to a modal profile, in one command.

  pianoforge-profile doctor                    what is installed, built, and what is missing
  pianoforge-profile banks                     the banks it knows, and how far each one is
  pianoforge-profile fetch salamander          a bank downloaded (checked file by file), or how to get it
  pianoforge-profile build piano-in-162        the whole chain, resumable (the same command again after a stop)
  pianoforge-profile build --sfz my.sfz --state my-piano --license "..."   any SFZ piano
  pianoforge-profile status in162-close        where a build is

Every command takes --corpus (default: <project>/corpus, or $PIANOFORGE_CORPUS). See docs/building-a-profile.md.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shutil
import sys
from pathlib import Path

from pfprofile import PROJECT_ROOT, __version__
from pfprofile.banks import Bank, find, load_banks


def _corpus(args) -> Path:
    return Path(args.corpus or os.environ.get("PIANOFORGE_CORPUS") or PROJECT_ROOT / "corpus")


def _cc(specs: list[str] | None, scale: float) -> dict[int, float]:
    out = {}
    for spec in specs or []:
        n, _, v = spec.partition("=")
        if not n.strip().isdigit() or not v.strip():
            raise SystemExit(f"expected N=VALUE, got {spec!r}")
        out[int(n)] = float(v)
    return out


def cmd_doctor(args) -> int:
    ok = True

    def line(good: bool | None, text: str) -> None:
        nonlocal ok
        ok = ok and good is not False
        print(f"  [{'ok' if good else '--' if good is None else '!!'}] {text}")

    print(f"pianoforge-profile {__version__}, Python {platform.python_version()} ({sys.executable})")
    line(sys.version_info >= (3, 11), "Python 3.11 or later")
    for module, package in (("bench", "tools/bench"), ("forge", "tools/forge")):
        try:
            __import__(module)
            line(True, f"{module} installed")
        except ImportError:
            line(False, f"{module} missing: python -m pip install -e {package}")
    try:
        from forge import native

        line(native.available, "the forge's native kernels" + ("" if native.available else
             " not built (about ten times slower without): cmake -S tools/forge/native -B build/forge-native, then "
             "cmake --build build/forge-native --config Release"))
    except ImportError:
        pass
    corpus = _corpus(args)
    corpus.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(corpus).free / 1e9
    line(free >= 15, f"corpus {corpus}: {free:.0f} GB free (a bank's renders and measures take 5 to 15 GB; the bank "
                     f"itself 1 to 10 GB)")
    exe = "PianoForge.exe" if os.name == "nt" else "PianoForge"
    apps = [p for p in (PROJECT_ROOT / "build").glob(f"*/app/PianoForgeApp_artefacts/*/{exe}")] + \
           [p for p in (PROJECT_ROOT / "build").glob("*/app/PianoForgeApp_artefacts/*/PianoForge.app")]
    line(True if apps else None, f"the application: {apps[0]}" if apps else
         "the application is not built (to play the profile: cmake --preset ..., see the README)")
    print("Ready." if ok else "Something is missing: see the lines marked !!")
    return 0 if ok else 1


def _status_of(bank: Bank, corpus: Path) -> str:
    ref = corpus / "reference" / bank.state
    profile = ref / "modal-profile.bin"
    if profile.exists():
        when = dt.datetime.fromtimestamp(profile.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
        return f"profile built {when} ({profile.stat().st_size / 1e6:.0f} MB)"
    build = ref / "build.json"
    if build.exists():
        data = json.loads(build.read_text(encoding="utf-8"))
        i = data.get("current")
        stage = data["stages"][i]["name"] if i is not None else data.get("result")
        return f"build {data.get('result')} ({stage})"
    if (ref / "index.csv").exists():
        return "rendered, not measured"
    if bank.sfz_path(corpus).exists():
        return "bank there, not built"
    return "bank not fetched"


def cmd_banks(args) -> int:
    corpus = _corpus(args)
    for bank in load_banks().values():
        print(f"{bank.id:14} {bank.name} ({bank.piano}); {bank.license}")
        print(f"{'':14} state {bank.state}: {_status_of(bank, corpus)}; source {bank.source}; {bank.size}")
    print("\nAny other SFZ piano: pianoforge-profile build --sfz <file.sfz> --state <name> [--license ...]")
    return 0


def cmd_fetch(args) -> int:
    bank = find(args.bank)
    if bank is None:
        raise SystemExit(f"unknown bank {args.bank!r}: {', '.join(load_banks())}")
    corpus = _corpus(args)
    kind = bank.fetch.get("kind")
    if kind == "github":
        from pfprofile.fetch import fetch_github

        fetch_github(bank.fetch["repository"], bank.fetch["commit"], corpus / bank.fetch["folder"])
    elif bank.sfz_path(corpus).exists():
        print(f"{bank.name}: there ({bank.sfz_path(corpus)})")
    else:
        print(f"{bank.name} is downloaded by hand ({bank.fetch.get('url', bank.source)}):\n")
        print(bank.fetch.get("instructions", "").replace("<corpus>", str(corpus)))
        return 1
    from pfprofile.sfzinfo import inspect

    info = inspect(bank.sfz_path(corpus), {**{n: v / 127.0 for n, v in bank.cc.items()}, **bank.hdcc})
    print(f"{bank.name}: {info.samples} samples ({info.missing} missing), keys {info.keys[0]}-{info.keys[1]}, "
          f"{len(info.ranges)} velocity layers, pedal-down samples: {'yes' if info.has_pedal_down else 'no'}")
    return 0 if info.missing == 0 else 1


def cmd_build(args) -> int:
    from pfprofile.build import Options, build

    if args.sfz:
        if not args.state:
            raise SystemExit("--sfz needs --state (the name of what is built, e.g. my-grand)")
        bank = Bank(id=args.state, name=args.name or Path(args.sfz).stem, state=args.state, sfz=str(Path(args.sfz).resolve()),
                    author=args.author or "", license=args.license or "", source=args.source or "", credit=args.credit or "",
                    cc=_cc(args.cc, 1.0), hdcc=_cc(args.hdcc, 1.0), sample_rate=args.sample_rate,
                    align_onsets_ms=args.align_onsets, note=args.note or "")
    else:
        if not args.bank:
            raise SystemExit("which bank? pianoforge-profile build <bank> (see banks), or --sfz <file> --state <name>")
        bank = find(args.bank)
        if bank is None:
            raise SystemExit(f"unknown bank {args.bank!r}: {', '.join(load_banks())} (or --sfz <file> --state <name>)")
    options = Options(corpus=_corpus(args), jobs=args.jobs, fresh=args.fresh, layers=args.layers,
                      sympathetic=args.sympathetic_from, fetch=not args.no_fetch)
    return build(bank, options)


def cmd_status(args) -> int:
    corpus = _corpus(args)
    bank = find(args.state)
    state = bank.state if bank else args.state
    ref = corpus / "reference" / state
    build = ref / "build.json"
    if not build.exists():
        print(f"{state}: no build (" + (_status_of(bank, corpus) if bank else "unknown") + ")")
        return 1
    data = json.loads(build.read_text(encoding="utf-8"))
    print(f"{state}: {data.get('result')}")
    for s in data["stages"]:
        took = f" ({s['seconds']:.0f} s)" if s.get("seconds") is not None else ""
        print(f"  {s['status']:8} {s['name']}{took}" + (f": {s['message']}" if s.get("message") else ""))
    forge = ref / "progress.json"
    if forge.exists() and data.get("current") is not None and data["stages"][data["current"]]["name"] == "forge":
        p = json.loads(forge.read_text(encoding="utf-8"))
        steps = p.get("steps", [])
        done = sum(1 for s in steps if s.get("status") in ("done", "skipped"))
        print(f"  forge: {done} of {len(steps)} steps")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="pianoforge-profile", description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--corpus", help="corpus folder (default: <project>/corpus or $PIANOFORGE_CORPUS)")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("doctor", help="what is installed and built, what is missing")
    p.set_defaults(func=cmd_doctor)
    p = sub.add_parser("banks", help="the banks the tool knows, and how far each one is")
    p.set_defaults(func=cmd_banks)
    p = sub.add_parser("fetch", help="download a bank (checked file by file), or say how to get it")
    p.add_argument("bank", help="a bank's id (see banks)")
    p.set_defaults(func=cmd_fetch)
    p = sub.add_parser("build", help="a bank to a modal profile: every stage, resumable")
    p.add_argument("bank", nargs="?", help="a bank's id (see banks); or --sfz and --state")
    p.add_argument("--sfz", help="any SFZ piano's .sfz file")
    p.add_argument("--state", help="with --sfz: the name of the state built (e.g. my-grand)")
    p.add_argument("--name", help="with --sfz: the bank's name")
    p.add_argument("--author", help="with --sfz: who recorded and made it")
    p.add_argument("--license", help="with --sfz: its licence (kept with the renders)")
    p.add_argument("--source", help="with --sfz: where it comes from")
    p.add_argument("--credit", help="with --sfz: the credit due")
    p.add_argument("--note", help="with --sfz: what the settings are")
    p.add_argument("--cc", action="append", metavar="N=VALUE", help="with --sfz: a controller, 0 to 127 (repeatable)")
    p.add_argument("--hdcc", action="append", metavar="N=VALUE", help="with --sfz: a controller, 0 to 1 (repeatable)")
    p.add_argument("--sample-rate", type=int, help="with --sfz: render rate (default: the bank's own)")
    p.add_argument("--align-onsets", type=float, default=3.0, metavar="MS",
                   help="with --sfz: the samples' leading silence cut down to this (default 3 ms; a negative value: as is)")
    p.add_argument("--layers", help="velocities measured: auto (one per layer of samples), all, or e.g. 32,64,96,127")
    p.add_argument("--sympathetic-from", help="the sympathetic resonance's calibration: auto, none, a bank id, a state "
                                              "or a sympathetic.json (for a bank with no pedal-down samples)")
    p.add_argument("--fresh", action="store_true", help="measure everything again (renders already done are kept)")
    p.add_argument("--no-fetch", action="store_true", help="do not download a missing bank")
    p.add_argument("--jobs", type=int, default=0, help="worker processes (default: one per core)")
    p.set_defaults(func=cmd_build)
    p = sub.add_parser("status", help="where a build is")
    p.add_argument("state", help="a bank's id or a state's name")
    p.set_defaults(func=cmd_status)

    args = ap.parse_args(argv)
    if getattr(args, "align_onsets", None) is not None and args.align_onsets < 0:
        args.align_onsets = None
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
