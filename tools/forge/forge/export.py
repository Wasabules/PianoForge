"""`forge export`: the modal profile played by the C++ engine, from the measurements of every velocity layer.

Inputs, in the corpus state folder: modes-vNNN.json (`forge modes`) and soundboard-vNNN.json (`forge soundboard`)
for each velocity layer, release-curves.json (`forge release-curves`), and the single-notes takes themselves for
the attack residuals.

Per note and per layer, the profile holds:
  - the modes of every partial (frequency, decay, complex amplitude per channel at the note-on);
  - the soundboard body: the complex amplitude of each shared resonance;
  - the attack residual: what neither the strings nor the body explain (the hammer, the knock, the first instants
    before the modes settle), up to the start of the partials' analysis window and at least RESIDUAL_S, then faded
    out over RESIDUAL_FADE_S. Before that start, the modes are extrapolated back to the note-on, not measured: the
    residual is what keeps the attack exact. The window starts later in the bass, where the partials are close
    (0.19 s for A0, 0.08 s from C2 up). In the top octave the modes miss a third to half of the sound well past
    that (C7 to C8: 3 to 10 dB under the take over the first half second): the residual runs on while it is
    within RESIDUAL_QUIET_DB of the take, up to RESIDUAL_MAX_S.
The layers are kept as they were measured: each one is a real strike, with its own beating phases. Interpolating
amplitudes between two strikes gives neither (studies/velocity_linearity.py); the engine plays the nearest layer
with the levels interpolated per partial.

Two checks against the recording, where the measurements are extrapolated:
  - the body may not be louder than what the take holds besides the strings (limit_body);
  - a partial whose modes would grow again past its analysis window, where the take stays under the floor, is
    faded out from the window's end (fade_times).

Binary format (little endian), modal-profile.bin:
  header      char[8] "PFMODAL1", u32 version (2), f64 residual sample rate, u32 channels (2)
  body        u32 P, then P x (f64 freq Hz, f64 decay 1/s)
  dampers     f64 step s, u32 notes, u32 holds, u32 points, notes x i32 midi, holds x f64 hold s,
              notes x holds x points x f32 dB (level after the key release against the note held)
  notes       u32 count, then per note:
                u32 midi, u32 layers, f64 f0, then per layer:
                  u32 velocity, u32 partials, u32 modes, u32 residual frames,
                  partials x (u32 partial number, u32 modes of this partial, f32 fade time s, 0: none),
                  modes x (f64 freq Hz, f64 decay 1/s, f32 left re, f32 left im, f32 right re, f32 right im),
                  P x (f32 left re, f32 left im, f32 right re, f32 right im)   body amplitudes,
                  f32 residual scale, frames x channels x i16 (interleaved; sample = i16 x scale)
  sections    (version 3) until the end of the file: char[4] tag, u32 payload bytes, payload; a reader skips the
              tags it does not know.
                SYMP  free strings of the sympathetic resonance (forge sympathetic): f32 drive onset s, f32 velocity
                      exponent, f32 reference velocity, u32 strings, then per string u32 midi, u32 resonators,
                      resonators x (f64 freq Hz, f64 decay 1/s, f32 coupling gain 1/s, f32 left re, f32 left im,
                      f32 right re, f32 right im); then f32 drive decay s (0: none), u32 registers, registers x
                      (i32 first note, f32 onset s, f32 drive decay s) (both absent from older profiles); then u32
                      registers, registers x (f32 gain dB, f32 velocity exponent) (absent from older profiles: 0 dB
                      and the exponent above). Each voice drives the strings with gain x (1 - exp(-t / onset))
                      exp(-t / decay) (velocity / reference) ^ exponent, those of its note's register (one register
                      per exciting note measured).
                PEDL  damper lift per CC64 value (half pedal): 128 x f32, 1 lifted, 0 resting on the strings
                UNIS  the coupled strings of each unison (forge unison: Weinreich's model fitted to each partial's
                      strongest modes): u32 notes, then per note u32 midi, u32 layers, then per layer u32 velocity,
                      u32 partials (those of the layer, in order), partials x (u8 strings, 0: no model, u8 x 3 mode
                      indices within the partial by ascending frequency, f32 x 3 string detunings Hz against their
                      mean, f32 bridge admittance re, f32 im, 1/s); the engine moves the measured modes by what the
                      model gives for the soft pedal and for another unison tuning
                DAMP  damper curves per group of partials (forge release-curves): u32 groups, groups x u32 first
                      partial number, notes x holds x groups x points x f32 dB against the overall curve (the
                      notes, holds and points of the dampers above)
                DAMV  damper curves per velocity (forge release-curves): u32 velocities, velocities x i32 velocity,
                      velocities x notes x holds x points x f32 dB (overall, as the dampers above), u32 groups
                      (0: none), velocities x notes x holds x groups x points x f32 dB (against that velocity's
                      overall curve); the engine interpolates between velocities in dB
                RELN  knock of the key release on the soundboard (forge release-noise): u32 notes, u32 velocities,
                      notes x i32 midi, velocities x i32 velocity, notes x velocities x f32 level dB, notes x
                      velocities x P x (f32 left re, left im, right re, right im) amplitudes at the release; then
                      u32 holds, holds x f32 hold s, holds x f32 dB (the level after that hold against the one kept;
                      absent from older profiles)
                LEVL  the layers' measures, for the engine between two layers: f32 attack window s, f32 sustain
                      window from s, f32 to s, u32 notes, then per note u32 midi, u32 layers, then per layer u32
                      velocity, f32 attack mean square (its modes, soundboard and residual, mono, over the attack
                      window, as the engine plays them), u32 partials (those of the layer, in order), partials x f32
                      RMS of the partial over the sustain window (the sum of its modes, cross terms included: two
                      strong modes of a partial may cancel each other), both channels averaged
Convention: x_c(t) = Re(a_c exp((-decay + 2 pi i freq) t)), t in seconds from the note-on.
The profile is calibrated on a commercial library: it stays in the corpus folder and is never published.
"""

