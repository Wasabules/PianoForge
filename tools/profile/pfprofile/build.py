"""Building a profile from a bank, stage by stage, each one resumable (run the same command again after a stop):

  check                the bank's files (fetched when the tool knows where from), its keys, its samples, the tools
  prepare              the bank as a bench state (bench prepare-sfz): its controllers, licence, source
  render-reference     the single notes at 16 velocities and the key releases (bench render reference.toml)
  render-interactions  the pedal pairs and the half pedal (bench render interactions.toml)
  layers               which rendered velocities to measure (one per layer of samples) and where the sympathetic
                       resonance's calibration comes from (the bank's pedal-down samples, or another bank's)
  forge                every measure, then the profile (forge pipeline --resume)
  verify               the profile there, whole, newer than its renders

Progress in <corpus>/reference/<state>/build.json (the forge monitor shows it; the forge's own steps are in its
progress.json beside it), the log in build-log.txt there.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from pfprofile import PROJECT_ROOT
from pfprofile.banks import Bank, find
from pfprofile.sfzinfo import BankInfo, inspect, layers_for, rendered_velocities

STAGES = ("check", "prepare", "render-reference", "render-interactions", "layers", "forge", "verify")
SESSIONS = PROJECT_ROOT / "tools" / "bench" / "sessions"
# What the forge reads, and only that: the other passes (layer discovery, repetitions, probes...) serve studies.
REFERENCE_PASSES = "single-notes,releases"
INTERACTION_PASSES = "pedal-down,pedal-up,half-pedal"
MAGIC = b"PFMODAL1"
MIN_FREE_GB = 5.0


class BuildError(Exception):
    pass


@dataclass
class Options:
    corpus: Path
    jobs: int = 0
    fresh: bool = False             # measure everything again (no --resume)
    layers: str | None = None       # override: "auto", "all", "32,64,..."
    sympathetic: str | None = None  # override: "auto", "none", a bank id, a state, a sympathetic.json
    fetch: bool = True              # fetch a bank the tool knows how to


class Progress:
    """build.json: the stages, the one under way, how each ended."""

    def __init__(self, path: Path, bank: Bank):
        self.path = path
        self.data = {"state": bank.state, "bank": bank.id, "name": bank.name, "started": time.time(), "ended": None,
                     "result": "running", "current": None, "pid": os.getpid(),
                     "stages": [{"name": s, "status": "waiting", "seconds": None, "message": ""} for s in STAGES]}
        self.write()

    def write(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, indent=1), encoding="utf-8")
        tmp.replace(self.path)

    def begin(self, i: int) -> None:
        self.data["current"] = i
        self.data["stages"][i].update(status="running", started=time.time())
        self.write()

    def end(self, i: int, status: str, message: str = "") -> None:
        stage = self.data["stages"][i]
        stage.update(status=status, message=message, seconds=round(time.time() - stage.get("started", time.time()), 1))
        self.write()

    def finish(self, result: str) -> None:
        self.data.update(result=result, ended=time.time(), current=None)
        self.write()


class Log:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = open(path, "a", encoding="utf-8")

    def __call__(self, text: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {text}"
        print(line, flush=True)
        self.file.write(line + "\n")
        self.file.flush()

    def raw(self, text: str) -> None:  # a child's line, as it came
        print(text, flush=True)
        self.file.write(text + "\n")
        self.file.flush()


def _run(command: list[str], log: Log) -> None:
    """A bench or forge command in its own process, its output into the log as it comes."""
    log("$ " + " ".join(f'"{c}"' if " " in c else c for c in command[1:]))
    env = {**os.environ, "PYTHONUNBUFFERED": "1", "PYTHONIOENCODING": "utf-8"}
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                          errors="replace", env=env, cwd=PROJECT_ROOT) as p:
        for line in p.stdout:
            log.raw("    " + line.rstrip())
    if p.returncode != 0:
        raise BuildError(f"{command[2]} stopped (exit code {p.returncode}): see the lines above")


def bench(corpus: Path, *args: str) -> list[str]:
    return [sys.executable, "-m", "bench.cli", "--corpus", str(corpus), *args]


def forge(*args: str) -> list[str]:
    return [sys.executable, "-m", "forge.cli", *args]


def state_dir(corpus: Path, state: str, session: str = "reference") -> Path:
    return corpus / session / state


def resolve_layers(spec: str | list[int], info: BankInfo, rendered: list[int]) -> list[int]:
    if isinstance(spec, list):
        layers = [int(v) for v in spec]
    elif spec == "all":
        layers = list(rendered)
    elif spec == "auto":
        layers = layers_for(info.ranges, rendered)
    else:
        layers = [int(v) for v in str(spec).split(",") if v.strip()]
    wrong = [v for v in layers if v not in rendered]
    if wrong or not layers:
        raise BuildError(f"layers {wrong or layers}: not among the rendered velocities {rendered}")
    return sorted(set(layers))


def own_calibration(path: Path) -> bool:
    """A sympathetic.json calibrated on its bank's own pedal-down samples (not borrowed from another)."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return "gains" in data and not data.get("calibration_from")


