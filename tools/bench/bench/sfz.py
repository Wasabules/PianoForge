"""Offline SFZ player: renders the bench's takes from an SFZ sample bank, as an SFZ sampler (sfizz) plays it.

Free sample banks (the Salamander Grand Piano, the Piano in 162...) are references as the Kontakt library is: the
same sessions, the same corpus, so the forge measures them the same way. Each take is played from silence, with
nothing carried over from the take before (Kontakt is silenced between takes for the same reason). What the bank
draws at random (lorand/hirand: its round robins) depends only on the strike: its key, its velocity and how many
times the key was struck before in the take. The same strike plays the same sample in every take (the forge compares
a release take with the held note, sample for sample until the release), and renders are reproducible.

What is played (what piano banks use: SFZ 1 and 2, and the ARIA extensions of the SFZ Instruments pianos):
  preprocessor  #define $NAME value (substituted from there on), #include "file" (relative to the root .sfz, also
                within a line), // and /* */ comments
  headers       <control> (default_path, set_ccN, set_hdccN, note_offset, octave_offset), <global>, <master>,
                <group>, <region> (each inherits the levels above it), <curve> (curve_index, vN)
  selection     key, lokey, hikey, lovel, hivel, loccN/hiccN, lohdccN/hihdccN, sw_lokey, sw_hikey, sw_last,
                sw_default, seq_length, seq_position, lorand, hirand, trigger (attack, release, release_key, first,
                legato), on_loccN/on_hiccN (regions a controller starts)
  playback      sample, offset, end, delay, pitch_keycenter, pitch_keytrack, pitch_veltrack, tune, transpose,
                volume, amplitude, amp_veltrack, amp_velcurve_N, amp_keytrack, amp_keycenter, pan, width,
                ampeg_delay/start/attack/hold/decay/sustain/release, rt_decay, note_polyphony, polyphony, group,
                off_by, off_mode, off_time, loop_mode (no_loop, one_shot); and the controllers' depths on them
                (_onccN or _ccN, with _curveccN)
  pedals        sustain (CC64 from 64): a released key rings on until the pedal lifts, and its release regions sound
                then; sostenuto (CC66): the keys down when it went down
An opcode outside these is listed in SfzInstrument.unsupported (and in the corpus manifest), never dropped silently:
a bank that relies on it is not played as its sampler would.

Where SFZ leaves it open, the player follows sfizz:
  - velocities and controllers are normalized (0 to 1); a controller's depth on `amplitude` multiplies (the
    controller at 0 silences, at 1 gives the depth in %), on every other opcode it adds depth x curve(value);
  - curves 0 to 6 are linear, bipolar, inverted, inverted bipolar, x^2, sqrt(x) and sqrt(1 - x);
  - the default velocity curve is the square of the velocity; amp_veltrack t (from -100 to 100 %) mixes it as
    (1 - |t|) + |t| x curve (1 - curve when t < 0);
  - the envelope's attack is linear; its decay and release are exponential, exp(-9) (-78 dB) over the time given and
    never shorter than 25 ms; a voice cut by another (off_by, polyphony) fades over 10 ms (off_mode fast, the
    default), its ampeg_release (normal) or off_time (time, which off_time alone selects);
  - a release region sounds at the velocity of the note-on, rt_decay dB lower for every second the key was held;
  - a controller starts its regions (on_loccN) at velocity 127; one random number is drawn per event (a note-on,
    a note-off, a controller), for all its regions; the sequence counters (seq_position) are per region;
  - a new voice beyond note_polyphony (same key, same group) or polyphony (same group) cuts the oldest one.
Samples are pitched and brought to the session's rate by pedalboard's windowed-sinc resampler (64 taps: spurious
components 108 dB down); a sample at its own pitch and rate is played as it is, sample for sample.

Two departures from the sampler. A draw that falls between the random ranges of a bank's regions (Piano in 162:
from 0.500 to 0.501, between its two round robins) plays the regions of the nearest range, not silence: one strike
in a thousand would be silent in a sampler, and the forge would measure it. And, asked for by the state (align_ms),
the attacks aligned. A bank's samples may start
with silence, unevenly (Piano in 162: 0.6 ms in the median, up to 70 ms in some forte bass samples), which a
sampler plays as latency and the forge would learn as such (it times everything from the note-on). Aligned, a
sample starts align_ms before its attack (the bench's: the first crossing of its peak - 30 dB), faded in over 1 ms;
one already starting within align_ms of its attack, or where the bank sets an offset, plays as it is.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from collections import Counter, OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

import numpy as np
import soundfile as sf

from bench.protocol import Take

PLAYER_VERSION = "3"  # 2: the random draws depend on the strike, not on the take; 3: no gap between ranges
SUSTAIN_CC, SOSTENUTO_CC = 64, 66
PEDAL_DOWN = 64                  # a pedal is down from this raw value
EG_RATE = 9.0                    # exponential segments: exp(-9) over the time given
EG_MIN_S = 0.025
FAST_OFF_S = 0.010
END_LEVEL = 1e-5                 # a released voice ends below -100 dB
DEFAULT_CC = {7: 100 / 127, 10: 0.5, 11: 1.0}
CACHE_BYTES = 400 << 20          # samples and pitched samples kept in memory, per process
ONSET_DB = -30.0                 # a sample's attack: the first crossing of its peak - 30 dB (the bench's)
ALIGN_FADE_S = 0.001             # fade-in of a sample started past its beginning
ATTACK_TRIGGERS = ("attack", "first", "legato")
TRIGGERS = ATTACK_TRIGGERS + ("release", "release_key")

_NOTES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
_KEY_OPCODES = {"key", "lokey", "hikey", "pitch_keycenter", "sw_lokey", "sw_hikey", "sw_last", "sw_default",
                "amp_keycenter"}
_DIRECTIVE = re.compile(r'#define\s+(\$\w+)\s+(\S+)|#include\s+"([^"]+)"')
_VARIABLE = re.compile(r"\$\w+")
_TOKEN = re.compile(r"<(\w+)>|(?:(?<=\s)|(?<=>)|^)([A-Za-z_][A-Za-z0-9_]*)=", re.M)

# Opcodes the player reads (a controller's depth on them is recognized by _MODULATION below).
_PLAYED = {
    "sample", "offset", "end", "delay", "key", "lokey", "hikey", "lovel", "hivel", "pitch_keycenter",
    "pitch_keytrack", "pitch_veltrack", "tune", "transpose", "volume", "amplitude", "amp_veltrack", "amp_keytrack",
    "amp_keycenter", "pan", "width", "ampeg_delay", "ampeg_start", "ampeg_attack", "ampeg_hold", "ampeg_decay",
    "ampeg_sustain", "ampeg_release", "rt_decay", "note_polyphony", "polyphony", "group", "off_by", "off_mode",
    "off_time", "loop_mode", "trigger", "sw_lokey", "sw_hikey", "sw_last", "sw_default", "seq_length", "seq_position",
    "lorand", "hirand",
}
# Opcodes that change nothing in the sound (names and labels, editor hints, what a host shows).
_COSMETIC = re.compile(r"^(\w*_label|label_cc\d+|hint_\w+|image|note_selfmask|sw_label|note_name|key_label\d*|"
                       r"master_label|global_label|group_label|region_label)$")
_MODULATED = ("amplitude", "volume", "pan", "amp_veltrack", "offset", "delay", "tune", "pitch", "ampeg_delay",
              "ampeg_start", "ampeg_attack", "ampeg_hold", "ampeg_decay", "ampeg_sustain", "ampeg_release")
_MODULATION = re.compile(r"^(" + "|".join(_MODULATED) + r"|gain)_(?:on)?cc(\d+)$")
_MOD_CURVE = re.compile(r"^(" + "|".join(_MODULATED) + r"|gain)_curvecc(\d+)$")
_CONDITION = re.compile(r"^(lo|hi)(hd)?cc(\d+)$")
_CC_TRIGGER = re.compile(r"^on_(lo|hi)(hd)?cc(\d+)$")
_VELCURVE = re.compile(r"^amp_velcurve_(\d+)$")

Log = Callable[[str], None]


# --- curves ------------------------------------------------------------------------------------------------

def _default_curve(index: int) -> np.ndarray:
    x = np.arange(128) / 127.0
    table = {0: x, 1: 2 * x - 1, 2: 1 - x, 3: 1 - 2 * x, 4: x * x, 5: np.sqrt(x), 6: np.sqrt(1 - x)}
    return table.get(index, x)


def _points_curve(points: dict[int, float], first: float = 0.0, last: float = 1.0) -> np.ndarray:
    """128 values from some of them, linear in between; v000 and v127 default to `first` and `last`."""
    pts = {0: first, 127: last, **points}
    xs = sorted(pts)
    return np.interp(np.arange(128), xs, [pts[k] for k in xs])


def _eval(curve: np.ndarray, x: float) -> float:
    return float(np.interp(min(1.0, max(0.0, x)) * 127.0, np.arange(128), curve))


# --- preprocessing and parsing -----------------------------------------------------------------------------

def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _substitute(text: str, defines: dict[str, str]) -> str:
    if "$" not in text:
        return text

    def value(m: re.Match) -> str:
        name = m[0]
        while len(name) > 1:  # the longest defined name: "$VELTRACK" before "$VEL"
            if name in defines:
                return defines[name] + m[0][len(name):]
            name = name[:-1]
        return m[0]
    return _VARIABLE.sub(value, text)


def _expand(path: Path, root: Path, defines: dict[str, str], depth: int = 0) -> str:
    """The text of an SFZ file with its includes inlined and its variables substituted, in order."""
    if depth > 32:
        raise ValueError(f"{path}: includes nested too deep")
    text = _strip_comments(_resolve(path).read_text(encoding="utf-8", errors="replace"))
    out, pos = [], 0
    for m in _DIRECTIVE.finditer(text):
        out.append(_substitute(text[pos:m.start()], defines))
        if m[1]:
            defines[m[1]] = _substitute(m[2], defines)
        else:
            out.append("\n" + _expand(root / _substitute(m[3], defines).replace("\\", "/"), root, defines, depth + 1)
                       + "\n")
        pos = m.end()
    out.append(_substitute(text[pos:], defines))
    return "".join(out)


def _tokens(text: str) -> list[tuple[str, dict[str, str]]]:
    """[(header, {opcode: value})] in order. A value runs to the next opcode or the end of its line (sample names
    may hold spaces)."""
    items: list[tuple[str, dict[str, str]]] = []
    found = list(_TOKEN.finditer(text))
    for i, m in enumerate(found):
        if m[1]:
            items.append((m[1].lower(), {}))
            continue
        end = found[i + 1].start() if i + 1 < len(found) else len(text)
        value = text[m.end():end].split("\n", 1)[0].strip()
        if not items:
            items.append(("", {}))
        items[-1][1][m[2]] = value
    return items


def _resolve(path: Path) -> Path:
    """The file, its name's case corrected if needed (banks made on Windows, read where case matters)."""
    if path.exists():
        return path
    parts, current = path.parts, Path(path.anchor) if path.is_absolute() else Path(".")
    for part in parts[1:] if path.is_absolute() else parts:
        candidate = current / part
        if not candidate.exists() and current.is_dir():
            lower = part.lower()
            match = next((c for c in current.iterdir() if c.name.lower() == lower), None)
            candidate = match if match is not None else candidate
        current = candidate
    return current