from __future__ import annotations

import json
import struct
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

from forge import release_noise, soundboard, sympathetic, unison
from forge.modes import BandExtractor, synthesize
from forge.note_modes import clean, partial_modes

MAGIC, VERSION = b"PFMODAL1", 3
RESIDUAL_S, RESIDUAL_FADE_S = 0.10, 0.04
RESIDUAL_MAX_S = 0.5        # the longest attack residual (the top octave)
RESIDUAL_QUIET_DB = -10.0   # the residual ends once it stays this far under the take ...
RESIDUAL_QUIET_S = 0.1      # ... for this long (20 ms windows)
BODY_WINDOWS_S = (0.3, 1.0, soundboard.T1)  # ends of the windows where the body is checked (the first one starts
                                            # where the residual ends)
BODY_MARGIN_DB = 3.0    # how much louder than the take (besides the strings) the body may be, per band and window
FADE_EXCESS_DB, FADE_EXCESS_S = 6.0, 1.0  # past its window, a partial louder than the take by this, this long, fades
FADE_HOLD_MARGIN_S = 0.5                    # a window ending this close to the end of the hold ran to it
ATTACK_S = 0.06             # the layers' measures (section LEVL): the attack's window ...
SUSTAIN_S = (0.3, 2.0)      # ... and the sustain's, seconds from the note-on


def velocities(corpus: Path) -> list[int]:
    """The velocity layers for which both the modes and the soundboard have been measured."""
    found = []
    for path in sorted(corpus.glob("modes-v*.json")):
        v = int(path.stem.split("-v")[1])
        if (corpus / f"soundboard-v{v:03d}.json").exists():
            found.append(v)
    return found


def residual_seconds(note: dict) -> float:
    """Length of the attack residual: the fade starts once every partial is inside its analysis window."""
    start = max((p["t0"] for p in note["partials"] if p["modes"]), default=0.0)
    return max(RESIDUAL_S, start + RESIDUAL_FADE_S)


