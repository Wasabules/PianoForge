# SPDX-License-Identifier: AGPL-3.0-or-later

"""Tests of the soundboard amplitude fit: shared poles, bins under the partials left out."""

import unittest

import numpy as np

from forge.soundboard import BAND, fit_note

FS_D = 1378.125


def body(poles, amps, n):
    m = np.arange(n)
    return (amps @ (poles[:, None] ** m[None, :]))  # channels x samples


class SoundboardFitTest(unittest.TestCase):
    def setUp(self):
        centre = sum(BAND) / 2
        freqs = np.array([80.0, 140.0, 233.0, 410.0, 615.0])
        decays = np.array([6.0, 9.0, 12.0, 15.0, 20.0])
        self.poles = np.exp((-decays + 2j * np.pi * (freqs - centre)) / FS_D)
        self.freqs = freqs
        rng = np.random.default_rng(3)
        self.amps = rng.standard_normal((2, 5)) + 1j * rng.standard_normal((2, 5))
        self.z = body(self.poles, self.amps, 3000)

    def test_recovers_amplitudes_without_partials(self):
        found = fit_note(self.z, FS_D, self.poles, [], half_width=10.0, ridge=0.0)
        self.assertLess(np.max(np.abs(found - self.amps)), 1e-8)

    def test_resonance_under_a_partial_is_left_at_zero_and_others_still_fit(self):
        partial = 236.0  # 3 Hz from the 233 Hz resonance: not measurable under that partial
        signal = self.z + 50.0 * np.exp(2j * np.pi * (partial - sum(BAND) / 2) * np.arange(3000) / FS_D)[None, :]
        found = fit_note(signal, FS_D, self.poles, [partial], half_width=10.0, ridge=1e-6)
        self.assertEqual(found[0, 2], 0)
        others = [0, 1, 3, 4]
        error = np.abs(found[:, others] - self.amps[:, others]) / np.abs(self.amps[:, others])
        self.assertLess(np.max(error), 0.1)


if __name__ == "__main__":
    unittest.main()
