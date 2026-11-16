"""The profile builder's own logic: its recipes, what it reads in a bank (layers, pedal-down samples, keys), the
check of a downloaded file, where the sympathetic resonance's calibration comes from. (The whole chain, a bank to a
profile, is the bench's and the forge's, tested there, and run end to end by hand: docs/building-a-profile.md.)"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
import soundfile as sf

from pfprofile.banks import find, load_banks
from pfprofile.build import BuildError, own_calibration, resolve_layers, resolve_sympathetic
from pfprofile.fetch import git_blob_sha1
from pfprofile.sfzinfo import BankInfo, inspect, layers_for, rendered_velocities
from pfprofile import PROJECT_ROOT

GRID = [8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127]


def _bank(folder: Path, pedal_down: bool, keys=(21, 108)) -> Path:
    """A tiny SFZ piano: one sample per octave, three velocity layers, pedal-up samples (and pedal-down ones)."""
    samples = folder / "samples"
    samples.mkdir(parents=True)
    t = np.arange(4410) / 44100.0
    sf.write(samples / "a.wav", (0.3 * np.sin(2 * np.pi * 440 * t)).astype(np.float32), 44100, subtype="PCM_24")
    lines = []
    for lo in range(keys[0], keys[1] + 1, 12):
        hi = min(keys[1], lo + 11)
        for lovel, hivel in ((1, 40), (41, 90), (91, 127)):
            base = f"<region> sample=samples/a.wav lokey={lo} hikey={hi} pitch_keycenter=69 lovel={lovel} hivel={hivel}"
            if pedal_down:
                lines.append(base + " locc64=0 hicc64=63")
                lines.append(base + " locc64=64 hicc64=127")
            else:
                lines.append(base)
    path = folder / "piano.sfz"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


class Recipes(unittest.TestCase):
    def test_known_banks(self):
        banks = load_banks()
        self.assertEqual(set(banks), {"salamander", "piano-in-162"})
        s = banks["salamander"]
        self.assertEqual((s.state, s.layers, s.sympathetic_from, s.hdcc), ("salamander-dry", "all", "piano-in-162", {21: 0.0, 22: 0.0}))
        self.assertEqual(s.fetch["kind"], "github")
        self.assertEqual(len(s.fetch["commit"]), 40)
        self.assertEqual(find("in162-close").id, "piano-in-162")  # by its state's name too
        self.assertIsNone(find("nothing"))

    def test_rendered_grid(self):
        self.assertEqual(rendered_velocities(PROJECT_ROOT / "tools" / "bench" / "sessions" / "reference.toml"), GRID)


class Layers(unittest.TestCase):
    def test_one_per_layer(self):
        # Piano in 162's five dynamics, as its SFZ has them: the loudest rendered velocity inside each.
        self.assertEqual(layers_for([(1, 33), (34, 64), (65, 80), (81, 101), (102, 127)], GRID), [32, 64, 80, 96, 127])

    def test_fine_banks_take_the_grid(self):
        sixteen = [(1, 26), (27, 34), (35, 36), (37, 43), (44, 46), (47, 50), (51, 56), (57, 64), (65, 72), (73, 80),
                   (81, 88), (89, 96), (97, 104), (105, 112), (113, 120), (121, 127)]
        self.assertEqual(layers_for(sixteen, GRID), GRID)

    def test_narrow_layer_left_out(self):
        self.assertEqual(layers_for([(1, 60), (61, 62), (63, 127)], GRID), [56, 127])

    def test_overrides(self):
        info = BankInfo(1, 1, 0, (21, 108), [(1, 64), (65, 127)], 0, {})
        self.assertEqual(resolve_layers("auto", info, GRID), [64, 127])
        self.assertEqual(resolve_layers("all", info, GRID), GRID)
        self.assertEqual(resolve_layers("32,64", info, GRID), [32, 64])
        with self.assertRaises(BuildError):
            resolve_layers("30", info, GRID)  # not rendered


class BankReading(unittest.TestCase):
    def test_layers_keys_pedal(self):
        with tempfile.TemporaryDirectory() as d:
            info = inspect(_bank(Path(d) / "with", pedal_down=True))
            self.assertEqual(info.ranges, [(1, 40), (41, 90), (91, 127)])
            self.assertTrue(info.full_keyboard)
            self.assertTrue(info.has_pedal_down)
            self.assertEqual(info.missing, 0)
            info = inspect(_bank(Path(d) / "without", pedal_down=False))
            self.assertFalse(info.has_pedal_down)
            info = inspect(_bank(Path(d) / "short", pedal_down=False, keys=(36, 96)))
            self.assertFalse(info.full_keyboard)


class Download(unittest.TestCase):
    def test_git_blob_hash(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "hello.txt"
            f.write_bytes(b"hello\n")
            self.assertEqual(git_blob_sha1(f), "ce013625030ba8dba906f756967f9e9ca394464a")  # git hash-object


class Sympathetic(unittest.TestCase):
    def test_where_the_calibration_comes_from(self):
        with tempfile.TemporaryDirectory() as d:
            corpus = Path(d)
            with_pedal = BankInfo(1, 1, 0, (21, 108), [], 880, {})
            without = BankInfo(1, 1, 0, (21, 108), [], 0, {})
            self.assertEqual(resolve_sympathetic("auto", with_pedal, corpus, "a")[0], "own")
            self.assertEqual(resolve_sympathetic("auto", without, corpus, "a")[0], "skip")  # nothing to borrow yet
            self.assertEqual(resolve_sympathetic("none", with_pedal, corpus, "a")[0], "skip")
            own = corpus / "reference" / "in162-close" / "sympathetic.json"
            own.parent.mkdir(parents=True)
            own.write_text(json.dumps({"gains": [1.0], "source": "x"}), encoding="utf-8")
            borrowed = corpus / "reference" / "other" / "sympathetic.json"
            borrowed.parent.mkdir(parents=True)
            borrowed.write_text(json.dumps({"gains": [1.0], "calibration_from": str(own)}), encoding="utf-8")
            self.assertTrue(own_calibration(own))
            self.assertFalse(own_calibration(borrowed))  # never borrowed at second hand
            mode, path, _ = resolve_sympathetic("auto", without, corpus, "salamander-dry")
            self.assertEqual((mode, path), ("borrow", own))
            mode, path, _ = resolve_sympathetic("piano-in-162", without, corpus, "salamander-dry")  # a bank's id
            self.assertEqual((mode, path), ("borrow", own))
            self.assertEqual(resolve_sympathetic("salamander", without, corpus, "x")[0], "skip")  # not built


if __name__ == "__main__":
    unittest.main()