def resolve_sympathetic(spec: str, info: BankInfo, corpus: Path, state: str) -> tuple[str, Path | None, str]:
    """('own' | 'borrow' | 'skip', the sympathetic.json borrowed, why)."""
    if spec == "none":
        return "skip", None, "left out, as asked"
    if spec == "auto":
        if info.has_pedal_down:
            return "own", None, f"its own pedal-down samples ({info.pedal_down_regions} regions)"
        for other in sorted((corpus / "reference").glob("*/sympathetic.json")):
            if other.parent.name != state and own_calibration(other):
                return "borrow", other, f"no pedal-down sample: borrowed from {other.parent.name}, calibrated on its own"
        return "skip", None, ("no pedal-down sample, and no bank calibrated on its own pedal-down samples is built yet "
                              "(build one, e.g. piano-in-162, then this one again)")
    if spec.endswith(".json"):
        path = Path(spec)
    else:
        bank = find(spec)
        path = state_dir(corpus, bank.state if bank else spec) / "sympathetic.json"
    if not path.exists():
        return "skip", None, f"{spec} is not built yet: build it first, then this one again (left out for now)"
    return "borrow", path, f"borrowed from {path.parent.name}"


def check_tools(log: Log) -> None:
    for module in ("bench", "forge"):
        try:
            __import__(module)
        except ImportError:
            raise BuildError(f"the {module} package is not installed: pip install -e tools/{module} (see doctor)")
    from forge import native

    if not native.available:
        log("  !! the forge's native kernels are not built: it will be about ten times slower "
            "(cmake -S tools/forge/native -B build/forge-native && cmake --build build/forge-native --config Release)")


