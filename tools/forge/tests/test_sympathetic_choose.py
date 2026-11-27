# SPDX-License-Identifier: AGPL-3.0-or-later

"""The fast scoring of the drive shapes' combinations (sympathetic._Tables) against assess() itself, on measures
made here."""

import unittest

import numpy as np

from forge import sympathetic as s


def measures(seed=0, notes=24, configs=4):
    rng = np.random.default_rng(seed)
    out = []
    nb, nw = len(s.BANDS) - 1, len(s.WINDOWS_S)
    for note in rng.choice(np.arange(21, 109), notes, replace=False):
        base = rng.uniform(1e-8, 1e-4, (nw, nb))
        for velocity in (40, 100):
            library = base * (velocity / 64) ** 1.4 * rng.lognormal(0, 0.5, (nw, nb))
            library[rng.random((nw, nb)) < 0.1] = 0.0  # unmeasured
            model = base[None] * rng.lognormal(0, 0.7, (configs, nw, nb)) * rng.uniform(0.5, 2.0)
            model[:, rng.random((nw, nb)) < 0.05] = 0.0
            out.append({"take": f"{note}-{velocity}", "note": int(note), "velocity": velocity, "model": model,
                        "library": library})
    return out


class Choose(unittest.TestCase):
    def test_scores_are_assess(self):
        ms = measures()
        tables = s._Tables(ms)
        rng = np.random.default_rng(1)
        for _ in range(30):
            combo = list(rng.integers(0, 4, len(s.REGISTERS)))
            r = s.assess(ms, combo)
            p90, median = tables.score(combo)
            self.assertAlmostEqual(p90, r["p90"], places=9)
            self.assertAlmostEqual(median, r["median"], places=9)

    def test_choose_is_the_exhaustive_search(self):
        import itertools

        ms = measures(seed=4, notes=16, configs=3)
        shapes = [(0.1, 0.0), (0.3, 0.0), (0.5, 2.0)]
        combo, r = s.choose(ms, shapes)
        best = min(itertools.product(range(3), repeat=len(s.REGISTERS)),
                   key=lambda c: (s.assess(ms, list(c))["p90"], s.assess(ms, list(c))["median"]))
        self.assertEqual(combo, list(best))
        self.assertEqual(r["p90"], s.assess(ms, list(best))["p90"])


if __name__ == "__main__":
    unittest.main()