def quiet_from(r: np.ndarray, x: np.ndarray, fs: float, start: float) -> float:
    """The first instant from `start` after which the residual r stays RESIDUAL_QUIET_DB under the take x for
    RESIDUAL_QUIET_S (20 ms windows); the end of r if it never does."""
    w = int(round(0.02 * fs))
    count = r.shape[1] // w
    pr = np.mean(r[:, :count * w].reshape(r.shape[0], count, w) ** 2, axis=(0, 2))
    px = np.mean(x[:, :count * w].reshape(x.shape[0], count, w) ** 2, axis=(0, 2))
    quiet = pr < px * 10 ** (RESIDUAL_QUIET_DB / 10)
    span = int(round(RESIDUAL_QUIET_S / 0.02))
    for k in range(int(np.ceil(start / 0.02)), count - span + 1):
        if quiet[k:k + span].all():
            return k * 0.02
    return count * 0.02


def residual(path: Path, note: dict, body_freqs, body_decays, body_amps) -> tuple[np.ndarray, int]:
    """Attack residual of one take (frames x channels), faded out, at the take's sample rate."""
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    longest = int(round(RESIDUAL_MAX_S * fs))
    on = meta["note_on_sample"]
    x = audio[on:on + longest].T
    model = synthesize(partial_modes(note), x.shape[0], x.shape[1], fs)
    model += soundboard.synthesize_body(body_freqs, body_decays, body_amps, x.shape[1], fs)
    r = x - model
    base = residual_seconds(note)
    seconds = min(RESIDUAL_MAX_S, max(base, quiet_from(r, x, fs, base - RESIDUAL_FADE_S) + RESIDUAL_FADE_S))
    n = int(round(seconds * fs))
    r = r[:, :n]
    fade = int(round(RESIDUAL_FADE_S * fs))
    r[:, n - fade:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, fade)))
    return r.T, fs


def layer_measures(note: dict, r: np.ndarray, fs: float, body_freqs, body_decays, body_amps) -> tuple[float, list[float]]:
    """The layer's measures (section LEVL), as the engine's pf::measureLayers computes them from the profile: the
    attack's mean square (its modes, soundboard and residual, mono, over the first ATTACK_S, past Nyquist left
    out) and each partial's RMS over SUSTAIN_S (the sum of its modes, in closed form, both channels averaged)."""
    partials = [p for p in note["partials"] if p["modes"]]
    n = max(1, int(round(ATTACK_S * fs)))
    t = np.arange(n) / fs
    x = np.zeros(n)
    k = min(n, r.shape[0])
    x[:k] = r[:k].mean(axis=1)
    modes = np.array([m for p in partials for m in p["modes"]], dtype=float).reshape(-1, 6)
    freqs = np.concatenate([modes[:, 0], np.asarray(body_freqs, dtype=float)])
    decays = np.concatenate([modes[:, 1], np.asarray(body_decays, dtype=float)])
    amps = np.concatenate([0.5 * (modes[:, 2] + modes[:, 4] + 1j * (modes[:, 3] + modes[:, 5])),
                           0.5 * (np.asarray(body_amps)[0] + np.asarray(body_amps)[1])])
    keep = freqs < 0.5 * fs
    freqs, decays, amps = freqs[keep], decays[keep], amps[keep]
    for i in range(0, len(freqs), 64):  # in chunks: 64 modes x the window at a time
        s = -decays[i:i + 64, None] + 2j * np.pi * freqs[i:i + 64, None]
        x += np.real(amps[i:i + 64, None] * np.exp(s * t[None, :])).sum(axis=0)
    attack = float(np.mean(x * x))

    t0, t1 = SUSTAIN_S

    def integral(z: np.ndarray) -> np.ndarray:  # of e^(z t) over [t0, t1]
        small = np.abs(z) < 1e-9
        safe = np.where(small, 1.0, z)
        return np.where(small, t1 - t0, (np.exp(safe * t1) - np.exp(safe * t0)) / safe)

    levels = []
    for p in partials:
        m = np.asarray(p["modes"], dtype=float).reshape(-1, 6)
        z = -m[:, 1] + 2j * np.pi * m[:, 0]
        left, right = m[:, 2] + 1j * m[:, 3], m[:, 4] + 1j * m[:, 5]
        same = integral(z[:, None] + z[None, :])
        cross = integral(z[:, None] + np.conj(z)[None, :])
        e = 0.25 * np.real((np.outer(left, left) + np.outer(right, right)) * same
                           + (np.outer(left, left.conj()) + np.outer(right, right.conj())) * cross).sum()
        levels.append(float(np.sqrt(max(e, 0.0) / (t1 - t0))))
    return attack, levels


