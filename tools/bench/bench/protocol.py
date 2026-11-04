"""Measurement sessions: a TOML description becomes a list of takes, timed to the sample.

A take is a list of MIDI events (time in samples, bytes) plus a duration. It does not depend on any
instrument: the same take can be rendered by Kontakt, by PianoForge, or played on a real keyboard.
"""

from __future__ import annotations

import hashlib
import json
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

CHANNEL = 0  # MIDI channel 1
DEFAULT_RELEASE_VELOCITY = 64  # MIDI default when the keyboard does not measure release velocity

_LETTERS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
_NAMES = ["C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B"]  # "s" for sharp: safe in file names


def note_to_midi(name: str | int) -> int:
    """"C4" -> 60, "A0" -> 21, "F#3" or "Gb3" -> 54. Integers are returned as is."""
    if isinstance(name, int):
        n = name
    else:
        m = re.fullmatch(r"([A-Ga-g])(#|s|b)?(-?\d+)", name.strip())
        if not m:
            raise ValueError(f"invalid note name: {name!r}")
        letter, accidental, octave = m.groups()
        n = _LETTERS[letter.upper()] + {"#": 1, "s": 1, "b": -1, None: 0}[accidental] + 12 * (int(octave) + 1)
    if not 0 <= n <= 127:
        raise ValueError(f"note outside the MIDI range: {name!r}")
    return n


def midi_to_note(n: int) -> str:
    return f"{_NAMES[n % 12]}{n // 12 - 1}"


def _ms(seconds: float) -> int:
    """Durations appear in take names as whole milliseconds: no dot, so no ambiguity with file extensions."""
    return round(seconds * 1000)


def _values(spec, convert) -> list[int]:
    """Accepts a value, a list, or a range "first..last" or "first..last:step"."""
    if isinstance(spec, list):
        return [convert(x) for x in spec]
    if isinstance(spec, str):
        m = re.fullmatch(r"\s*(\S+?)\s*\.\.\s*(\S+?)\s*(?::\s*(\d+))?\s*", spec)
        if m:
            first, last, step = convert(m[1]), convert(m[2]), int(m[3] or 1)
            if last < first or step < 1:
                raise ValueError(f"invalid range: {spec!r}")
            return list(range(first, last + 1, step))
    return [convert(spec)]


def _velocity(v) -> int:
    v = int(v)
    if not 1 <= v <= 127:
        raise ValueError(f"velocity outside 1..127: {v}")
    return v


def _interpolate(points: list[list[float]], note: int) -> float:
    """Piecewise-linear law [[note, value], ...], constant beyond both ends."""
    pts = sorted((note_to_midi(p[0]) if isinstance(p[0], str) else int(p[0]), float(p[1])) for p in points)
    if note <= pts[0][0]:
        return pts[0][1]
    for (n0, v0), (n1, v1) in zip(pts, pts[1:]):
        if note <= n1:
            return v0 + (v1 - v0) * (note - n0) / (n1 - n0)
    return pts[-1][1]


def note_on(n: int, v: int) -> tuple[int, ...]:
    return (0x90 | CHANNEL, n, v)


def note_off(n: int, v: int = DEFAULT_RELEASE_VELOCITY) -> tuple[int, ...]:
    return (0x80 | CHANNEL, n, v)


def cc(number: int, value: int) -> tuple[int, ...]:
    return (0xB0 | CHANNEL, number, value)


@dataclass
class Take:
    pass_name: str
    id: str  # relative path without extension, e.g. "single-notes/060-C4_v064_h20000ms"
    duration: int  # in samples
    events: list[tuple[int, tuple[int, ...]]]
    meta: dict = field(default_factory=dict)

    def key(self, context: dict) -> str:
        """Fingerprint of the take and of its render context: used to resume an interrupted generation."""
        content = {"id": self.id, "duration": self.duration, "events": self.events, "context": context}
        return hashlib.sha256(json.dumps(content, sort_keys=True).encode()).hexdigest()[:16]


@dataclass
class Session:
    name: str
    sample_rate: int
    takes: list[Take]
    source: str  # original TOML text, copied into the corpus manifest

    def passes(self) -> list[str]:
        return list(dict.fromkeys(t.pass_name for t in self.takes))


class _Builder:
    def __init__(self, pass_name: str, fs: int):
        self.pass_name, self.fs = pass_name, fs

    def samples(self, seconds: float) -> int:
        return round(seconds * self.fs)

    def take(self, name: str, duration_s: float, events: list[tuple[float, tuple[int, ...]]], **meta) -> Take:
        duration = self.samples(duration_s)
        evts = sorted(((self.samples(t), b) for t, b in events), key=lambda e: e[0])
        if evts and evts[-1][0] >= duration:
            raise ValueError(f"{name}: an event falls after the end of the take")
        return Take(self.pass_name, f"{self.pass_name}/{name}", duration, evts, {"pass": self.pass_name, **meta})


