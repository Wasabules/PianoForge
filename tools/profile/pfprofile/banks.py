# SPDX-License-Identifier: AGPL-3.0-or-later

"""The banks the tool knows (pfprofile/banks/*.toml): where each comes from, how it is set up as a bench state, how
the forge measures it. Any other SFZ bank is built from the command line (build --sfz ... --state ...)."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

BANKS_DIR = Path(__file__).resolve().parent / "banks"


@dataclass
class Bank:
    id: str
    name: str
    state: str                       # the corpus state built from it (corpus/<session>/<state>)
    sfz: str                         # relative to the corpus folder (or absolute)
    piano: str = ""
    author: str = ""
    license: str = ""
    source: str = ""
    credit: str = ""
    size: str = ""
    cc: dict[int, float] = field(default_factory=dict)    # 0 to 127
    hdcc: dict[int, float] = field(default_factory=dict)  # 0 to 1
    sample_rate: int | None = None
    align_onsets_ms: float | None = 3.0
    note: str = ""
    layers: str | list[int] = "auto"  # "auto" (found in the bank), "all" (every rendered velocity), or a list
    sympathetic_from: str = "auto"    # "auto", "none", a bank id, a state name, or a sympathetic.json path
    fetch: dict = field(default_factory=dict)

    def sfz_path(self, corpus: Path) -> Path:
        p = Path(self.sfz)
        return p if p.is_absolute() else corpus / p


def _bank(data: dict) -> Bank:
    b, s, f = data.get("bank", {}), data.get("state", {}), data.get("forge", {})
    return Bank(
        id=b["id"], name=b.get("name", b["id"]), state=s["name"], sfz=s["sfz"], piano=b.get("piano", ""),
        author=b.get("author", ""), license=b.get("license", ""), source=b.get("source", ""), credit=b.get("credit", ""),
        size=b.get("size", ""), cc={int(k): float(v) for k, v in s.get("cc", {}).items()},
        hdcc={int(k): float(v) for k, v in s.get("hdcc", {}).items()}, sample_rate=s.get("sample_rate"),
        align_onsets_ms=s.get("align_onsets_ms", 3.0), note=s.get("note", ""), layers=f.get("layers", "auto"),
        sympathetic_from=str(f.get("sympathetic_from", "auto")), fetch=data.get("fetch", {}))


def load_banks() -> dict[str, Bank]:
    banks = {}
    for path in sorted(BANKS_DIR.glob("*.toml")):
        bank = _bank(tomllib.loads(path.read_text(encoding="utf-8")))
        banks[bank.id] = bank
    return banks


def find(name: str) -> Bank | None:
    """A bank by its id or its state's name."""
    banks = load_banks()
    if name in banks:
        return banks[name]
    for bank in banks.values():
        if bank.state == name:
            return bank
    return None