def parse_key(value: str, offset: int = 0) -> int:
    """MIDI note of an SFZ key value: a number or a name ("c4" = 60, "c#4", "db4")."""
    v = value.strip().lower()
    if re.fullmatch(r"-?\d+", v):
        n = int(v)
    else:
        m = re.fullmatch(r"([a-g])(#|b)?(-?\d+)", v)
        if not m:
            raise ValueError(f"invalid key: {value!r}")
        n = _NOTES[m[1]] + {"#": 1, "b": -1, None: 0}[m[2]] + 12 * (int(m[3]) + 1)
    return n + offset if n >= 0 else n


# --- the instrument ----------------------------------------------------------------------------------------

@dataclass
class Modulation:
    depth: float
    curve: int = 0


@dataclass
class Region:
    index: int
    ops: dict[str, str]
    sample: Path
    lokey: int
    hikey: int
    keycenter: int
    lovel: int
    hivel: int
    trigger: str
    conditions: list[tuple[int, float, float]]        # (cc, low, high), normalized
    cc_trigger: tuple[int, float, float] | None       # (cc, low, high): started by that controller
    switch: tuple[int, int, int | None] | None        # (sw_lokey, sw_hikey, sw_last)
    mods: dict[str, dict[int, Modulation]] = field(default_factory=dict)
    velcurve: np.ndarray | None = None

    def num(self, name: str, default: float) -> float:
        value = self.ops.get(name)
        return float(value) if value not in (None, "") else default

    def text(self, name: str, default: str) -> str:
        return self.ops.get(name, default).strip().lower() or default


