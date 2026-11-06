"""Unit tests of the forge parts for the pedal, the dampers and the sympathetic resonance (no corpus needed)."""

import unittest

import numpy as np

from forge import export, release_curves, sympathetic


class DamperGroupsTest(unittest.TestCase):
    def test_group_of(self):
        self.assertEqual([release_curves.group_of(n) for n in (1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 80)],
                         [0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5])

    def test_group_curves(self):
        count = 100
        t = np.arange(count) * release_curves.STEP_S
        held = np.ones((2, count))
        rel = np.stack([10 ** (-20 * t / 10), 10 ** (-60 * t / 10)])  # groups at -20 and -60 dB per second
        overall = np.concatenate([[0.0], -20 * t])                    # the overall curve follows the first
        out = release_curves.group_curves(rel, held, overall)
        self.assertEqual(out.shape, (2, count + 1))
        self.assertTrue(np.all(np.abs(out[0, 10:-10]) < 0.5))
        k = 50  # 0.5 s: the second group 20 dB under the overall curve
        self.assertAlmostEqual(out[1, k + 1], -20.0, delta=1.0)

    def test_group_louder_than_held_is_ignored(self):
        count = 50
        held = np.ones((1, count))
        rel = np.full((1, count), 10.0)  # +10 dB: the release noise in a weak partial's band, not a damper
        out = release_curves.group_curves(rel, held, np.zeros(count + 1))
        self.assertTrue(np.all(out == 0.0))


class SympatheticTest(unittest.TestCase):
    def test_driven_resonator_answers_its_own_frequency(self):
        fs = 4000.0
        t = np.arange(int(4 * fs)) / fs
        res = np.array([[440.0, 1.0, 1.0, 0.0, 1.0, 0.0]])
        near = sympathetic.response(np.sin(2 * np.pi * 440.2 * t), fs, res)
        far = sympathetic.response(np.sin(2 * np.pi * 523.0 * t), fs, res)
        ratio = sympathetic.power(near, fs, 2.0, 4.0) / sympathetic.power(far, fs, 2.0, 4.0)
        self.assertGreater(10 * np.log10(ratio), 30.0)
        # Steady state, unit coupling per second: the sine's positive-frequency half (amplitude 1/2) over
        # |sigma + i delta omega|.
        self.assertAlmostEqual(np.max(np.abs(near[0, -2000:])), 0.5 / np.hypot(1.0, 2 * np.pi * 0.2), delta=0.02)

    def test_onset_and_velocity(self):
        env = sympathetic.onset_envelope(1000, 1000.0, 0.3)
        self.assertAlmostEqual(env[300], 1 - np.exp(-1), places=3)
        self.assertTrue(np.all(sympathetic.onset_envelope(10, 1000.0, 0.0) == 1.0))
        scale = sympathetic.velocity_scale([{"take": "a", "velocity": 128}], 0.5)
        self.assertAlmostEqual(scale["a"], 2.0)


class ResidualLengthTest(unittest.TestCase):
    def test_quiet_from(self):
        fs = 1000.0
        n = 500
        x = np.ones((2, n))
        r = np.full((2, n), 0.5)   # -6 dB: the model misses a quarter of the energy
        r[:, 300:] = 0.1           # -20 dB from 0.3 s
        self.assertAlmostEqual(export.quiet_from(r, x, fs, 0.1), 0.3, places=6)
        self.assertAlmostEqual(export.quiet_from(np.zeros((2, n)), x, fs, 0.1), 0.1, places=6)
        self.assertAlmostEqual(export.quiet_from(np.ones((2, n)), x, fs, 0.1), 0.5, places=6)


if __name__ == "__main__":
    unittest.main()
