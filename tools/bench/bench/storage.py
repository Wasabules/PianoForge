# SPDX-License-Identifier: AGPL-3.0-or-later

"""Corpus on disk: one audio file and one JSON file per take, a manifest and a CSV index.

corpus/<session>/<state>/
  manifest.json
  <pass>/<take>.flac   (24-bit FLAC; float WAV if the peak exceeds 0 dBFS, so nothing is clipped)
  <pass>/<take>.json
  index.csv            (rebuilt from the JSON files)
"""

from __future__ import annotations

import csv
import io
import json
import os
from pathlib import Path

import numpy as np
import soundfile as sf

from bench.protocol import Take

INDEX_FIELDS = ["id", "pass", "type", "note", "note_name", "velocity", "hold", "interval", "pedal", "duration_s",
                "peak_dbfs", "attack_delay_ms", "tail_db", "release_decay_40db_s", "defects", "file", "key"]
RESERVED_JSON = ("manifest.json", "check.json")


def _with_extension(path: Path, extension: str) -> Path:
    # Never Path.with_suffix(): a take name may contain dots, which would be taken for an extension.
    return path.with_name(path.name + extension)


class Corpus:
    def __init__(self, root: str | Path):
        self.root = Path(root)

    def _base(self, take: Take) -> Path:
        return self.root.joinpath(*take.id.split("/"))

    def meta(self, take: Take) -> dict | None:
        path = _with_extension(self._base(take), ".json")
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None

    def is_up_to_date(self, take: Take, key: str) -> bool:
        m = self.meta(take)
        return bool(m and m.get("key") == key and (self.root / m["file"]).exists())

    def write(self, take: Take, audio: np.ndarray, fs: int, meta: dict) -> Path:
        base = self._base(take)
        base.parent.mkdir(parents=True, exist_ok=True)
        as_float = bool(audio.size) and float(np.max(np.abs(audio))) >= 0.999
        path = _with_extension(base, ".wav" if as_float else ".flac")
        tmp = path.with_name(path.name + ".tmp" + path.suffix)
        sf.write(tmp, audio.T, fs, subtype="FLOAT" if as_float else "PCM_24")
        os.replace(tmp, path)  # atomic write: an interruption never leaves a truncated file
        self.update_meta({**meta, "file": path.relative_to(self.root).as_posix()})
        return path

    def update_meta(self, meta: dict) -> None:
        """(Re)writes the JSON of a take, next to its audio file."""
        audio = self.root / meta["file"]
        path = audio.with_name(audio.name.rsplit(".", 1)[0] + ".json")
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
        os.replace(tmp, path)

    def read_audio(self, meta: dict) -> tuple[np.ndarray, int]:
        data, fs = sf.read(self.root / meta["file"], dtype="float32", always_2d=True)
        return data.T.copy(), fs

    def metas(self) -> list[dict]:
        """The takes' JSON files: in the passes' folders (the state's own folder also holds the forge's outputs)."""
        return [json.loads(p.read_text(encoding="utf-8")) for p in sorted(self.root.rglob("*.json"))
                if p.name not in RESERVED_JSON and p.parent != self.root]

    def write_manifest(self, manifest: dict) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8")

    def rebuild_index(self) -> int:
        """index.csv from the takes' metadata, written only when it changes: the forge takes an index newer than its
        measures for new renders (forge pipeline --resume measures again what is older than it)."""
        rows = self.metas()
        out = io.StringIO(newline="")
        w = csv.DictWriter(out, fieldnames=INDEX_FIELDS, extrasaction="ignore")
        w.writeheader()
        for m in rows:
            w.writerow({**m, "defects": "|".join(m.get("defects", []))})
        index = self.root / "index.csv"
        text = out.getvalue()
        if not index.exists() or index.read_bytes() != text.encode("utf-8"):  # (bytes: the csv lines end in \r\n)
            with open(index, "w", newline="", encoding="utf-8") as f:
                f.write(text)
        return len(rows)
