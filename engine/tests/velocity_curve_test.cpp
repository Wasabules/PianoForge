// SPDX-License-Identifier: AGPL-3.0-or-later

// Velocity curve tests (no framework): exits with a non-zero code on the first failure.
#include <pianoforge/velocity_curve.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

bool monotone(const pf::VelocityCurve& c)
{
    for (int v = 1; v < 127; ++v)
        if (c.map(v + 1) < c.map(v))
            return false;
    return true;
}

} // namespace

int main()
{
    pf::VelocityCurve curve;

    bool identity = true;
    for (int v = 0; v < 128; ++v)
        identity = identity && curve.map(v) == v;
    check(identity, "default settings: the curve is the identity");

    pf::VelocityCurve::Settings b2;
    b2.inputMax = 114; // measured ceiling of the Korg B2
    curve.set(b2);
    check(curve.map(114) == 127 && curve.map(120) == 127, "input max 114: 114 and above map to 127");
    check(curve.map(1) == 1 && curve.map(0) == 0, "velocity 1 stays 1, and 0 (note-off) stays 0");
    check(curve.map(57) > 57 && monotone(curve), "input max 114: middle velocities are raised, monotone");

    curve.set(pf::VelocityCurve::shaped({}, 0.6));
    check(curve.map(64) > 64 && monotone(curve), "soft shape (x^0.6): louder for a light touch, monotone");
    curve.set(pf::VelocityCurve::shaped({}, 1.6));
    check(curve.map(64) < 64 && monotone(curve), "hard shape (x^1.6): softer for a light touch, monotone");

    pf::VelocityCurve::Settings zigzag;
    zigzag.points = { 0.0f, 0.8f, 0.2f, 0.9f, 0.5f }; // decreasing points must not make the curve decrease
    curve.set(zigzag);
    check(monotone(curve), "points out of order: the curve stays monotone");

    pf::VelocityCurve::Settings flat;
    flat.points = { 0.3f, 0.3f, 0.3f, 0.3f, 0.3f };
    curve.set(flat);
    check(curve.map(1) == curve.map(127), "flat points: constant output (fixed velocity)");

    pf::VelocityCurve::Settings broken;
    broken.inputMin = 120;
    broken.inputMax = 20; // inverted range: clamped, never a division by zero
    curve.set(broken);
    check(monotone(curve) && curve.map(127) >= 1, "inverted input range: handled without failure");

    std::printf("%s\n", failures == 0 ? "RESULT: every check passes" : "RESULT: at least one check fails");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
