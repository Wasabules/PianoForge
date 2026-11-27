# SPDX-License-Identifier: AGPL-3.0-or-later

"""The coupled-unison fit recovers a known system: its poles, from the strings' detunings and the bridge."""
import unittest

import numpy as np

from forge import unison


def system_modes(s, Y, f_ref=440.0):
    n = len(s)
    lam = np.linalg.eigvals(np.diag(s) + Y * np.ones((n, n)))
    return [[float(l.imag / (2 * np.pi)), float(-l.real), 0.1, 0.0, 0.1, 0.0] for l in lam]


class UnisonTest(unittest.TestCase):
    def check(self, detune_hz, Y):
        s = -1.0 + 2j * np.pi * (440.0 + np.asarray(detune_hz))
        modes = system_modes(s, Y)
        fit = unison.fit_partial(modes, len(detune_hz))
        self.assertIsNotNone(fit)
        lam, _ = unison.model(np.asarray(fit["detune_hz"]), complex(*fit["y"]))
        want = np.sort_complex(np.array([-m[1] + 2j * np.pi * m[0] for m in modes]) - np.mean([-m[1] + 2j * np.pi * m[0] for m in modes]))
        got = lam - np.mean(lam)
        got = got[np.argsort(got.imag)]
        want = want[np.argsort(want.imag)]
        self.assertLess(np.max(np.abs(got - want)), 1e-3 * max(1.0, np.max(np.abs(want))))
        return fit

    def test_three_strings(self):
        fit = self.check([-0.4, 0.1, 0.5], -2.0 + 0.3j)
        self.assertLess(fit["y"][0], 0.0)

    def test_two_strings(self):
        fit = self.check([-0.3, 0.3], -1.5 + 0.0j)
        self.assertLess(fit["y"][0], 0.0)

    def test_soft_pedal_weakens_the_prompt_sound(self):
        lam, r0 = unison.model(np.array([-0.4, 0.1, 0.5]), -2.0 + 0.3j)
        _, r1 = unison.model(np.array([-0.4, 0.1, 0.5]), -2.0 + 0.3j, h=np.array([1.0, 1.0, 0.0]))
        fast = int(np.argmin(lam.real))  # the strings moving together: the bridge damps it most
        self.assertLess(abs(r1[fast]), abs(r0[fast]))
        slow = int(np.argmax(lam.real))
        self.assertGreater(abs(r1[slow]), abs(r0[slow]))  # the motions where the strings oppose, excited


if __name__ == "__main__":
    unittest.main()
