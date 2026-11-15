// Modal engine tests on a synthetic profile (no corpus needed): exits with a non-zero code on the first failure.
#include <pianoforge/modal_engine.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

/// One partial of two modes (a beating unison) per layer; the loud layer is 4 times (12 dB) louder.
pf::ModalProfile::Layer makeLayer(int velocity, float amplitude)
{
    pf::ModalProfile::Layer l;
    l.velocity = velocity;
    l.modes.push_back({ 440.0, 1.0, { amplitude, 0.0f }, { amplitude, 0.0f } });
    l.modes.push_back({ 440.7, 3.0, { 0.5f * amplitude, 0.0f }, { 0.5f * amplitude, 0.0f } });
    l.partials.push_back({ 1, 0, 2, 0.0f });
    l.bodyLeft = { { 0.01f * amplitude, 0.0f } };
    l.bodyRight = { { 0.01f * amplitude, 0.0f } };
    l.bodyLevel = 0.01f * amplitude;
    l.residualFrames = 0;
    // Level as the loader computes it (RMS over the first second, cross terms ignored).
    double e = 0.0;
    for (const auto& m : l.modes) {
        const double x = 2.0 * m.decay;
        e += 0.5 * std::norm(m.left) * (1.0 - std::exp(-x)) / x;
    }
    l.partials[0].level = (float) std::sqrt(e);
    l.level = l.partials[0].level;
    return l;
}

std::shared_ptr<const pf::ModalProfile> makeProfile()
{
    auto p = std::make_shared<pf::ModalProfile>();
    p->residualRate = kRate;
    p->bodyFreq = { 150.0 };
    p->bodyDecay = { 4.0 };
    p->noteIndex.fill(-1);
    pf::ModalProfile::Note n;
    n.midi = 69;
    n.f0 = 440.0;
    n.layers.push_back(makeLayer(40, 0.05f));
    n.layers.push_back(makeLayer(100, 0.2f));
    p->notes.push_back(std::move(n));
    p->noteIndex[69] = 0;
    p->maxModesPerLayer = 2;
    // Damper: -60 dB within 0.2 s whatever the hold, measured for one note.
    p->dampers.stepSeconds = 0.01;
    p->dampers.notes = { 69 };
    p->dampers.holds = { 1.0 };
    p->dampers.logHolds = { 0.0 };
    p->dampers.length = 21;
    for (int k = 0; k < 21; ++k)
        p->dampers.db.push_back(-3.0f * (float) k);
    return p;
}

/// Two partials of one mode each, whose levels grow differently with the velocity, and an attack residual.
constexpr double kRampF[2] = { 300.0, 1500.0 }, kRampDecay[2] = { 2.0, 6.0 };
constexpr int kResidualFrames = 4800; // 0.1 s at kRate

double residualSample(int i)
{
    return 0.02 * std::sin(kTwoPi * 2500.0 * i / kRate) * (1.0 - (double) i / kResidualFrames);
}

pf::ModalProfile::Layer makeRampLayer(int velocity, float a1, float a2)
{
    pf::ModalProfile::Layer l;
    l.velocity = velocity;
    const float amps[2] = { a1, a2 };
    double total = 0.0;
    for (int p = 0; p < 2; ++p) {
        l.modes.push_back({ kRampF[p], kRampDecay[p], { amps[p], 0.0f }, { amps[p], 0.0f } });
        const double x = 2.0 * kRampDecay[p];
        const auto level = (float) std::sqrt(0.5 * amps[p] * amps[p] * (1.0 - std::exp(-x)) / x);
        l.partials.push_back({ p + 1, p, 1, level });
        total += (double) level * level;
    }
    l.level = (float) std::sqrt(total);
    for (int i = 0; i < kResidualFrames; ++i) {
        l.residual.push_back((float) residualSample(i));
        l.residual.push_back((float) residualSample(i));
    }
    l.residualFrames = kResidualFrames;
    return l;
}

std::shared_ptr<const pf::ModalProfile> makeRampProfile()
{
    auto p = std::make_shared<pf::ModalProfile>();
    p->residualRate = kRate;
    p->noteIndex.fill(-1);
    pf::ModalProfile::Note n;
    n.midi = 72;
    n.layers.push_back(makeRampLayer(40, 0.05f, 0.002f));
    n.layers.push_back(makeRampLayer(100, 0.2f, 0.05f));
    p->notes.push_back(std::move(n));
    p->noteIndex[72] = 0;
    p->maxModesPerLayer = 2;
    p->maxPartialsPerLayer = 2;
    return p;
}

double rms(const std::vector<float>& x, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i)
        s += (double) x[(std::size_t) i] * x[(std::size_t) i];
    return std::sqrt(s / std::max(1, to - from));
}

/// Renders `frames` samples with the given events (offsets in absolute samples), in blocks of 64.
void render(pf::ModalEngine& e, std::vector<pf::Event> events, int frames, std::vector<float>& l, std::vector<float>& r)
{
    l.assign((std::size_t) frames, 0.0f);
    r.assign((std::size_t) frames, 0.0f);
    std::size_t next = 0;
    std::vector<pf::Event> local;
    for (int s = 0; s < frames; s += 64) {
        const int n = std::min(64, frames - s);
        local.clear();
        while (next < events.size() && (int) events[next].offset < s + n) {
            auto ev = events[next++];
            ev.offset -= (std::uint32_t) s;
            local.push_back(ev);
        }
        float* out[2] = { l.data() + s, r.data() + s };
        e.process(out, 2, n, local.data(), (int) local.size());
    }
}

} // namespace

