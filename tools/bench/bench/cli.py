"""Bench command line: list | prepare | render | check | reanalyze | summary | keyboard."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import json
import math
import os
import sys
import time
from pathlib import Path

from bench import VERSION
from bench.checks import analyze, max_diff_db, max_diff_dbfs
from bench.protocol import Session, Take, load_session
from bench.storage import Corpus

PROJECT_ROOT = Path(__file__).resolve().parents[3]
FLAC_BYTES_PER_S = 32_000  # 24-bit stereo at 48 kHz after compression, measured on the Noire reference corpus

# Check thresholds: two renders of the same instrument must be identical up to numerical noise; a stored
# file may only differ from a fresh render by the 24-bit FLAC rounding (about -144 dBFS).
DETERMINISM_MAX_DB = -120.0
STORAGE_MAX_DBFS = -138.0


def _corpus_dir(args) -> Path:
    return Path(args.corpus or os.environ.get("PIANOFORGE_CORPUS") or PROJECT_ROOT / "corpus")


def _state_path(args, name: str) -> Path:
    return _corpus_dir(args) / "states" / f"{name}.bin"


def _logger(file: Path | None):
    def log(message: str) -> None:
        line = f"{dt.datetime.now():%H:%M:%S} {message}"
        print(line, flush=True)
        if file:
            with open(file, "a", encoding="utf-8") as f:
                f.write(line + "\n")
    return log


def _json_safe(x):
    """Replaces non-finite floats (-inf: identical renders) with None, for standard JSON."""
    if isinstance(x, float) and not math.isfinite(x):
        return None
    if isinstance(x, dict):
        return {k: _json_safe(v) for k, v in x.items()}
    if isinstance(x, list):
        return [_json_safe(v) for v in x]
    return x


def _duration(seconds: float) -> str:
    h, rest = divmod(round(seconds), 3600)
    return f"{h} h {rest // 60:02d} min" if h else f"{rest // 60} min {rest % 60:02d} s"


def _select(session: Session, passes: str | None, limit: int | None) -> list[Take]:
    takes = session.takes
    if passes:
        wanted = {p.strip() for p in passes.split(",")}
        unknown = wanted - set(session.passes())
        if unknown:
            raise SystemExit(f"unknown pass(es): {', '.join(sorted(unknown))}; available: {', '.join(session.passes())}")
        takes = [t for t in takes if t.pass_name in wanted]
    return takes[:limit] if limit else takes


def _corpus_for(args, session: Session) -> Corpus:
    return Corpus(_corpus_dir(args) / session.name / args.state)


# --- list -------------------------------------------------------------------------------------------------

def cmd_list(args) -> int:
    session = load_session(args.session)
    total_n, total_s = 0, 0.0
    print(f"Session '{session.name}' at {session.sample_rate} Hz")
    for name in session.passes():
        takes = [t for t in session.takes if t.pass_name == name]
        seconds = sum(t.duration for t in takes) / session.sample_rate
        total_n, total_s = total_n + len(takes), total_s + seconds
        print(f"  {name:<18} {len(takes):>5} takes  {_duration(seconds):>14}  ~{seconds * FLAC_BYTES_PER_S / 1e9:.1f} GB")
    print(f"  {'total':<18} {total_n:>5} takes  {_duration(total_s):>14}  ~{total_s * FLAC_BYTES_PER_S / 1e9:.1f} GB")
    return 0


# --- prepare ----------------------------------------------------------------------------------------------

def cmd_prepare(args) -> int:
    from bench.kontakt import KontaktHost, fingerprint

    host = KontaktHost()
    if args.from_state:
        host.set_state(_state_path(args, args.from_state).read_bytes())
    print("\nThe Kontakt window is about to open. Load and set up the instrument, then close the window:")
    print("the complete plugin state will be saved as", repr(args.state), "\n")
    host.open_editor()
    state = host.state()
    path = _state_path(args, args.state)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(state)
    path.with_suffix(".vstpreset").write_bytes(bytes(host.plugin.preset_data))
    info = {"name": args.state, "fingerprint": fingerprint(state), "kontakt": host.version,
            "date": dt.datetime.now().isoformat(timespec="seconds"), "note": args.note or ""}
    print(f"State saved: {path} ({len(state)} bytes, fingerprint {info['fingerprint']})")
    print("Checking that the instrument sounds...")
    info["verification"] = host.wait_until_loaded()
    path.with_suffix(".json").write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


# --- render -----------------------------------------------------------------------------------------------

def cmd_render(args) -> int:
    from bench.kontakt import KontaktHost, fingerprint

    session = load_session(args.session)
    takes = _select(session, args.passes, args.limit)
    state = _state_path(args, args.state).read_bytes()
    corpus = _corpus_for(args, session)
    corpus.root.mkdir(parents=True, exist_ok=True)
    log = _logger(corpus.root / "log.txt")

    host = KontaktHost(fs=session.sample_rate, block=args.block, log=log)
    context = {"state": fingerprint(state), "fs": session.sample_rate, "block": args.block, "kontakt": host.version,
               "bench": VERSION}
    todo = []
    for t in takes:
        meta = corpus.meta(t)
        if corpus.is_up_to_date(t, t.key(context)) and not (args.redo_defects and meta.get("defects")):
            continue
        todo.append(t)
    log(f"{len(takes)} takes selected, {len(takes) - len(todo)} already up to date, {len(todo)} to render")
    if not todo:
        corpus.rebuild_index()
        return 0

    host.set_state(state)
    ready = host.wait_until_loaded()
    corpus.write_manifest({"session": session.name, "state": args.state, "context": context, "ready": ready,
                           "date": dt.datetime.now().isoformat(timespec="seconds"), "session_source": session.source})

    start, audio_s, defects = time.monotonic(), 0.0, collections.Counter()
    try:
        for i, t in enumerate(todo, 1):
            t0 = time.monotonic()
            host.silence()
            audio = host.render_take(t, speed=args.speed)
            result = analyze(audio, session.sample_rate, t.meta.get("note_on_sample"), t.events,
                             t.meta.get("note_off_sample"))
            meta = {**t.meta, "id": t.id, "key": t.key(context), "duration_s": round(t.duration / session.sample_rate, 4),
                    "events": t.events, "speed": args.speed, "render_s": round(time.monotonic() - t0, 3), **result}
            corpus.write(t, audio, session.sample_rate, meta)
            audio_s += t.duration / session.sample_rate
            defects.update(result["defects"])
            if i == 1 or i % args.log_every == 0 or i == len(todo) or result["defects"]:
                elapsed = time.monotonic() - start
                remaining = elapsed / i * (len(todo) - i)
                flag = f"  !! {', '.join(result['defects'])}" if result["defects"] else ""
                log(f"[{i}/{len(todo)}] {t.id}  peak {result['peak_dbfs']} dBFS  "
                    f"(x{audio_s / elapsed:.1f} real time, ~{_duration(remaining)} left){flag}")
    except KeyboardInterrupt:
        log("Interrupted: takes already written are kept; run the same command again to resume.")
        corpus.rebuild_index()
        return 130
    n = corpus.rebuild_index()
    log(f"Done: {len(todo)} takes rendered in {_duration(time.monotonic() - start)}; index: {n} takes; "
        f"defects: {dict(defects) or 'none'}")
    return 0


# --- check ------------------------------------------------------------------------------------------------

def cmd_check(args) -> int:
    """Re-renders a sample of takes: determinism and fidelity of the fast render against a paced render."""
    from bench.kontakt import KontaktHost

    session = load_session(args.session)
    corpus = _corpus_for(args, session)
    rendered = [t for t in _select(session, args.passes, None) if corpus.meta(t)]
    if not rendered:
        raise SystemExit("no rendered take to check: run 'render' first")
    step = max(1, len(rendered) // args.count)
    sample = rendered[::step][: args.count]
    log = _logger(corpus.root / "log.txt")
    host = KontaktHost(fs=session.sample_rate, block=args.block, log=log)
    host.set_state(_state_path(args, args.state).read_bytes())
    host.wait_until_loaded()
    results = []
    for t in sample:
        stored, _ = corpus.read_audio(corpus.meta(t))
        host.silence()
        fast = host.render_take(t)
        host.silence()
        paced = host.render_take(t, speed=args.reference_speed)
        r = {"id": t.id,
             "fast_vs_paced_db": max_diff_db(paced, fast),        # determinism and fidelity of the fast render
             "file_vs_render_dbfs": max_diff_dbfs(stored, fast)}  # is the stored file this very render?
        results.append(r)
        log(f"{t.id}: fast vs paced x{args.reference_speed:g}: {r['fast_vs_paced_db']:.1f} dB; "
            f"file vs render: {r['file_vs_render_dbfs']:.1f} dBFS")
    worst_render = max(r["fast_vs_paced_db"] for r in results)
    worst_file = max(r["file_vs_render_dbfs"] for r in results)
    passed = worst_render < DETERMINISM_MAX_DB and worst_file < STORAGE_MAX_DBFS
    verdict = "PASS" if passed else "FAIL"
    (corpus.root / "check.json").write_text(json.dumps(_json_safe(
        {"date": dt.datetime.now().isoformat(timespec="seconds"), "verdict": verdict,
         "worst_fast_vs_paced_db": worst_render, "worst_file_vs_render_dbfs": worst_file,
         "note": "fast_vs_paced in dB below the peak, file_vs_render in dBFS; null = bit-identical",
         "results": results}), indent=1), encoding="utf-8")
    log(f"Check {verdict}: fast vs paced, worst {worst_render:.1f} dB (threshold {DETERMINISM_MAX_DB:g}); "
        f"file vs render, worst {worst_file:.1f} dBFS (threshold {STORAGE_MAX_DBFS:g})")
    return 0 if passed else 1


# --- reanalyze --------------------------------------------------------------------------------------------

def cmd_reanalyze(args) -> int:
    """Recomputes the analyses of rendered takes (when the checks evolve), without Kontakt."""
    session = load_session(args.session)
    corpus = _corpus_for(args, session)
    defects = collections.Counter()
    metas = corpus.metas()
    for m in metas:
        audio, fs = corpus.read_audio(m)
        events = [(t, tuple(b)) for t, b in m.get("events", [])]
        result = analyze(audio, fs, m.get("note_on_sample"), events, m.get("note_off_sample"))
        for key in ("tail_db", "pre_note_db", "attack_delay_ms", "spurious_onsets_s", "release_decay_40db_s"):
            m.pop(key, None)
        m.update(result)
        corpus.update_meta(m)
        defects.update(result["defects"])
    corpus.rebuild_index()
    print(f"{len(metas)} takes reanalyzed; defects: {dict(defects) or 'none'}")
    return 0


# --- summary ----------------------------------------------------------------------------------------------

def cmd_summary(args) -> int:
    session = load_session(args.session)
    corpus = _corpus_for(args, session)
    metas = corpus.metas()
    if not metas:
        raise SystemExit(f"empty corpus: {corpus.root}")
    by_pass = collections.defaultdict(list)
    for m in metas:
        by_pass[m["pass"]].append(m)
    print(f"Corpus {corpus.root}")
    for name, ms in by_pass.items():
        expected = sum(1 for t in session.takes if t.pass_name == name)
        defects = collections.Counter(d for m in ms for d in m.get("defects", []))
        peaks = [m["peak_dbfs"] for m in ms if m.get("peak_dbfs") is not None]
        print(f"  {name:<18} {len(ms):>5}/{expected:<5} max peak {max(peaks, default=float('nan')):6.1f} dBFS  "
              f"defects: {dict(defects) or 'none'}")
    return 0


# --- keyboard ---------------------------------------------------------------------------------------------

def cmd_keyboard(args) -> int:
    from bench.keyboard_probe import analyze_capture, input_ports, listen, pick_port

    if args.list:
        print("MIDI inputs:", input_ports() or "none")
        return 0
    index, name = pick_port(args.port)
    print(f"Listening to '{name}'. Play now:")
    print("  1. a few notes from softest to loudest, then release them slowly, then quickly;")
    print("  2. the sustain pedal: press it very slowly all the way down, then lift it very slowly;")
    print("  3. the other pedals, if any.")
    print(f"The probe stops after {args.silence:g} s without playing.\n")
    capture = listen(index, args.wait, args.silence, args.duration)
    if not capture:
        return 1
    result = analyze_capture(capture)
    folder = _corpus_dir(args) / "keyboard"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"probe-{dt.datetime.now():%Y%m%d-%H%M%S}.json"
    path.write_text(json.dumps({"port": name, "analysis": result, "capture": capture}, indent=1), encoding="utf-8")
    print("\nSummary:")
    print(json.dumps(result, indent=2))
    print(f"\nCapture saved: {path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        # Line by line: the log stays readable when output is redirected to a file (background runs).
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser(prog="bench", description=__doc__)
    ap.add_argument("--corpus", help="corpus folder (default: <project>/corpus or $PIANOFORGE_CORPUS)")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("list", help="expand a session and estimate its duration, without rendering anything")
    p.add_argument("session")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("prepare", help="open Kontakt, set up the instrument, save the plugin state")
    p.add_argument("--state", required=True, help="name of the state to save (e.g. noire-dry)")
    p.add_argument("--from-state", help="start from an already saved state")
    p.add_argument("--note", help="free description of the settings")
    p.set_defaults(func=cmd_prepare)

    for name, func, help_text in (("render", cmd_render, "render a session (resumes where it stopped)"),
                                  ("check", cmd_check, "check determinism and fidelity of the fast render"),
                                  ("reanalyze", cmd_reanalyze, "recompute the analyses of rendered takes, without Kontakt"),
                                  ("summary", cmd_summary, "summarize a rendered corpus")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("session")
        p.add_argument("--state", required=True)
        p.add_argument("--passes", help="comma-separated list of passes")
        p.add_argument("--block", type=int, default=512)
        p.set_defaults(func=func)
        if name == "render":
            p.add_argument("--limit", type=int)
            p.add_argument("--speed", type=float, help="caps the speed (x real time); default: as fast as possible")
            p.add_argument("--redo-defects", action="store_true", help="render again the takes flagged with defects")
            p.add_argument("--log-every", type=int, default=20)
        if name == "check":
            p.add_argument("--count", type=int, default=10)
            p.add_argument("--reference-speed", type=float, default=1.0)

    p = sub.add_parser("keyboard", help="listen to the MIDI keyboard and infer its capabilities (pedal, velocities)")
    p.add_argument("--list", action="store_true", help="list MIDI inputs and exit")
    p.add_argument("--port", help="part of the MIDI input name (default: the first one)")
    p.add_argument("--wait", type=float, default=600.0, help="maximum wait for the first event, in s")
    p.add_argument("--silence", type=float, default=20.0, help="stop after this silence, in s")
    p.add_argument("--duration", type=float, default=600.0, help="maximum capture duration, in s")
    p.set_defaults(func=cmd_keyboard)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
