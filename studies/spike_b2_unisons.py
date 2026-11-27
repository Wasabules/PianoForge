# SPDX-License-Identifier: AGPL-3.0-or-later

"""Spike B v2: the unison modes and the soundboard body. Do they bring back the beating, the double decay and the
warmth under the note that made spike B sound "digital"?

For a few single-notes takes, versions are written and compared with the recording:
  1-original   the reference corpus (Noire);
  2-v1         spike B: one smooth partial (two-slope decay) + the measured attack residual (250 ms);
  3-v2         the modes measured by `forge modes` + the measured attack residual (250 ms);
  4-v3         the modes + the soundboard body (`forge soundboard`) + an attack residual of 100 ms only,
               taken after both (what neither the strings nor the body explain: the knock, the hammer).
Plus an A/B file per note: original, v1, v3 in sequence, one gain for the three (peak at -3 dBFS).

Metrics (phase-insensitive): level per time window, spectral distance; the partial envelope error, the RMS dB
difference between the envelopes of the first partials demodulated identically in both signals (beating shows
here); and the body level, in the band under the fundamental (30 Hz to 0.8 f0, at most 700 Hz).

Usage: .venv/Scripts/python studies/spike_b2_unisons.py [midi ...]
       (needs modes-v064.json and soundboard-v064.json in the corpus state)
"""
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).parent))
import spike_b_modal_note as v1  # noqa: E402
from forge import soundboard  # noqa: E402
from forge.modes import BandExtractor, envelope_error_db, synthesize  # noqa: E402
from forge.note_modes import partial_modes  # noqa: E402

CORPUS = Path("corpus/reference/noire-dry")
OUT = Path("corpus/spikes/spike-b2")
VELOCITY = 64
AB_SECONDS, GAP_SECONDS = 7.0, 0.6


def attack_fade(n: int, fs: int, seconds: float, fade_s: float) -> np.ndarray:
    fade = np.zeros(n)
    r_end, f_len = int(seconds * fs), int(fade_s * fs)
    fade[: r_end - f_len] = 1.0
    fade[r_end - f_len: r_end] = 0.5 * (1 + np.cos(np.linspace(0, np.pi, f_len)))
    return fade


def partial_envelope_error(original: np.ndarray, other: np.ndarray, fs: int, note: dict, count: int = 12) -> float:
    """Median over the first partials of the envelope error between two signals, on each partial's window."""
    ea, eb = BandExtractor(original, fs), BandExtractor(other, fs)
    f0 = note["f0"]
    pass_hz, stop_hz = min(0.33 * f0, 30.0), min(0.66 * f0, 60.0)
    errors = []
    for p in note["partials"][:count]:
        za, fs_d = ea.baseband(p["centre"], pass_hz, stop_hz)
        zb, _ = eb.baseband(p["centre"], pass_hz, stop_hz)
        i0, i1 = int(p["t0"] * fs_d), min(int(p["t1"] * fs_d), za.shape[1])
        if i1 - i0 > 10:
            errors.append(envelope_error_db(za[:, i0:i1], zb[:, i0:i1].T, fs_d))
    return float(np.median(errors)) if errors else float("nan")


def body_levels(signal: np.ndarray, fs: int, f0: float) -> list[float]:
    """Level (dB) in the band under the fundamental, per window: 0.03-0.25 s, 0.25-1 s, 1-2.5 s."""
    hi = min(0.8 * f0, 700.0)
    if hi < 60.0:
        return [float("nan")] * 3
    z, fs_d = BandExtractor(signal, fs).baseband((30.0 + hi) / 2, (hi - 30.0) / 2, (hi - 30.0) / 2 + 10)
    return [10 * np.log10(np.mean(np.abs(z[:, int(a * fs_d):int(b * fs_d)]) ** 2) + 1e-30)
            for a, b in ((0.03, 0.25), (0.25, 1.0), (1.0, 2.5))]


