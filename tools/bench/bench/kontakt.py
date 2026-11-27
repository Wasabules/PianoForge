# SPDX-License-Identifier: AGPL-3.0-or-later

"""Offline hosting of Kontakt 8 (VST3) through pedalboard.

Measured on the development machine (Kontakt 8.14.1, pedalboard 0.9.26):
- loading the plugin takes 45 to 130 s;
- a plugin reset costs exactly the initialization timeout given to pedalboard. The plugin is therefore
  loaded with a short timeout and never reset: between two takes, all sound is cut (CC120, CC123) and the
  host waits for digital silence;
- without reset, rendering is 45 to 80 times faster than real time with Noire, and bit-identical to a
  render paced at real time (disk streaming keeps up).
"""

from __future__ import annotations

import hashlib
import time
from typing import Callable

import numpy as np
import pedalboard

from bench.checks import SILENT_DBFS, peak_dbfs
from bench.protocol import Take, cc, note_off, note_on

KONTAKT_VST3 = r"C:\Program Files\Common Files\VST3\Kontakt 8.vst3"
SILENCE_DBFS = -120.0
# Hosted by pedalboard, Kontakt 8 always runs at 44.1 kHz whatever the sample rate requested (measured: a C4
# rendered at 48 kHz came out at 284.7 Hz, i.e. 48000/44100 too high; at 96 kHz, 569.4 Hz). Sessions rendered
# with Kontakt must therefore use 44.1 kHz, and every run checks the pitch of A4 before rendering anything.
KONTAKT_SAMPLE_RATE = 44100
MAX_PITCH_ERROR_CENTS = 30.0

Log = Callable[[str], None]


