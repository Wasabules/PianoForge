# SPDX-License-Identifier: AGPL-3.0-or-later

"""What the bank's SFZ says that the measurement needs: its keys, its velocity layers (which rendered velocities to
measure: one per layer of samples), and whether it has pedal-down samples (the sympathetic resonance calibrated on its
own, or borrowed)."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from pathlib import Path

PIANO_KEYS = (21, 108)
PEDAL_SHARE = 0.2  # pedal-down note samples among its note samples, at least: a bank that has them (not a few noises)
FINE_LAYERS = 12        # a bank with this many layers or more: the whole rendered grid (finer than it in places)


@dataclass
class BankInfo:
    regions: int
    samples: int
    missing: int
    keys: tuple[int, int]
    ranges: list[tuple[int, int]]  # the velocity ranges of the samples, at middle C, pedal up
    pedal_down_regions: int        # note samples played with the sustain pedal down
    unsupported: dict
    note_regions: int = 0          # every note sample

    @property
    def has_pedal_down(self) -> bool:
        return self.pedal_down_regions >= max(8, PEDAL_SHARE * self.note_regions)

    @property
    def full_keyboard(self) -> bool:
        return self.keys[0] <= PIANO_KEYS[0] and self.keys[1] >= PIANO_KEYS[1]


def _note_regions(instrument):
    return [r for r in instrument.regions if r.trigger == "attack" and r.cc_trigger is None]


def inspect(sfz: Path, cc: dict[int, float] | None = None) -> BankInfo:
    """`cc`: the state's controllers (0 to 1), the others at 0."""
    from bench.sfz import SfzInstrument

    cc = dict(cc or {})
    instrument = SfzInstrument(sfz, cc)
    notes = _note_regions(instrument)

    def plays(region, pedal: float) -> bool:
        values = dict(cc)
        values[64] = pedal
        return all(lo - 1e-9 <= values.get(n, 0.0) <= hi + 1e-9 for n, lo, hi in region.conditions)

    ranges = Counter((r.lovel, r.hivel) for r in notes if r.lokey <= 60 <= r.hikey and plays(r, 0.0))
    pedal_down = sum(1 for r in notes if any(n == 64 and lo >= 0.5 for n, lo, hi in r.conditions))
    keyed = [r for r in notes if r.lokey >= 0]
    keys = (min(r.lokey for r in keyed), max(r.hikey for r in keyed)) if keyed else (0, -1)
    described = instrument.describe()
    return BankInfo(regions=len(instrument.regions), samples=len(instrument.samples()), missing=len(instrument.missing()),
                    keys=keys, ranges=sorted(ranges), pedal_down_regions=pedal_down,
                    unsupported=described.get("unsupported", {}), note_regions=len(notes))


def layers_for(ranges: list[tuple[int, int]], rendered: list[int]) -> list[int]:
    """One rendered velocity per layer of samples: the loudest inside it (where its samples play at full level). A
    layer narrower than the grid between two rendered velocities is reached by neither: left out. With many layers,
    the whole grid."""
    if len(ranges) >= FINE_LAYERS or not ranges:
        return sorted(rendered)
    picks = set()
    for lo, hi in ranges:
        inside = [v for v in rendered if lo <= v <= hi]
        if inside:
            picks.add(max(inside))
    return sorted(picks)


def rendered_velocities(session: Path) -> list[int]:
    """The velocities of the session's single-notes pass."""
    import tomllib

    data = tomllib.loads(session.read_text(encoding="utf-8"))
    for p in data.get("pass", []):
        if p.get("name") == "single-notes":
            v = p.get("velocities")
            if isinstance(v, list):
                return [int(x) for x in v]
    raise SystemExit(f"{session}: no single-notes pass with a list of velocities")