def limit_body(path: Path, note: dict, freqs: np.ndarray, decays: np.ndarray, amps: np.ndarray,
               start: float) -> tuple[np.ndarray, int]:
    """The soundboard may not be louder than what the recording holds besides the strings. The body fit weighs the
    tail of its window most, where the slow resonances are measured: a fast resonance is barely constrained there
    and, on a few quiet takes, comes out far too loud (E2 at velocity 8: 15 dB at 703 Hz). The body is compared
    with the take minus the modes, in the bins the fit used (away from the partials), per third octave, in windows
    from `start` (the end of the residual, which covers what comes before) to T1. Where it is louder than the take
    by more than BODY_MARGIN_DB (the take also holds noise, and the estimate varies), the band's resonances are
    brought down to that margin. Returns the amplitudes (channels x resonances, at the note-on) and how many were
    lowered."""
    lo, hi = soundboard.BAND
    centre = (lo + hi) / 2
    z, fs_d = soundboard.residual_baseband(path, note, seconds=soundboard.T1 + 0.5)
    i0 = int(round(soundboard.T0 * fs_d))
    n = z.shape[1]
    poles = np.exp((-decays + 2j * np.pi * (freqs - centre)) / fs_d)
    bounds = [max(start, soundboard.T0), *BODY_WINDOWS_S]
    windows = [(int((a - soundboard.T0) * fs_d), min(n, int((b - soundboard.T0) * fs_d)))
               for a, b in zip(bounds[:-1], bounds[1:])]
    windows = [(a, b) for a, b in windows if b - a >= 16]
    if not windows:
        return amps, 0
    edges = lo * 2.0 ** (np.arange(0, 22) / 3)
    band = np.clip(np.searchsorted(edges, freqs, side="right") - 1, 0, len(edges) - 2)
    partials, half_width = soundboard.excluded(note)

    def spectrum(x: np.ndarray, a: int, b: int) -> tuple[np.ndarray, np.ndarray]:
        nfft = 1 << int(np.ceil(np.log2(2 * (b - a))))
        f = centre + np.fft.fftfreq(nfft, 1 / fs_d)
        return f, np.sum(np.abs(np.fft.fft(x[:, a:b] * np.hanning(b - a), nfft, axis=1)) ** 2, axis=0)

    data = [spectrum(z, a, b) for a, b in windows]
    basis = poles[None, :] ** (i0 + np.arange(n))[:, None]  # samples x resonances
    amps = np.array(amps, dtype=np.complex128)
    lowered = np.zeros(len(freqs), dtype=bool)
    limit = 10 ** (BODY_MARGIN_DB / 10)
    for _ in range(4):  # lowering one band changes its neighbours a little: settle
        model = (basis @ amps.T).T
        changed = False
        for w, (a, b) in enumerate(windows):
            f, ed = data[w]
            _, em = spectrum(model, a, b)
            keep = (f >= lo) & (f <= hi)
            for p in partials:
                keep &= np.abs(f - p) > half_width
            for k in np.unique(band):
                sel = keep & (f >= edges[k]) & (f < edges[k + 1])
                e_model, e_data = em[sel].sum(), ed[sel].sum()
                if e_model > limit * e_data * 1.01:
                    which = band == k
                    amps[:, which] *= np.sqrt(limit * e_data / e_model)
                    lowered |= which & (np.abs(amps[0]) + np.abs(amps[1]) > 0)
                    changed = True
        if not changed:
            break
    return amps, int(lowered.sum())


