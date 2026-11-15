"""The native variable projection (tools/forge/native) against the Python reference (modes._refine_python), on
partials made here: known modes, beating, two channels, noise. Skipped when the library is not built."""

import unittest

import numpy as np

from forge import native
from forge.modes import _refine_python

FS_D = 200.0  # a baseband rate of the analysis


def partial(modes, n=3000, noise=0.0, seed=0):
    """Channels x samples: sum of (decay 1/s, offset Hz, left amplitude, right amplitude)."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / FS_D
    z = np.zeros((2, n), dtype=np.complex128)
    for decay, offset, left, right in modes:
        carrier = np.exp((-decay + 2j * np.pi * offset) * t)
        z[0] += left * carrier
        z[1] += right * carrier
    z += noise * (rng.standard_normal(z.shape) + 1j * rng.standard_normal(z.shape))
    weight = np.exp(0.5 * 0.8 * t)  # flattens part of the decay, as partial_window does
    return z, weight


@unittest.skipUnless(native.available, "the forge's native library is not built")
class Native(unittest.TestCase):
    def check(self, modes, start_decay, start_offset, noise=0.0, tolerance=1e-3):
        z, weight = partial(modes, noise=noise)
        decay, offset, amps, res = native.vp_refine(z, weight, FS_D, np.array(start_decay), np.array(start_offset))
        ref = _refine_python(z, weight, FS_D, np.array(start_decay), np.array(start_offset))
        self.assertLessEqual(res, ref[3] * (1 + 1e-6) + 1e-12)  # at least as good a fit as the reference
        order, ref_order = np.argsort(offset), np.argsort(ref[1])
        np.testing.assert_allclose(offset[order], ref[1][ref_order], atol=tolerance)
        np.testing.assert_allclose(decay[order], ref[0][ref_order], rtol=tolerance, atol=tolerance)
        np.testing.assert_allclose(amps[:, order], ref[2][:, ref_order], rtol=1e-2, atol=1e-6)
        return decay[order], offset[order], amps[:, order], res

    def test_one_mode_exact(self):
        decay, offset, amps, res = self.check([(0.8, 0.3, 1.0 + 0.5j, 0.7 - 0.2j)], [0.5], [0.0])
        self.assertAlmostEqual(decay[0], 0.8, places=6)
        self.assertAlmostEqual(offset[0], 0.3, places=6)
        np.testing.assert_allclose(amps[:, 0], [1.0 + 0.5j, 0.7 - 0.2j], atol=1e-8)
        self.assertLess(res, 1e-20)

    def test_beating_unison(self):
        """Three strings 0.4 and 0.9 Hz apart, a fast and a slow decay (prompt sound, after-sound)."""
        modes = [(2.5, -0.4, 0.8, 0.6), (0.4, 0.0, 0.5j, 0.4), (1.2, 0.5, -0.3, 0.2j)]
        decay, offset, _, res = self.check(modes, [2.0, 0.5, 1.0], [-0.3, 0.05, 0.4])
        np.testing.assert_allclose(offset, [-0.4, 0.0, 0.5], atol=1e-6)
        np.testing.assert_allclose(decay, [2.5, 0.4, 1.2], rtol=1e-6)
        self.assertLess(res, 1e-20)

    def test_noise(self):
        modes = [(1.5, -0.2, 1.0, 0.8), (0.3, 0.35, 0.4, -0.5)]
        self.check(modes, [1.0, 0.5], [-0.1, 0.3], noise=0.01)

    def test_close_modes_ill_conditioned(self):
        """Two poles almost the same: the fit stays finite, as good as the reference's."""
        z, weight = partial([(0.5, 0.0, 1.0, 1.0)], noise=1e-4)
        out = native.vp_refine(z, weight, FS_D, np.array([0.5, 0.5001]), np.array([0.0, 0.0001]))
        ref = _refine_python(z, weight, FS_D, np.array([0.5, 0.5001]), np.array([0.0, 0.0001]))
        self.assertIsNotNone(out)
        self.assertLessEqual(out[3], ref[3] * 1.01 + 1e-12)


