# SPDX-License-Identifier: AGPL-3.0-or-later

"""Composes "Study No. 1 in A minor", an original test piece, and writes it as a standard MIDI file.

The piece is written to exercise what tells a piano from a sampler, in about a minute:
  bars 1-4    p, legato with the sustain pedal: left-hand broken chords, simple right-hand melody
  bars 5-8    crescendo from mf to f, melody doubled in sixths, wider left hand
  bars 9-10   mf staccato without pedal, repeated notes (re-strikes and key releases)
  bars 11-12  ff climax: full chords at velocities 115-127, pedal down
  bars 13-14  diminuendo, descending arpeggio into the undamped top octave
  bars 15-16  pp final chord held with the pedal, then released (dampers)
Timing and velocities are slightly humanized with a fixed seed, so the file is reproducible.

Usage: .venv/Scripts/python tools/bench/pieces/compose_study_no1.py   (writes study_no1.mid next to this script)
"""
import random
from pathlib import Path

import mido

TPB = 480  # ticks per quarter note
BPM = 76
rng = random.Random(1)
events: list[tuple[float, mido.Message]] = []  # (time in beats, message)

NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def n(name: str) -> int:
    """'A2' -> 45, 'C#5' -> 73."""
    acc = 1 if "#" in name else -1 if name[1:2] == "b" else 0
    return NAMES[name[0]] + acc + 12 * (int(name.rstrip("#b")[-1] if name[-1].isdigit() else name[-1]) + 1)


def note(beat: float, length: float, pitch: str | int, velocity: int, legato: float = 0.95) -> None:
    p = pitch if isinstance(pitch, int) else n(pitch)
    jitter = rng.uniform(-0.012, 0.012)  # about +-10 ms at 76 BPM
    v = max(1, min(127, velocity + rng.randint(-4, 4)))
    events.append((beat + jitter, mido.Message("note_on", note=p, velocity=v)))
    events.append((beat + jitter + length * legato, mido.Message("note_off", note=p, velocity=64)))


def pedal(beat: float, down: bool) -> None:
    events.append((beat, mido.Message("control_change", control=64, value=127 if down else 0)))


def pedal_change(beat: float) -> None:
    """Legato pedaling: lift just after the new harmony is struck, press again a moment later."""
    pedal(beat + 0.05, False)
    pedal(beat + 0.25, True)


# Left-hand broken chords: (bass, fifth, tenth) per harmony.
HARMONIES = {"Am": ("A2", "E3", "C4"), "F": ("F2", "C3", "A3"), "C": ("C3", "G3", "E4"), "G": ("G2", "D3", "B3")}
PROGRESSION = ["Am", "F", "C", "G"]


def broken_chord(bar: int, harmony: str, velocity: int, wide: bool = False) -> None:
    bass, fifth, tenth = HARMONIES[harmony]
    pattern = [bass, fifth, tenth, fifth, tenth, fifth, tenth, fifth]
    if wide:
        pattern[0] = n(bass) - 12  # octave below
    for i, p in enumerate(pattern):
        note(bar * 4 + i * 0.5, 0.5, p, velocity - (6 if i else 0), legato=1.0)


# Bars 1-4: p, legato pedal.
pedal(0.0, True)
melody_a = [("E5", 2), ("D5", 1), ("C5", 1), ("C5", 2), ("B4", 1), ("A4", 1),
            ("G4", 2), ("A4", 1), ("B4", 1), ("D5", 2), ("C5", 1), ("B4", 1)]
beat = 0.0
for bar, harmony in enumerate(PROGRESSION):
    if bar:
        pedal_change(bar * 4)
    broken_chord(bar, harmony, 42)
for pitch, length in melody_a:
    note(beat, length, pitch, 52)
    beat += length

# Bars 5-8: crescendo mf -> f, melody in sixths.
melody_b = [("A5", 2), ("G5", 1), ("E5", 1), ("F5", 2), ("E5", 1), ("C5", 1),
            ("E5", 2), ("F5", 1), ("G5", 1), ("B5", 2), ("A5", 1), ("G5", 1)]
for i, harmony in enumerate(PROGRESSION):
    bar = 4 + i
    pedal_change(bar * 4)
    broken_chord(bar, harmony, 58 + 10 * i, wide=True)
beat = 16.0
for k, (pitch, length) in enumerate(melody_b):
    v = 66 + 2 * k
    note(beat, length, pitch, v)
    note(beat, length, n(pitch) - 9, v - 10)  # a sixth below
    beat += length

# Bars 9-10: mf staccato, no pedal, repeated notes.
pedal(32.0 - 0.1, False)
for k in range(8):
    b = 32.0 + 0.5 * k
    note(b, 0.5, "E5", 70 + (6 if k % 2 == 0 else 0), legato=0.35)
    note(b, 0.5, "C5", 60, legato=0.35)
    if k % 2 == 0:
        note(b, 1.0, "A2", 72, legato=0.3)
for k in range(8):
    b = 36.0 + 0.5 * k
    note(b, 0.5, "D5" if k < 4 else "B4", 74, legato=0.35)
    note(b, 0.5, "B4" if k < 4 else "G#4", 62, legato=0.35)
    if k % 2 == 0:
        note(b, 1.0, "E2", 76, legato=0.3)

# Bars 11-12: ff climax with the pedal.
pedal(40.0 + 0.02, True)
for pitch, v in [("F1", 118), ("F2", 120), ("C3", 112), ("A3", 110), ("C5", 118), ("F5", 124), ("A5", 127)]:
    note(40.0, 4.0, pitch, v)
pedal_change(44.0)
for pitch, v in [("E1", 116), ("E2", 118), ("B2", 110), ("G#3", 108), ("B4", 116), ("E5", 122), ("G#5", 125)]:
    note(44.0, 3.0, pitch, v)
for k, pitch in enumerate(["E6", "D6", "B5"]):  # bridge into the diminuendo
    note(47.0 + k / 3, 1 / 3, pitch, 96 - 8 * k)

# Bars 13-14: diminuendo, descending arpeggio starting in the undamped top octave.
pedal_change(48.0)
arpeggio = ["A7", "E7", "C7", "A6", "E6", "C6", "A5", "E5", "C5", "A4", "E4", "C4", "A3", "E3", "C3", "A2"]
for k, pitch in enumerate(arpeggio):
    note(48.0 + 0.5 * k, 0.5, pitch, 78 - 3 * k, legato=1.0)

# Bars 15-16: pp final chord held, then the pedal is released.
pedal_change(56.0)
for pitch in ["A1", "A2", "E3", "A3", "C4", "E4", "A4"]:
    note(56.0, 6.0, pitch, 30)
pedal(62.5, False)

# Write the file.
events.sort(key=lambda e: e[0])
track = mido.MidiTrack()
track.append(mido.MetaMessage("track_name", name="Study No. 1 in A minor (PianoForge test piece)", time=0))
track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(BPM), time=0))
previous = 0
for beat, message in events:
    tick = max(0, round(beat * TPB))
    track.append(message.copy(time=tick - previous))
    previous = tick
track.append(mido.MetaMessage("end_of_track", time=TPB))
midi = mido.MidiFile(ticks_per_beat=TPB)
midi.tracks.append(track)
out = Path(__file__).with_name("study_no1.mid")
midi.save(out)
notes = sum(1 for _, m in events if m.type == "note_on")
print(f"{out}: {notes} notes, {midi.length:.1f} s")