def fingerprint(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


class KontaktHost:
    def __init__(self, fs: int = KONTAKT_SAMPLE_RATE, block: int = 512, path: str = KONTAKT_VST3, log: Log = print):
        if fs != KONTAKT_SAMPLE_RATE:
            raise ValueError(f"Kontakt hosted by pedalboard runs at {KONTAKT_SAMPLE_RATE} Hz whatever the requested "
                             f"rate: render sessions at sample_rate = {KONTAKT_SAMPLE_RATE} (requested: {fs})")
        self.fs, self.block, self.log = fs, block, log
        start = time.monotonic()
        log("Loading Kontakt (one to two minutes)...")
        self.plugin = pedalboard.load_plugin(path, plugin_name="Kontakt 8", initialization_timeout=1.0)
        if not self.plugin.is_instrument:
            raise RuntimeError(f"{path} does not report itself as an instrument")
        log(f"Kontakt {self.plugin.version} loaded in {time.monotonic() - start:.0f} s")

    @property
    def version(self) -> str:
        return str(self.plugin.version)

    # --- state --------------------------------------------------------------------------------------------

    def state(self) -> bytes:
        return bytes(self.plugin.raw_state)

    def set_state(self, state: bytes) -> None:
        self.plugin.raw_state = state

    def open_editor(self) -> None:
        self.plugin.show_editor()  # blocks until the window is closed

    # --- rendering ----------------------------------------------------------------------------------------

    def _render(self, events: list[tuple[int, tuple[int, ...]]], n: int) -> np.ndarray:
        # +0.25 sample: the event lands on the same sample whether the host rounds or truncates the time.
        messages = [(bytes(b), (t + 0.25) / self.fs) for t, b in events]
        audio = self.plugin(messages, duration=n / self.fs, sample_rate=self.fs, num_channels=2,
                            buffer_size=self.block, reset=False)
        if audio.shape[1] != n:  # the host may truncate the duration by one sample
            if abs(audio.shape[1] - n) > 1:
                raise RuntimeError(f"unexpected length: {audio.shape[1]} instead of {n}")
            audio = np.pad(audio, ((0, 0), (0, max(0, n - audio.shape[1]))))[:, :n]
        return audio

    def _render_paced(self, events: list[tuple[int, tuple[int, ...]]], n: int, speed: float) -> np.ndarray:
        """Renders in chunks, never faster than `speed` times real time (1.0: strict real time)."""
        chunk = 8 * self.block
        parts, start = [], time.monotonic()
        for a in range(0, n, chunk):
            b = min(n, a + chunk)
            parts.append(self._render([(t - a, m) for t, m in events if a <= t < b], b - a))
            wait = start + b / self.fs / speed - time.monotonic()
            if wait > 0:
                time.sleep(wait)
        return np.concatenate(parts, axis=1)

    def render_take(self, take: Take, speed: float | None = None) -> np.ndarray:
        if speed is None:
            return self._render(take.events, take.duration)
        return self._render_paced(take.events, take.duration, speed)

    def silence(self, max_s: float = 120.0) -> float:
        """Cuts every voice and waits for digital silence; returns the tail rendered, in seconds."""
        events = [(0, cc(120, 0)), (0, cc(123, 0)), (0, cc(64, 0)), (0, cc(66, 0)), (0, cc(67, 0))]
        chunk, rendered, quiet = round(0.25 * self.fs), 0, 0
        while quiet < 2:
            if rendered > max_s * self.fs:
                raise RuntimeError("Kontakt does not return to silence")
            audio = self._render(events, chunk)
            events, rendered = [], rendered + chunk
            quiet = quiet + 1 if peak_dbfs(audio) < SILENCE_DBFS else 0
        return rendered / self.fs

    def measure_a4(self) -> float:
        """Fundamental of A4 (velocity 64), from a zero-padded spectrum of one second of the sustain."""
        n = round(1.5 * self.fs)
        audio = self._render([(0, note_on(69, 64)), (round(1.3 * self.fs), note_off(69))], n)
        self.silence()
        x = audio.mean(axis=0)[round(0.2 * self.fs): round(1.2 * self.fs)]
        size = 1 << 21
        spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), size))
        freqs = np.fft.rfftfreq(size, 1 / self.fs)
        band = (freqs > 300) & (freqs < 600)
        return float(freqs[band][np.argmax(spectrum[band])])

    def wait_until_loaded(self, max_s: float = 900.0) -> dict:
        """Kontakt loads its samples in the background: wait until probe notes sound, consistently."""
        probes = [(21, 40), (21, 120), (60, 64), (108, 40), (108, 120)]
        n = round(1.5 * self.fs)
        start, previous = time.monotonic(), None
        while time.monotonic() - start < max_s:
            peaks = []
            for note, vel in probes:
                self.silence()
                audio = self._render([(0, note_on(note, vel)), (round(0.5 * self.fs), note_off(note))], n)
                peaks.append(round(peak_dbfs(audio), 2))
            sounding = all(p > SILENT_DBFS for p in peaks)
            if sounding and previous is not None and max(abs(a - b) for a, b in zip(peaks, previous)) < 0.1:
                self.silence()
                waited = time.monotonic() - start
                a4 = self.measure_a4()
                cents = 1200 * np.log2(a4 / 440.0)
                self.log(f"Instrument ready after {waited:.0f} s; probe peaks (dBFS): {peaks}; A4 at {a4:.2f} Hz "
                         f"({cents:+.1f} cents)")
                if abs(cents) > MAX_PITCH_ERROR_CENTS:
                    raise RuntimeError(f"A4 sounds at {a4:.2f} Hz ({cents:+.1f} cents): sample-rate mismatch between "
                                       f"the host and the plugin, renders would be off pitch")
                return {"wait_s": round(waited, 1), "probe_peaks_dbfs": peaks, "a4_hz": round(a4, 3)}
            previous = peaks if sounding else None
            self.log(f"  still loading... probe peaks: {peaks}")
            time.sleep(3.0)
        raise TimeoutError("the instrument does not sound: no instrument in the state, or loading takes too long")
