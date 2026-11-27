# SPDX-License-Identifier: AGPL-3.0-or-later

"""Order of magnitude of the number of resonators of a full piano (worst case: sustain pedal down).

Rough assumptions, to be replaced by a calibrated profile:
- log-linear B: ~2e-4 at A0, minimum ~1e-4 around C3 (key 28), ~1e-2 at C8
- 1 string up to key 10, 2 strings up to key 26, 3 above; 2 polarizations per string
- ~8 floating-point operations per complex resonator and per sample, at 48 kHz
Usage: python -I resonator_budget.py
"""
import math

FS = 48000


def inharmonicity(k, k_min=28):
    lb = -3.7 - 0.3 * (k - 1) / (k_min - 1) if k <= k_min else -4.0 + 2.0 * (k - k_min) / (88 - k_min)
    return 10 ** lb


def strings(k):
    return 1 if k <= 10 else (2 if k <= 26 else 3)


for f_max in (10_000, 16_000):
    total, partials = 0, {}
    for k in range(1, 89):
        f0, b, n = 27.5 * 2 ** ((k - 1) / 12), inharmonicity(k), 0
        while (n + 1) * f0 * math.sqrt(1 + b * (n + 1) ** 2) <= f_max:
            n += 1
        partials[k] = n
        total += n * strings(k) * 2
    print(f"band <= {f_max} Hz: {total} resonators, ~{total * 8 * FS / 1e9:.1f} GFLOP/s"
          f" (partials: A0 {partials[1]}, C4 {partials[40]}, C8 {partials[88]}), state ~{total * 20 / 1e6:.2f} MB")
