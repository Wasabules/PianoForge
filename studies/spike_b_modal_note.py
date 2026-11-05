"""Spike B: can a modal model rebuild a recorded piano note? First PianoForge synthesis.

For a single-note take of the reference corpus, every partial found by the forge (stiff-string law) becomes a
pair of damped sinusoids, the prompt sound and the after-sound, with the frequency, amplitude, phase and two
decay rates measured on the recording (per channel). Two versions are written:
  - modal only;
  - hybrid: modal + the measured attack residual (recording minus modal over the first 250 ms, faded out), the
    architecture of the pre-analysis (physics for the sustain, measured transient for the attack).
The residual left by the modal part (what it does not capture: attack, beating, noise) is measured per time
window, and spectrograms are plotted.

Usage: .venv/Scripts/python studies/spike_b_modal_note.py [midi ...] [--velocity 64]
"""
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from forge.partials import find_partials, two_slope_fit

CORPUS = Path("corpus/reference/noire-dry")
OUT = Path("corpus/spikes/spike-b")
RESIDUAL_S, FADE_S, T_REF_S = 0.25, 0.05, 0.03
DB = 20 / np.log(10)


def demodulate(x: np.ndarray, fs: int, freq: float, spacing: float, hop: int) -> np.ndarray:
    """Complex amplitude of one partial (moving-average heterodyne), sampled every `hop` samples."""
    t = np.arange(len(x)) / fs
    base = x * np.exp(-2j * np.pi * freq * t)
    width = max(int(round(fs * 2.0 / spacing)), int(0.02 * fs))
    c = np.cumsum(np.concatenate([[0], base]))
    avg = (c[width:] - c[:-width]) / width  # avg[k] covers samples k .. k+width-1, centred at k + width/2
    centres = np.arange(0, len(avg), hop) + width / 2
    return 2 * avg[::hop], centres / fs


def model_partials(x: np.ndarray, fs: int, freqs: list[float], spacing: float):
    """Per partial: (freq, a_prompt, rate_prompt, a_after, rate_after, phase) at t = 0 (the note-on)."""
    hop = int(0.01 * fs)
    params = []
    for f in freqs:
        z, tz = demodulate(x, fs, f, spacing, hop)
        env = DB * np.log(np.maximum(np.abs(z), 1e-12))
        usable = np.flatnonzero(env > env.max() - 50)
        if len(usable) < 20:
            continue
        sel = (tz >= T_REF_S) & (np.arange(len(env)) <= usable[-1])
        e, t = env[sel], tz[sel]
        s1, s2, knee, _ = two_slope_fit(e, 0.01)
        # A string only loses energy: a fitted slope can turn positive on beating or on the noise floor, and an
        # exponential growth would then dominate the whole note. Decays are kept strictly negative.
        s1, s2 = min(s1, -0.05), min(s2, -0.05)
        k = max(1, min(len(e) - 2, int(round((knee) / 0.01))))
        a1 = np.polyval([s1, np.polyfit(t[:k] - t[0], e[:k], 1)[1]], 0.0)  # level of each line at t[0]
        a2 = np.polyval([s2, np.polyfit(t[k:] - t[0], e[k:], 1)[1]], 0.0)
        # Back to t = 0 (the note-on): the lines are anchored at t[0] = T_REF_S. A double decay only exists when the
        # late slope is gentler than the early one (the after-sound outlasts the prompt sound); a steeper late
        # slope is the note sinking into the noise, and extrapolating it back would invent a huge component.
        start = 10 ** ((a1 - s1 * t[0]) / 20)
        if s2 > s1:
            l2 = min(10 ** ((a2 - s2 * t[0]) / 20), start)  # the after-sound never exceeds the initial level
            l1 = max(start - l2, 0.0)
        else:
            l1, l2 = start, 0.0
        i_ref = int(np.argmin(np.abs(tz - T_REF_S)))
        phase = float(np.angle(z[i_ref]) - 2 * np.pi * f * 0.0)  # demodulation is referenced to t = 0
        params.append((f, l1, s1 / DB, l2, s2 / DB, phase))
    return params


def synthesize(params, n: int, fs: int) -> np.ndarray:
    t = np.arange(n) / fs
    out = np.zeros(n)
    for f, a1, r1, a2, r2, phase in params:
        out += (a1 * np.exp(r1 * t) + a2 * np.exp(r2 * t)) * np.cos(2 * np.pi * f * t + phase)
    return out


def level_db(x: np.ndarray) -> float:
    return 10 * np.log10(np.mean(x ** 2) + 1e-30)


