"""What does the reference library do with the sustain pedal? Analysis of the 'interactions' session.

1. Pedal down vs pedal up, key held in both: spectral balance per band and correlation over time.
   Extra energy that builds up after the attack points to pedal-down recordings (sympathetic resonance).
2. Pedal noise: peak of the pedal-only take and of the pre-roll of pedal-down takes (pedal pressed at 0 s).
3. Half pedal: level along a CC64 ramp (127 at 2.0 s down to 0 at 6.0 s, key released at 1.2 s), relative to
   the same note held without pedal (single-notes pass of the reference session), aligned on the note-on.

Usage: .venv/Scripts/python studies/noire_pedal_behavior.py [corpus root]
"""
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

corpus = Path(sys.argv[1] if len(sys.argv) > 1 else "corpus")
inter = corpus / "interactions" / "noire-dry"
single = corpus / "reference" / "noire-dry" / "single-notes"


def load(path: Path) -> tuple[np.ndarray, int]:
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    return audio.T, fs


def rms_db(x: np.ndarray) -> float:
    r = float(np.sqrt(np.mean(x ** 2))) if x.size else 0.0
    return 20 * np.log10(r) if r > 0 else -np.inf


def mean_spectrum_db(x: np.ndarray, fs: int, n: int = 8192) -> tuple[np.ndarray, np.ndarray]:
    w = np.hanning(n)
    frames = [np.abs(np.fft.rfft(x[0, i:i + n] * w)) ** 2 for i in range(0, x.shape[1] - n, n // 2)]
    return 10 * np.log10(np.mean(frames, axis=0) + 1e-30), np.fft.rfftfreq(n, 1 / fs)


BANDS = [(20, 200), (200, 1000), (1000, 4000), (4000, 12000)]

print("1) Pedal down minus pedal up, key held (1.05 s to 4.95 s)")
print("   spectral balance per band (dB) | waveform difference relative to the level, early and late")
for down_path in sorted((inter / "pedal-down").glob("*.flac")):
    name = down_path.stem.replace("_h4000ms", "")
    up_path = next((inter / "pedal-up").glob(f"{name}_h*.flac"), None)
    if up_path is None:
        continue
    down, fs = load(down_path)
    up, _ = load(up_path)
    a, b = round(1.05 * fs), round(4.95 * fs)
    sd, f = mean_spectrum_db(down[:, a:b], fs)
    su, _ = mean_spectrum_db(up[:, a:b], fs)
    balance = [np.mean(sd[(f >= lo) & (f < hi)]) - np.mean(su[(f >= lo) & (f < hi)]) for lo, hi in BANDS]
    early = slice(round(1.05 * fs), round(2.0 * fs))
    late = slice(round(3.0 * fs), round(4.95 * fs))
    diff = lambda s: rms_db(down[:, s] - up[:, s]) - rms_db(up[:, s])
    print(f"   {name:<12} " + " ".join(f"{v:+5.2f}" for v in balance)
          + f" | early {diff(early):6.1f} dB, late {diff(late):6.1f} dB")

print("\n2) Pedal noise")
pedal, fs = load(next((inter / "pedal-only").glob("*.flac")))
peak = np.max(np.abs(pedal))
print(f"   pedal-only take: {'digital silence' if peak == 0 else f'peak {20 * np.log10(peak):.1f} dBFS'}")
preroll = max(np.max(np.abs(load(p)[0][:, : round(0.95 * fs)])) for p in (inter / "pedal-down").glob("*.flac"))
print(f"   pre-roll of pedal-down takes: {'digital silence' if preroll == 0 else f'peak {20 * np.log10(preroll):.1f} dBFS'}")

print("\n3) Half pedal: level relative to the same note held without pedal (CC64 value: dB)")
for ramp_path in sorted((inter / "half-pedal").glob("*.flac")):
    note = ramp_path.stem.split("_")[0]
    held_path = next(single.glob(f"{note}_v080_h*.flac"), None)
    if held_path is None:
        continue
    ramp, fs = load(ramp_path)
    held, _ = load(held_path)
    on_ramp, on_held, win = round(1.0 * fs), round(0.05 * fs), round(0.05 * fs)
    row = []
    for cc in (127, 104, 88, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0):
        k = round((2.0 + 4.0 * (1 - cc / 127) - 1.0) * fs)  # samples since the note-on when CC64 == cc
        row.append(f"{cc}:{rms_db(ramp[:, on_ramp + k:on_ramp + k + win]) - rms_db(held[:, on_held + k:on_held + k + win]):+.1f}")
    print(f"   {note:<8} " + "  ".join(row))
