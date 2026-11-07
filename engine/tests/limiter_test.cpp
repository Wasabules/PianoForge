// Output limiter tests: transparent under the ceiling, nothing over it, back to unity after a burst, the same gain
// on both channels, block-size independent. Exits non-zero on a failure.
#include <pianoforge/limiter.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

constexpr double kRate = 48000.0;
constexpr double kPi = 3.141592653589793;

void run(pf::Limiter& lim, std::vector<float>& l, std::vector<float>& r, int block)
{
    for (std::size_t s = 0; s < l.size(); s += (std::size_t) block) {
        const int n = (int) std::min<std::size_t>((std::size_t) block, l.size() - s);
        lim.process(l.data() + s, r.data() + s, n);
    }
}

} // namespace

int main()
{
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 0.1f);

    // 1. Under the ceiling: bit for bit.
    {
        pf::Limiter lim;
        lim.prepare(kRate);
        std::vector<float> l(48000), r(48000);
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = noise(rng), r[i] = noise(rng);
        for (auto* x : { &l, &r })
            for (auto& v : *x)
                v = std::clamp(v, -0.85f, 0.85f);
        const auto a = l, b = r;
        run(lim, l, r, 64);
        check(l == a && r == b && lim.takeReduction() == 0.0f, "under the ceiling: the input, bit for bit");
    }

    // 2. A burst over full scale: nothing over the ceiling, then back to unity; both channels alike.
    {
        pf::Limiter lim;
        lim.prepare(kRate);
        const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
        std::vector<float> l(96000), r(96000);
        for (std::size_t i = 0; i < l.size(); ++i) {
            const double t = (double) i / kRate;
            const double amp = t < 0.5 ? 2.0 : 0.3; // +6 dBFS for 0.5 s, then quiet
            l[i] = (float) (amp * std::sin(2 * kPi * 220.0 * t));
            r[i] = (float) (0.5 * amp * std::sin(2 * kPi * 330.0 * t));
        }
        const auto inL = l, inR = r;
        run(lim, l, r, 64);
        float over = 0.0f;
        for (std::size_t i = 0; i < l.size(); ++i)
            over = std::max({ over, std::abs(l[i]), std::abs(r[i]) });
        bool linked = true; // the same gain on both channels where both are not tiny
        for (std::size_t i = 0; i < l.size(); ++i)
            if (std::abs(inL[i]) > 0.1f && std::abs(inR[i]) > 0.1f)
                linked = linked && std::abs(l[i] / inL[i] - r[i] / inR[i]) < 1e-4f;
        bool back = true; // a second after the burst: untouched
        for (std::size_t i = (std::size_t) (1.5 * kRate); i < l.size(); ++i)
            back = back && l[i] == inL[i] && r[i] == inR[i];
        const float reduction = lim.takeReduction();
        std::printf("      +6 dBFS burst: peak out %.4f (ceiling %.4f), reduction %.1f dB\n", over, ceiling, reduction);
        check(over <= ceiling * 1.0001f, "a burst over full scale: nothing over the ceiling");
        check(linked, "one gain for both channels: the image does not move");
        check(back && reduction < -5.0f && reduction > -8.0f, "the gain comes back to unity after the burst");
    }

    // 3. Blocks of 64 or 509 samples: the same output.
    {
        std::vector<float> l1(48000), r1(48000);
        for (std::size_t i = 0; i < l1.size(); ++i)
            l1[i] = 3.0f * noise(rng), r1[i] = 3.0f * noise(rng);
        auto l2 = l1, r2 = r1;
        pf::Limiter a, b;
        a.prepare(kRate);
        b.prepare(kRate);
        run(a, l1, r1, 64);
        run(b, l2, r2, 509);
        check(l1 == l2 && r1 == r2, "blocks of 64 or 509 samples: the same output, sample for sample");
    }

    // 4. Off: untouched, even over full scale.
    {
        pf::Limiter lim;
        lim.prepare(kRate);
        lim.enabled = false;
        std::vector<float> l(4800, 1.5f), r(4800, -1.5f);
        run(lim, l, r, 64);
        check(l[100] == 1.5f && r[100] == -1.5f, "switched off: the input untouched");
    }
    return failures ? 1 : 0;
}
