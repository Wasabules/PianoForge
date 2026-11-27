# SPDX-License-Identifier: AGPL-3.0-or-later

"""Bench command line: list | prepare | prepare-sfz | render | check | reanalyze | summary | keyboard."""

from __future__ import annotations

import argparse
import collections
import contextlib
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

# Check thresholds, in absolute dBFS: two renders of the same instrument must agree to numerical noise, and a
# stored file may only differ from a fresh render by the 24-bit FLAC rounding (about -144 dBFS). Measured at
# 44.1 kHz: a paced render may differ from the fast one by -120 to -134 dBFS on the first takes after loading,
# far below audibility, so the determinism threshold is absolute rather than relative to a soft take's peak.
DETERMINISM_MAX_DBFS = -110.0
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


# --- instruments ------------------------------------------------------------------------------------------

def _state_info(args) -> dict:
    """The state's description, states/<name>.json: a Kontakt state (bench prepare, its plugin state in <name>.bin)
    or an SFZ bank (bench prepare-sfz, "player": "sfz")."""
    path = _corpus_dir(args) / "states" / f"{args.state}.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def _is_sfz(info: dict) -> bool:
    return info.get("player") == "sfz"


def _sfz_file(args, info: dict) -> Path:
    path = Path(info["sfz"])
    return path if path.is_absolute() else _corpus_dir(args) / path


def _sfz_cc(info: dict) -> dict[int, float]:
    return {int(n): float(v) for n, v in info.get("cc", {}).items()}


def _sfz_host(args, info: dict, fs: int, log):
    from bench.sfz import SfzHost

    return SfzHost(_sfz_file(args, info), fs, _sfz_cc(info), block=args.block, log=log,
                   align_ms=info.get("align_onsets_ms"))


def _load(args, info: dict) -> Session:
    """The session, its takes timed at the bank's own rate for an SFZ state."""
    return load_session(args.session, info.get("sample_rate") if _is_sfz(info) else None)


def _instrument(args, info: dict, fs: int, log):
    """The state's instrument and the context its renders are keyed on. Kontakt is loaded without its state:
    _set_state() follows, once there is something to render (loading it takes minutes)."""
    if _is_sfz(info):
        host = _sfz_host(args, info, fs, log)
        return host, {"state": host.instrument.fingerprint(), "fs": fs, "block": args.block, "sfz": host.version,
                      "bench": VERSION}
    from bench.kontakt import KontaktHost, fingerprint

    state = _state_path(args, args.state).read_bytes()
    host = KontaktHost(fs=fs, block=args.block, log=log)
    return host, {"state": fingerprint(state), "fs": fs, "block": args.block, "kontakt": host.version, "bench": VERSION}


def _set_state(args, info: dict, host) -> None:
    if not _is_sfz(info):
        host.set_state(_state_path(args, args.state).read_bytes())


def _render_one(host, take: Take, fs: int, corpus: Corpus, context: dict, speed: float | None) -> dict:
    """Renders a take from silence, analyzes it and stores it; returns the analysis."""
    t0 = time.monotonic()
    host.silence()
    audio = host.render_take(take, speed=speed)
    result = analyze(audio, fs, take.meta.get("note_on_sample"), take.events, take.meta.get("note_off_sample"))
    meta = {**take.meta, "id": take.id, "key": take.key(context), "duration_s": round(take.duration / fs, 4),
            "events": take.events, "speed": speed, "render_s": round(time.monotonic() - t0, 3), **result,
            **getattr(host, "last_info", {})}  # an SFZ bank: where a sample cut the note short
    corpus.write(take, audio, fs, meta)
    return result


# An SFZ bank renders in several processes (nothing is shared between takes); each one holds its own player.
_WORKER: dict = {}


def _worker_start(sfz: str, fs: int, cc: dict, block: int, align_ms, corpus: str, context: dict) -> None:
    from bench.sfz import SfzHost

    _WORKER.update(host=SfzHost(sfz, fs, cc, block=block, log=lambda message: None, align_ms=align_ms), fs=fs,
                   corpus=Corpus(corpus), context=context)


def _worker_take(take: Take) -> dict:
    return _render_one(_WORKER["host"], take, _WORKER["fs"], _WORKER["corpus"], _WORKER["context"], None)


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
    state, preset = host.state(), bytes(host.plugin.preset_data)
    # Check before saving: switching snapshots in the editor can leave Kontakt without any instrument,
    # and an empty state must never replace a good one.
    print("Checking that the instrument sounds...")
    try:
        verification = host.wait_until_loaded(max_s=args.verify_timeout)
    except TimeoutError:
        print(f"Nothing saved: the instrument stays silent after {args.verify_timeout:g} s "
              f"(is an instrument loaded in the Kontakt window?). Run 'prepare' again.")
        return 1
    path = _state_path(args, args.state)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(state)
    path.with_suffix(".vstpreset").write_bytes(preset)
    info = {"name": args.state, "fingerprint": fingerprint(state), "kontakt": host.version,
            "date": dt.datetime.now().isoformat(timespec="seconds"), "note": args.note or "", "verification": verification}
    path.with_suffix(".json").write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"State saved: {path} ({len(state)} bytes, fingerprint {info['fingerprint']})")
    return 0