class SfzInstrument:
    """An SFZ bank, parsed: its regions with their opcodes, curves and controllers' initial values."""

    def __init__(self, path: str | Path, cc: dict[int, float] | None = None):
        self.path = Path(path).resolve()
        self.root = self.path.parent
        defines: dict[str, str] = {}
        self.text = _expand(self.path, self.root, defines)
        self.curves: dict[int, np.ndarray] = {}
        self.cc = np.zeros(128)
        for n, v in DEFAULT_CC.items():
            self.cc[n] = v
        self.regions: list[Region] = []
        self.unsupported: Counter[str] = Counter()
        self.switch_default: int | None = None
        self._build(_tokens(self.text))
        for n, v in (cc or {}).items():
            self.cc[int(n)] = min(1.0, max(0.0, float(v)))

    # -- building

    def _build(self, items) -> None:
        control: dict[str, str] = {}
        levels: dict[str, dict[str, str]] = {"global": {}, "master": {}, "group": {}}
        merged: list[tuple[dict[str, str], dict[str, str]]] = []
        for header, ops in items:
            if "key" in ops:  # key= is lokey, hikey and pitch_keycenter at its own level (a region's beats its group's)
                for name in ("lokey", "hikey", "pitch_keycenter"):
                    ops.setdefault(name, ops["key"])
            if header == "control":
                control.update(ops)
                for name, value in ops.items():
                    m = re.fullmatch(r"set_(hd)?cc(\d+)", name)
                    if m:
                        self.cc[int(m[2])] = float(value) if m[1] else float(value) / 127.0
                    elif name not in ("default_path", "note_offset", "octave_offset") and not _COSMETIC.match(name):
                        self.unsupported[f"<control> {name}"] += 1
            elif header == "global":
                levels = {"global": ops, "master": {}, "group": {}}
            elif header == "master":
                levels["master"], levels["group"] = ops, {}
            elif header == "group":
                levels["group"] = ops
            elif header == "region":
                merged.append(({**levels["global"], **levels["master"], **levels["group"], **ops}, dict(control)))
            elif header == "curve":
                points = {int(k[1:]): float(v) for k, v in ops.items() if re.fullmatch(r"v\d{3}", k)}
                if "curve_index" in ops:
                    self.curves[int(ops["curve_index"])] = _points_curve(points)
            else:
                self.unsupported[f"<{header}>"] += 1
        for ops, ctl in merged:
            self._region(ops, ctl)

    def _region(self, ops: dict[str, str], control: dict[str, str]) -> None:
        offset = int(float(control.get("note_offset", 0))) + 12 * int(float(control.get("octave_offset", 0)))
        keyed = {k: (str(parse_key(v, offset)) if k in _KEY_OPCODES else v) for k, v in ops.items()}
        if "sample" not in keyed:
            return  # a region without a sample plays nothing
        name = keyed["sample"].replace("\\", "/")
        sample = _resolve(self.root / control.get("default_path", "").replace("\\", "/") / name)
        conditions, cc_trigger, mods, velpoints = [], None, {}, {}
        lows: dict[int, float] = {}
        highs: dict[int, float] = {}
        trig_lo: dict[int, float] = {}
        trig_hi: dict[int, float] = {}
        for k, v in keyed.items():
            if k in _PLAYED or _COSMETIC.match(k):
                continue
            if m := _CONDITION.match(k):
                value = float(v) if m[2] else float(v) / 127.0
                (lows if m[1] == "lo" else highs)[int(m[3])] = value
            elif m := _CC_TRIGGER.match(k):
                value = float(v) if m[2] else float(v) / 127.0
                (trig_lo if m[1] == "lo" else trig_hi)[int(m[3])] = value
            elif m := _MODULATION.match(k):
                target = "volume" if m[1] == "gain" else m[1]
                mods.setdefault(target, {}).setdefault(int(m[2]), Modulation(0.0)).depth = float(v)
            elif m := _MOD_CURVE.match(k):
                target = "volume" if m[1] == "gain" else m[1]
                mods.setdefault(target, {}).setdefault(int(m[2]), Modulation(0.0)).curve = int(float(v))
            elif m := _VELCURVE.match(k):
                velpoints[int(m[1])] = float(v)
            else:
                self.unsupported[k] += 1
        for n in sorted(set(lows) | set(highs)):
            conditions.append((n, lows.get(n, 0.0), highs.get(n, 1.0)))
        if trig_lo or trig_hi:
            n = sorted(set(trig_lo) | set(trig_hi))[0]
            cc_trigger = (n, trig_lo.get(n, 0.0), trig_hi.get(n, 1.0))
        switch = None
        if "sw_last" in keyed or "sw_lokey" in keyed:
            switch = (int(keyed.get("sw_lokey", 0)), int(keyed.get("sw_hikey", 127)),
                      int(keyed["sw_last"]) if "sw_last" in keyed else None)
            if "sw_default" in keyed and self.switch_default is None:
                self.switch_default = int(keyed["sw_default"])
        loop = keyed.get("loop_mode", "no_loop").strip().lower()
        if loop not in ("no_loop", "one_shot"):
            self.unsupported[f"loop_mode={loop}"] += 1
        trigger = keyed.get("trigger", "attack").strip().lower()
        if trigger not in TRIGGERS:
            self.unsupported[f"trigger={trigger}"] += 1
        lokey = int(keyed.get("lokey", 0))
        hikey = int(keyed.get("hikey", 127))
        region = Region(
            index=len(self.regions), ops=keyed, sample=sample, lokey=lokey, hikey=hikey,
            keycenter=int(keyed.get("pitch_keycenter", 60)), lovel=int(float(keyed.get("lovel", 1))),
            hivel=int(float(keyed.get("hivel", 127))), trigger=trigger,
            conditions=conditions, cc_trigger=cc_trigger, switch=switch, mods=mods,
            velcurve=_points_curve({k: v for k, v in velpoints.items()}) if velpoints else None)
        self.regions.append(region)

    # -- what it holds

    def curve(self, index: int) -> np.ndarray:
        return self.curves.get(index) if index in self.curves else _default_curve(index)

    def samples(self) -> list[Path]:
        return sorted({r.sample for r in self.regions})

    def missing(self) -> list[Path]:
        return [p for p in self.samples() if not p.exists()]

    def fingerprint(self) -> str:
        """The bank's text after preprocessing, the controllers set, and each sample's name and size."""
        h = hashlib.sha256(self.text.encode("utf-8"))
        h.update(json.dumps([round(float(v), 6) for v in self.cc]).encode())
        for p in self.samples():
            h.update(f"{p.relative_to(self.root).as_posix() if p.is_relative_to(self.root) else p}"
                     f":{p.stat().st_size if p.exists() else -1}".encode())
        return h.hexdigest()[:16]

    def describe(self) -> dict:
        return {"regions": len(self.regions), "samples": len(self.samples()),
                "triggers": dict(Counter(r.trigger if r.cc_trigger is None else "cc" for r in self.regions)),
                "keys": [min(r.lokey for r in self.regions), max(r.hikey for r in self.regions)],
                "unsupported": dict(self.unsupported)}


