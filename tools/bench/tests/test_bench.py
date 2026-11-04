"""Bench tests that do not need Kontakt: python -m unittest discover -s tools/bench/tests"""

import math
import tempfile
import unittest
from pathlib import Path

import numpy as np

from bench.checks import analyze, max_diff_db
from bench.keyboard_probe import analyze_capture
from bench.protocol import load_session, midi_to_note, note_to_midi, parse_session
from bench.storage import Corpus

SESSIONS = Path(__file__).resolve().parents[1] / "sessions"
FS = 48000


class Notes(unittest.TestCase):
    def test_names(self):
        self.assertEqual(note_to_midi("A0"), 21)
        self.assertEqual(note_to_midi("C4"), 60)
        self.assertEqual(note_to_midi("C8"), 108)
        self.assertEqual(note_to_midi("F#3"), note_to_midi("Gb3"))
        self.assertEqual(note_to_midi("Fs3"), 54)
        self.assertEqual(midi_to_note(61), "Cs4")
        for n in range(128):
            self.assertEqual(note_to_midi(midi_to_note(n)), n)

    def test_invalid(self):
        for name in ("H2", "C", "C#", "G9x"):
            with self.assertRaises(ValueError):
                note_to_midi(name)


class Protocol(unittest.TestCase):
    def test_single_note(self):
        s = parse_session("""
            [session]
            name = "t"
            [[pass]]
            name = "p"
            type = "note"
            notes = "C4..D4"
            velocities = [10, 100]
            hold = 1.0
            tail = 0.5
        """)
        self.assertEqual(len(s.takes), 3 * 2)
        t = s.takes[0]
        self.assertEqual(t.id, "p/060-C4_v010_h1000ms")
        self.assertEqual(t.duration, round(1.55 * FS))
        self.assertEqual(t.events, [(round(0.05 * FS), (0x90, 60, 10)), (round(1.05 * FS), (0x80, 60, 64))])
        self.assertEqual(t.meta["note_on_sample"], round(0.05 * FS))

    def test_pedal_and_hold_law(self):
        s = parse_session("""
            [[pass]]
            name = "p"
            type = "note"
            notes = ["A0", "C4", "C8", "C2"]
            velocities = 64
            pedal = "down"
            hold_by_note = [["A0", 30.0], ["C4", 20.0], ["C8", 10.0]]
        """)
        holds = [t.meta["hold"] for t in s.takes]
        self.assertEqual(holds[:3], [30.0, 20.0, 10.0])
        self.assertAlmostEqual(holds[3], 30.0 - 10.0 * (36 - 21) / (60 - 21), places=3)
        self.assertEqual(s.takes[0].events[0], (0, (0xB0, 64, 127)))

    def test_shipped_sessions(self):
        ref = load_session(SESSIONS / "reference.toml")
        count = {p: sum(1 for x in ref.takes if x.pass_name == p) for p in ref.passes()}
        self.assertEqual(count["layer-discovery"], 6 * 127)
        self.assertEqual(count["single-notes"], 88 * 16)
        self.assertEqual(count["releases"], 22 * 3 * 3)
        self.assertEqual(count["repetitions"], 3 * 4 * 2)
        for name in ("reference", "interactions", "smoke"):
            session = load_session(SESSIONS / f"{name}.toml")
            for t in session.takes:
                self.assertTrue(all(0 <= time < t.duration for time, _ in t.events), t.id)
                self.assertNotIn(".", t.id.split("/")[-1], t.id)  # a dot would look like a file extension

    def test_midi_file_pass(self):
        s = load_session(SESSIONS / "pieces.toml")
        t = s.takes[0]
        self.assertEqual(t.id, "pieces/study_no1")
        self.assertEqual(t.meta["notes"], 180)
        self.assertTrue(all(b[0] & 0x0F == 0 for _, b in t.events))  # everything on channel 1
        self.assertTrue(all(0 <= time < t.duration for time, _ in t.events))
        self.assertEqual(sum(1 for _, b in t.events if b[0] & 0xF0 == 0xB0), 24)  # pedal events

    def test_errors(self):
        with self.assertRaises(ValueError):
            parse_session('[[pass]]\nname="p"\ntype="unknown"')
        with self.assertRaises(ValueError):
            parse_session('[[pass]]\nname="p"\ntype="note"\nnotes="C4"\nvelocities=[0]\nhold=1.0')

    def test_stable_key(self):
        a = load_session(SESSIONS / "reference.toml").takes[10]
        b = load_session(SESSIONS / "reference.toml").takes[10]
        self.assertEqual(a.key({"state": "x"}), b.key({"state": "x"}))
        self.assertNotEqual(a.key({"state": "x"}), a.key({"state": "y"}))


def synthetic_note(start: int, length: int, t60: float = 2.0, peak: float = 0.5) -> np.ndarray:
    t = np.arange(length - start) / FS
    s = peak * np.sin(2 * np.pi * 220 * t) * np.exp(-math.log(1000) * t / t60)
    mono = np.concatenate([np.zeros(start), s]).astype(np.float32)
    return np.stack([mono, mono])