@unittest.skipUnless(native.available, "the forge's native library is not built")
class Resonators(unittest.TestCase):
    def test_against_lfilter(self):
        from forge.sympathetic import BANDS, _response_python, band_of, response

        rng = np.random.default_rng(3)
        fs = 11025.0
        x = rng.standard_normal(20000) * np.exp(-np.arange(20000) / 5000.0)
        k = 300
        res = np.column_stack([rng.uniform(30, 4000, k), rng.uniform(0.1, 20, k), rng.standard_normal((k, 4))])
        gains = rng.uniform(0, 2, len(BANDS) - 1)
        gains[3] = 0.0  # a band switched off
        extra = rng.uniform(0, 3, len(BANDS) - 1)
        bands = band_of(res[:, 0])
        expected = _response_python(x, fs, res, gains[bands], extra[bands])
        got = response(x, fs, res, gains, extra)
        np.testing.assert_allclose(got, expected, rtol=1e-9, atol=1e-12 * np.max(np.abs(expected)))
        np.testing.assert_allclose(response(x, fs, res), _response_python(x, fs, res, np.ones(k), np.zeros(k)),
                                   rtol=1e-9, atol=1e-12 * np.max(np.abs(expected)))


@unittest.skipUnless(native.available, "the forge's native library is not built")
class DampedSums(unittest.TestCase):
    def test_against_numpy(self):
        rng = np.random.default_rng(5)
        fs, k, length, start = 44100.0, 37, 300000, 1234  # 37 modes: four full blocks of eight, and five
        freqs, decays = rng.uniform(20, 15000, k), rng.uniform(0.05, 30, k)
        amps = rng.standard_normal((2, k)) + 1j * rng.standard_normal((2, k))
        t = (start + np.arange(length)) / fs
        expected = np.zeros((2, length))
        for j in range(k):
            carrier = np.exp((-decays[j] + 2j * np.pi * freqs[j]) * t)
            for c in range(2):
                expected[c] += np.real(amps[c, j] * carrier)
        got = native.damped_sum(freqs, decays, amps, length, fs, start)
        np.testing.assert_allclose(got, expected, rtol=0, atol=1e-10 * np.max(np.abs(expected)))

    def test_modes_and_body(self):
        from forge.modes import Mode, PartialModes, synthesize
        from forge.soundboard import synthesize_body

        partials = [PartialModes(1, 220.0, [Mode(220.1, 0.8, [1 + 1j, 0.5]), Mode(219.7, 2.0, [0.3j, -0.2])]),
                    PartialModes(2, 440.5, [Mode(440.6, 1.5, [0.2, 0.1 + 0.1j])])]
        n, fs = 50000, 44100.0
        t = np.arange(n) / fs
        expected = np.zeros((2, n))
        for p in partials:
            for m in p.modes:
                for c in range(2):
                    expected[c] += np.real(m.amps[c] * np.exp((-m.decay + 2j * np.pi * m.freq) * t))
        np.testing.assert_allclose(synthesize(partials, 2, n, fs), expected, atol=1e-12)
        body = synthesize_body(np.array([700.0, 1300.0]), np.array([20.0, 35.0]),
                               np.array([[1.0 + 0j, 0.5j], [0.3, -0.2 + 0.1j]]), n, fs)
        ref = np.zeros((2, n))
        for f, d, a in ((700.0, 20.0, (1.0, 0.3)), (1300.0, 35.0, (0.5j, -0.2 + 0.1j))):
            for c in range(2):
                ref[c] += np.real(a[c] * np.exp((-d + 2j * np.pi * f) * t))
        np.testing.assert_allclose(body, ref, atol=1e-12)


if __name__ == "__main__":
    unittest.main()