# --- the sample cache --------------------------------------------------------------------------------------

class _Cache:
    """Least recently used, bounded in bytes: the samples as read, and the pitched ones."""

    def __init__(self, limit: int = CACHE_BYTES):
        self.limit, self.used, self.items = limit, 0, OrderedDict()

    def get(self, key):
        if key in self.items:
            self.items.move_to_end(key)
            return self.items[key][0]
        return None

    def put(self, key, value, nbytes: int) -> None:
        if key in self.items:
            self.used -= self.items.pop(key)[1]
        self.items[key] = (value, nbytes)
        self.used += nbytes
        while self.used > self.limit and len(self.items) > 1:
            _, (_, size) = self.items.popitem(last=False)
            self.used -= size


# --- voices ------------------------------------------------------------------------------------------------

@dataclass
class _Voice:
    region: Region
    key: int
    velocity: float          # normalized
    start: int               # output sample where it starts
    gains: tuple[float, float]
    cents: float
    offset: int
    envelope: dict[str, float]
    group: int
    off_by: int | None
    order: int
    event: int               # the note-on (or controller) that started it: its siblings never cut it
    releases: list[tuple[int, float]] = field(default_factory=list)  # (sample, release time s)
    sustained: bool = False  # key up, held by a pedal

    @property
    def released(self) -> bool:
        return bool(self.releases)


