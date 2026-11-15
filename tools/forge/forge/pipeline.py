"""`forge pipeline`: every step of a corpus state's profile, in order, with its progress (progress.json, the forge
monitor) and its log (forge-log.txt, appended).

Order: analyze; modes at each velocity layer; soundboard (velocity 64 first: the shared resonances); release-curves
(its groups of partials come from the modes); release-noise; half-pedal; sympathetic; export. A failing optional
step (release-noise, half-pedal, sympathetic) is reported and skipped; the others stop the pipeline.

The workers run with one BLAS thread each (as many processes as cores already: more threads only contend), which
also makes the results the same from one machine to the next: a few ill-conditioned partials move with the order of
the floating-point sums of a multithreaded BLAS.
"""

from __future__ import annotations

import os
import sys
import time
import traceback
from pathlib import Path

LAYERS = (64, 8, 16, 24, 32, 40, 48, 56, 72, 80, 88, 96, 104, 112, 120, 127)
OPTIONAL = ("release-noise", "half-pedal", "sympathetic")
STEPS = ("analyze", "modes", "soundboard", "release-curves", "release-noise", "half-pedal", "sympathetic", "export")


class _Tee:
    """Writes to the console and to the log, line by line."""

    def __init__(self, stream, log: Path):
        self.stream, self.log = stream, open(log, "a", encoding="utf-8")

    def write(self, text: str) -> int:
        self.stream.write(text)
        self.log.write(text)
        self.log.flush()
        return len(text)

    def flush(self) -> None:
        self.stream.flush()
        self.log.flush()


def plan(layers: list[int], skip: set[str]) -> list[tuple[str, str, int | None]]:
    """(name shown, step, velocity) in order."""
    ordered = [64] + [v for v in layers if v != 64] if 64 in layers else list(layers)
    out: list[tuple[str, str, int | None]] = []
    for step in STEPS:
        if step in skip:
            continue
        if step in ("modes", "soundboard"):
            out += [(f"{step} v{v}", step, v) for v in ordered]
        else:
            out.append((step, step, None))
    return out


def _fresh(output: Path, corpus: Path) -> bool:
    """An output newer than the renders (index.csv) and the analysis it builds on."""
    index = corpus / "index.csv"
    return output.exists() and (not index.exists() or output.stat().st_mtime > index.stat().st_mtime)


def _run_step(step: str, velocity: int | None, corpus: Path, jobs: int, resume: bool,
              sympathetic_from: Path | None = None) -> str:
    """Runs one step; returns 'done' or 'skipped' (resume: its output is already there)."""
    if step == "analyze":
        from forge.analysis import run
        list(run(corpus, 64))
    elif step == "modes":
        if resume and _fresh(corpus / f"modes-v{velocity:03d}.json", corpus):
            return "skipped"
        from forge.note_modes import run
        run(corpus, velocity, (21, 108), jobs)
    elif step == "soundboard":
        if resume and _fresh(corpus / f"soundboard-v{velocity:03d}.json", corpus):
            return "skipped"
        from forge.soundboard import run
        run(corpus, velocity, jobs)
    elif step == "release-curves":
        from forge.release_curves import write
        write(corpus)
    elif step == "release-noise":
        from forge.release_noise import run
        run(corpus, jobs)
    elif step == "half-pedal":
        from forge.half_pedal import write
        write(corpus, None)
    elif step == "sympathetic":
        from forge.sympathetic import run
        run(corpus, None, jobs, None, sympathetic_from)
    elif step == "export":
        from forge.export import run
        run(corpus, jobs)
    return "done"


def run(corpus: Path, layers: list[int] | None = None, skip: set[str] | None = None, jobs: int = 0,
        resume: bool = False, sympathetic_from: Path | None = None) -> int:
    for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):  # before numpy, in the workers too
        os.environ[name] = "1"
    from forge import native, progress

    corpus = Path(corpus)
    steps = plan(list(layers or LAYERS), skip or set())
    sys.stdout = _Tee(sys.stdout, corpus / "forge-log.txt")
    print(f"{time.strftime('%H:%M:%S')} == forge pipeline {corpus.name}: {len(steps)} steps "
          f"(native kernels: {'yes' if native.available else 'no, the Python code'})")
    progress.start(corpus, [name for name, _, _ in steps])
    status = "done"
    for i, (name, step, velocity) in enumerate(steps):
        print(f"{time.strftime('%H:%M:%S')} == {name}")
        progress.begin(i)
        started = time.time()
        try:
            result = _run_step(step, velocity, corpus, jobs, resume, sympathetic_from)
            progress.end(i, result)
            print(f"{time.strftime('%H:%M:%S')}    {name}: {result} ({time.time() - started:.0f} s)")
        except (Exception, SystemExit) as error:  # a step's SystemExit carries its reason
            message = str(error) or type(error).__name__
            if not isinstance(error, SystemExit):
                traceback.print_exc(file=sys.stdout)
            print(f"{time.strftime('%H:%M:%S')} !! {name} failed: {message}")
            progress.end(i, "failed", message)
            if step not in OPTIONAL:
                status = "failed"
                break
        except KeyboardInterrupt:
            progress.end(i, "failed", "interrupted")
            progress.finish("stopped")
            raise
    progress.finish(status)
    print(f"{time.strftime('%H:%M:%S')} == pipeline {status}")
    return 0 if status == "done" else 1