def build(bank: Bank, options: Options) -> int:
    corpus = options.corpus.resolve()
    reference = state_dir(corpus, bank.state)
    progress = Progress(reference / "build.json", bank)
    log = Log(reference / "build-log.txt")
    log(f"== build {bank.name} -> {bank.state} (corpus {corpus})")
    info: BankInfo | None = None
    layers: list[int] = []
    sympathetic: tuple[str, Path | None, str] = ("skip", None, "")
    stage = 0
    try:
        for stage, name in enumerate(STAGES):
            progress.begin(stage)
            started = time.time()
            log(f"== {name}")
            message = ""
            if name == "check":
                check_tools(log)
                sfz = bank.sfz_path(corpus)
                if not sfz.exists():
                    kind = bank.fetch.get("kind")
                    if kind == "github" and options.fetch:
                        from pfprofile.fetch import fetch_github

                        fetch_github(bank.fetch["repository"], bank.fetch["commit"], corpus / bank.fetch["folder"], log)
                    elif kind == "manual":
                        raise BuildError(f"{bank.name} is not there yet. " + bank.fetch.get("instructions", "")
                                         .replace("<corpus>", str(corpus)))
                    else:
                        raise BuildError(f"{sfz} not found")
                info = inspect(sfz, {**{n: v / 127.0 for n, v in bank.cc.items()}, **bank.hdcc})
                if info.missing:
                    raise BuildError(f"{info.missing} of {info.samples} samples missing: the bank is incomplete "
                                     f"(fetch it again, or extract it whole)")
                if not info.full_keyboard:
                    raise BuildError(f"the bank's keys go from {info.keys[0]} to {info.keys[1]}: a piano profile needs "
                                     f"the 88 keys, 21 (A0) to 108 (C8)")
                free = shutil.disk_usage(corpus).free / 1e9
                if free < MIN_FREE_GB:
                    log(f"  !! {free:.0f} GB free on the corpus' disk: the renders and measures take about 2 GB")
                message = (f"{info.regions} regions, {info.samples} samples, keys {info.keys[0]}-{info.keys[1]}, "
                           f"{len(info.ranges)} velocity layers, pedal-down samples: "
                           f"{'yes' if info.has_pedal_down else 'no'}")
                if info.unsupported:
                    log(f"  opcodes the bench's player does not play: {info.unsupported}")
            elif name == "prepare":
                states = corpus / "states" / f"{bank.state}.json"
                current = json.loads(states.read_text(encoding="utf-8")) if states.exists() else {}
                wanted_cc = {str(n): round(v / 127.0, 6) for n, v in bank.cc.items()}
                wanted_cc.update({str(n): round(v, 6) for n, v in bank.hdcc.items()})
                same = (current.get("player") == "sfz" and Path(corpus / current.get("sfz", "")).resolve()
                        == bank.sfz_path(corpus).resolve() and current.get("cc", {}) == dict(sorted(wanted_cc.items()))
                        and current.get("align_onsets_ms") == bank.align_onsets_ms)
                if same and not options.fresh:
                    message = "already prepared"
                else:
                    args = ["prepare-sfz", "--state", bank.state, "--sfz", str(bank.sfz_path(corpus))]
                    for n, v in bank.cc.items():
                        args += ["--cc", f"{n}={v}"]
                    for n, v in bank.hdcc.items():
                        args += ["--hdcc", f"{n}={v}"]
                    if bank.sample_rate:
                        args += ["--sample-rate", str(bank.sample_rate)]
                    if bank.align_onsets_ms is not None:
                        args += ["--align-onsets", str(bank.align_onsets_ms)]
                    for flag, value in (("--author", bank.author), ("--license", bank.license),
                                        ("--source", bank.source), ("--note", bank.note)):
                        if value:
                            args += [flag, value]
                    _run(bench(corpus, *args), log)
            elif name == "render-reference":
                _run(bench(corpus, "render", str(SESSIONS / "reference.toml"), "--state", bank.state, "--passes",
                           REFERENCE_PASSES, *(["--jobs", str(options.jobs)] if options.jobs else [])), log)
            elif name == "render-interactions":
                _run(bench(corpus, "render", str(SESSIONS / "interactions.toml"), "--state", bank.state, "--passes",
                           INTERACTION_PASSES, *(["--jobs", str(options.jobs)] if options.jobs else [])), log)
            elif name == "layers":
                rendered = rendered_velocities(SESSIONS / "reference.toml")
                layers = resolve_layers(options.layers or bank.layers, info, rendered)
                sympathetic = resolve_sympathetic(options.sympathetic or bank.sympathetic_from, info, corpus, bank.state)
                message = (f"{len(layers)} layers ({', '.join(map(str, layers))}); sympathetic resonance: "
                           f"{sympathetic[2]}")
                (reference / "build-plan.json").write_text(json.dumps(
                    {"layers": layers, "sympathetic": sympathetic[0],
                     "sympathetic_from": str(sympathetic[1]) if sympathetic[1] else None, "why": sympathetic[2],
                     "ranges": info.ranges}, indent=1), encoding="utf-8")
            elif name == "forge":
                args = ["pipeline", "--corpus", str(reference), "--layers", ",".join(map(str, layers))]
                if not options.fresh:
                    args.append("--resume")
                if sympathetic[0] == "borrow":
                    args += ["--sympathetic-from", str(sympathetic[1])]
                elif sympathetic[0] == "skip":
                    args += ["--skip", "sympathetic"]
                if options.jobs:
                    args += ["--jobs", str(options.jobs)]
                _run(forge(*args), log)
            elif name == "verify":
                profile = reference / "modal-profile.bin"
                index = reference / "index.csv"
                if not profile.exists():
                    raise BuildError("no modal-profile.bin: the forge's export did not run (see forge-log.txt)")
                with open(profile, "rb") as f:
                    if f.read(len(MAGIC)) != MAGIC:
                        raise BuildError(f"{profile} is not a modal profile")
                if index.exists() and profile.stat().st_mtime < index.stat().st_mtime:
                    raise BuildError("the profile is older than the renders: the export did not run again")
                message = f"{profile} ({profile.stat().st_size / 1e6:.1f} MB)"
            progress.end(stage, "done", message)
            log(f"   {name}: done in {time.time() - started:.0f} s" + (f" - {message}" if message else ""))
    except (BuildError, SystemExit) as error:
        text = str(error)
        progress.end(stage, "failed", text)
        progress.finish("failed")
        log(f"!! {STAGES[stage]} failed: {text}")
        return 1
    except KeyboardInterrupt:
        progress.end(stage, "stopped", "interrupted")
        progress.finish("stopped")
        log("!! interrupted: run the same command again to resume")
        return 130
    progress.finish("done")
    log(f"== {bank.state}: the profile is ready. To play it:")
    log(f"   in PianoForge: Engine > Profile B... and pick {reference / 'modal-profile.bin'} (Ctrl+3 plays it),")
    log(f"   or File > Change corpus... and pick {reference / 'single-notes'};")
    log("   in the plugin: Choose profile... and pick the same file.")
    if bank.credit:
        log(f"   Credit: {bank.credit} ({bank.license}).")
    return 0
