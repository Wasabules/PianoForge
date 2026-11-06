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

        // 2c. A partial with a fade time (set by the forge past its analysis window) dies out from there: an extra
        // -60 dB per kPartialFadeSeconds, the other partial untouched.
        auto faded = std::make_shared<pf::ModalProfile>(*ramp);
        for (auto& layer : faded->notes[0].layers)
            layer.partials[1].fadeSeconds = 0.25f;
        pf::ModalEngine f;
        f.install(faded);
        f.bodyEnabled = false;
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
        a.restrikeFadeSeconds = 0.3f;
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

    return failures ? 1 : 0;
}
