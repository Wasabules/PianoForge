"""Tests of the high-resolution mode analysis on synthetic partials with known modes."""

import unittest

import numpy as np

from forge.modes import BandExtractor, analyze_partial, fit_amplitudes, esprit_poles, synthesize

FS = 44100


def tone(modes, seconds, fs=FS, channels=2, noise_db=-120.0, seed=1):
    """modes: (freq, decay, [amp per channel]) summed as Re(a exp((-decay + 2 pi i f) t))."""
    t = np.arange(int(seconds * fs)) / fs
    x = np.zeros((channels, len(t)))
    for f, d, amps in modes:
        carrier = np.exp((-d + 2j * np.pi * f) * t)
        for c in range(channels):
            x[c] += np.real(amps[c] * carrier)
    rng = np.random.default_rng(seed)
    return x + 10 ** (noise_db / 20) * rng.standard_normal(x.shape)


class BasebandTest(unittest.TestCase):
    def test_flat_passband_keeps_amplitude_and_phase(self):
        a = 0.3 * np.exp(0.7j)
        x = tone([(261.9, 0.0, [a, a])], 2.0)
        z, fs_d = BandExtractor(x, FS).baseband(261.6, 30.0, 60.0)
        m = np.arange(z.shape[1]) / fs_d
        expected = a * np.exp(2j * np.pi * 0.3 * m)
        middle = slice(int(0.3 * fs_d), int(1.5 * fs_d))
        self.assertLess(np.max(np.abs(z[0, middle] - expected[middle])), 1e-4)

    def test_neighbouring_partials_are_rejected(self):
        x = tone([(261.6, 0.0, [1.0, 1.0]), (523.5, 0.0, [1.0, 1.0])], 2.0)
        z, fs_d = BandExtractor(x, FS).baseband(523.5, 30.0, 60.0)
        middle = slice(int(0.3 * fs_d), int(1.5 * fs_d))
        self.assertLess(np.max(np.abs(np.abs(z[0, middle]) - 1.0)), 1e-4)


class EspritTest(unittest.TestCase):
    def test_noiseless_poles(self):
        poles = np.array([0.999 * np.exp(0.01j), 0.995 * np.exp(-0.02j)])
        m = np.arange(400)
        z = (1.0 * poles[0] ** m + 0.5j * poles[1] ** m)[None, :]
        found = esprit_poles(z, 2)
        for p in poles:
            self.assertLess(np.min(np.abs(found - p)), 1e-9)
        amps, res = fit_amplitudes(z, found)
        self.assertLess(res, 1e-18)


class PartialTest(unittest.TestCase):
    def test_trichord_beating_and_double_decay(self):
        # Three unison strings: one strongly damped mode (prompt sound) and two weakly damped ones (after-sound),
        # 0.35 and 0.6 Hz apart, plus a neighbouring partial and noise 100 dB down.
        truth = [(262.10, 1.6, [0.10, 0.08 * np.exp(0.4j)]),
                 (262.45, 0.25, [0.02 * np.exp(1.0j), 0.025]),
                 (262.70, 0.20, [0.012 * np.exp(-2.0j), 0.01 * np.exp(0.3j)])]
        x = tone(truth + [(524.8, 1.0, [0.05, 0.05])], 12.0, noise_db=-100.0)
        result = analyze_partial(BandExtractor(x, FS), 1, 262.3, 262.3, t_start=0.1, max_order=6)
        self.assertIsNotNone(result)
        self.assertEqual(len(result.modes), 3, f"orders: {result.orders_db}")
        for f, d, amps in truth:
            m = min(result.modes, key=lambda m: abs(m.freq - f))
            self.assertAlmostEqual(m.freq, f, delta=0.01)
            self.assertAlmostEqual(m.decay, d, delta=0.05 * d)
            self.assertLess(abs(m.amps[0] - amps[0]) / abs(amps[0]), 0.05)
        self.assertLess(result.residual_db, -40.0)

    def test_resynthesis_matches_the_signal(self):
        truth = [(110.0, 0.8, [0.2, 0.15]), (110.3, 0.15, [0.03, 0.04j])]
        x = tone(truth, 8.0, noise_db=-110.0)
        result = analyze_partial(BandExtractor(x, FS), 1, 110.15, 110.0, t_start=0.1)
        y = synthesize([result], 2, x.shape[1], FS)
        err = np.max(np.abs(x[:, FS:] - y[:, FS:]))
        self.assertLess(20 * np.log10(err / 0.2), -60.0)


if __name__ == "__main__":
    unittest.main()
