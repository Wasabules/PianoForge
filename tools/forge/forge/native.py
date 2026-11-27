# SPDX-License-Identifier: AGPL-3.0-or-later

"""The forge's native kernels (tools/forge/native, C++), used when built; the Python code otherwise.

The library is looked for in forge/_native/ (where the build copies it) or at PIANOFORGE_FORGE_NATIVE_LIB.
PIANOFORGE_FORGE_NATIVE=0 turns it off (the Python reference, for comparisons).
"""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path

import numpy as np

VERSION = 5  # 2: the resonator bank; 3: sums of damped sinusoids; 4-5: the three-string unison
_FILES = {"win32": "pianoforge_forge_native.dll", "darwin": "libpianoforge_forge_native.dylib"}


def _load():
    if os.environ.get("PIANOFORGE_FORGE_NATIVE", "1") == "0":
        return None
    name = _FILES.get(sys.platform, "libpianoforge_forge_native.so")
    for candidate in (os.environ.get("PIANOFORGE_FORGE_NATIVE_LIB"), str(Path(__file__).parent / "_native" / name)):
        if not candidate or not Path(candidate).exists():
            continue
        try:
            lib = ctypes.CDLL(candidate)
        except OSError:
            continue
        lib.pf_native_version.restype = ctypes.c_int
        if lib.pf_native_version() != VERSION:
            continue
        p = ctypes.POINTER(ctypes.c_double)
        lib.pf_vp_refine.restype = ctypes.c_double
        lib.pf_vp_refine.argtypes = [p, ctypes.c_int, ctypes.c_int, p, ctypes.c_double, ctypes.c_int, p,
                                     ctypes.c_int, p, p]
        lib.pf_resonator_bank.restype = None
        lib.pf_resonator_bank.argtypes = [p, ctypes.c_int, ctypes.c_double, p, ctypes.c_int, p, p, p]
        lib.pf_unison3_fits.restype = None
        lib.pf_unison3_fits.argtypes = [p, ctypes.c_double, p, p]
        lib.pf_damped_sum.restype = None
        lib.pf_damped_sum.argtypes = [p, p, p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_double,
                                      ctypes.c_longlong, p]
        return lib
    return None


_LIB = _load()
available = _LIB is not None


def _ptr(a: np.ndarray):
    return a.ctypes.data_as(ctypes.POINTER(ctypes.c_double))


def vp_refine(seg: np.ndarray, weight: np.ndarray, fs_d: float, decay: np.ndarray, offset: np.ndarray,
              max_nfev: int = 200):
    """modes._refine, native: (decay, offset, amplitudes at the window start (channels x modes), weighted relative
    residual), or None when nothing finite came out (the caller falls back on the Python version)."""
    seg = np.ascontiguousarray(seg, dtype=np.complex128)
    weight = np.ascontiguousarray(weight, dtype=np.float64)
    channels, n = seg.shape
    k = len(decay)
    theta0 = np.ascontiguousarray(np.concatenate([np.log(np.maximum(decay, 1e-5)), offset]), dtype=np.float64)
    theta = np.empty(2 * k)
    amps = np.empty((channels, k), dtype=np.complex128)
    res = _LIB.pf_vp_refine(_ptr(seg.view(np.float64)), channels, n, _ptr(weight), float(fs_d), k, _ptr(theta0),
                            int(max_nfev), _ptr(theta), _ptr(amps.view(np.float64)))
    if not np.isfinite(res) or res < 0 or not np.all(np.isfinite(theta)) or not np.all(np.isfinite(amps)):
        return None
    return np.exp(np.clip(theta[:k], -12.0, 8.0)), theta[k:].copy(), amps, float(res)


def resonator_bank(x: np.ndarray, fs: float, res: np.ndarray, gains: np.ndarray | None = None,
                   extras: np.ndarray | None = None) -> np.ndarray:
    """sympathetic.response, native: the stereo response (2 x samples) of the resonators `res` (k x 6: freq, decay,
    wl re, wl im, wr re, wr im) to the mono drive x, each with its gain (per second) and extra decay (1/s)."""
    x = np.ascontiguousarray(x, dtype=np.float64)
    res = np.ascontiguousarray(res, dtype=np.float64).reshape(-1, 6)
    k = res.shape[0]
    g = None if gains is None else np.ascontiguousarray(gains, dtype=np.float64)
    e = None if extras is None else np.ascontiguousarray(extras, dtype=np.float64)
    y = np.empty((2, len(x)))
    _LIB.pf_resonator_bank(_ptr(x), len(x), float(fs), _ptr(res), k, None if g is None else _ptr(g),
                           None if e is None else _ptr(e), _ptr(y))
    return y


def damped_sum(freqs: np.ndarray, decays: np.ndarray, amps: np.ndarray, length: int, fs: float,
               start: int = 0) -> np.ndarray:
    """The sum of damped sinusoids Re(a_ck exp((-decay_k + 2 pi i freq_k) t)), t = (start + n) / fs, per channel
    (amps: channels x k complex): modes.synthesize and soundboard.synthesize_body, native (channels x length)."""
    freqs = np.ascontiguousarray(freqs, dtype=np.float64).ravel()
    decays = np.ascontiguousarray(decays, dtype=np.float64).ravel()
    amps = np.ascontiguousarray(np.atleast_2d(amps), dtype=np.complex128)
    channels, k = amps.shape
    out = np.empty((channels, int(length)))
    if k == 0 or length <= 0:
        out[:] = 0.0
        return out
    _LIB.pf_damped_sum(_ptr(freqs), _ptr(decays), _ptr(amps.view(np.float64)), k, channels, int(length), float(fs),
                       int(start), _ptr(out))
    return out


def unison3(rel: np.ndarray, scale: float) -> list[tuple[float, np.ndarray]]:
    """unison.py's three-string least squares, native: (cost, y) from each of the six starting points that gave a
    finite fit."""
    rel = np.ascontiguousarray(rel, dtype=np.complex128)
    y = np.zeros((6, 2))
    cost = np.zeros(6)
    _LIB.pf_unison3_fits(_ptr(rel.view(np.float64)), float(scale), _ptr(y), _ptr(cost))
    return [(float(c), y[i].copy()) for i, c in enumerate(cost) if np.isfinite(c)]