class SfzPlayer:
    """Plays takes (lists of MIDI events) from an SfzInstrument, at a given output rate."""

    def __init__(self, instrument: SfzInstrument, fs: int, cache_bytes: int = CACHE_BYTES,
                 align_ms: float | None = None):
        self.instrument, self.fs, self.align_ms = instrument, fs, align_ms
        self.cache = _Cache(cache_bytes)
        self.switch_ranges = [(r.switch[0], r.switch[1]) for r in instrument.regions if r.switch]
        self.onsets: dict[Path, int] = {}

    # -- samples

    def _sample(self, path: Path) -> tuple[np.ndarray, int]:
        cached = self.cache.get(("raw", path))
        if cached is not None:
            return cached
        data, sr = sf.read(path, dtype="float32", always_2d=True)
        data = data.T
        if data.shape[0] == 1:
            data = np.repeat(data, 2, axis=0)
        entry = (np.ascontiguousarray(data[:2]), int(sr))
        self.cache.put(("raw", path), entry, entry[0].nbytes)
        return entry

    def aligned_start(self, path: Path) -> int:
        """Where the sample starts once its attack is aligned (0: at its beginning, as the bank has it)."""
        if self.align_ms is None:
            return 0
        if path not in self.onsets:
            data, sr = self._sample(path)
            envelope = np.max(np.abs(data), axis=0)
            above = np.flatnonzero(envelope > float(envelope.max()) * 10 ** (ONSET_DB / 20))
            lead = int(round(self.align_ms / 1000.0 * sr))
            self.onsets[path] = int(above[0]) - lead if above.size and above[0] > lead else 0
        return self.onsets[path]

    def _pitched(self, path: Path, start: int, stop: int, cents: float, frames: int) -> np.ndarray:
        """The sample from `start` to `stop` (source frames), pitched by `cents` and brought to the output rate:
        at least `frames` output frames when the sample is that long."""
        data, sr = self._sample(path)
        ratio = 2.0 ** (cents / 1200.0)
        source = data[:, start:stop]
        if abs(ratio - 1.0) < 1e-12 and sr == self.fs:
            return source
        key = ("pitched", path, start, stop, round(cents, 6))
        cached = self.cache.get(key)
        if cached is not None and (cached[1] or cached[0].shape[1] >= frames):  # (audio, the whole sample)
            return cached[0]
        from pedalboard import Resample
        from pedalboard.io import StreamResampler

        need = min(source.shape[1], int(math.ceil(frames * sr * ratio / self.fs)) + 1024)
        resampler = StreamResampler(sr * ratio, float(self.fs), 2, Resample.Quality.WindowedSinc64)
        out = np.concatenate([resampler.process(np.ascontiguousarray(source[:, :need])), resampler.process(None)],
                             axis=1)
        self.cache.put(key, (out, need == source.shape[1]), out.nbytes)
        return out

    # -- controllers and selection

    def _mod(self, region: Region, name: str, cc: np.ndarray) -> float:
        total = 0.0
        for n, m in region.mods.get(name, {}).items():
            total += m.depth * _eval(self.instrument.curve(m.curve), cc[n])
        return total

    def _amplitude_factor(self, region: Region, cc: np.ndarray) -> float:
        factor = region.num("amplitude", 100.0) / 100.0
        for n, m in region.mods.get("amplitude", {}).items():
            factor *= m.depth / 100.0 * _eval(self.instrument.curve(m.curve), cc[n])
        return factor

    @staticmethod
    def _conditions(region: Region, cc: np.ndarray) -> bool:
        return all(lo - 1e-9 <= cc[n] <= hi + 1e-9 for n, lo, hi in region.conditions)

    # -- voices

    def _voice(self, region: Region, key: int, velocity: float, t: int, cc: np.ndarray, order: int, event: int,
               held_s: float = 0.0) -> _Voice:
        r = region
        veltrack = (r.num("amp_veltrack", 100.0) + self._mod(r, "amp_veltrack", cc)) / 100.0
        veltrack = max(-1.0, min(1.0, veltrack))
        curve = _eval(r.velcurve, velocity) if r.velcurve is not None else velocity * velocity
        if veltrack < 0:
            curve = 1.0 - curve
        vel_gain = (1.0 - abs(veltrack)) + abs(veltrack) * curve
        db = r.num("volume", 0.0) + self._mod(r, "volume", cc)
        db += r.num("amp_keytrack", 0.0) * (key - int(r.num("amp_keycenter", 60)))
        if r.trigger in ("release", "release_key"):
            db -= r.num("rt_decay", 0.0) * held_s
        gain = 10.0 ** (db / 20.0) * self._amplitude_factor(r, cc) * vel_gain
        pan = max(-1.0, min(1.0, (r.num("pan", 0.0) + self._mod(r, "pan", cc)) / 100.0))
        if pan == 0.0:
            gains = (gain, gain)
        else:  # constant power, unity at the centre
            angle = (pan + 1.0) * math.pi / 4.0
            gains = (gain * math.sqrt(2.0) * math.cos(angle), gain * math.sqrt(2.0) * math.sin(angle))
        cents = ((key - r.keycenter) * r.num("pitch_keytrack", 100.0) + r.num("tune", 0.0)
                 + self._mod(r, "tune", cc) + self._mod(r, "pitch", cc) + 100.0 * r.num("transpose", 0.0)
                 + r.num("pitch_veltrack", 0.0) * velocity)
        offset = max(0, int(round(r.num("offset", 0.0) + self._mod(r, "offset", cc))))
        delay = max(0.0, r.num("delay", 0.0) + self._mod(r, "delay", cc))
        env = {name: max(0.0, r.num(f"ampeg_{name}", default) + self._mod(r, f"ampeg_{name}", cc))
               for name, default in (("delay", 0.0), ("start", 0.0), ("attack", 0.0), ("hold", 0.0),
                                     ("decay", 0.0), ("sustain", 100.0), ("release", 0.0))}
        off_by = r.ops.get("off_by")
        return _Voice(region=r, key=key, velocity=velocity, start=t + int(round(delay * self.fs)), gains=gains,
                      cents=cents, offset=offset, envelope=env, group=int(r.num("group", 0)),
                      off_by=int(off_by) if off_by not in (None, "") else None, order=order, event=event)

    def _off_time(self, region: Region) -> float:
        mode = region.text("off_mode", "time" if "off_time" in region.ops else "fast")
        if mode == "normal":
            return max(region.num("ampeg_release", 0.0), EG_MIN_S)
        if mode == "time":
            return region.num("off_time", FAST_OFF_S)
        return FAST_OFF_S

    def _cut(self, v: _Voice, t: int) -> None:
        if v.start <= t:
            v.releases.append((t, self._off_time(v.region)))
        else:
            v.releases.append((v.start, 0.0))  # not started yet: never sounds

    # -- rendering

    def render(self, events: list[tuple[int, tuple[int, ...]]], n: int) -> np.ndarray:
        """The take: events (time in samples, MIDI bytes) over n samples; returns (2, n) float32."""
        inst = self.instrument
        cc = inst.cc.copy()
        drawn: Counter = Counter()

        def draw(*what) -> float:
            """A number in [0, 1) for this event: the same for the same strike (or the same pedal press), whatever
            else the take holds."""
            count = drawn[what]
            drawn[what] += 1
            digest = hashlib.sha256(repr((*what, count)).encode()).digest()
            return int.from_bytes(digest[:8], "little") / 2.0 ** 64

        voices: list[_Voice] = []
        held: dict[int, tuple[int, float]] = {}       # key -> (note-on sample, velocity)
        pending: list[tuple[int, int, float]] = []    # key up under the sustain pedal: (key, note-on sample, velocity)
        sostenuto: set[int] = set()
        sostenuto_pending: list[tuple[int, int, float]] = []
        switch = inst.switch_default
        counters: Counter[int] = Counter()
        order = event = 0

        def start(region: Region, key: int, velocity: float, t: int, held_s: float = 0.0) -> None:
            nonlocal order
            v = self._voice(region, key, velocity, t, cc, order, event, held_s)
            order += 1
            older = [o for o in voices if o.event != event]  # the voices of one event never cut each other
            for other in older:  # off_by, then the polyphonies
                if other.off_by is not None and other.off_by == v.group and not self._ended(other, t):
                    self._cut(other, t)
            for name, same_key in (("note_polyphony", True), ("polyphony", False)):
                limit = region.ops.get(name)
                if limit in (None, ""):
                    continue
                same = [o for o in older if o.group == v.group and (o.key == key or not same_key)
                        and not self._ended(o, t)]
                for o in sorted(same, key=lambda o: o.order)[:max(0, len(same) - int(float(limit)) + 1)]:
                    self._cut(o, t)
            voices.append(v)

        def matches(region: Region, key: int, vel: int) -> bool:
            if region.cc_trigger is not None or not (region.lokey <= key <= region.hikey):
                return False
            if not (region.lovel <= vel <= region.hivel) or not self._conditions(region, cc):
                return False
            if region.switch is not None and region.switch[2] is not None and switch != region.switch[2]:
                return False
            return True

        def release_regions(key: int, on: int, velocity: float, t: int, kinds: tuple[str, ...]) -> None:
            vel = max(1, min(127, int(round(velocity * 127))))
            number = draw("release", kinds, key, vel)
            candidates = [r for r in inst.regions if r.trigger in kinds and matches(r, key, vel)]
            for region in self._choose(candidates, number, counters):
                start(region, key, velocity, t, (t - on) / self.fs)

        def key_up(key: int, t: int) -> None:
            """The key's voices enter their release (a one-shot, or a release region, plays on to its end)."""
            for v in voices:
                if v.key == key and not v.released and not v.sustained and v.region.trigger in ATTACK_TRIGGERS \
                        and v.region.text("loop_mode", "no_loop") != "one_shot":
                    v.releases.append((max(t, v.start), v.envelope["release"]))

        for t, msg in events:
            event += 1
            status, data = msg[0] & 0xF0, msg[1:]
            if status == 0x90 and data[1] > 0:
                key, vel = data[0], data[1]
                if any(lo <= key <= hi for lo, hi in self.switch_ranges):
                    switch = key
                velocity = vel / 127.0
                held[key] = (t, velocity)
                number = draw("on", key, vel)
                others = len(held) > 1
                candidates = [r for r in inst.regions if r.trigger in ATTACK_TRIGGERS and matches(r, key, vel)
                              and not (r.trigger == "first" and others or r.trigger == "legato" and not others)]
                for region in self._choose(candidates, number, counters):
                    start(region, key, velocity, t)
            elif status == 0x80 or status == 0x90:
                key = data[0]
                if key not in held:
                    continue
                on, velocity = held.pop(key)
                release_regions(key, on, velocity, t, ("release_key",))
                if key in sostenuto:
                    sostenuto_pending.append((key, on, velocity))
                    for v in voices:
                        if v.key == key and not v.released:
                            v.sustained = True
                elif cc[SUSTAIN_CC] * 127 >= PEDAL_DOWN - 1e-6:
                    pending.append((key, on, velocity))
                    for v in voices:
                        if v.key == key and not v.released:
                            v.sustained = True
                else:
                    key_up(key, t)
                    release_regions(key, on, velocity, t, ("release",))
            elif status == 0xB0:
                number, value = data[0], data[1] / 127.0
                before = cc[number]
                cc[number] = value
                was_down, down = before * 127 >= PEDAL_DOWN - 1e-6, value * 127 >= PEDAL_DOWN - 1e-6
                if number == SUSTAIN_CC and was_down and not down:
                    for key, on, velocity in pending:
                        if key in held or key in sostenuto:
                            continue
                        for v in voices:
                            if v.key == key and v.sustained:
                                v.sustained = False
                        key_up(key, t)
                        release_regions(key, on, velocity, t, ("release",))
                    pending = [p for p in pending if p[0] in held or p[0] in sostenuto]
                elif number == SOSTENUTO_CC and down and not was_down:
                    sostenuto = set(held)
                elif number == SOSTENUTO_CC and was_down and not down:
                    keep = cc[SUSTAIN_CC] * 127 >= PEDAL_DOWN - 1e-6
                    for key, on, velocity in sostenuto_pending:
                        if key in held:
                            continue
                        if keep:
                            pending.append((key, on, velocity))
                            continue
                        for v in voices:
                            if v.key == key and v.sustained:
                                v.sustained = False
                        key_up(key, t)
                        release_regions(key, on, velocity, t, ("release",))
                    sostenuto, sostenuto_pending = set(), []
                if value != before:
                    random = draw("cc", number, data[1])
                    candidates = [r for r in inst.regions if r.cc_trigger and r.cc_trigger[0] == number
                                  and r.cc_trigger[1] - 1e-9 <= value <= r.cc_trigger[2] + 1e-9
                                  and self._conditions(r, cc)]
                    for region in self._choose(candidates, random, counters):
                        start(region, region.keycenter, 1.0, t)
        return self._mix(voices, n)

    @staticmethod
    def _choose(candidates: list[Region], draw: float, counters: Counter) -> list[Region]:
        """Of the regions an event matches: those whose random range holds the draw (in [0, 1)), or, when it falls
        in a gap between ranges, those of the nearest range; then the round robins (each region counts the events
        it matched)."""
        def distance(r: Region) -> float:
            lo, hi = r.num("lorand", 0.0), r.num("hirand", 1.0)
            return 0.0 if lo <= draw < hi else min(abs(draw - lo), abs(draw - hi))

        if not candidates:
            return []
        nearest = min(distance(r) for r in candidates)
        chosen = []
        for r in candidates:
            if distance(r) != nearest:
                continue
            length = int(r.num("seq_length", 1))
            if length > 1:
                position = counters[r.index] % length + 1
                counters[r.index] += 1
                if position != int(r.num("seq_position", 1)):
                    continue
            chosen.append(r)
        return chosen

    def _ended(self, v: _Voice, t: int) -> bool:
        """Whether a released voice is already silent at t."""
        if not v.releases:
            return False
        level, last = 1.0, None
        for at, seconds in v.releases:
            if at > t:
                break
            if last is not None:
                level *= math.exp(-self._rate(last[1]) * (at - last[0]))
            last = (at, seconds)
        if last is None:
            return False
        level *= math.exp(-self._rate(last[1]) * (t - last[0]))
        return level < END_LEVEL

    def _rate(self, seconds: float) -> float:
        """Exponential segment: the decay per sample (0 s: at once)."""
        if seconds <= 0.0:
            return float("inf")
        return EG_RATE / (max(seconds, EG_MIN_S) * self.fs)

    def _envelope(self, v: _Voice, length: int) -> tuple[np.ndarray, int]:
        """The voice's amplitude over `length` samples from its start, and where it ends (<= length)."""
        e, fs = v.envelope, self.fs
        n = np.arange(length, dtype=np.float64)
        env = np.ones(length)
        d = int(round(e["delay"] * fs))
        a = int(round(e["attack"] * fs))
        h = int(round(e["hold"] * fs))
        sustain = min(1.0, e["sustain"] / 100.0)
        env[:d] = 0.0
        if a > 0:
            seg = slice(d, min(length, d + a))
            start = e["start"] / 100.0
            env[seg] = start + (1.0 - start) * (n[seg] - d) / a
        decay_from = d + a + h
        if decay_from < length and sustain < 1.0:
            k = self._rate(e["decay"])
            tail = n[decay_from:] - decay_from
            env[decay_from:] = sustain + (1.0 - sustain) * (np.exp(-k * tail) if math.isfinite(k) else 0.0)
        end = length
        releases = sorted((at - v.start, s) for at, s in v.releases)
        current: tuple[int, float] | None = None
        for at, seconds in releases:
            at = max(0, at)
            if at >= length:
                break
            k = self._rate(seconds)
            if current is not None and k <= current[1]:
                continue  # a slower release never overrides a faster one
            level = env[at]
            if not math.isfinite(k):
                env[at:] = 0.0
                end = min(end, at)
                break
            env[at:] = level * np.exp(-k * (n[at:] - at))
            current = (at, k)
            if level > END_LEVEL:
                end = min(end, at + int(math.ceil(math.log(level / END_LEVEL) / k)) + 1)
            else:
                end = min(end, at)
        return env[:end], end

    def _mix(self, voices: list[_Voice], n: int) -> np.ndarray:
        out = np.zeros((2, n), dtype=np.float64)
        for v in voices:
            if v.start >= n or (v.gains[0] == 0.0 and v.gains[1] == 0.0):
                continue
            data, sr = self._sample(v.region.sample)
            stop = data.shape[1]
            if "end" in v.region.ops:
                stop = min(stop, int(v.region.num("end", stop - 1)) + 1)
            aligned = self.aligned_start(v.region.sample)
            fade = aligned > v.offset  # started past the beginning the bank plays: faded in
            offset = max(v.offset, aligned)
            if offset >= stop:
                continue
            length = n - v.start
            env, length = self._envelope(v, length)
            if length <= 0:
                continue
            audio = self._pitched(v.region.sample, offset, stop, v.cents, length)
            length = min(length, audio.shape[1])
            seg = audio[:, :length] * env[:length]
            if fade:
                f = min(length, int(round(ALIGN_FADE_S * self.fs)))
                seg[:, :f] *= 0.5 - 0.5 * np.cos(np.pi * (np.arange(f) + 0.5) / f)
            out[0, v.start:v.start + length] += v.gains[0] * seg[0]
            out[1, v.start:v.start + length] += v.gains[1] * seg[1]
        return out.astype(np.float32)


