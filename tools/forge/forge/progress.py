# SPDX-License-Identifier: AGPL-3.0-or-later

"""The forge's progress, for the forge monitor (tools/forge/monitor): progress.json in the corpus state folder.

`forge pipeline` writes its plan there (every step, its status and how long it took) and the step under way
reports how far it is through track() (notes, takes, layers). Outside a pipeline, track() changes nothing.

progress.json:
  state, corpus, pid, started, updated (seconds since the epoch), status (running, done, failed, stopped),
  steps[]: name, status (pending, running, done, failed, skipped), seconds (when over), error (when failed),
           and for the step under way: phase (what it is doing), done, total, unit, started, updated
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Iterable, Iterator, TypeVar

T = TypeVar("T")
_path: Path | None = None
_doc: dict | None = None
_current: int | None = None
_last_write = 0.0


def _write(force: bool = False) -> None:
    global _last_write
    if _path is None or _doc is None:
        return
    now = time.time()
    if not force and now - _last_write < 0.5:
        return
    _doc["updated"] = now
    tmp = _path.with_name(_path.name + ".tmp")
    try:
        tmp.write_text(json.dumps(_doc, indent=1), encoding="utf-8")
        os.replace(tmp, _path)
        _last_write = now
    except OSError:
        pass  # the monitor reading it at that instant (Windows): next time


def start(corpus: Path, steps: list[str]) -> None:
    """A pipeline begins: its plan, every step pending."""
    global _path, _doc, _current
    _path = Path(corpus) / "progress.json"
    _doc = {"state": Path(corpus).name, "corpus": str(Path(corpus).resolve()), "pid": os.getpid(),
            "started": time.time(), "status": "running",
            "steps": [{"name": name, "status": "pending"} for name in steps]}
    _current = None
    _write(force=True)


def begin(index: int) -> None:
    global _current
    if _doc is None:
        return
    _current = index
    _doc["steps"][index].update(status="running", started=time.time(), done=0, total=0, unit="", phase="")
    _write(force=True)


def end(index: int, status: str, error: str = "") -> None:
    global _current
    if _doc is None:
        return
    step = _doc["steps"][index]
    step["status"] = status
    if "started" in step:
        step["seconds"] = round(time.time() - step["started"], 1)
    if error:
        step["error"] = error
    _current = None
    _write(force=True)


def finish(status: str) -> None:
    if _doc is None:
        return
    _doc["status"] = status
    _write(force=True)


def track(items: Iterable[T], total: int, unit: str = "notes", phase: str = "") -> Iterator[T]:
    """Yields `items` and reports each one done to the step under way."""
    step = _doc["steps"][_current] if _doc is not None and _current is not None else None
    if step is not None:
        step.update(done=0, total=int(total), unit=unit, phase=phase, phase_started=time.time())
        _write(force=True)
    for item in items:
        yield item
        if step is not None:
            step["done"] += 1
            _write(force=step["done"] == step["total"])
