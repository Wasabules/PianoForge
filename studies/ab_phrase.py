"""A short musical phrase played by both engines of the application, for listening and for checks.

The phrase is written as a standard MIDI file, rendered offline by the application (`--render`, the same code as
the listening lab), once through the reference player (Noire samples) and once through the modal engine. Then an
A/B file is written: reference, silence, modal, with one gain for both (peak at -3 dBFS).

The phrase exercises what single notes do not: chords and the shared soundboard, the sustain pedal, the dampers
(detached notes), re-strikes, and velocities between the layers.

Usage: .venv/Scripts/python studies/ab_phrase.py
Writes corpus/spikes/engine/phrase.mid, phrase_reference.wav, phrase_modal.wav, phrase_AB_reference-modal.flac.
"""
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

APP = Path("build/windows-msvc/app/PianoForgeApp_artefacts/Release/PianoForge.exe")
CORPUS = Path("corpus/reference/noire-dry/single-notes")
OUT = Path("corpus/spikes/engine")
PPQ, TEMPO = 480, 500_000  # 120 bpm: one beat = 0.5 s


def phrase() -> list[tuple[float, bytes]]:
    """(time in s, MIDI message) pairs."""
    ev = []

    def note(t, length, key, velocity):
        ev.append((t, bytes([0x90, key, velocity])))
        ev.append((t + length, bytes([0x80, key, 0])))

    def pedal(t, down):
        ev.append((t, bytes([0xB0, 64, 127 if down else 0])))

    # Single notes across the keyboard, at velocities between the layers.
    note(0.0, 1.2, 33, 52)    # A1
    note(1.5, 1.2, 60, 90)    # C4
    note(3.0, 1.2, 84, 61)    # C6
    # A pedalled chord, then a melody over it.
    pedal(4.6, True)
    for key, vel in ((48, 70), (55, 66), (64, 74), (72, 82)):
        note(4.7, 0.6, key, vel)
    for i, (key, vel) in enumerate(((76, 70), (74, 64), (72, 76), (79, 88))):
        note(5.6 + 0.45 * i, 0.4, key, vel)
    pedal(8.4, False)
    # Detached arpeggio, no pedal: the dampers.
    for i, key in enumerate((60, 64, 67, 72, 76, 79, 84)):
        note(9.0 + 0.18 * i, 0.12, key, 72)
    # Re-strikes.
    for i, vel in enumerate((100, 84, 110)):
        note(10.8 + 0.4 * i, 0.3, 69, vel)
    # A loud bass octave, held then released.
    note(12.4, 2.0, 28, 112)
    note(12.4, 2.0, 40, 104)
    return sorted(ev, key=lambda e: (e[0], e[1][0] != 0x80))


def write_midi(path: Path, events: list[tuple[float, bytes]]) -> None:
    track, last = bytearray(), 0
    track += b"\x00\xff\x51\x03" + TEMPO.to_bytes(3, "big")
    for t, msg in events:
        tick = int(round(t * 1e6 / TEMPO * PPQ))
        delta, last = tick - last, tick
        var = [delta & 0x7F]
        delta >>= 7
        while delta:
            var.insert(0, 0x80 | (delta & 0x7F))
            delta >>= 7
        track += bytes(var) + msg
    track += b"\x00\xff\x2f\x00"
    path.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ) + b"MTrk" + struct.pack(">I", len(track)) + track)


def render(midi: Path, out: Path, modal: bool) -> np.ndarray:
    args = [str(APP), "--render", str(midi), str(out), str(CORPUS)] + (["--engine", "modal"] if modal else [])
    subprocess.run(args, check=True)
    print(out.with_suffix(".txt").read_text(encoding="utf-8").strip())
    audio, fs = sf.read(out, always_2d=True)
    return audio, fs


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    midi = OUT / "phrase.mid"
    write_midi(midi, phrase())
    ref, fs = render(midi, OUT / "phrase_reference.wav", False)
    mod, _ = render(midi, OUT / "phrase_modal.wav", True)
    n = max(len(ref), len(mod))
    gap = np.zeros((int(1.0 * fs), 2))
    pad = lambda x: np.vstack([x, np.zeros((n - len(x), 2))])
    ab = np.vstack([pad(ref), gap, pad(mod)])
    ab *= 10 ** (-3 / 20) / max(np.max(np.abs(ab)), 1e-9)
    sf.write(OUT / "phrase_AB_reference-modal.flac", ab, fs, subtype="PCM_24")
    rms = lambda x: 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)
    print(f"Levels: reference {rms(ref):.1f} dB, modal {rms(mod):.1f} dB (RMS over the phrase)")
    print(f"Written {OUT / 'phrase_AB_reference-modal.flac'}")
    sys.exit(0)