# --- the bench's host --------------------------------------------------------------------------------------

class SfzHost:
    """The bench's instrument host for an SFZ bank, interchangeable with KontaktHost."""

    def __init__(self, sfz: str | Path, fs: int, cc: dict[int, float] | None = None, block: int = 512,
                 log: Log = print, cache_bytes: int = CACHE_BYTES, align_ms: float | None = None):
        self.fs, self.block, self.log = fs, block, log
        self.instrument = SfzInstrument(sfz, cc)
        self.player = SfzPlayer(self.instrument, fs, cache_bytes, align_ms)

    @property
    def version(self) -> str:
        """The player, and how it departs from the sampler (part of the renders' context)."""
        aligned = "" if self.player.align_ms is None else f", attacks aligned {self.player.align_ms:g} ms"
        return f"sfz-player {PLAYER_VERSION}{aligned}"

    def silence(self, max_s: float = 0.0) -> float:
        return 0.0  # every take starts from silence

    def render_take(self, take: Take, speed: float | None = None) -> np.ndarray:
        return self.player.render(take.events, take.duration)

    def measure_a4(self) -> float:
        """Fundamental of A4 (velocity 64), from a zero-padded spectrum of one second of the sustain (as Kontakt's)."""
        n = round(1.5 * self.fs)
        audio = self.player.render([(0, (0x90, 69, 64)), (round(1.3 * self.fs), (0x80, 69, 64))], n)
        x = audio.mean(axis=0)[round(0.2 * self.fs): round(1.2 * self.fs)]
        size = 1 << 21
        spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), size))
        freqs = np.fft.rfftfreq(size, 1 / self.fs)
        band = (freqs > 300) & (freqs < 600)
        return float(freqs[band][np.argmax(spectrum[band])])

    def wait_until_loaded(self, max_s: float = 0.0) -> dict:
        missing = self.instrument.missing()
        if missing:
            raise FileNotFoundError(f"{len(missing)} samples missing, e.g. {missing[0]}")
        a4 = self.measure_a4()
        cents = 1200 * math.log2(a4 / 440.0)
        description = self.instrument.describe()
        self.log(f"SFZ bank ready: {description['regions']} regions, {description['samples']} samples; A4 at "
                 f"{a4:.2f} Hz ({cents:+.1f} cents)" + (f"; unsupported: {description['unsupported']}"
                                                         if description["unsupported"] else ""))
        return {"a4_hz": round(a4, 3), **description}


def native_rate(instrument: SfzInstrument) -> int:
    """The sample rate most of the bank's samples have."""
    rates = Counter(sf.info(str(p)).samplerate for p in instrument.samples() if p.exists())
    if not rates:
        raise FileNotFoundError("no sample of the bank is there")
    return rates.most_common(1)[0][0]