def _pass_note(b: _Builder, cfg: dict) -> list[Take]:
    """Single note. Also covers releases (several holds) and the sustain pedal held down."""
    notes = _values(cfg["notes"], note_to_midi)
    velocities = _values(cfg["velocities"], _velocity)
    pedal = cfg.get("pedal", "up")
    if pedal not in ("up", "down"):
        raise ValueError(f"unknown pedal state: {pedal!r} (expected 'up' or 'down')")
    preroll = float(cfg.get("preroll", 1.0 if pedal == "down" else 0.05))
    tail = float(cfg.get("tail", 3.0))
    pedal_hold = float(cfg.get("pedal_hold", 2.0))
    takes = []
    for n in notes:
        if "hold_by_note" in cfg:
            holds = [round(_interpolate(cfg["hold_by_note"], n), 3)]
        elif "holds" in cfg:
            holds = [float(h) for h in cfg["holds"]]
        else:
            holds = [float(cfg["hold"])]
        for h in holds:
            for v in velocities:
                evts = [(preroll, note_on(n, v)), (preroll + h, note_off(n))]
                end = preroll + h + tail
                if pedal == "down":
                    evts += [(0.0, cc(64, 127)), (preroll + h + pedal_hold, cc(64, 0))]
                    end += pedal_hold
                name = f"{n:03d}-{midi_to_note(n)}_v{v:03d}_h{_ms(h)}ms"
                takes.append(b.take(name, end, evts, type="note", note=n, note_name=midi_to_note(n), velocity=v,
                                    hold=h, pedal=pedal, note_on_sample=b.samples(preroll),
                                    note_off_sample=b.samples(preroll + h)))
    return takes


def _pass_repetition(b: _Builder, cfg: dict) -> list[Take]:
    """Repeated notes: the key comes back up part-way through the interval before the next strike."""
    notes = _values(cfg["notes"], note_to_midi)
    velocities = _values(cfg["velocities"], _velocity)
    strikes = int(cfg.get("strikes", 4))
    hold_ratio = float(cfg.get("hold_ratio", 0.6))
    preroll, tail, final_hold = 0.05, float(cfg.get("tail", 3.0)), float(cfg.get("final_hold", 2.0))
    takes = []
    for n in notes:
        for dt in cfg["intervals"]:
            for v in velocities:
                evts = []
                for i in range(strikes):
                    t_on = preroll + i * dt
                    t_off = t_on + (final_hold if i == strikes - 1 else dt * hold_ratio)
                    evts += [(t_on, note_on(n, v)), (t_off, note_off(n))]
                end = preroll + (strikes - 1) * dt + final_hold + tail
                name = f"{n:03d}-{midi_to_note(n)}_v{v:03d}_dt{_ms(dt)}ms"
                takes.append(b.take(name, end, evts, type="repetition", note=n, note_name=midi_to_note(n), velocity=v,
                                    interval=dt, strikes=strikes, note_on_sample=b.samples(preroll)))
    return takes


def _pass_pedal_ramp(b: _Builder, cfg: dict) -> list[Take]:
    """Half pedal: pedal down, note struck then released, pedal lifted progressively."""
    notes = _values(cfg["notes"], note_to_midi)
    velocities = _values(cfg["velocities"], _velocity)
    preroll, wait = 1.0, float(cfg.get("wait", 1.0))
    ramp, steps, tail = float(cfg.get("ramp_duration", 4.0)), int(cfg.get("steps", 32)), float(cfg.get("tail", 2.0))
    takes = []
    for n in notes:
        for v in velocities:
            ramp_start = preroll + wait
            evts = [(0.0, cc(64, 127)), (preroll, note_on(n, v)), (preroll + 0.2, note_off(n))]
            evts += [(ramp_start + ramp * i / steps, cc(64, round(127 * (1 - i / steps)))) for i in range(1, steps + 1)]
            name = f"{n:03d}-{midi_to_note(n)}_v{v:03d}_ramp{_ms(ramp)}ms"
            takes.append(b.take(name, ramp_start + ramp + tail, evts, type="pedal_ramp", note=n,
                                note_name=midi_to_note(n), velocity=v, note_on_sample=b.samples(preroll)))
    return takes


def _pass_pedal_only(b: _Builder, cfg: dict) -> list[Take]:
    hold, tail = float(cfg.get("hold", 2.0)), float(cfg.get("tail", 2.0))
    evts = [(0.05, cc(64, 127)), (0.05 + hold, cc(64, 0))]
    return [b.take(f"pedal_h{_ms(hold)}ms", 0.05 + hold + tail, evts, type="pedal_only")]


def _pass_silence(b: _Builder, cfg: dict) -> list[Take]:
    duration = float(cfg.get("duration", 5.0))
    return [b.take(f"silence_{_ms(duration)}ms", duration, [], type="silence")]


_PASSES = {
    "note": _pass_note,
    "repetition": _pass_repetition,
    "pedal_ramp": _pass_pedal_ramp,
    "pedal_only": _pass_pedal_only,
    "silence": _pass_silence,
}


def parse_session(text: str) -> Session:
    data = tomllib.loads(text)
    header = data.get("session", {})
    fs = int(header.get("sample_rate", 48000))
    takes: list[Take] = []
    names = set()
    for cfg in data.get("pass", []):
        name, kind = cfg["name"], cfg["type"]
        if name in names:
            raise ValueError(f"duplicate pass: {name!r}")
        names.add(name)
        if kind not in _PASSES:
            raise ValueError(f"unknown pass type: {kind!r} (expected one of: {', '.join(_PASSES)})")
        takes += _PASSES[kind](_Builder(name, fs), cfg)
    # File names are compared case-insensitively on Windows: two takes must never share one.
    if len({t.id.lower() for t in takes}) != len(takes):
        raise ValueError("two takes share the same id")
    return Session(header.get("name", "session"), fs, takes, text)


def load_session(path: str | Path) -> Session:
    return parse_session(Path(path).read_text(encoding="utf-8"))