# --- prepare-sfz ------------------------------------------------------------------------------------------

def cmd_prepare_sfz(args) -> int:
    """An SFZ bank as a state: the bench then renders it as it renders Kontakt (render --state <name>)."""
    from bench.sfz import PLAYER_VERSION, SfzHost, SfzInstrument, native_rate

    sfz = Path(args.sfz).resolve()
    cc: dict[int, float] = {}
    for spec, scale in [(s, 127.0) for s in args.cc or []] + [(s, 1.0) for s in args.hdcc or []]:
        number, _, value = spec.partition("=")
        if not number.strip().isdigit() or not value.strip():
            raise SystemExit(f"expected N=value, got {spec!r}")
        cc[int(number)] = min(1.0, max(0.0, float(value) / scale))
    states = _corpus_dir(args) / "states"
    path = states / f"{args.state}.json"
    if (states / f"{args.state}.bin").exists() or (path.exists() and not _is_sfz(_state_info(args))):
        raise SystemExit(f"'{args.state}' is a Kontakt state: choose another name")
    instrument = SfzInstrument(sfz, cc)
    missing = instrument.missing()
    if missing:
        raise SystemExit(f"{len(missing)} of {len(instrument.samples())} samples missing, e.g. {missing[0]}")
    rate = args.sample_rate or native_rate(instrument)
    host = SfzHost(sfz, rate, cc, align_ms=args.align_onsets)
    verification = host.wait_until_loaded()
    corpus = _corpus_dir(args).resolve()
    stored = sfz.relative_to(corpus).as_posix() if sfz.is_relative_to(corpus) else sfz.as_posix()
    info = {"name": args.state, "player": "sfz", "sfz": stored,
            "cc": {str(n): round(v, 6) for n, v in sorted(cc.items())}, "sample_rate": rate,
            "align_onsets_ms": args.align_onsets,
            "fingerprint": instrument.fingerprint(), "player_version": PLAYER_VERSION,
            "date": dt.datetime.now().isoformat(timespec="seconds"), "author": args.author or "",
            "license": args.license or "", "source": args.source or "", "note": args.note or "",
            "verification": verification}
    states.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"State saved: {path} ({verification['regions']} regions, {verification['samples']} samples, "
          f"{rate} Hz, fingerprint {info['fingerprint']})")
    if verification["unsupported"]:
        print(f"Opcodes the player does not play: {verification['unsupported']}")
    return 0


# --- render -----------------------------------------------------------------------------------------------