def fade_times(path: Path, note: dict) -> dict[int, float]:
    """Partial number -> fade time (s from the note-on). The analysis window of a partial ends at the last instant
    it is above its floor (its maximum - 50 dB, the noise + 15 dB, or the note's loudest partial - 80 dB); the modes
    are measured up to there only. Past it they can ring on far louder than the take: from a slow decay, near the
    floor for tens of seconds (B0 at velocity 16: 800 Hz still 42 dB under the note's peak after 10 s, the take
    45 dB lower, Noire being denoised), or growing again where close modes cancelled each other over the window
    (B0 at velocity 8, partial 14: back from -40 to -17 dB). The take is measured in the partial's own band up to
    the end of the hold: a partial louder than it by FADE_EXCESS_DB for more than FADE_EXCESS_S is faded out from
    its window's end, where it is at its floor. A window that ran to the end of the hold is left alone."""
    hold = note["hold_s"]
    todo = [p for p in note["partials"] if p["modes"] and p["t1"] < hold - FADE_HOLD_MARGIN_S]
    if not todo:
        return {}
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    audio, fs = sf.read(path, dtype="float64", always_2d=True)
    extractor = BandExtractor(audio[meta["note_on_sample"]:meta["note_off_sample"]].T, fs)
    pass_hz = min(0.33 * note["f0"], 30.0)  # the bands of the analysis (modes.partial_window)
    stop_hz = min(0.66 * note["f0"], 2 * pass_hz)
    smear = 1.5 / (stop_hz - pass_hz)
    out = {}
    for p in todo:
        z, fs_d = extractor.baseband(p["centre"], pass_hz, stop_hz)
        i1, i2 = int(p["t1"] * fs_d), min(z.shape[1], int((hold - smear) * fs_d))
        if i2 - i1 < FADE_EXCESS_S * fs_d:
            continue
        m = np.asarray(p["modes"], dtype=np.float64)
        t = np.arange(i1, i2) / fs_d
        basis = np.exp(np.outer(t, -m[:, 1] + 2j * np.pi * (m[:, 0] - p["centre"])))
        model = basis @ np.stack([m[:, 2] + 1j * m[:, 3], m[:, 4] + 1j * m[:, 5]], axis=1)  # samples x channels
        k = max(1, int(0.5 * fs_d))
        smooth = lambda e: np.convolve(e, np.ones(k) / k, mode="same")
        e_model = smooth(np.mean(np.abs(model) ** 2, axis=1))
        e_take = smooth(np.mean(np.abs(z[:, i1:i2]) ** 2, axis=0))
        if np.sum(e_model > e_take * 10 ** (FADE_EXCESS_DB / 10)) > FADE_EXCESS_S * fs_d:
            out[p["n"]] = float(p["t1"])
    return out


def _layer_task(args) -> dict:
    path, note, body = args
    path = Path(path)
    freqs, decays = np.asarray(body[0]), np.asarray(body[1])
    amps, lowered = limit_body(path, note, freqs, decays, np.asarray(body[2]), residual_seconds(note))
    r, fs = residual(path, note, freqs, decays, amps)  # with the body as it will be played
    return {"midi": note["midi"], "velocity": note["velocity"], "residual": r, "fs": fs, "body": amps,
            "lowered": lowered, "fades": fade_times(path, note),
            "measures": layer_measures(note, r, fs, freqs, decays, amps)}


def _unison_task(args) -> tuple[int, int, list]:
    midi, velocity, note = args
    return midi, velocity, unison.layer_table(note)


def _layer_bytes(note: dict, body_amps: np.ndarray, r: np.ndarray, fades: dict[int, float]) -> bytes:
    partials = [p for p in note["partials"] if p["modes"]]
    modes = [m for p in partials for m in p["modes"]]
    out = [struct.pack("<4I", note["velocity"], len(partials), len(modes), r.shape[0])]
    table = np.zeros(len(partials), dtype=[("n", "<u4"), ("modes", "<u4"), ("fade", "<f4")])
    for i, p in enumerate(partials):
        table[i] = (p["n"], len(p["modes"]), fades.get(p["n"], 0.0))
    out.append(table.tobytes())
    rec = np.zeros(len(modes), dtype=[("f", "<f8"), ("d", "<f8"), ("a", "<f4", (4,))])
    for i, m in enumerate(modes):
        rec[i] = (m[0], m[1], m[2:6])
    out.append(rec.tobytes())
    out.append(np.stack([body_amps.real[0], body_amps.imag[0], body_amps.real[1], body_amps.imag[1]],
                        axis=1).astype("<f4").tobytes())
    # Full scale is 1: anything far above it is a failed fit upstream, not a piano.
    largest = max([float(np.max(np.abs(rec["a"]))) if len(rec) else 0.0, float(np.max(np.abs(body_amps)))])
    peak = float(np.max(np.abs(r))) or 1.0
    if not np.isfinite(peak) or peak > 10.0 or not np.isfinite(largest) or largest > 10.0:
        raise SystemExit(f"note {note['midi']} velocity {note['velocity']}: absurd amplitudes (modes or body "
                         f"{largest:.3g}, residual {peak:.3g}); run forge soundboard again for this velocity")
    out.append(struct.pack("<f", peak / 32767.0))
    out.append(np.round(r / peak * 32767.0).clip(-32767, 32767).astype("<i2").tobytes())
    return b"".join(out)


