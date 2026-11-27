# SPDX-License-Identifier: AGPL-3.0-or-later

"""How long should the previous voice of a re-struck note keep sounding? Fit against the reference library.

For each take of the 'repetitions' pass (C2, C4, C6 struck 4 times at 50 to 500 ms, velocities 48 and 96),
the reference player is simulated on the very same MIDI events with the single-note takes of the same
velocity: a key release applies the measured damper decay, and a re-strike makes the previous voice of the
same note fade to -40 dB within T seconds. The level envelope is compared with the library's own render,
and the T that fits best is reported.

Usage: .venv/Scripts/python studies/restrike_fit.py [corpus/reference/noire-dry]
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

root = Path(sys.argv[1] if len(sys.argv) > 1 else "corpus/reference/noire-dry")
FADES = [0.015, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2, math.inf]  # seconds to -40 dB; inf = previous voice untouched
WIN = 0.01  # envelope window (s)


def load(path: Path):
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    return audio.T, fs, meta


def envelope_db(x: np.ndarray, fs: int) -> np.ndarray:
    w = round(WIN * fs)
    n = x.shape[1] // w
    rms = np.sqrt(np.mean(x[:, : n * w].reshape(x.shape[0], n, w) ** 2, axis=(0, 2)))
    return 20 * np.log10(np.maximum(rms, 1e-12))


def per_sample_log_rate(t40: float, fs: int) -> float:
    return -math.log(100.0) / (t40 * fs) if math.isfinite(t40) and t40 > 0 else 0.0


def simulate(events, single: np.ndarray, t40: float, fade: float, n: int, fs: int, note_on: int = 0) -> np.ndarray:
    """Same rules as ReferencePlayer: decay rates only get faster; a voice is a gain-shaped copy of the take."""
    voices = []  # (start, rate changes [(time, log rate)], key down)
    for t, b in events:
        kind = b[0] & 0xF0
        if kind == 0x90 and b[2] > 0:
            for v in voices:  # re-strike: previous voices fade
                v["changes"].append((t, per_sample_log_rate(fade, fs)))
                v["down"] = False
            voices.append({"start": t, "changes": [], "down": True})
        elif kind in (0x80, 0x90):
            for v in voices:
                if v["down"]:
                    v["down"] = False
                    v["changes"].append((t, per_sample_log_rate(t40, fs)))
    out = np.zeros((2, n))
    single = single[:, note_on:]  # playback starts at the note-on of the take, after its pre-roll
    for v in voices:
        start = v["start"]
        length = min(single.shape[1], n - start)
        rate = np.zeros(length)
        current = 0.0
        for t, r in sorted(v["changes"]):
            current = min(current, r)  # the faster decay wins
            rate[max(0, t - start):] = np.minimum(rate[max(0, t - start):], current)
        gain = np.exp(np.cumsum(rate))
        out[:, start:start + length] += single[:, :length] * gain
    return out


results = {}
for path in sorted((root / "repetitions").glob("*.flac")):
    reference, fs, meta = load(path)
    note, velocity = meta["note"], meta["velocity"]
    single_path = next((root / "single-notes").glob(f"{note:03d}-*_v{velocity:03d}_h*.flac"))
    single, _, single_meta = load(single_path)
    t40 = single_meta.get("release_decay_40db_s") or math.inf
    events = [(t, tuple(b)) for t, b in meta["events"]]
    second_strike = sorted(t for t, b in events if b[0] & 0xF0 == 0x90)[1]
    ref_env = envelope_db(reference, fs)
    first = second_strike // round(WIN * fs)
    valid = ref_env > ref_env.max() - 60  # ignore the noise floor
    errors = []
    for fade in FADES:
        sim_env = envelope_db(simulate(events, single, t40, fade, reference.shape[1], fs,
                                       single_meta.get("note_on_sample", 0)), fs)
        mask = valid.copy()
        mask[:first] = False
        errors.append(float(np.mean(np.abs(sim_env[mask] - ref_env[mask]))))
    best = FADES[int(np.argmin(errors))]
    results[path.stem] = (best, errors)
    print(f"{path.stem:<26} best fade {best:>6} s | mean |dB error|: "
          + " ".join(f"{f}:{e:.1f}" for f, e in zip(FADES, errors)))

totals = np.mean([e for _, e in results.values()], axis=0)
print("\nAverage over all takes: " + " ".join(f"{f}:{e:.2f}" for f, e in zip(FADES, totals)))
print(f"Best overall: previous voice fades to -40 dB in {FADES[int(np.argmin(totals))]} s")