def spectral_distance_db(a: np.ndarray, b: np.ndarray, fs: int) -> float:
    """Phase-insensitive: mean |dB difference| between short-time magnitude spectra (bins within 60 dB of the
    loudest), what the ear compares; a sample-wise difference would mostly measure phase drift."""
    n, hop = 2048, 512
    win = np.hanning(n)
    def stft(x):
        x = x.mean(axis=0)
        frames = [np.abs(np.fft.rfft(x[i:i + n] * win)) for i in range(0, len(x) - n, hop)]
        return 20 * np.log10(np.maximum(np.array(frames), 1e-9)) if frames else np.zeros((0, n // 2 + 1))
    sa, sb = stft(a), stft(b)
    if sa.size == 0:
        return float("nan")
    floor = sa.max() - 60  # both spectra floored 60 dB under the peak: silence vs noise floor is not a difference
    sa, sb = np.maximum(sa, floor), np.maximum(sb, floor)
    mask = (sa > floor) | (sb > floor)
    return float(np.mean(np.abs(sa - sb)[mask]))


def run(midi: int, velocity: int) -> dict:
    path = next((CORPUS / "single-notes").glob(f"{midi:03d}-*_v{velocity:03d}_h*.flac"))
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    on, off = meta["note_on_sample"], meta["note_off_sample"]
    original = audio[on:off].T  # from the note-on to the key release
    n = original.shape[1]
    mono = original.mean(axis=0)
    seg = mono[int(0.05 * fs): int(0.05 * fs) + int(np.clip(40.0 / (440 * 2 ** ((midi - 69) / 12)), 0.4, 3.0) * fs)]
    f0, B, partials = find_partials(seg, fs, midi)
    freqs = [p.freq for p in partials]

    modal = np.zeros_like(original)
    count = 0
    for ch in range(2):
        params = model_partials(original[ch], fs, freqs, f0)
        modal[ch] = synthesize(params, n, fs)
        count = len(params)
    fade = np.ones(n)
    r_end, f_len = int(RESIDUAL_S * fs), int(FADE_S * fs)
    fade[r_end:] = 0.0
    fade[r_end - f_len: r_end] = 0.5 * (1 + np.cos(np.linspace(0, np.pi, f_len)))
    hybrid = modal + (original - modal) * fade

    windows = [(0.0, 0.1), (0.1, 1.0), (1.0, 5.0), (5.0, n / fs)]
    report = {"note": meta["note_name"], "velocity": velocity, "f0": round(f0, 3), "B": B, "partials": count}
    for name, sig in (("modal", modal), ("hybrid", hybrid)):
        report[name] = {}
        for a, b in windows:
            if b <= a:
                continue
            i0, i1 = int(a * fs), int(b * fs)
            report[name][f"{a:.1f}-{b:.1f}s"] = {
                "level_db": round(level_db(sig[:, i0:i1]) - level_db(original[:, i0:i1]), 1),
                "spectral_distance_db": round(spectral_distance_db(original[:, i0:i1], sig[:, i0:i1], fs), 1)}
    OUT.mkdir(parents=True, exist_ok=True)
    stem = f"{midi:03d}-{meta['note_name']}_v{velocity:03d}"
    for name, sig in (("1-original", original), ("2-modal", modal), ("3-hybrid", hybrid)):
        sf.write(OUT / f"{stem}_{name}.flac", np.clip(sig.T, -1, 1), fs, subtype="PCM_24")
    return report, (original, modal, hybrid, fs, stem)


def plot(items) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(len(items), 3, figsize=(15, 3.6 * len(items)), squeeze=False)
    for row, (original, modal, hybrid, fs, stem) in enumerate(items):
        for col, (sig, title) in enumerate(((original, "recording (Noire)"), (modal, "modal only"),
                                            (hybrid, "modal + attack residual"))):
            ax = axes[row, col]
            dur = min(sig.shape[1] / fs, 6.0)
            ax.specgram(sig.mean(axis=0)[: int(dur * fs)], NFFT=4096, Fs=fs, noverlap=3584, cmap="magma",
                        vmin=-150, vmax=-40, scale="dB")
            ax.set_ylim(20, 8000)
            ax.set_yscale("symlog", linthresh=300)
            ax.set_title(f"{stem}: {title}", loc="left", fontsize=10)
    fig.tight_layout()
    fig.savefig(OUT / "spike_b.png", dpi=90)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    velocity = int(sys.argv[sys.argv.index("--velocity") + 1]) if "--velocity" in sys.argv else 64
    notes = [int(a) for a in args if a.isdigit() and str(velocity) != a] or [33, 60, 84]
    items = []
    for midi in notes:
        report, item = run(midi, velocity)
        items.append(item)
        print(json.dumps(report))
    plot(items)
    print(f"Files in {OUT}")
