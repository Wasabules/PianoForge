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

    // 2. Between two layers: the level is interpolated (log), the nearest layer is played.
    {
        auto level = [&](int velocity) {
            pf::ModalEngine e;
            e.install(profile);
            e.bodyEnabled = false;
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

    // 5. Re-strike and all-sound-off.
    {
        pf::ModalEngine e;
        e.install(profile);
        e.prepare(kRate, 64);
        render(e, { { 0, 0x90, 69, 100 }, { 4800, 0x90, 69, 100 } }, 9600, l, r);
        check(e.activeVoices.load() == 2, "re-strike: the previous voice keeps ringing while it fades");
        e.allSoundOff();
        render(e, {}, 14400, l, r); // 15 ms time constant: -60 dB after 0.1 s, freed below -100 dB
        check(e.activeVoices.load() == 0 && rms(l, 9600, 14400) < 1e-6, "all sound off: silence within 0.2 s");
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

    std::printf(failures ? "%d failure(s)\n" : "All modal engine tests passed\n", failures);
    return failures ? 1 : 0;
}