BODY_OUTLIER = 15.0  # a body this many times louder (against its strings) than the note's median layer is a failed fit


def string_level(note: dict) -> float:
    """RMS amplitude of the note's modes over the first second, cross terms ignored (as the engine computes it)."""
    e = 0.0
    for p in note["partials"]:
        for m in p["modes"]:
            a2 = 0.5 * sum(v * v for v in m[2:6])
            x = 2.0 * max(m[1], 1e-6)
            e += 0.5 * a2 * (1.0 - np.exp(-x)) / x
    return float(np.sqrt(e))


def repair_bodies(modes: dict, boards: dict, layers: list[int]) -> list[tuple[int, int]]:
    """The soundboard fit fails on a few takes (A0 at velocity 40: 130 times too loud), where the partials leave
    almost no bin to measure the body in. Against the strings of the same take, the body is about as loud from one
    layer to the next; a layer BODY_OUTLIER times above the note's median gets the body of the median layer instead,
    scaled to its own strings. Returns the (note, velocity) repaired."""
    repaired = []
    for m in sorted({m for v in layers for m in boards[v][2]}):
        ratio = {}
        for v in layers:
            if m in boards[v][2] and m in modes[v] and string_level(modes[v][m]) > 0:
                ratio[v] = float(np.sqrt(np.mean(np.abs(boards[v][2][m]) ** 2))) / string_level(modes[v][m])
        if len(ratio) < 3:
            continue
        median = float(np.median(list(ratio.values())))
        ref = min(ratio, key=lambda v: abs(ratio[v] - median))
        for v, r in ratio.items():
            if r > BODY_OUTLIER * median:
                boards[v][2][m] = boards[ref][2][m] * (string_level(modes[v][m]) / string_level(modes[ref][m]))
                repaired.append((m, v))
    return repaired


def section(tag: bytes, payload: bytes) -> bytes:
    return tag + struct.pack("<I", len(payload)) + payload


