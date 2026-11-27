# SPDX-License-Identifier: AGPL-3.0-or-later

"""Precision of a damped resonator in float32: direct form vs complex form.

Direct form:  y[n] = a1*y[n-1] - a2*y[n-2] + x[n],  a1 = 2r*cos(theta), a2 = r^2
Complex form: z[n] = p*z[n-1] + x[n],              p = r*e^(i*theta), output = Im(z)
Usage: python -I resonator_precision.py   (no dependency)
"""
import array
import math

FS = 48000.0
f32 = lambda x: array.array('f', [x])[0]  # IEEE float32 rounding
cents = lambda a, b: 1200 * math.log2(a / b)


def poles(f, t60):
    r = math.exp(-math.log(1000) / (t60 * FS))
    return r, 2 * math.pi * f / FS


print("1) Error caused by rounding the coefficients alone to float32")
print(f"{'f0 (Hz)':>8} {'T60':>4} | {'direct: cents':>13} {'Hz':>8} {'T60':>7} | {'complex: cents':>14} {'T60':>7}")
for f, t60 in [(27.5, 40), (55, 30), (110, 25), (261.6, 12), (1000, 5), (4186, 1)]:
    r, th = poles(f, t60)
    a1, a2 = f32(2 * r * math.cos(th)), f32(r * r)
    rd = math.sqrt(a2)
    fd = math.acos(a1 / (2 * rd)) * FS / (2 * math.pi)
    td = -math.log(1000) / (FS * math.log(rd))
    c, s = f32(r * math.cos(th)), f32(r * math.sin(th))
    fc = math.atan2(s, c) * FS / (2 * math.pi)
    tc = -math.log(1000) / (FS * math.log(math.hypot(c, s)))
    print(f"{f:8.1f} {t60:4.0f} | {cents(fd, f):+13.3f} {fd - f:+8.4f} {100 * (td / t60 - 1):+6.2f}% |"
          f" {cents(fc, f):+14.5f} {100 * (tc / t60 - 1):+6.2f}%")

print("\n2) 5 s simulation of an A0 (27.5 Hz, T60 = 40 s), states and arithmetic rounded to float32")
f, t60, n_samples = 27.5, 40.0, int(FS * 5)
r, th = poles(f, t60)
ref = [r ** (n + 1) * math.sin(th * (n + 1)) for n in range(n_samples)]  # Im(p^(n+1)), in double
peak = max(abs(v) for v in ref)
# direct form, same impulse response up to a factor: y[n] = r^n sin(theta(n+1)) / sin(theta)
a1, a2 = f32(2 * r * math.cos(th)), f32(r * r)
y1 = y2 = 0.0
x, err_d, g = 1.0, 0.0, r * math.sin(th)
for n in range(n_samples):
    y = f32(f32(a1 * y1) - f32(a2 * y2) + x)
    x = 0.0
    y2, y1 = y1, y
    err_d = max(err_d, abs(y * g - ref[n]))
# complex form: z starts at p, then z <- z*p
c, s = f32(r * math.cos(th)), f32(r * math.sin(th))
zr, zi = c, s
err_c = abs(zi - ref[0])
for n in range(1, n_samples):
    zr, zi = f32(f32(zr * c) - f32(zi * s)), f32(f32(zr * s) + f32(zi * c))
    err_c = max(err_c, abs(zi - ref[n]))
print(f"   largest deviation from the exact signal: direct = {20 * math.log10(err_d / peak):.1f} dB below the peak,"
      f" complex = {20 * math.log10(err_c / peak):.1f} dB")