int main()
{
    const auto profile = makeProfile();
    const auto ramped = makeRampProfile();
    std::vector<float> l, r;

    // 1. At a layer's velocity, the output is exactly that layer's modes (plus the body), from the note-on sample.
    {
        pf::ModalEngine e;
        e.install(profile);
        e.bodyEnabled = false;
        e.prepare(kRate, 64);
        const int on = 100, frames = 24000;
        render(e, { { (std::uint32_t) on, 0x90, 69, 100 } }, frames, l, r);
        double err = 0.0, peak = 0.0;
        for (int s = 0; s < frames; ++s) {
            const double t = (double) (s - on) / kRate;
            const double want = s < on ? 0.0
                                       : 0.2 * std::exp(-1.0 * t) * std::cos(kTwoPi * 440.0 * t)
                                             + 0.1 * std::exp(-3.0 * t) * std::cos(kTwoPi * 440.7 * t);
            err = std::max(err, std::abs(l[(std::size_t) s] - want));
            peak = std::max(peak, std::abs(want));
        }
        std::printf("      largest error %.1f dB under the peak\n", 20.0 * std::log10(err / peak));
        check(err < 1e-3 * peak, "a layer's velocity plays its modes exactly (-60 dB), from the note-on sample");
        check(e.lastLayerVelocity.load() == 100 && e.activeVoices.load() == 1, "the nearest layer is reported");
    }

    // 2. Between two layers, blendLayers off: the level is interpolated (log), the nearest layer is played.
    {
        auto level = [&](int velocity) {
            pf::ModalEngine e;
            e.install(profile);
            e.bodyEnabled = false;
            e.blendLayers = false;
            e.prepare(kRate, 64);
            render(e, { { 0, 0x90, 69, (std::uint8_t) velocity } }, 4800, l, r);
            return std::make_pair(rms(l, 0, 4800), e.lastLayerVelocity.load());
        };
        const auto [l40, n40] = level(40);
        const auto [l70, n70] = level(70);
        const auto [l85, n85] = level(85);
        const auto [l100, n100] = level(100);
        const double gap = 20.0 * std::log10(l100 / l40);
        std::printf("      levels: v40 %.1f dB, v70 %.1f dB, v85 %.1f dB, v100 %.1f dB\n", 20 * std::log10(l40),
                    20 * std::log10(l70), 20 * std::log10(l85), 20 * std::log10(l100));
        check(std::abs(20.0 * std::log10(l70 / l40) - 0.5 * gap) < 0.3, "velocity 70 sits halfway in dB between the layers 40 and 100");
        check(l40 < l70 && l70 < l85 && l85 < l100, "the level grows with the velocity");
        check(n70 == 40 && n85 == 100, "the nearest layer is played (70 -> 40 at a tie, 85 -> 100)");
        const auto [l20, n20] = level(20);
        check(l20 < l40 && n20 == 40, "below the softest layer, the level keeps falling");
    }

    // 2b. Between two layers, with partials that do not grow alike: the attack is the nearest layer's strike at the
    // overall gain (modes and residual together, as measured), then each partial reaches its own level.
    {
        const auto ramp = makeRampProfile();
        const auto& near = ramp->notes[0].layers[1];
        const auto& other = ramp->notes[0].layers[0];
        const double alpha = (85.0 - 40.0) / 60.0; // velocity 85, nearest layer 100
        auto interp = [&](double a, double b) { return std::exp((1.0 - alpha) * std::log(a) + alpha * std::log(b)); };
        const double overall = interp(other.level, near.level) / near.level;
        double g[2];
        for (int p = 0; p < 2; ++p)
            g[p] = interp(other.partials[(std::size_t) p].level, near.partials[(std::size_t) p].level)
                   / near.partials[(std::size_t) p].level;
        const auto rampLength = (int) std::llround(pf::ModalEngine::kLevelRampSeconds * kRate);

        pf::ModalEngine e;
        e.install(ramp);
        e.bodyEnabled = false;
        e.blendLayers = false;
        e.prepare(kRate, 64);
        const int frames = 24000;
        render(e, { { 0, 0x90, 72, 85 } }, frames, l, r);
        double errAttack = 0.0, errRamp = 0.0, errAfter = 0.0, peak = 0.0;
        for (int s = 0; s < frames; ++s) {
            const double t = s / kRate;
            double want = s < kResidualFrames ? overall * residualSample(s) : 0.0;
            for (int p = 0; p < 2; ++p) {
                // log-linear from the overall gain to the partial's own over the ramp
                const double x = std::clamp((double) (s - kResidualFrames) / rampLength, 0.0, 1.0);
                const double gain = overall * std::pow(g[p] / overall, x);
                want += gain * near.modes[(std::size_t) p].left.real() * std::exp(-kRampDecay[p] * t)
                        * std::cos(kTwoPi * kRampF[p] * t);
            }
            double& slot = s < kResidualFrames ? errAttack : s < kResidualFrames + rampLength ? errRamp : errAfter;
            slot = std::max(slot, (double) std::abs(l[(std::size_t) s] - want));
            peak = std::max(peak, std::abs(want));
        }
        std::printf("      gains: overall %.3f, partial 1 %.3f, partial 2 %.3f; errors under the peak: attack %.1f dB, "
                    "ramp %.1f dB, after %.1f dB\n",
                    overall, g[0], g[1], 20 * std::log10(errAttack / peak + 1e-12), 20 * std::log10(errRamp / peak + 1e-12),
                    20 * std::log10(errAfter / peak + 1e-12));
        check(std::abs(g[1] / g[0] - 1.0) > 0.2, "(the two partials do need different gains)");
        check(errAttack < 1e-3 * peak, "attack: the nearest layer's modes and residual, all at the overall gain");
        check(errRamp < 1e-3 * peak, "then a smooth log-linear ramp to each partial's level");
        check(errAfter < 1e-3 * peak, "after the ramp: each partial at its interpolated level, exact poles back");

        // 2c. A partial with a fade time (set by the forge past its analysis window) dies out from there: an extra
        // -60 dB per kPartialFadeSeconds, the other partial untouched.
        auto faded = std::make_shared<pf::ModalProfile>(*ramp);
        for (auto& layer : faded->notes[0].layers)
            layer.partials[1].fadeSeconds = 0.25f;
        pf::ModalEngine f;
        f.install(faded);
        f.bodyEnabled = false;
        f.blendLayers = false;
        f.prepare(kRate, 64);
        std::vector<float> lf, rf;
        render(f, { { 0, 0x90, 72, 85 } }, frames, lf, rf);
        const int fadeAt = (int) std::llround(0.25 * kRate);
        double errFade = 0.0;
        for (int s = 0; s < frames; ++s) {
            const double t = s / kRate;
            double want = s < kResidualFrames ? overall * residualSample(s) : 0.0;
            for (int p = 0; p < 2; ++p) {
                const double x = std::clamp((double) (s - kResidualFrames) / rampLength, 0.0, 1.0);
                double gain = overall * std::pow(g[p] / overall, x);
                if (p == 1 && s > fadeAt)
                    gain *= std::pow(1000.0, -(s - fadeAt) / (pf::ModalEngine::kPartialFadeSeconds * kRate));
                want += gain * near.modes[(std::size_t) p].left.real() * std::exp(-kRampDecay[p] * t)
                        * std::cos(kTwoPi * kRampF[p] * t);
            }
            errFade = std::max(errFade, (double) std::abs(lf[(std::size_t) s] - want));
        }
        std::printf("      fade: largest error %.1f dB under the peak\n", 20 * std::log10(errFade / peak + 1e-12));
        check(errFade < 1e-3 * peak, "a partial's fade time: it dies out from there, the others ring on");

        // 2d. Blended (the default): over the residual, the nearer layer's whole strike at one gain, which brings the
        // attack's energy (the profile's measure: its first attackSeconds) to the one interpolated in dB between
        // the two layers' attacks; then the same log-linear ramp to each partial's level (here, one mode per partial
        // and the same poles in both layers: the levels of 2b). The softer layer's residual is 4 times louder (as in
        // the bass of the reference, where it is most of a soft attack): its attack does not grow as its partials.
        constexpr double kSofterResidual = 4.0;
        auto uneven = std::make_shared<pf::ModalProfile>(*ramp);
        for (auto& x : uneven->notes[0].layers[0].residual)
            x = (float) (x * kSofterResidual);
        pf::measureLayers(*uneven); // the layers' measures, as the forge exports them
        const int attackFrames = (int) std::lround(uneven->attackSeconds * kRate);
        auto attackEnergy = [&](const pf::ModalProfile::Layer& layer, double residualScale) {
            double e = 0.0;
            for (int s = 0; s < attackFrames; ++s) {
                double x = residualScale * residualSample(s);
                for (const auto& m : layer.modes)
                    x += m.left.real() * std::exp(-m.decay * s / kRate) * std::cos(kTwoPi * m.freq * s / kRate);
                e += x * x;
            }
            return e;
        };
        const double attackGain = std::sqrt(interp(attackEnergy(other, kSofterResidual), attackEnergy(near, 1.0)) / attackEnergy(near, 1.0));
        pf::ModalEngine b;
        b.install(uneven);
        b.bodyEnabled = false;
        b.prepare(kRate, 64);
        std::vector<float> lb, rb;
        render(b, { { 0, 0x90, 72, 85 } }, frames, lb, rb);
        double errBlendAttack = 0.0, errBlendRamp = 0.0, errBlendAfter = 0.0;
        for (int s = 0; s < frames; ++s) {
            const double t = s / kRate;
            double want = s < kResidualFrames ? attackGain * residualSample(s) : 0.0;
            for (int p = 0; p < 2; ++p) {
                const double x = std::clamp((double) (s - kResidualFrames) / rampLength, 0.0, 1.0);
                const double gain = attackGain * std::pow(g[p] / attackGain, x);
                want += gain * near.modes[(std::size_t) p].left.real() * std::exp(-kRampDecay[p] * t)
                        * std::cos(kTwoPi * kRampF[p] * t);
            }
            double& slot = s < kResidualFrames ? errBlendAttack : s < kResidualFrames + rampLength ? errBlendRamp : errBlendAfter;
            slot = std::max(slot, (double) std::abs(lb[(std::size_t) s] - want));
        }
        std::printf("      blended at velocity 85: attack gain %.3f (overall %.3f); errors under the peak: attack %.1f dB, "
                    "ramp %.1f dB, after %.1f dB\n",
                    attackGain, overall, 20 * std::log10(errBlendAttack / peak + 1e-12),
                    20 * std::log10(errBlendRamp / peak + 1e-12), 20 * std::log10(errBlendAfter / peak + 1e-12));
        check(std::abs(attackGain / overall - 1.0) > 0.05, "(the attack's energy and the partials' levels do grow differently)");
        check(errBlendAttack < 1e-3 * peak, "blended: the attack is the nearer layer's whole strike at the attack's gain");
        check(errBlendRamp < 1e-3 * peak && errBlendAfter < 1e-3 * peak, "then the ramp to each partial's interpolated level");

        // Continuity: from one velocity to the next, the attack's energy (blended) follows the velocity without a
        // step; at the overall gain alone (blendLayers off), it steps where the nearest layer changes (70 -> 71):
        // there, the residual and the partials do not grow alike.
        auto attackLevel = [&](int velocity, bool blended) {
            pf::ModalEngine a;
            a.install(uneven);
            a.bodyEnabled = false;
            a.blendLayers = blended;
            a.prepare(kRate, 64);
            std::vector<float> x, y;
            render(a, { { 0, 0x90, 72, (std::uint8_t) velocity } }, attackFrames, x, y);
            return 20.0 * std::log10(rms(x, 0, attackFrames));
        };
        double worstBlend = 0.0, worstNearest = 0.0;
        for (int v = 41; v < 100; ++v) {
            worstBlend = std::max(worstBlend, std::abs(attackLevel(v + 1, true) - attackLevel(v, true)));
            worstNearest = std::max(worstNearest, std::abs(attackLevel(v + 1, false) - attackLevel(v, false)));
        }
        const double perStep = std::abs(10.0 * std::log10(attackEnergy(near, 1.0) / attackEnergy(other, kSofterResidual))) / 60.0;
        std::printf("      attack level from one velocity to the next: largest step %.2f dB blended (%.2f dB per velocity "
                    "step between the layers), %.2f dB at the overall gain alone\n", worstBlend, perStep, worstNearest);
        check(worstBlend < perStep + 0.01 && worstNearest > 3.0 * perStep, "blended: the attack's level follows the velocity without a step");

        // 2e. Blended, with layers whose partials differ: once the ramp is over, a shared partial's mode takes the
        // frequency and decay moved toward the other layer's (frequency linear, decay in log; toward the upper layer
        // at velocity 85, a quarter of the way to the lower one), and a partial only the other layer has comes in
        // over the ramp, from -60 dB, at its interpolated level and its weight (1 - alpha, the farther layer's).
        auto apart = std::make_shared<pf::ModalProfile>(*ramp);
        {
            auto& low = apart->notes[0].layers[0];
            low.modes[1].freq = 1510.0;
            low.modes[1].decay = 9.0;
            low.modes.push_back({ 2200.0, 3.0, { 0.004f, 0.0f }, { 0.004f, 0.0f } });
            double total = 0.0;
            for (std::size_t p = 0; p < low.modes.size(); ++p) {
                const auto& m = low.modes[p];
                const double x = 2.0 * m.decay;
                const auto level = (float) std::sqrt(0.5 * std::norm(m.left) * (1.0 - std::exp(-x)) / x);
                if (p < low.partials.size())
                    low.partials[p].level = level;
                else
                    low.partials.push_back({ (int) p + 1, (int) p, 1, level });
                total += (double) level * level;
            }
            low.level = (float) std::sqrt(total);
            apart->maxModesPerLayer = 3;
            apart->maxPartialsPerLayer = 3;
            pf::measureLayers(*apart);
        }
        pf::ModalEngine m;
        m.install(apart);
        m.bodyEnabled = false;
        m.prepare(kRate, 64);
        std::vector<float> lm, rm;
        render(m, { { 0, 0x90, 72, 85 } }, frames, lm, rm);
        // A partial's complex amplitude around `at` (s): demodulated at `freq` through a Hann window of 50 ms.
        auto partialAt = [&](double freq, double at) {
            const int half = 1200, centre = (int) std::lround(at * kRate);
            std::complex<double> c;
            double w = 0.0;
            for (int s = centre - half; s < centre + half; ++s) {
                const double hann = 0.5 - 0.5 * std::cos(kTwoPi * (s - centre + half) / (2 * half));
                c += hann * (double) lm[(std::size_t) s] * std::polar(1.0, -kTwoPi * freq * s / kRate);
                w += hann;
            }
            return 2.0 * c / w;
        };
        const auto& low = apart->notes[0].layers[0];
        const double toward = 1.0 - alpha;
        const double wantFreq = 1500.0 + toward * (1510.0 - 1500.0), wantDecay = 6.0 * std::pow(9.0 / 6.0, toward);
        const auto c1 = partialAt(wantFreq, 0.3), c2 = partialAt(wantFreq, 0.45);
        const double gotDecay = std::log(std::abs(c1) / std::abs(c2)) / 0.15;
        const double gotFreq = wantFreq + std::arg(c2 / c1) / (kTwoPi * 0.15);
        const double otherGain = interp(low.level, near.level) / low.level * (1.0 - alpha);
        const double wantThird = 0.004 * otherGain * std::exp(-3.0 * 0.3);
        const double third = std::abs(partialAt(2200.0, 0.3)), thirdEarly = std::abs(partialAt(2200.0, 0.03));
        std::printf("      moved poles: %.3f Hz %.3f /s (want %.3f Hz %.3f /s); the other layer's partial %.2f dB against "
                    "its level, %.1f dB at the note-on\n", gotFreq, gotDecay, wantFreq, wantDecay,
                    20.0 * std::log10(third / wantThird), 20.0 * std::log10(thirdEarly / (0.004 * otherGain)));
        check(std::abs(gotFreq - wantFreq) < 0.05 && std::abs(gotDecay - wantDecay) < 0.03,
              "blended: after the ramp, a mode's frequency and decay move toward the other layer's");
        check(std::abs(20.0 * std::log10(third / wantThird)) < 0.2 && thirdEarly < 0.01 * 0.004 * otherGain,
              "blended: a partial only the other layer has comes in over the ramp, from silence, at its level");
    }

    // 2f. A partial whose two strong modes nearly cancel each other in one layer (as the forge fits some unisons):
    // its level as it sounds, far under the sum of its modes' energies, is what is interpolated; on both sides of the
    // boundary between the layers, the same sustain (blendLayers off: the nearest layer's poles, unmoved).
    {
        auto p = std::make_shared<pf::ModalProfile>();
        p->residualRate = kRate;
        p->noteIndex.fill(-1);
        pf::ModalProfile::Note n;
        n.midi = 60;
        auto layer = [](int velocity, std::vector<pf::ModalProfile::Mode> modes) {
            pf::ModalProfile::Layer l;
            l.velocity = velocity;
            double e = 0.0; // the loader's level: the modes' energies over the first second
            for (const auto& m : modes)
                e += 0.5 * std::norm(m.left) * (1.0 - std::exp(-2.0 * m.decay)) / (2.0 * m.decay);
            l.partials.push_back({ 1, 0, (int) modes.size(), (float) std::sqrt(e) });
            l.level = l.partials[0].level;
            l.modes = std::move(modes);
            return l;
        };
        n.layers.push_back(layer(40, { { 440.0, 1.0, { 0.05f, 0.0f }, { 0.05f, 0.0f } } }));
        n.layers.push_back(layer(100, { { 440.0, 1.0, { 0.2f, 0.0f }, { 0.2f, 0.0f } },
                                        { 440.05, 1.0, { -0.19f, 0.0f }, { -0.19f, 0.0f } } }));
        p->notes.push_back(std::move(n));
        p->noteIndex[60] = 0;
        p->maxModesPerLayer = 2;
        p->maxPartialsPerLayer = 1;
        pf::measureLayers(*p);
        const int from = (int) std::lround(p->sustainFrom * kRate);
        const int to = (int) std::lround(p->sustainTo * kRate);
        auto sustain = [&](int velocity, bool blended) {
            pf::ModalEngine e;
            e.install(p);
            e.bodyEnabled = false;
            e.blendLayers = blended;
            e.prepare(kRate, 64);
            render(e, { { 0, 0x90, 60, (std::uint8_t) velocity } }, to, l, r);
            return 20.0 * std::log10(rms(l, from, to));
        };
        const double s40 = sustain(40, false), s70 = sustain(70, false), s71 = sustain(71, false), s100 = sustain(100, false);
        const auto& layers = p->notes[0].layers;
        std::printf("      cancelling modes: sustain v40 %.1f dB, v70 %.1f dB, v71 %.1f dB, v100 %.1f dB (the profile's levels "
                    "%.1f dB apart)\n", s40, s70, s71, s100, 20.0 * std::log10(layers[1].level / layers[0].level));
        check(std::abs(s70 - 0.5 * (s40 + s100)) < 0.05 && std::abs(s71 - s70) < std::abs(s100 - s40) / 60.0 + 0.05,
              "a partial's level as it sounds is interpolated: the same sustain on both sides of the boundary");
        // Blended: at velocity 71, the upper layer's two modes interfere, and keep their poles (moved toward the
        // lower layer's one, 440.05 Hz about halfway to 440 Hz, they would beat half as fast and cancel each other
        // less): the partial at its interpolated level, as it sounds.
        const double b70 = sustain(70, true), b71 = sustain(71, true);
        const double want71 = s40 + (71.0 - 40.0) / 60.0 * (s100 - s40);
        std::printf("      blended: v70 %.2f dB, v71 %.2f dB (interpolated %.2f dB)\n", b70, b71, want71);
        check(std::abs(b70 - 0.5 * (s40 + s100)) < 0.05 && std::abs(b71 - want71) < 0.05,
              "blended: a partial whose modes interfere keeps its poles, at its interpolated level");
    }

    // 3. Key release: the damper curve; sustain pedal: the dampers stay up until the pedal is released.
    {
        pf::ModalEngine e;
        e.install(profile);
        e.prepare(kRate, 64);
        const int off = 4800, frames = 48000;
        render(e, { { 0, 0x90, 69, 100 }, { (std::uint32_t) off, 0x80, 69, 0 } }, frames, l, r);
        const double before = rms(l, off - 960, off), after = rms(l, off + 9600, off + 10560);
        check(20.0 * std::log10(after / before) < -50.0, "key release: the damper curve brings the note down (> 50 dB in 0.2 s)");
        check(e.activeVoices.load() == 0, "a damped voice is freed");

        pf::ModalEngine s;
        s.install(profile);
        s.bodyEnabled = false;
        s.prepare(kRate, 64);
        render(s, { { 0, 0xB0, 64, 127 }, { 0, 0x90, 69, 100 }, { (std::uint32_t) off, 0x80, 69, 0 },
                    { 24000, 0xB0, 64, 0 } },
               frames, l, r);
        const double held = rms(l, off + 9600, off + 10560), free = rms(l, 24000 + 9600, 24000 + 10560);
        const double natural = 0.2 * std::exp(-1.0 * (off + 10000) / kRate) / std::sqrt(2.0);
        check(std::abs(20.0 * std::log10(held / natural)) < 1.5, "sustain pedal down: the released key keeps ringing");
        check(20.0 * std::log10(free / held) < -50.0, "pedal up: the damper lands");
    }

    // 4. Soundboard: shared by the notes, it rings on after a short note is cut, and it can be switched off.
    {
        pf::ModalEngine e;
        e.install(profile);
        e.residualEnabled = false;
        e.prepare(kRate, 64);
        render(e, { { 0, 0x90, 69, 100 } }, 4800, l, r);
        check(e.activeModes.load() == 3, "one voice of 2 modes plus the shared resonance");
        e.bodyEnabled = false;
        render(e, {}, 64, l, r);
        check(e.activeModes.load() == 2, "body off: the shared resonance is silenced");
    }

    // 5. One string per note. A re-strike strikes the string while it rings: at that sample, the output is what the
    //    string was going to play plus the new strike's first sample (no step); the note keeps one voice; repeated
    //    strikes build up as on the reference (+7 to +9 dB after 8, measured on Noire). Then all sound off.
    {
        auto strike = [&](std::vector<pf::Event> events, int frames, float absorption = 0.0f) {
            pf::ModalEngine e;
            e.bodyEnabled = false; // the strings alone
            e.restrikeAbsorption = absorption;
            e.install(profile);
            e.prepare(kRate, 64);
            std::vector<float> a, b;
            render(e, std::move(events), frames, a, b);
            return std::make_pair(a, e.activeVoices.load());
        };
        const auto alone = strike({ { 0, 0x90, 69, 100 } }, 4801).first;      // the first strike, not struck again
        const auto fresh = strike({ { 0, 0x90, 69, 100 } }, 1).first;         // a strike's first sample
        const auto [again, voices] = strike({ { 0, 0x90, 69, 100 }, { 4800, 0x90, 69, 100 } }, 4801);
        const double step = std::abs((double) again[4800] - ((double) alone[4800] + fresh[0]));
        std::printf("      re-struck at 0.1 s: %.9f against %.9f + %.9f (difference %.1e)\n", again[4800], alone[4800], fresh[0], step);
        check(step < 1e-6 && voices == 1, "re-strike: the string goes on and the new strike adds to it, one voice for the note");
        // Repeated strikes: what a string does is the sum of each strike alone (linear); the engine carries each
        // partial's sum into the new strike's modes rather than every mode on its own, which a beating partial
        // shows a little.
        std::vector<pf::Event> repeated;
        for (int k = 0; k < 8; ++k)
            repeated.push_back({ (std::uint32_t) (k * 4800), 0x90, 69, 100 });
        const auto built = strike(repeated, 8 * 4800 + 4800).first;
        const auto single = strike({ { 0, 0x90, 69, 100 } }, 9 * 4800).first;
        std::vector<float> sumOf((std::size_t) (9 * 4800), 0.0f);
        for (int k = 0; k < 8; ++k)
            for (int i = k * 4800; i < 9 * 4800; ++i)
                sumOf[(std::size_t) i] += single[(std::size_t) (i - k * 4800)];
        const double gainDb = 20.0 * std::log10(rms(built, 7 * 4800 + 960, 9 * 4800) / rms(single, 960, 2 * 4800));
        const double againstSum = 20.0 * std::log10(rms(built, 7 * 4800 + 960, 9 * 4800) / rms(sumOf, 7 * 4800 + 960, 9 * 4800));
        std::printf("      8 strikes 0.1 s apart: %+.1f dB after the last against after the first; %+.2f dB against the sum of the strikes alone\n",
                    gainDb, againstSum);
        check(gainDb > 3.0 && std::abs(againstSum) < 0.05, "without absorption, repeated strikes add exactly (a linear string)");

        // The hammer absorbs half the vibration's energy over its contact: the waveform still goes on at the strike's
        // sample, and once the contact is over, what is left of the first strike is sqrt(1/2) of it.
        const auto absorbed = strike({ { 0, 0x90, 69, 100 }, { 4800, 0x90, 69, 100 } }, 9600, 0.5f).first;
        const auto longAlone = strike({ { 0, 0x90, 69, 100 } }, 9600).first;
        const auto freshLong = strike({ { 0, 0x90, 69, 100 } }, 4800).first;
        const double stepAbsorbed = std::abs((double) absorbed[4800] - ((double) longAlone[4800] + freshLong[0]));
        double left = 0.0, before = 0.0;
        for (int i = 4800 + 300; i < 9600; ++i) {
            const double old = absorbed[(std::size_t) i] - freshLong[(std::size_t) (i - 4800)];
            left += old * old;
            before += (double) longAlone[(std::size_t) i] * longAlone[(std::size_t) i];
        }
        const double keptShare = std::sqrt(left / before);
        std::printf("      absorption 0.5: step %.1e at the strike, the first strike at %.4f of itself after the contact (sqrt 0.5 = 0.7071)\n",
                    stepAbsorbed, keptShare);
        check(stepAbsorbed < 1e-6 && std::abs(keptShare - std::sqrt(0.5)) < 0.01,
              "the hammer absorbs its share of the vibration over the contact, without a step");

        pf::ModalEngine e;
        e.install(profile);
        e.prepare(kRate, 64);
        render(e, { { 0, 0x90, 69, 100 }, { 4800, 0x90, 69, 100 } }, 9600, l, r);
        e.allSoundOff();
        render(e, {}, 14400, l, r); // 15 ms time constant: -60 dB after 0.1 s, freed below -100 dB
        check(e.activeVoices.load() == 0 && rms(l, 9600, 14400) < 1e-6, "all sound off: silence within 0.2 s");
    }

    // 5b. Every note keeps its voice: a glissando of the 88 notes with the pedal down gives the sum of each note
    //     played alone (nothing taken from one note for another).
    {
        auto many = std::make_shared<pf::ModalProfile>();
        many->residualRate = kRate;
        many->noteIndex.fill(-1);
        for (int midi = 21; midi <= 108; ++midi) {
            pf::ModalProfile::Note n;
            n.midi = midi;
            n.f0 = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            pf::ModalProfile::Layer layer;
            layer.velocity = 80;
            layer.modes.push_back({ n.f0, 0.5, { 0.01f, 0.0f }, { 0.01f, 0.0f } });
            layer.partials.push_back({ 1, 0, 1, 0.005f });
            layer.level = 0.005f;
            n.layers.push_back(layer);
            many->noteIndex[(std::size_t) midi] = (int) many->notes.size();
            many->notes.push_back(std::move(n));
        }
        many->maxModesPerLayer = 1;
        const int frames = (int) (3.0 * kRate);
        auto play = [&](const std::vector<pf::Event>& events, std::vector<float>& out, int& voices) {
            pf::ModalEngine e;
            e.install(many);
            e.prepare(kRate, 64);
            std::vector<float> right;
            render(e, events, frames, out, right);
            voices = e.activeVoices.load();
            return e.stolenVoices.load();
        };
        std::vector<pf::Event> gliss { { 0, 0xB0, 64, 127 } };
        for (int midi = 21; midi <= 108; ++midi)
            gliss.push_back({ (std::uint32_t) ((midi - 21) * 480), 0x90, (std::uint8_t) midi, 80 });
        std::vector<float> all, one;
        int voices = 0, unused = 0;
        const auto stolen = play(gliss, all, voices);
        std::vector<double> sum((std::size_t) frames, 0.0);
        for (int midi = 21; midi <= 108; ++midi) {
            play({ { 0, 0xB0, 64, 127 }, { (std::uint32_t) ((midi - 21) * 480), 0x90, (std::uint8_t) midi, 80 } }, one, unused);
            for (int i = 0; i < frames; ++i)
                sum[(std::size_t) i] += one[(std::size_t) i];
        }
        double worst = 0.0, peak = 0.0;
        for (int i = 0; i < frames; ++i) {
            worst = std::max(worst, std::abs(all[(std::size_t) i] - sum[(std::size_t) i]));
            peak = std::max(peak, std::abs(sum[(std::size_t) i]));
        }
        std::printf("      88-note glissando, pedal down: %d voices, %u stolen, %.1f dB from the sum of the notes alone\n", voices,
                    stolen, 20.0 * std::log10(worst / peak + 1e-12));
        check(voices == 88 && stolen == 0 && worst < 1e-5 * peak, "88 notes with the pedal down: every note keeps its voice");
    }

    // 6. A missing note is ignored; a profile swap while playing silences the old voices.
    {
        pf::ModalEngine e;
        e.install(profile);
        e.prepare(kRate, 64);
        render(e, { { 0, 0x90, 60, 100 } }, 480, l, r);
        check(e.activeVoices.load() == 0 && e.lastLayerVelocity.load() == 0, "a note without data is ignored");
        render(e, { { 0, 0x90, 69, 100 } }, 480, l, r);
        e.install(nullptr);
        render(e, {}, 480, l, r);
        check(e.activeVoices.load() == 0 && rms(l, 0, 480) == 0.0, "no profile: silence");
    }

    // 7. The pedal sets the damper's speed: pressing it again stops the release where it is (pedalCatch, or the
    //    release runs to its end), and a half pedal runs the curve slower.
    {
        const int off = 4800;
        double held = 0.0, heldHalf = 0.0; // the note held, over the windows measured below
        {
            pf::ModalEngine e;
            e.install(profile);
            e.bodyEnabled = false;
            e.prepare(kRate, 64);
            render(e, { { 0, 0x90, 69, 100 } }, off + 6000, l, r);
            held = rms(l, off + 4800, off + 5280);
            heldHalf = rms(l, off + 4560, off + 5040);
        }
        auto levelAfter = [&](bool catching) {
            pf::ModalEngine e;
            e.install(profile);
            e.bodyEnabled = false;
            e.pedalCatch = catching;
            e.prepare(kRate, 64);
            // Key released, pedal pressed again 10 ms later (the curve is at -3 dB there).
            render(e, { { 0, 0x90, 69, 100 }, { (std::uint32_t) off, 0x80, 69, 0 }, { (std::uint32_t) off + 480, 0xB0, 64, 127 } },
                   off + 6000, l, r);
            return 20.0 * std::log10(rms(l, off + 4800, off + 5280) / held + 1e-12);
        };
        const double caught = levelAfter(true), released = levelAfter(false);
        std::printf("      re-pedalled 10 ms after the release: %.1f dB against the note held (no catch: %.1f dB)\n", caught, released);
        check(std::abs(caught + 3.0) < 1.0, "pedal pressed again: what is left of the note rings on");
        check(std::abs(released + 30.0) < 1.5, "without pedalCatch: the release runs on, as in the reference");

        auto half = std::make_shared<pf::ModalProfile>(*profile);
        half->pedalLift.fill(0.0f);
        half->pedalLift[40] = 0.5f; // CC64 = 40: the curve at half speed
        for (int v = 64; v < 128; ++v)
            half->pedalLift[(std::size_t) v] = 1.0f;
        pf::ModalEngine e;
        e.install(half);
        e.bodyEnabled = false;
        e.prepare(kRate, 64);
        render(e, { { 0, 0xB0, 64, 40 }, { 0, 0x90, 69, 100 }, { (std::uint32_t) off, 0x80, 69, 0 } }, off + 5200, l, r);
        const double at = 20.0 * std::log10(rms(l, off + 4560, off + 5040) / heldHalf);
        std::printf("      half pedal, 0.1 s after the release: %.1f dB (the curve's -15 dB at 0.05 s)\n", at);
        check(std::abs(at + 15.0) < 1.0, "half pedal: the damper curve runs at the pedal's speed");
    }

    // 8. Tuning: the reference A4 and the temperament scale the note's frequencies.
    {
        auto peak = [&](const std::vector<float>& x, double lo, double hi) {
            double best = 0.0, bestF = lo;
            for (double f = lo; f <= hi; f += 0.02) {
                std::complex<double> s = 0.0;
                for (int i = 4800; i < 28800; ++i)
                    s += (double) x[(std::size_t) i] * std::exp(std::complex<double>(0.0, -kTwoPi * f * i / kRate));
                if (std::abs(s) > best) {
                    best = std::abs(s);
                    bestF = f;
                }
            }
            return bestF;
        };
        auto measure = [&](float a4, float centsC) {
            pf::ModalEngine e;
            e.install(ramped);
            e.residualEnabled = false;
            e.referenceA4 = a4;
            e.pitchClassCents[0] = centsC; // note 72 is a C
            e.prepare(kRate, 64);
            render(e, { { 0, 0x90, 72, 100 } }, 28800, l, r);
            return peak(l, 290.0, 320.0);
        };
        const double at442 = measure(442.0f, 0.0f), quarter = measure(440.0f, 50.0f);
        std::printf("      300 Hz partial: %.2f Hz at A4 = 442, %.2f Hz at +50 cents\n", at442, quarter);
        check(std::abs(at442 - 300.0 * 442.0 / 440.0) < 0.1, "reference A4 = 442 Hz: every frequency scaled by 442 / 440");
        check(std::abs(quarter - 300.0 * std::pow(2.0, 50.0 / 1200.0)) < 0.1, "temperament: a pitch class offset in cents");
    }

    // 9. Sympathetic resonance: a free string (A3, its second partial on the A4 played) rings with the pedal down,
    //    or under its key held; with the pedal up and no key held, the output is the note's alone.
    {
        std::shared_ptr<pf::ModalProfile> withString = std::make_shared<pf::ModalProfile>(*profile);
        pf::ModalProfile::Note a3;
        a3.midi = 57;
        a3.layers.push_back(makeLayer(100, 1e-4f)); // its voice is nearly silent: a key held without a sound
        withString->notes.push_back(std::move(a3));
        withString->noteIndex[57] = 1;
        pf::ModalProfile::String string;
        string.midi = 57;
        string.resonators.push_back({ 440.3, 0.8, 40.0f, { 1.0f, 0.0f }, { 1.0f, 0.0f } });
        withString->strings.push_back(string);
        withString->stringResonators = 1;
        auto run = [&](bool sympathetic, bool heldKeys, std::vector<pf::Event> events) {
            pf::ModalEngine e;
            e.install(withString);
            e.bodyEnabled = false;
            e.sympatheticEnabled = sympathetic;
            e.heldKeysResonate = heldKeys;
            e.prepare(kRate, 64);
            std::vector<float> out, unused;
            render(e, std::move(events), 96000, out, unused);
            return out;
        };
        auto difference = [&](const std::vector<float>& a, const std::vector<float>& b, int from, int to) {
            double s = 0.0;
            for (int i = from; i < to; ++i)
                s += std::pow((double) a[(std::size_t) i] - b[(std::size_t) i], 2.0);
            return std::sqrt(s / (to - from));
        };
        const std::vector<pf::Event> pedal { { 0, 0xB0, 64, 127 }, { 0, 0x90, 69, 100 }, { 4800, 0x80, 69, 0 } };
        const auto on = run(true, true, pedal), off = run(false, true, pedal);
        const double ring = difference(on, off, 48000, 96000);
        std::printf("      pedal down: the free string at %.1f dB against the note, 1 to 2 s after it\n",
                    20.0 * std::log10(ring / rms(off, 48000, 96000)));
        check(ring > 1e-3 * rms(off, 48000, 96000), "pedal down: the free string sets ringing by the note");
        const std::vector<pf::Event> up { { 0, 0x90, 69, 100 }, { 48000, 0x80, 69, 0 } };
        check(difference(run(true, true, up), run(false, true, up), 0, 96000) == 0.0,
              "pedal up, no key held: the note alone, sample for sample");
        const std::vector<pf::Event> held { { 0, 0x90, 57, 100 }, { 2400, 0x90, 69, 100 }, { 48000, 0x80, 69, 0 } };
        const auto heldOn = run(true, true, held), heldOff = run(true, false, held), none = run(false, true, held);
        check(difference(heldOn, none, 24000, 48000) > 1e-3 * rms(none, 24000, 48000), "a key held: its string rings");
        check(difference(heldOff, none, 0, 96000) == 0.0, "heldKeysResonate off: only the pedal frees the strings");

        // The drive's decay (profile): the string is driven less and less, its resonance comes out weaker late.
        const auto plain = withString;
        auto fading = std::make_shared<pf::ModalProfile>(*withString);
        fading->stringDriveDecaySeconds = 0.25f;
        withString = fading;
        const auto faded = run(true, true, pedal);
        withString = plain;
        const double ringFaded = difference(faded, off, 48000, 96000);
        std::printf("      drive decaying in 0.25 s: the free string %.1f dB under the steady drive, 1 to 2 s after the note\n",
                    20.0 * std::log10(ringFaded / ring));
        check(ringFaded < 0.5 * ring && ringFaded > 0.0, "a decaying drive: the free string rings, weaker late");
    }

    // 10. Damper groups: the high partials die faster than the overall curve says.
    {
        auto grouped = std::make_shared<pf::ModalProfile>(*ramped);
        auto& d = grouped->dampers;
        d.stepSeconds = 0.01;
        d.notes = { 72 };
        d.holds = { 1.0 };
        d.logHolds = { 0.0 };
        d.length = 21;
        d.db.clear();
        for (int k = 0; k < 21; ++k)
            d.db.push_back(-0.5f * (float) k);        // overall: -10 dB at 0.2 s
        d.groupFirst = { 1, 2 };
        d.groupDb.clear();
        for (int g = 0; g < 2; ++g)
            for (int k = 0; k < 21; ++k)
                d.groupDb.push_back(g == 0 ? 0.0f : -1.5f * (float) k); // partial 2: -30 dB more at 0.2 s
        auto amplitude = [&](const std::vector<float>& x, int from, int to, double f) {
            std::complex<double> s = 0.0;
            for (int i = from; i < to; ++i)
                s += (double) x[(std::size_t) i] * std::exp(std::complex<double>(0.0, -kTwoPi * f * i / kRate));
            return 2.0 * std::abs(s) / (to - from);
        };
        pf::ModalEngine e;
        e.install(grouped);
        e.prepare(kRate, 64);
        const int off = 14400; // after the residual (0.1 s) and the level ramp (0.05 s)
        render(e, { { 0, 0x90, 72, 100 }, { (std::uint32_t) off, 0x80, 72, 0 } }, off + 14400, l, r);
        const double before = amplitude(l, off - 4800, off, kRampF[1]) / amplitude(l, off - 4800, off, kRampF[0]);
        const double after = amplitude(l, off + 9600, off + 14400, kRampF[1]) / amplitude(l, off + 9600, off + 14400, kRampF[0]);
        const double drop = 20.0 * std::log10(after / before);
        // Their natural decays differ too (6 and 2 per second): the windows are 0.3 s apart, -10.4 dB.
        const double natural = 20.0 * std::log10(std::exp(-(kRampDecay[1] - kRampDecay[0]) * 0.3));
        std::printf("      partial 2 against partial 1 over the release: %.1f dB (natural %.1f, group -30 at 0.2 s)\n", drop, natural);
        check(std::abs(drop - natural + 30.0) < 4.0, "damper groups: the partials of the high group die faster");
    }

    // 11. Release noise: the soundboard gets the knock when the damper lands, not before.
    {
        auto knock = std::make_shared<pf::ModalProfile>(*profile);
        knock->releaseNoise.notes = { 69 };
        knock->releaseNoise.velocities = { 100 };
        knock->releaseNoise.levelDb = { -80.0f };
        knock->releaseNoise.left = { { 0.01f, 0.0f } };
        knock->releaseNoise.right = { { 0.01f, 0.0f } };
        auto run = [&](bool noise) {
            pf::ModalEngine e;
            e.install(knock);
            e.releaseNoiseEnabled = noise;
            e.prepare(kRate, 64);
            std::vector<float> out, unused;
            render(e, { { 0, 0x90, 69, 100 }, { 9600, 0x80, 69, 0 } }, 24000, out, unused);
            return out;
        };
        const auto with = run(true), without = run(false);
        double before = 0.0, after = 0.0;
        for (int i = 0; i < 9600; ++i)
            before = std::max(before, (double) std::abs(with[(std::size_t) i] - without[(std::size_t) i]));
        for (int i = 9600; i < 24000; ++i)
            after = std::max(after, (double) std::abs(with[(std::size_t) i] - without[(std::size_t) i]));
        check(before == 0.0 && after > 5e-3, "release noise: the soundboard knock when the damper lands");
    }

    std::printf(failures ? "%d failure(s)\n" : "All modal engine tests passed\n", failures);
    // 12. Sostenuto (CC66): it catches the dampers raised when it is pressed, and only those.
    {
        auto run = [&](std::vector<pf::Event> events) {
            pf::ModalEngine e;
            e.install(profile);
            e.bodyEnabled = false;
            e.prepare(kRate, 64);
            std::vector<float> out, unused;
            render(e, std::move(events), 48000, out, unused);
            return out;
        };
        const int off = 4800;
        const double natural = 0.2 * std::exp(-1.0 * (off + 10000) / kRate) / std::sqrt(2.0);
        // A key held when it is pressed: released, it rings on; the sostenuto up, its damper lands.
        const auto caught = run({ { 0, 0x90, 69, 100 }, { 2400, 0xB0, 66, 127 }, { (std::uint32_t) off, 0x80, 69, 0 },
                                  { 24000, 0xB0, 66, 0 } });
        const double held = rms(caught, off + 9600, off + 10560), free = rms(caught, 24000 + 9600, 24000 + 10560);
        check(std::abs(20.0 * std::log10(held / natural)) < 1.5, "sostenuto: the key held when it was pressed rings on, released");
        check(20.0 * std::log10(free / held) < -50.0, "sostenuto up: that damper lands");
        // A key struck after it: damped at its release as usual.
        const auto later = run({ { 0, 0xB0, 66, 127 }, { 100, 0x90, 69, 100 }, { (std::uint32_t) off, 0x80, 69, 0 } });
        check(20.0 * std::log10(rms(later, off + 9600, off + 10560) / rms(later, off - 960, off)) < -50.0,
              "sostenuto: a key struck after it is damped at its release");
        // Pressed under the sustain pedal, it catches every damper: the pedal up, the released note rings on.
        const auto all = run({ { 0, 0xB0, 64, 127 }, { 0, 0x90, 69, 100 }, { 2400, 0x80, 69, 0 }, { (std::uint32_t) off, 0xB0, 66, 127 },
                               { 7200, 0xB0, 64, 0 } });
        const double afterPedal = rms(all, 7200 + 9600, 7200 + 10560);
        const double naturalThere = 0.2 * std::exp(-1.0 * (7200 + 10080) / kRate) / std::sqrt(2.0);
        check(std::abs(20.0 * std::log10(afterPedal / naturalThere)) < 1.5,
              "sostenuto pressed under the sustain pedal: everything raised stays up once the pedal is released");
    }

    // 13. Settings: another engine given this one's settings sounds the same, sample for sample.
    {
        pf::ModalEngine a;
        a.unisonSpread = 1.5f;
        a.pedalCatch = false;
        a.referenceA4 = 442.0f;
        a.pitchClassCents[9] = -6.0f;
        a.sympatheticGain = 0.5f;
        a.install(profile);
        a.prepare(kRate, 64);
        pf::ModalEngine b;
        b.applySettings(a.settings());
        b.install(profile);
        b.prepare(kRate, 64);
        const std::vector<pf::Event> events { { 0, 0xB0, 64, 127 }, { 10, 0x90, 69, 90 }, { 4800, 0x80, 69, 0 },
                                              { 9600, 0x90, 69, 60 }, { 12000, 0xB0, 64, 0 } };
        std::vector<float> la, lb, unused;
        render(a, events, 24000, la, unused);
        render(b, events, 24000, lb, unused);
        check(la == lb && a.settings().referenceA4 == 442.0f && b.pedalCatch.load() == false,
              "settings copied to another engine: the same sound, sample for sample");
    }

    // 14. Coupled strings (a trichord: A4): the partial's modes are those of a known unison (three detuned strings
    //     on a bridge), with the model. Unchanged by default (bit for bit); the soft pedal (the hammer misses a
    //     string) weakens the attack and leaves relatively more after-sound; a perfect unison stops the beating,
    //     a wider one beats more.
    {
        using C = std::complex<double>;
        const double detune[3] = { -0.3, 0.05, 0.4 };
        const C Y(-2.0, 0.3);
        C s[3], lam[3];
        for (int i = 0; i < 3; ++i)
            s[i] = C(-1.0, kTwoPi * (440.0 + detune[i]));
        // The eigenvalues: roots of prod (l - s_i) - Y sum_i prod_{j != i} (l - s_j) (Durand-Kerner).
        const C e1 = s[0] + s[1] + s[2], e2 = s[0] * s[1] + s[0] * s[2] + s[1] * s[2], e3 = s[0] * s[1] * s[2];
        const C c2 = -(e1 + 3.0 * Y), c1 = e2 + 2.0 * Y * e1, c0 = -(e3 + Y * e2);
        for (int i = 0; i < 3; ++i)
            lam[i] = s[i] + Y + C(0.01 * (i + 1), 0.013 * (i + 1));
        for (int iter = 0; iter < 200; ++iter)
            for (int i = 0; i < 3; ++i) {
                C den(1.0, 0.0);
                for (int j = 0; j < 3; ++j)
                    if (j != i)
                        den *= lam[i] - lam[j];
                lam[i] -= (((lam[i] + c2) * lam[i] + c1) * lam[i] + c0) / den;
            }
        std::sort(lam, lam + 3, [](const C& a, const C& b) { return a.imag() < b.imag(); });
        auto coupled = std::make_shared<pf::ModalProfile>(*profile);
        auto& layer = coupled->notes[0].layers[1]; // the loud layer, played at 100
        layer.modes.clear();
        for (int k = 0; k < 3; ++k) {
            C sv(0.0), svv(0.0);
            for (int i = 0; i < 3; ++i) {
                const C v = 1.0 / (lam[k] - s[i]);
                sv += v;
                svv += v * v;
            }
            const C r = 0.2 * sv * sv / svv; // struck alike, the bridge summing the strings
            layer.modes.push_back({ lam[k].imag() / kTwoPi, -lam[k].real(), std::complex<float>(r), std::complex<float>(r) });
        }
        layer.partials = { { 1, 0, 3, layer.partials[0].level } };
        coupled->maxModesPerLayer = 3;
        auto plain = std::make_shared<pf::ModalProfile>(*coupled);
        pf::ModalProfile::Unison u;
        u.strings = 3;
        u.modes = { 0, 1, 2 };
        const double mean = (detune[0] + detune[1] + detune[2]) / 3.0;
        for (int i = 0; i < 3; ++i)
            u.detuneHz[(std::size_t) i] = (float) (detune[i] - mean);
        u.bridge = std::complex<float>(Y);
        coupled->notes[0].layers[1].unison = { u };

        auto play = [&](std::shared_ptr<const pf::ModalProfile> prof, float spread, int soft) {
            pf::ModalEngine e;
            e.bodyEnabled = false;
            e.unisonSpread = spread;
            e.install(prof);
            e.prepare(kRate, 64);
            std::vector<float> l, r;
            render(e, { { 0, 0xB0, 67, (std::uint8_t) soft }, { 0, 0x90, 69, 100 } }, (int) (4.0 * kRate), l, r);
            return l;
        };
        auto level = [&](const std::vector<float>& x, double a, double b) {
            double e = 0.0;
            for (auto i = (std::size_t) (a * kRate); i < (std::size_t) (b * kRate); ++i)
                e += (double) x[i] * x[i];
            return 10.0 * std::log10(e / ((b - a) * kRate) + 1e-30);
        };
        // The 20 ms log envelope over 0.1 to 1.5 s against a straight line (one exponential decay): how far it
        // wobbles (dB RMS), and how often it turns (beats).
        auto envelope = [&](const std::vector<float>& x) {
            std::vector<double> env;
            for (double t = 0.1; t + 0.02 <= 1.5; t += 0.02)
                env.push_back(level(x, t, t + 0.02));
            const double n = (double) env.size();
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (std::size_t i = 0; i < env.size(); ++i) {
                sx += (double) i;
                sy += env[i];
                sxx += (double) i * i;
                sxy += (double) i * env[i];
            }
            const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), offset = (sy - slope * sx) / n;
            for (std::size_t i = 0; i < env.size(); ++i)
                env[i] -= offset + slope * (double) i;
            return env;
        };
        auto wobble = [&](const std::vector<float>& x) {
            double v = 0.0;
            const auto env = envelope(x);
            for (const double e : env)
                v += e * e;
            return std::sqrt(v / (double) env.size());
        };
        auto turns = [&](const std::vector<float>& x) {
            const auto env = envelope(x);
            int count = 0;
            for (std::size_t i = 2; i < env.size(); ++i)
                count += (env[i] - env[i - 1]) * (env[i - 1] - env[i - 2]) < 0.0 ? 1 : 0;
            return count;
        };
        const auto base = play(plain, 1.0f, 0), same = play(coupled, 1.0f, 0);
        const auto soft = play(coupled, 1.0f, 127), unison0 = play(coupled, 0.0f, 0), unison3 = play(coupled, 3.0f, 0);
        const double attack = level(soft, 0.0, 0.05) - level(same, 0.0, 0.05);
        const double after = (level(soft, 2.5, 3.5) - level(soft, 0.0, 0.05)) - (level(same, 2.5, 3.5) - level(same, 0.0, 0.05));
        std::printf("      soft pedal: attack %+.1f dB, after-sound against the attack %+.1f dB; envelope against one decay: "
                    "%.2f dB, %d turns (perfect unison %.2f dB, %d; three times the detuning %.2f dB, %d)\n", attack, after,
                    wobble(same), turns(same), wobble(unison0), turns(unison0), wobble(unison3), turns(unison3));
        check(base == same, "coupled strings: the measured modes, unchanged by default (bit for bit)");
        check(attack < -1.0 && attack > -6.0 && after > 1.0, "the soft pedal: a softer attack, relatively more after-sound");
        check(wobble(unison0) < 0.3 * wobble(same) && wobble(unison3) > wobble(same),
              "the unison's tuning: a single decay in perfect unison, deeper beating when wider");
    }

    // Voicing: a high shelf on the voice's whole sound, the partials above the corner (4 x f0) moved, the fundamental
    // kept; none at 0 dB (the sound as measured, bit for bit); following the velocity as asked.
    {
        auto p = std::make_shared<pf::ModalProfile>();
        p->residualRate = kRate;
        p->noteIndex.fill(-1);
        pf::ModalProfile::Note n;
        n.midi = 69;
        n.f0 = 440.0;
        pf::ModalProfile::Layer layer;
        layer.velocity = 100;
        layer.modes.push_back({ 440.0, 1.0, { 0.2f, 0.0f }, { 0.2f, 0.0f } });   // partial 1
        layer.modes.push_back({ 3520.0, 1.0, { 0.2f, 0.0f }, { 0.2f, 0.0f } });  // partial 8, an octave above the corner
        layer.partials.push_back({ 1, 0, 1, 0.1f });
        layer.partials.push_back({ 8, 1, 1, 0.1f });
        layer.level = 0.14f;
        n.layers.push_back(std::move(layer));
        p->notes.push_back(std::move(n));
        p->noteIndex[69] = 0;
        p->maxModesPerLayer = 2;
        auto play = [&](float db, float follow, int velocity) {
            pf::ModalEngine e;
            e.install(p);
            e.bodyEnabled = false;
            e.voicingBassDb = db;
            e.voicingMiddleDb = db;
            e.voicingTrebleDb = db;
            e.voicingVelocity = follow;
            e.prepare(kRate, 64);
            std::vector<float> a, b;
            render(e, { { 0, 0x90, 69, (std::uint8_t) velocity } }, kRate, a, b);
            return a;
        };
        auto amplitude = [](const std::vector<float>& x, double f) {
            std::complex<double> s;
            for (int i = (int) (0.2 * kRate); i < (int) x.size(); ++i)
                s += (double) x[(std::size_t) i] * std::polar(1.0, -kTwoPi * f * i / kRate);
            return std::abs(s);
        };
        const auto plain = play(0.0f, 0.0f, 100), softer = play(-12.0f, 0.0f, 100), harder = play(6.0f, 0.0f, 100);
        const auto half = play(-12.0f, 1.0f, 64);
        pf::ModalEngine untouched;
        untouched.install(p);
        untouched.bodyEnabled = false;
        untouched.prepare(kRate, 64);
        std::vector<float> u, ur;
        render(untouched, { { 0, 0x90, 69, 100 } }, kRate, u, ur);
        auto db = [&](const std::vector<float>& x, double f) { return 20.0 * std::log10(amplitude(x, f) / amplitude(plain, f)); };
        std::printf("      voicing -12 dB: partial 1 %+.2f dB, partial 8 %+.2f dB; +6 dB: %+.2f / %+.2f dB; -12 dB "
                    "following the velocity, at 64: %+.2f dB\n", db(softer, 440.0), db(softer, 3520.0), db(harder, 440.0),
                    db(harder, 3520.0), db(half, 3520.0) - 20.0 * std::log10(amplitude(play(0.0f, 1.0f, 64), 3520.0) / amplitude(plain, 3520.0)));
        check(u == plain, "voicing at 0 dB: no filter, the sound as measured (bit for bit)");
        check(std::abs(db(softer, 440.0)) < 0.5 && db(softer, 3520.0) < -10.0 && db(softer, 3520.0) > -13.0,
              "softer hammers: the partials above the corner lowered, the fundamental kept");
        check(std::abs(db(harder, 440.0)) < 0.5 && db(harder, 3520.0) > 5.0 && db(harder, 3520.0) < 6.5,
              "harder hammers: the partials above the corner raised");
        const double atHalf = db(half, 3520.0) - 20.0 * std::log10(amplitude(play(0.0f, 1.0f, 64), 3520.0) / amplitude(plain, 3520.0));
        check(atHalf < -5.0 && atHalf > -7.0, "following the velocity: half the voicing at velocity 64");

        // The attack's softness: the treble lowered at the strike, fading out; past it, the sound as measured.
        auto withEngine = [&](auto setup, std::vector<pf::Event> events, std::shared_ptr<const pf::ModalProfile> profile) {
            pf::ModalEngine e;
            e.install(profile);
            e.bodyEnabled = false;
            setup(e);
            e.prepare(kRate, 64);
            std::vector<float> a, b;
            render(e, std::move(events), (int) kRate * 2, a, b);
            return a;
        };
        const std::vector<pf::Event> strike = { { 0, 0x90, 69, 100 } };
        const auto asMeasured = withEngine([](pf::ModalEngine&) {}, strike, p);
        const auto soft = withEngine([](pf::ModalEngine& e) { e.attackSoftDb = -12.0f; e.attackSoftMs = 40.0f; }, strike, p);
        auto bandDb = [&](const std::vector<float>& x, const std::vector<float>& ref, double f, int from, int to) {
            std::complex<double> s, t;
            for (int i = from; i < to; ++i) {
                const auto w = std::polar(1.0, -kTwoPi * f * i / kRate);
                s += (double) x[(std::size_t) i] * w;
                t += (double) ref[(std::size_t) i] * w;
            }
            return 20.0 * std::log10(std::abs(s) / std::abs(t));
        };
        const double early = bandDb(soft, asMeasured, 3520.0, 0, 240), earlyLow = bandDb(soft, asMeasured, 440.0, 0, 480);
        const bool restored = std::equal(soft.begin() + (std::ptrdiff_t) (0.045 * kRate), soft.end(),
                                         asMeasured.begin() + (std::ptrdiff_t) (0.045 * kRate));
        std::printf("      attack -12 dB over 40 ms: partial 8 %+.2f dB over the first 5 ms, partial 1 %+.2f dB over 10 ms\n",
                    early, earlyLow);
        check(early < -7.0 && std::abs(earlyLow) < 1.5, "a softer attack: the treble lowered at the strike, the fundamental hardly");
        check(restored, "past the attack: the sound as measured (bit for bit)");

        // The lid: closed, the treble down by kLidClosedDb more than the whole.
        const auto closed = withEngine([](pf::ModalEngine& e) { e.lidOpen = 0.0f; }, strike, p);
        const double lidLow = bandDb(closed, asMeasured, 440.0, (int) (0.2 * kRate), (int) kRate);
        const double lidHigh = bandDb(closed, asMeasured, 3520.0, (int) (0.2 * kRate), (int) kRate);
        std::printf("      lid closed: partial 1 %+.2f dB, partial 8 %+.2f dB\n", lidLow, lidHigh);
        check(lidLow < -1.8 && lidLow > -3.2 && lidHigh < -9.5 && lidHigh > -12.0, "the lid closed: the treble down, the whole a little");
    }

    // Touch and resonance: the dynamic range, the sustain length, the dampers' speed.
    {
        const auto p = makeProfile(); // layers at velocities 40 and 100, 12 dB apart; dampers: -60 dB in 0.2 s
        auto play = [&](auto setup, std::vector<pf::Event> events, int frames) {
            pf::ModalEngine e;
            e.install(p);
            e.bodyEnabled = false;
            setup(e);
            e.prepare(kRate, 64);
            std::vector<float> a, b;
            render(e, std::move(events), frames, a, b);
            return a;
        };
        auto none = [](pf::ModalEngine&) {};
        auto halfRange = [](pf::ModalEngine& e) { e.dynamicRange = 0.5f; };
        const int second = (int) kRate;
        const auto soft = play(none, { { 0, 0x90, 69, 40 } }, second), softHalf = play(halfRange, { { 0, 0x90, 69, 40 } }, second);
        const auto loud = play(none, { { 0, 0x90, 69, 100 } }, second), loudHalf = play(halfRange, { { 0, 0x90, 69, 100 } }, second);
        const double softGain = 20.0 * std::log10(rms(softHalf, 0, second) / rms(soft, 0, second));
        const double loudGain = 20.0 * std::log10(rms(loudHalf, 0, second) / rms(loud, 0, second));
        std::printf("      dynamic range 50 %%: the pianissimo layer %+.2f dB, the fortissimo %+.2f dB\n", softGain, loudGain);
        check(std::abs(softGain - 6.0) < 0.5 && std::abs(loudGain) < 0.01,
              "the dynamic range halved: the soft layer 6 dB closer to the loud one, the loud one unchanged");

        const auto longer = play([](pf::ModalEngine& e) { e.sustainLength = 2.0f; }, { { 0, 0x90, 69, 100 } }, 2 * second);
        const auto plain = play(none, { { 0, 0x90, 69, 100 } }, 2 * second);
        const double start = 20.0 * std::log10(rms(longer, 0, 480) / rms(plain, 0, 480));
        const double later = 20.0 * std::log10(rms(longer, (int) (1.5 * kRate), 2 * second) / rms(plain, (int) (1.5 * kRate), 2 * second));
        std::printf("      sustain x2: %+.2f dB over the first 10 ms, %+.2f dB at 1.5 to 2 s\n", start, later);
        check(std::abs(start) < 0.5 && later > 5.0, "a longer sustain: the same strike, a slower decay");

        const std::vector<pf::Event> held = { { 0, 0x90, 69, 100 }, { (std::uint32_t) (kRate / 2), 0x80, 69, 0 } };
        const auto quick = play(none, held, second), slow = play([](pf::ModalEngine& e) { e.damperRate = 0.5f; }, held, second);
        const int from = (int) (0.55 * kRate), to = (int) (0.6 * kRate);
        const double slower = 20.0 * std::log10(rms(slow, from, to) / rms(quick, from, to));
        std::printf("      dampers at half speed: %+.2f dB 50 to 100 ms after the release\n", slower);
        check(slower > 6.0, "slower dampers: a longer release");
    }

    return failures ? 1 : 0;
}
