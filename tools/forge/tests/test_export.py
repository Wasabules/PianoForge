# SPDX-License-Identifier: AGPL-3.0-or-later

"""Unit tests of the profile export's layer measures (section LEVL), no corpus needed."""

import unittest

import numpy as np

from forge import export, soundboard
from forge.modes import synthesize
from forge.note_modes import partial_modes

FS = 44100.0


def make_note() -> dict:
    """Two partials: the first of two strong modes that nearly cancel each other, the second of one mode."""
    def mode(f, d, left, right):
        return [f, d, left.real, left.imag, right.real, right.imag]
    return {"midi": 60, "velocity": 64, "partials": [
        {"n": 1, "centre": 261.6, "t0": 0.0, "t1": 2.0, "envelope_error_db": 0.0,
         "modes": [mode(261.6, 0.8, 0.2 + 0.0j, 0.18 + 0.02j), mode(261.65, 0.9, -0.19 + 0.01j, -0.17 + 0.0j)]},
        {"n": 2, "centre": 523.3, "t0": 0.0, "t1": 2.0, "envelope_error_db": 0.0,
         "modes": [mode(523.3, 1.5, 0.05 - 0.02j, 0.04 + 0.01j)]},
        {"n": 3, "centre": 785.0, "t0": 0.0, "t1": 2.0, "envelope_error_db": 0.0, "modes": []},  # not exported
    ]}


class LayerMeasuresTest(unittest.TestCase):
    def test_against_a_synthesis(self):
        note = make_note()
        rng = np.random.default_rng(1)
        r = 0.01 * rng.standard_normal((int(0.1 * FS), 2))  # a residual (frames x channels)
        body_f, body_d = np.array([120.0, 2500.0]), np.array([5.0, 30.0])
        body_a = np.array([[0.01 + 0.0j, 0.003j], [0.008 + 0.001j, -0.002 + 0.0j]])
        attack, levels = export.layer_measures(note, r, FS, body_f, body_d, body_a)

        # the attack: the residual, the modes and the soundboard, mono, over the first ATTACK_S
        n = int(round(export.ATTACK_S * FS))
        x = synthesize(partial_modes(note), 2, n, FS) + soundboard.synthesize_body(body_f, body_d, body_a, n, FS)
        x = (x.T + r[:n]).mean(axis=1)
        self.assertAlmostEqual(attack / np.mean(x * x), 1.0, places=9)

        # each partial over the sustain: the RMS of the sum of its modes, both channels averaged
        t0, t1 = export.SUSTAIN_S
        fine = 192000.0
        a, b = int(round(t0 * fine)), int(round(t1 * fine))
        self.assertEqual(len(levels), 2)  # the partials with modes
        for p, level in zip([p for p in partial_modes(note) if p.modes], levels):
            y = synthesize([p], 2, b - a, fine, start=a)
            self.assertAlmostEqual(level / np.sqrt(np.mean(y * y)), 1.0, places=4)

        # the sum of the first partial's modes is far under the sum of their energies
        energies = sum(0.5 * 0.5 * (abs(m.amps[0]) ** 2 + abs(m.amps[1]) ** 2)
                       * (np.exp(-2 * m.decay * t0) - np.exp(-2 * m.decay * t1)) / (2 * m.decay) / (t1 - t0)
                       for m in partial_modes(note)[0].modes)
        self.assertLess(20 * np.log10(levels[0] / np.sqrt(energies)), -10.0)


if __name__ == "__main__":
    unittest.main()