def run(midi: int, notes: dict, board) -> dict:
    path = next((CORPUS / "single-notes").glob(f"{midi:03d}-*_v{VELOCITY:03d}_h*.flac"))
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    on, off = meta["note_on_sample"], meta["note_off_sample"]
    original = audio[on:off].T
    n = original.shape[1]
    note = notes[midi]

    _, (_, _, hybrid_v1, _, _) = v1.run(midi, VELOCITY)  # spike B v1 (also rewrites its own files)
    modes = synthesize(partial_modes(note), 2, n, fs)
    v2 = modes + (original - modes) * attack_fade(n, fs, 0.25, 0.05)
    freqs, decays, amps = board
    strings_and_body = modes + soundboard.synthesize_body(freqs, decays, amps[midi], n, fs)
    v3 = strings_and_body + (original - strings_and_body) * attack_fade(n, fs, 0.10, 0.04)

    report = {"note": meta["note_name"], "partials": len(note["partials"]),
              "modes": sum(len(p["modes"]) for p in note["partials"])}
    reference_body = body_levels(original, fs, note["f0"])
    windows = [(0.0, 0.1), (0.1, 1.0), (1.0, 5.0), (5.0, n / fs)]
    for name, sig in (("v1", hybrid_v1), ("v2", v2), ("v3", v3)):
        entry = {"partial_envelope_error_db": round(partial_envelope_error(original, sig, fs, note), 2),
                 "body_level_db (0.03-0.25, 0.25-1, 1-2.5 s)": [
                     round(b - a, 1) for a, b in zip(reference_body, body_levels(sig, fs, note["f0"]))]}
        for a, b in windows:
            if b <= a:
                continue
            i0, i1 = int(a * fs), int(b * fs)
            entry[f"{a:.1f}-{b:.1f}s"] = {
                "level_db": round(v1.level_db(sig[:, i0:i1]) - v1.level_db(original[:, i0:i1]), 1),
                "spectral_distance_db": round(v1.spectral_distance_db(original[:, i0:i1], sig[:, i0:i1], fs), 1)}
        report[name] = entry

    OUT.mkdir(parents=True, exist_ok=True)
    stem = f"{midi:03d}-{meta['note_name']}_v{VELOCITY:03d}"
    for name, sig in (("1-original", original), ("2-v1", hybrid_v1), ("3-v2", v2), ("4-v3", v3)):
        sf.write(OUT / f"{stem}_{name}.flac", np.clip(sig.T, -1, 1), fs, subtype="PCM_24")
    seg, gap = int(AB_SECONDS * fs), np.zeros((2, int(GAP_SECONDS * fs)))
    fade_out = np.ones(seg)
    fade_out[-int(0.3 * fs):] = np.linspace(1, 0, int(0.3 * fs))
    parts = []
    for sig in (original, hybrid_v1, v3):
        s = np.zeros((2, seg))
        s[:, : min(seg, n)] = sig[:, :seg]
        parts += [s * fade_out, gap]
    ab = np.concatenate(parts, axis=1)
    ab *= 10 ** (-3 / 20) / max(np.max(np.abs(ab)), 1e-9)  # one gain for the three: peak at -3 dBFS
    sf.write(OUT / f"{stem}_AB_original-v1-v3.flac", ab.T, fs, subtype="PCM_24")
    return report


if __name__ == "__main__":
    data = json.loads((CORPUS / f"modes-v{VELOCITY:03d}.json").read_text(encoding="utf-8"))
    notes = {note["midi"]: note for note in data["notes"]}
    board = soundboard.load(CORPUS / f"soundboard-v{VELOCITY:03d}.json")
    for old in OUT.glob("*_AB_original-v1-v2.flac"):
        old.unlink()
    for midi in [int(a) for a in sys.argv[1:]] or [21, 33, 60, 84, 96]:
        print(json.dumps(run(midi, notes, board)))
    print(f"Files in {OUT}")
