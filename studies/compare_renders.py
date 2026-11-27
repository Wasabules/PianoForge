# SPDX-License-Identifier: AGPL-3.0-or-later

"""Compares two renders of the same MIDI piece (e.g. the reference library in Kontakt vs PianoForge's player).

Outputs:
  - level and spectral balance differences, overall and per section of the piece;
  - <out>.png: both spectrograms, both level envelopes and their difference;
  - <out>_ab.wav: an A/B listening file alternating A and B every 4 s (short crossfades).

Usage: .venv/Scripts/python studies/compare_renders.py <a.flac|wav> <b.flac|wav> <out prefix> [section starts in s ...]
"""
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

a_path, b_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
sections = [float(x) for x in sys.argv[4:]] or [0.0]
a, fs = sf.read(a_path, dtype="float64", always_2d=True)
b, fs_b = sf.read(b_path, dtype="float64", always_2d=True)
assert fs == fs_b, "both renders must share the sample rate"
n = min(len(a), len(b))
a, b = a[:n].T, b[:n].T
name_a, name_b = a_path.stem if a_path.stem != b_path.stem else a_path.parent.parent.name, b_path.parent.name


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-12))


def envelope(x, win=0.05):
    w = round(win * fs)
    k = x.shape[1] // w
    return db(np.sqrt(np.mean(x[:, : k * w].reshape(2, k, w) ** 2, axis=(0, 2)))), w


BANDS = [(27, 110), (110, 440), (440, 1760), (1760, 7040), (7040, 20000)]


def band_levels(x):
    spectrum = np.abs(np.fft.rfft(x.mean(axis=0) * np.hanning(x.shape[1]))) ** 2
    f = np.fft.rfftfreq(x.shape[1], 1 / fs)
    return [10 * np.log10(np.sum(spectrum[(f >= lo) & (f < hi)]) + 1e-30) for lo, hi in BANDS]


env_a, w = envelope(a)
env_b, _ = envelope(b)
t_env = np.arange(len(env_a)) * w / fs
audible = np.maximum(env_a, env_b) > max(env_a.max(), env_b.max()) - 50

print(f"A = {a_path}\nB = {b_path}\n")
print(f"Overall: peak A {db(np.abs(a).max()):.1f} dBFS, B {db(np.abs(b).max()):.1f} dBFS | "
      f"RMS A {db(np.sqrt(np.mean(a ** 2))):.1f} dB, B {db(np.sqrt(np.mean(b ** 2))):.1f} dB | "
      f"mean |envelope difference| {np.mean(np.abs(env_b - env_a)[audible]):.1f} dB")
residual = db(np.sqrt(np.mean((a - b) ** 2))) - db(np.sqrt(np.mean(a ** 2)))
print(f"Null test: residual A - B is {residual:.1f} dB relative to A "
      f"(0 dB = unrelated signals, below -20 dB = very close)\n")

print("Per section: B - A, level (dB), mean |envelope difference| (dB) and spectral balance per band (dB)")
print(f"  {'section':<16} {'level':>6} {'|env|':>6}   " + "  ".join(f"{lo}-{hi} Hz" for lo, hi in BANDS))
bounds = sections + [n / fs]
for s0, s1 in zip(bounds, bounds[1:]):
    i0, i1 = round(s0 * fs), round(s1 * fs)
    level = db(np.sqrt(np.mean(b[:, i0:i1] ** 2))) - db(np.sqrt(np.mean(a[:, i0:i1] ** 2)))
    sel = (t_env >= s0) & (t_env < s1) & audible
    env_err = float(np.mean(np.abs(env_b - env_a)[sel])) if sel.any() else float("nan")
    bal = np.array(band_levels(b[:, i0:i1])) - np.array(band_levels(a[:, i0:i1]))
    print(f"  {s0:6.1f}-{s1:5.1f} s   {level:+6.1f} {env_err:6.1f}   " + "  ".join(f"{v:+9.1f}" for v in bal))

# A/B listening file: alternate every 4 s with 20 ms crossfades.
seg, fade = round(4 * fs), round(0.02 * fs)
mask = np.zeros(n)
for k in range(0, n, 2 * seg):
    mask[k + seg: k + 2 * seg] = 1.0
ramp = np.convolve(mask, np.ones(fade) / fade, mode="same")
ab = a * (1 - ramp) + b * ramp
sf.write(out.with_name(out.name + "_ab.wav"), ab.T, fs, subtype="FLOAT")
print(f"\nA/B file (A first, switching every 4 s): {out.with_name(out.name + '_ab.wav')}")

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit(0)

fig, axes = plt.subplots(4, 1, figsize=(14, 12), sharex=True, gridspec_kw={"height_ratios": [3, 3, 2, 1.5]})
for ax, x, label in ((axes[0], a, f"A: {a_path.name}"), (axes[1], b, f"B: {b_path.name}")):
    ax.specgram(x.mean(axis=0), NFFT=4096, Fs=fs, noverlap=3072, cmap="magma", vmin=-160, vmax=-40, scale="dB")
    ax.set_yscale("symlog", linthresh=200)
    ax.set_ylim(20, 16000)
    ax.set_ylabel("Hz")
    ax.set_title(label, loc="left")
axes[2].plot(t_env, env_a, label="A", lw=1)
axes[2].plot(t_env, env_b, label="B", lw=1, alpha=0.8)
axes[2].set_ylim(max(env_a.max(), env_b.max()) - 70, max(env_a.max(), env_b.max()) + 3)
axes[2].set_ylabel("level (dB, 50 ms)")
axes[2].legend(loc="upper right")
diff = np.where(audible, env_b - env_a, np.nan)
axes[3].plot(t_env, diff, color="tab:red", lw=1)
axes[3].axhline(0, color="grey", lw=0.5)
axes[3].set_ylim(-15, 15)
axes[3].set_ylabel("B - A (dB)")
axes[3].set_xlabel("time (s)")
for ax in axes:
    for s in sections[1:]:
        ax.axvline(s, color="white" if ax in axes[:2] else "grey", lw=0.6, ls="--")
fig.tight_layout()
fig.savefig(out.with_suffix(".png"), dpi=110)
print(f"Figure: {out.with_suffix('.png')}")