def cmd_render(args) -> int:
    info = _state_info(args)
    session = _load(args, info)
    takes = _select(session, args.passes, args.limit)
    corpus = _corpus_for(args, session)
    corpus.root.mkdir(parents=True, exist_ok=True)
    log = _logger(corpus.root / "log.txt")

    host, context = _instrument(args, info, session.sample_rate, log)
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

    _set_state(args, info, host)
    ready = host.wait_until_loaded()
    manifest = {"session": session.name, "state": args.state, "context": context, "ready": ready,
                "date": dt.datetime.now().isoformat(timespec="seconds"), "session_source": session.source}
    if _is_sfz(info):  # the bank, its settings, licence and source: what the corpus was rendered from
        manifest["instrument"] = {k: info[k] for k in ("sfz", "cc", "sample_rate", "align_onsets_ms", "author",
                                                       "license", "source", "note") if k in info}
    corpus.write_manifest(manifest)

    jobs = max(1, args.jobs or (os.cpu_count() or 2) - 2) if _is_sfz(info) else 1
    if jobs > 1:
        log(f"Rendering in {jobs} processes")
    start, audio_s, defects = time.monotonic(), 0.0, collections.Counter()
    try:
        with contextlib.ExitStack() as stack:
            if jobs > 1:
                from concurrent.futures import ProcessPoolExecutor

                pool = stack.enter_context(ProcessPoolExecutor(
                    max_workers=jobs, initializer=_worker_start,
                    initargs=(str(_sfz_file(args, info)), session.sample_rate, _sfz_cc(info), args.block,
                              info.get("align_onsets_ms"), str(corpus.root), context)))
                stack.callback(pool.shutdown, wait=True, cancel_futures=True)
                results = pool.map(_worker_take, todo, chunksize=2)
            else:
                results = (_render_one(host, t, session.sample_rate, corpus, context, args.speed) for t in todo)
            for i, (t, result) in enumerate(zip(todo, results), 1):
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
    info = _state_info(args)
    session = _load(args, info)
    corpus = _corpus_for(args, session)
    rendered = [t for t in _select(session, args.passes, None) if corpus.meta(t)]
    if not rendered:
        raise SystemExit("no rendered take to check: run 'render' first")
    step = max(1, len(rendered) // args.count)
    sample = rendered[::step][: args.count]
    log = _logger(corpus.root / "log.txt")
    host, _ = _instrument(args, info, session.sample_rate, log)
    _set_state(args, info, host)
    host.wait_until_loaded()
    results = []
    for t in sample:
        stored, _ = corpus.read_audio(corpus.meta(t))
        host.silence()
        fast = host.render_take(t)
        host.silence()
        paced = host.render_take(t, speed=args.reference_speed)
        r = {"id": t.id,
             "fast_vs_paced_dbfs": max_diff_dbfs(paced, fast),    # determinism and fidelity of the fast render
             "fast_vs_paced_db": max_diff_db(paced, fast),        # the same, relative to the take's peak
             "file_vs_render_dbfs": max_diff_dbfs(stored, fast)}  # is the stored file this very render?
        results.append(r)
        log(f"{t.id}: fast vs paced x{args.reference_speed:g}: {r['fast_vs_paced_dbfs']:.1f} dBFS "
            f"({r['fast_vs_paced_db']:.1f} dB below the peak); file vs render: {r['file_vs_render_dbfs']:.1f} dBFS")
    worst_render = max(r["fast_vs_paced_dbfs"] for r in results)
    worst_file = max(r["file_vs_render_dbfs"] for r in results)
    passed = worst_render < DETERMINISM_MAX_DBFS and worst_file < STORAGE_MAX_DBFS
    verdict = "PASS" if passed else "FAIL"
    (corpus.root / "check.json").write_text(json.dumps(_json_safe(
        {"date": dt.datetime.now().isoformat(timespec="seconds"), "verdict": verdict,
         "worst_fast_vs_paced_dbfs": worst_render, "worst_file_vs_render_dbfs": worst_file,
         "note": "differences in dBFS (absolute), and fast_vs_paced_db relative to the peak; null = bit-identical",
         "results": results}), indent=1), encoding="utf-8")
    log(f"Check {verdict}: fast vs paced, worst {worst_render:.1f} dBFS (threshold {DETERMINISM_MAX_DBFS:g}); "
        f"file vs render, worst {worst_file:.1f} dBFS (threshold {STORAGE_MAX_DBFS:g})")
    return 0 if passed else 1


# --- reanalyze --------------------------------------------------------------------------------------------

def cmd_reanalyze(args) -> int:
    """Recomputes the analyses of rendered takes (when the checks evolve), without Kontakt."""
    session = _load(args, _state_info(args))
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
    session = _load(args, _state_info(args))
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

    p = sub.add_parser("prepare-sfz", help="describe an SFZ bank as a state, to render it like Kontakt")
    p.add_argument("--state", required=True, help="name of the state (e.g. salamander-dry)")
    p.add_argument("--sfz", required=True, help="the bank's .sfz file")
    p.add_argument("--cc", action="append", metavar="N=VALUE", help="a controller's value, 0 to 127 (repeatable)")
    p.add_argument("--hdcc", action="append", metavar="N=VALUE", help="a controller's value, 0 to 1 (repeatable)")
    p.add_argument("--sample-rate", type=int, help="render rate (default: the bank's own)")
    p.add_argument("--align-onsets", type=float, metavar="MS",
                   help="start each sample MS ms before its attack (silence the bank leaves before it is cut)")
    p.add_argument("--author", help="who recorded and made the bank")
    p.add_argument("--license", help="the bank's licence")
    p.add_argument("--source", help="where the bank comes from")
    p.add_argument("--note", help="free description of the settings")
    p.set_defaults(func=cmd_prepare_sfz)

    p = sub.add_parser("prepare", help="open Kontakt, set up the instrument, save the plugin state")
    p.add_argument("--state", required=True, help="name of the state to save (e.g. noire-dry)")
    p.add_argument("--from-state", help="start from an already saved state")
    p.add_argument("--note", help="free description of the settings")
    p.add_argument("--verify-timeout", type=float, default=180.0,
                   help="seconds to wait for the instrument to sound before giving up without saving")
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
            p.add_argument("--jobs", type=int, default=0,
                           help="processes rendering an SFZ bank (default: the cores but two; Kontakt: one)")
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