def sections(corpus: Path) -> list[bytes]:
    """The tagged sections of version 3, from the measurements found in the corpus state folder."""
    out = []
    path = corpus / "sympathetic.json"
    if path.exists():
        strings, shape = sympathetic.load(path)
        payload = [struct.pack("<3fI", shape["onset_s"], shape["velocity_exponent"], shape["reference_velocity"],
                               len(strings))]
        for midi, res in strings.items():
            rec = np.zeros(len(res), dtype=[("f", "<f8"), ("d", "<f8"), ("a", "<f4", (5,))])
            rec["f"], rec["d"], rec["a"] = res[:, 0], res[:, 1], res[:, 2:7]
            payload += [struct.pack("<2I", midi, len(res)), rec.tobytes()]
        payload.append(struct.pack("<fI", shape["drive_decay_s"], len(shape["registers"])))
        payload += [struct.pack("<iff", r[0], r[1], r[2]) for r in shape["registers"]]
        payload.append(struct.pack("<I", len(shape["registers"])))  # then each register's gain and exponent
        payload += [struct.pack("<ff", r[3], r[4]) for r in shape["registers"]]
        out.append(section(b"SYMP", b"".join(payload)))
        print(f"  sympathetic resonance: {sum(len(r) for r in strings.values())} resonators on {len(strings)} strings")
    else:
        print(f"  no {path.name}: no sympathetic resonance (run forge sympathetic)")
    curves = json.loads((corpus / "release-curves.json").read_text(encoding="utf-8"))
    if "group_db" in curves:
        groups = np.asarray(curves["group_db"], dtype="<f4")  # notes x holds x groups x points
        out.append(section(b"DAMP", struct.pack("<I", groups.shape[2])
                           + np.asarray(curves["group_first"], dtype="<u4").tobytes() + groups.tobytes()))
        print(f"  damper curves per group of partials: {groups.shape[2]} groups")
    if "curves_db_by_velocity" in curves:
        overall = np.asarray(curves["curves_db_by_velocity"], dtype="<f4")  # velocities x notes x holds x points
        groups = np.asarray(curves.get("group_db_by_velocity", []), dtype="<f4")
        payload = [struct.pack("<I", len(curves["velocities"])), np.asarray(curves["velocities"], dtype="<i4").tobytes(),
                   overall.tobytes(), struct.pack("<I", groups.shape[3] if groups.ndim == 5 else 0)]
        if groups.ndim == 5:
            payload.append(groups.tobytes())
        out.append(section(b"DAMV", b"".join(payload)))
        print(f"  damper curves per velocity: {curves['velocities']}")
    path = corpus / "release-noise.json"
    if path.exists():
        notes, velocities, amps = release_noise.load(path)
        levels = np.asarray(json.loads(path.read_text(encoding="utf-8"))["level_db"], dtype="<f4")
        data = json.loads(path.read_text(encoding="utf-8"))
        holds, hold_db = data.get("hold_s", []), data.get("hold_db", [])
        payload = [struct.pack("<2I", len(notes), len(velocities)), np.asarray(notes, dtype="<i4").tobytes(),
                   np.asarray(velocities, dtype="<i4").tobytes(), levels.tobytes(), amps.astype("<f4").tobytes(),
                   struct.pack("<I", len(holds)), np.asarray(holds, dtype="<f4").tobytes(),
                   np.asarray(hold_db, dtype="<f4").tobytes()]
        out.append(section(b"RELN", b"".join(payload)))
        print(f"  release noise: {len(notes)} notes x {len(velocities)} velocities")
    path = corpus / "pedal-curve.json"
    if path.exists():
        lift = np.asarray(json.loads(path.read_text(encoding="utf-8"))["lift"], dtype="<f4")
        if lift.shape != (128,):
            raise SystemExit(f"{path.name}: 128 values expected")
        out.append(section(b"PEDL", lift.tobytes()))
        print("  half pedal: measured damper lift curve")
    return out