class Checks(unittest.TestCase):
    def test_clean_note(self):
        a = analyze(synthetic_note(2400, FS * 6), FS, note_on_sample=2400)
        self.assertEqual(a["defects"], [])
        self.assertLess(a["attack_delay_ms"], 1.0)
        self.assertLess(a["tail_db"], -60)

    def test_defects(self):
        self.assertEqual(analyze(np.zeros((2, FS), np.float32), FS, note_on_sample=100)["defects"], ["silent"])
        self.assertIn("tail_too_short", analyze(synthetic_note(100, FS, t60=20.0), FS, 100)["defects"])
        self.assertIn("clipping", analyze(synthetic_note(100, FS * 6, peak=1.2), FS, 100)["defects"])
        late = analyze(synthetic_note(FS // 2, FS * 6), FS, note_on_sample=100)
        self.assertIn("missing_attack", late["defects"])

    def test_spurious_onsets(self):
        on, off = 2400, 2400 + 3 * FS
        events = [(on, (0x90, 57, 100)), (off, (0x80, 57, 64))]
        clean = synthetic_note(on, FS * 6)
        a = analyze(clean, FS, on, events, off)
        self.assertNotIn("spurious_onsets", a["defects"])
        self.assertIsNotNone(a["release_decay_40db_s"])
        # a second, unplayed note at 1.5 s: what a generative effect would produce
        intruder = clean + np.roll(synthetic_note(0, FS * 6, peak=0.3), round(1.5 * FS), axis=1) * \
            (np.arange(FS * 6) >= round(1.5 * FS))
        b = analyze(intruder.astype(np.float32), FS, on, events, off)
        self.assertIn("spurious_onsets", b["defects"])
        self.assertTrue(any(abs(t - 1.5) < 0.02 for t in b["spurious_onsets_s"]))

    def test_expected_silence(self):
        self.assertEqual(analyze(np.zeros((2, FS), np.float32), FS)["defects"], [])

    def test_difference(self):
        a = synthetic_note(100, FS)
        self.assertEqual(max_diff_db(a, a.copy()), -math.inf)
        b = a.copy()
        b[0, 500] += 1e-4
        self.assertAlmostEqual(max_diff_db(a, b), 20 * math.log10(1e-4 / 0.5), places=1)


class KeyboardProbe(unittest.TestCase):
    def test_switch_pedal_and_fixed_release(self):
        capture = [(0.0, [0x90, 60, 30]), (0.5, [0x80, 60, 64]), (1.0, [0x90, 72, 110]), (1.2, [0x80, 72, 64]),
                   (2.0, [0xB0, 64, 127]), (3.0, [0xB0, 64, 0])]
        a = analyze_capture(capture)
        self.assertTrue(a["sustain_pedal"].startswith("switch"))
        self.assertTrue(a["release_velocity"].startswith("absent"))
        self.assertEqual((a["strikes"]["velocity_min"], a["strikes"]["velocity_max"]), (30, 110))
        self.assertFalse(a["high_resolution_velocity_cc88"])

    def test_continuous_pedal_and_measured_release(self):
        capture = [(0.0, [0x90, 60, 30]), (0.5, [0x80, 60, 12]), (0.6, [0x90, 61, 40]), (0.9, [0x80, 61, 90])]
        capture += [(1.0 + i / 2, [0xB0, 64, v]) for i, v in enumerate((0, 20, 45, 70, 101, 127))]  # 0.5 s per position
        a = analyze_capture(capture)
        self.assertTrue(a["sustain_pedal"].startswith("continuous"))
        self.assertTrue(a["release_velocity"].startswith("present"))

    def test_stepped_switch_pedal(self):
        # Sequence captured on the Korg B2 with its supplied pedal: a quick ramp through two fixed steps.
        values = [(16.22, 74), (16.29, 127), (18.04, 0), (30.15, 38), (30.17, 74), (30.21, 127), (33.28, 38), (33.30, 0)]
        a = analyze_capture([(t, [0xB0, 64, v]) for t, v in values])
        self.assertTrue(a["sustain_pedal"].startswith("switch with stepped transitions"), a["sustain_pedal"])


class Storage(unittest.TestCase):
    def test_round_trip_and_resume(self):
        take = load_session(SESSIONS / "reference.toml").takes[0]
        audio = synthetic_note(2400, take.duration)
        with tempfile.TemporaryDirectory() as d:
            corpus = Corpus(d)
            self.assertFalse(corpus.is_up_to_date(take, "key1"))
            path = corpus.write(take, audio, FS, {"id": take.id, "pass": take.pass_name, "key": "key1"})
            self.assertEqual(path.suffix, ".flac")
            self.assertTrue(corpus.is_up_to_date(take, "key1"))
            self.assertFalse(corpus.is_up_to_date(take, "key2"))
            back, fs = corpus.read_audio(corpus.meta(take))
            self.assertEqual(fs, FS)
            self.assertLess(max_diff_db(audio, back), -130)  # 24-bit quantization
            self.assertEqual(corpus.rebuild_index(), 1)
            self.assertFalse(list(Path(d).rglob("*.tmp*")))

    def test_every_take_gets_its_own_file(self):
        # Regression: names with decimal durations ("t0.1", "t0.5") used to share one file.
        takes = [t for t in load_session(SESSIONS / "reference.toml").takes if t.pass_name in ("releases", "repetitions")]
        with tempfile.TemporaryDirectory() as d:
            corpus = Corpus(d)
            for t in takes:
                corpus.write(t, np.zeros((2, 16), np.float32), FS, {"id": t.id, "pass": t.pass_name, "key": "k"})
            self.assertEqual(len(corpus.metas()), len(takes))

    def test_peak_above_full_scale(self):
        take = load_session(SESSIONS / "reference.toml").takes[0]
        with tempfile.TemporaryDirectory() as d:
            path = Corpus(d).write(take, synthetic_note(100, FS, peak=1.5), FS, {"key": "k"})
            self.assertEqual(path.suffix, ".wav")


if __name__ == "__main__":
    unittest.main()