def run(corpus: Path, jobs: int = 0) -> list[Path]:
    layers = velocities(corpus)
    if not layers:
        raise SystemExit(f"No velocity layer with both modes-vNNN.json and soundboard-vNNN.json in {corpus}")
    start = time.time()
    modes, dropped_modes, dropped_partials = {}, 0, 0
    for v in layers:
        modes[v] = {}
        for n in json.loads((corpus / f"modes-v{v:03d}.json").read_text(encoding="utf-8"))["notes"]:
            modes[v][n["midi"]], dm, dp = clean(n)
            dropped_modes, dropped_partials = dropped_modes + dm, dropped_partials + dp
    print(f"  failed fits left out: {dropped_modes} modes, {dropped_partials} partials")
    boards = {v: soundboard.load(corpus / f"soundboard-v{v:03d}.json") for v in layers}
    freqs, decays = boards[layers[0]][0], boards[layers[0]][1]
    for v in layers[1:]:
        if len(boards[v][0]) != len(freqs) or np.max(np.abs(boards[v][0] - freqs)) > 1e-3:
            raise SystemExit(f"soundboard-v{v:03d}.json does not share the resonances of the other layers")
    if repaired := repair_bodies(modes, boards, layers):
        print(f"  soundboard fits replaced by the note's median layer: {repaired}")
    takes = {}
    for v in layers:
        for p in (corpus / "single-notes").glob(f"*_v{v:03d}_h*.flac"):
            takes[(int(p.name[:3]), v)] = p
    tasks = [(str(takes[(m, v)]), modes[v][m], (freqs, decays, boards[v][2][m]))
             for v in layers for m in sorted(modes[v]) if (m, v) in takes and m in boards[v][2]]
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        residuals = {(r["midi"], r["velocity"]): r for r in pool.map(_layer_task, tasks)}
    print(f"  residuals of {len(residuals)} takes ({time.time() - start:.0f} s)")
    lowered = [r for r in residuals.values() if r["lowered"]]
    print(f"  soundboard louder than the take, brought down: {sum(r['lowered'] for r in lowered)} resonances "
          f"in {len(lowered)} layers")
    faded = sum(len(r["fades"]) for r in residuals.values())
    print(f"  partials louder than the take past their analysis window, faded out from its end: {faded} "
          f"in {sum(1 for r in residuals.values() if r['fades'])} layers")
    rates = {r["fs"] for r in residuals.values()}
    if len(rates) != 1:
        raise SystemExit(f"Takes at several sample rates: {sorted(rates)}")

    curves = json.loads((corpus / "release-curves.json").read_text(encoding="utf-8"))
    db = np.asarray(curves["curves_db"], dtype="<f4")  # notes x holds x points
    out = [MAGIC, struct.pack("<IdI", VERSION, float(rates.pop()), 2)]
    out.append(struct.pack("<I", len(freqs)))
    out.append(np.stack([freqs, decays], axis=1).astype("<f8").tobytes())
    out.append(struct.pack("<d3I", curves["step_s"], db.shape[0], db.shape[1], db.shape[2]))
    out.append(np.asarray(curves["notes"], dtype="<i4").tobytes())
    out.append(np.asarray(curves["holds_s"], dtype="<f8").tobytes())
    out.append(db.tobytes())
    # A layer whose partials were all too quiet to analyse would play as silence: it is left out.
    notes = {}
    for m in sorted({m for (m, _) in residuals}):
        present = [v for v in layers if (m, v) in residuals and any(p["modes"] for p in modes[v][m]["partials"])]
        skipped = [v for v in layers if (m, v) in residuals and v not in present]
        if skipped:
            print(f"  note {m}: layers {skipped} left out (no partial analysed)")
        if present:
            notes[m] = present
    midis = list(notes)
    out.append(struct.pack("<I", len(midis)))
    total_modes = 0
    for m, present in notes.items():
        out.append(struct.pack("<2Id", m, len(present), modes[present[0]][m]["f0"]))
        for v in present:
            note, layer = modes[v][m], residuals[(m, v)]
            out.append(_layer_bytes(note, layer["body"], layer["residual"], layer["fades"]))
            total_modes += sum(len(p["modes"]) for p in note["partials"])
    out += sections(corpus)
    # The coupled strings of each unison, from the modes just written.
    with ProcessPoolExecutor(max_workers=jobs or None) as pool:
        tables = {(m, v): t for m, v, t in pool.map(_unison_task, [(m, v, modes[v][m]) for m, present in notes.items() for v in present])}
    payload = [struct.pack("<I", len(midis))]
    for m, present in notes.items():
        payload.append(struct.pack("<2I", m, len(present)))
        payload += [unison.layer_bytes(v, tables[(m, v)]) for v in present]
    out.append(section(b"UNIS", b"".join(payload)))
    # The layers' measures, from the residuals and the modes just written.
    payload = [struct.pack("<3fI", ATTACK_S, SUSTAIN_S[0], SUSTAIN_S[1], len(midis))]
    for m, present in notes.items():
        payload.append(struct.pack("<2I", m, len(present)))
        for v in present:
            attack, levels = residuals[(m, v)]["measures"]
            payload.append(struct.pack("<IfI", v, attack, len(levels)) + np.asarray(levels, dtype="<f4").tobytes())
    out.append(section(b"LEVL", b"".join(payload)))
    fitted = [t for table in tables.values() for t in table if t is not None]
    total = sum(1 for (m, _), table in tables.items() if unison.strings_of(m) > 1 for _ in table)
    print(f"  coupled strings: {len(fitted)} of {total} partials of the notes with 2 or 3 strings ({time.time() - start:.0f} s)")
    path = corpus / "modal-profile.bin"
    path.write_bytes(b"".join(out))
    print(f"  {len(midis)} notes x {len(layers)} layers, {total_modes} modes, "
          f"{path.stat().st_size / 1e6:.1f} MB ({time.time() - start:.0f} s)")
    return [path]
