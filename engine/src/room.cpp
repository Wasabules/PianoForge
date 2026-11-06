#include <pianoforge/room.hpp>

#include <algorithm>
#include <cmath>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define PF_HAVE_MXCSR 1
#endif

namespace pf {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kSoundSpeed = 343.0;          // m/s
constexpr double kReflection = 0.85;           // amplitude kept by a wall at each reflection
constexpr double kHighPassHz = 60.0;           // the reverberation's input
constexpr double kShelfHz = 3000.0;            // crossover of the highs' faster decay
constexpr double kReflectionLowPassHz = 7000.0; // air and walls, on the early reflections
constexpr double kGlideS = 0.02, kFadeS = 0.01;
constexpr double kDepthS = 0.000125;           // modulation depth of the line lengths (+-)
// Line lengths at the largest size, ms (geometric, 29 to 113 ms; rounded to primes of samples).
constexpr double kLineMs[Room::kLines] = { 29.3, 32.1, 35.1, 38.4, 42.0, 45.9, 50.2, 54.9,
                                           60.1, 65.7, 71.9, 78.7, 86.1, 94.2, 103.0, 112.7 };
constexpr double kDiffuserMs[Room::kDiffusers] = { 4.77, 3.59, 12.73, 9.31 };
constexpr float kDiffuserGain[Room::kDiffusers] = { 0.70f, 0.70f, 0.60f, 0.60f };

/// Flush-to-zero and denormals-are-zero for the scope (a decaying tail ends up denormal otherwise).
struct NoDenormals {
#if PF_HAVE_MXCSR
    unsigned int saved = _mm_getcsr();
    NoDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
    ~NoDenormals() { _mm_setcsr(saved); }
#endif
};

bool isPrime(std::uint32_t n) noexcept
{
    if (n < 2)
        return false;
    for (std::uint32_t d = 2; d * d <= n; ++d)
        if (n % d == 0)
            return false;
    return true;
}

std::uint32_t nextPrime(std::uint32_t n) noexcept
{
    while (!isPrime(n))
        ++n;
    return n;
}

/// Sign of row `row`, column `col` of the 16 x 16 Hadamard matrix (Sylvester).
float hadamard(int row, int col) noexcept
{
    int bits = row & col, parity = 0;
    while (bits != 0) {
        parity ^= bits & 1;
        bits >>= 1;
    }
    return parity != 0 ? -1.0f : 1.0f;
}

/// In place, scaled to be orthonormal (1/4).
inline void hadamard16(float* y) noexcept
{
    for (int len = 1; len < Room::kLines; len <<= 1)
        for (int i = 0; i < Room::kLines; i += len << 1)
            for (int j = i; j < i + len; ++j) {
                const float a = y[j], b = y[j + len];
                y[j] = a + b;
                y[j + len] = a - b;
            }
    for (int i = 0; i < Room::kLines; ++i)
        y[i] *= 0.25f;
}

// Input and output vectors of the network: L feeds the even lines, R the odd ones; the outputs are two orthogonal
// rows of the Hadamard matrix (decorrelated left and right tails).
struct Vectors {
    float in[Room::kLines] {}, outL[Room::kLines] {}, outR[Room::kLines] {};
    Vectors()
    {
        for (int i = 0; i < Room::kLines; ++i) {
            in[i] = hadamard(3, i) / std::sqrt(8.0f);
            outL[i] = hadamard(5, i) * 0.25f;
            outR[i] = hadamard(10, i) * 0.25f;
        }
    }
};
const Vectors kVectors;

} // namespace

void Room::Delay::allocate(std::size_t minimum)
{
    std::size_t n = 1;
    while (n < minimum)
        n <<= 1;
    data.assign(n, 0.0f);
    mask = (std::uint32_t) (n - 1);
}

Room::Settings Room::settings() const noexcept
{
    Settings s;
    s.enabled = enabled.load();
    s.levelDb = levelDb.load();
    s.size = size.load();
    s.decaySeconds = decaySeconds.load();
    s.brightness = brightness.load();
    s.predelayMs = predelayMs.load();
    s.width = width.load();
    return s;
}

void Room::applySettings(const Settings& s) noexcept
{
    levelDb = s.levelDb;
    size = s.size;
    decaySeconds = s.decaySeconds;
    brightness = s.brightness;
    predelayMs = s.predelayMs;
    width = s.width;
    enabled = s.enabled;
}

void Room::configure(Config& c, float newSize, float newPredelayMs) const noexcept
{
    c.size = newSize;
    c.predelayMs = newPredelayMs;
    const double scale = 0.3 + 0.7 * newSize; // the network and the diffusion
    const auto minimum = (std::uint32_t) std::ceil(depth) + 3;
    std::uint32_t previous = 0;
    for (int i = 0; i < kLines; ++i) {
        auto n = std::max(minimum, (std::uint32_t) std::lround(kLineMs[i] * scale * fs / 1000.0));
        n = nextPrime(std::max(n, previous + 1));
        c.lineDelay[(std::size_t) i] = previous = n;
    }
    for (int j = 0; j < kDiffusers; ++j)
        c.diffuserDelay[(std::size_t) j] = std::max<std::uint32_t>(1, (std::uint32_t) std::lround(kDiffuserMs[j] * scale * fs / 1000.0));
    c.predelay = (std::uint32_t) std::lround(std::clamp((double) newPredelayMs, 0.0, (double) kMaxPredelayMs) * fs / 1000.0);

    // Image sources of a rectangular room (first and second order), from the piano to two microphones.
    const double room = 0.45 + 1.55 * newSize;
    const double dim[3] = { 10.5 * room, 7.5 * room, 4.5 * room };
    const double src[3] = { 0.30 * dim[0], 0.45 * dim[1], std::min(1.0, 0.4 * dim[2]) };
    const double mic[2][3] = { { 0.62 * dim[0], 0.5 * dim[1] - 0.3, std::min(1.3, 0.6 * dim[2]) },
                               { 0.62 * dim[0], 0.5 * dim[1] + 0.3, std::min(1.3, 0.6 * dim[2]) } };
    // Per axis, an image is (1 - 2u) s + 2 l L after |2 l - u| reflections.
    struct Option {
        int u, l, reflections;
    };
    const Option options[] = { { 0, 0, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 2 }, { 0, -1, 2 } };
    int count = 0;
    double energy[2] = { 0.0, 0.0 };
    double gains[2][kTaps] {}, delays[2][kTaps] {};
    for (const auto& ox : options)
        for (const auto& oy : options)
            for (const auto& oz : options) {
                const int reflections = ox.reflections + oy.reflections + oz.reflections;
                if (reflections < 1 || reflections > 2 || count >= kTaps)
                    continue;
                const Option* o[3] = { &ox, &oy, &oz };
                double image[3];
                for (int a = 0; a < 3; ++a)
                    image[a] = (1 - 2 * o[a]->u) * src[a] + 2.0 * o[a]->l * dim[a];
                for (int m = 0; m < 2; ++m) {
                    double r = 0.0, r0 = 0.0;
                    for (int a = 0; a < 3; ++a) {
                        r += (image[a] - mic[m][a]) * (image[a] - mic[m][a]);
                        r0 += (src[a] - mic[m][a]) * (src[a] - mic[m][a]);
                    }
                    r = std::sqrt(r);
                    r0 = std::sqrt(r0);
                    gains[m][count] = std::pow(kReflection, reflections) * r0 / r;
                    delays[m][count] = (r - r0) / kSoundSpeed;
                    energy[m] += gains[m][count] * gains[m][count];
                }
                ++count;
            }
    for (int j = 0; j < kTaps; ++j) { // the early energy normalized, per microphone
        c.tapGainL[(std::size_t) j] = j < count ? (float) (gains[0][j] / std::sqrt(energy[0])) : 0.0f;
        c.tapGainR[(std::size_t) j] = j < count ? (float) (gains[1][j] / std::sqrt(energy[1])) : 0.0f;
        c.tapDelayL[(std::size_t) j] = std::max<std::uint32_t>(1, (std::uint32_t) std::lround(delays[0][j] * fs));
        c.tapDelayR[(std::size_t) j] = std::max<std::uint32_t>(1, (std::uint32_t) std::lround(delays[1][j] * fs));
    }
}

void Room::prepare(double sampleRate)
{
    fs = sampleRate;
    depth = (float) (kDepthS * fs);
    glide = (float) (1.0 - std::exp(-1.0 / (kGlideS * fs)));
    fadeStep = (float) (1.0 / (kFadeS * fs));
    hpCoef = (float) (1.0 / (1.0 + kTwoPi * kHighPassHz / fs));
    lpCoef = (float) (1.0 - std::exp(-kTwoPi * kReflectionLowPassHz / fs));
    for (int i = 0; i < kLines; ++i) {
        const double w = kTwoPi * (0.25 + 0.05 * i) / fs;
        rotC[(std::size_t) i] = (float) std::cos(w);
        rotS[(std::size_t) i] = (float) std::sin(w);
    }
    // The largest room: every ring sized for it.
    Config largest;
    configure(largest, 1.0f, kMaxPredelayMs);
    for (int i = 0; i < kLines; ++i)
        lines[(std::size_t) i].allocate(largest.lineDelay[(std::size_t) i] + (std::size_t) std::ceil(depth) + 4);
    std::uint32_t taps = 0;
    for (int j = 0; j < kTaps; ++j)
        taps = std::max({ taps, largest.tapDelayL[(std::size_t) j], largest.tapDelayR[(std::size_t) j] });
    for (auto& d : pre)
        d.allocate(largest.predelay + 2);
    mono.allocate(largest.predelay + taps + 2);
    for (auto& channel : diffusers)
        for (int j = 0; j < kDiffusers; ++j)
            channel[(std::size_t) j].allocate(largest.diffuserDelay[(std::size_t) j] + 2);
    reset();
    // Start on the current settings, without gliding to them.
    configure(cfg, std::clamp(size.load(), 0.0f, 1.0f), std::clamp(predelayMs.load(), 0.0f, kMaxPredelayMs));
    hasPending = false;
    fade = 1.0f;
    readSettings();
    updateTargets();
    wet = wetTarget;
    widthNow = widthTarget;
    late = lateTarget;
    absNow = absTarget;
}

void Room::reset() noexcept
{
    for (auto& d : pre)
        std::fill(d.data.begin(), d.data.end(), 0.0f);
    std::fill(mono.data.begin(), mono.data.end(), 0.0f);
    for (auto& l : lines)
        std::fill(l.data.begin(), l.data.end(), 0.0f);
    for (auto& channel : diffusers)
        for (auto& d : channel)
            std::fill(d.data.begin(), d.data.end(), 0.0f);
    shelfX.fill(0.0f);
    shelfY.fill(0.0f);
    apY.fill(0.0f);
    hpX[0] = hpX[1] = hpY[0] = hpY[1] = lpY[0] = lpY[1] = 0.0f;
    for (int i = 0; i < kLines; ++i) { // phases spread around the circle
        lfoC[(std::size_t) i] = (float) std::cos(kTwoPi * i / kLines);
        lfoS[(std::size_t) i] = (float) std::sin(kTwoPi * i / kLines);
    }
    pos = 0;
    samples = 0;
    cleared = true;
    sleeping = false;
}

void Room::readSettings() noexcept
{
    on = enabled.load(std::memory_order_relaxed);
    wetTarget = on ? (float) std::pow(10.0, std::clamp((double) levelDb.load(std::memory_order_relaxed), -80.0, 12.0) / 20.0) : 0.0f;
    widthTarget = std::clamp(width.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float newSize = std::clamp(size.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float newPredelay = std::clamp(predelayMs.load(std::memory_order_relaxed), 0.0f, kMaxPredelayMs);
    const Config& wanted = hasPending ? pending : cfg;
    if (newSize != wanted.size || newPredelay != wanted.predelayMs) {
        configure(pending, newSize, newPredelay);
        hasPending = true;
    }
    const float decay = std::clamp(decaySeconds.load(std::memory_order_relaxed), 0.1f, 30.0f);
    const float bright = std::clamp(brightness.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (decay != appliedDecay || bright != appliedBrightness) {
        appliedDecay = decay;
        appliedBrightness = bright;
        updateTargets();
    }
}

void Room::updateTargets() noexcept
{
    // Per line, the loss g0 = 10^(-3 d / (T60 fs)), and a first-order high shelf (bilinear, crossover kShelfHz)
    // from 1 to G = g_hi / g0, g_hi from the highs' T60: H(z) = ((G + K) + (K - G) z^-1) / ((1 + K) + (K - 1) z^-1).
    const double t = std::clamp((double) appliedDecay, 0.1, 30.0);
    const double th = t * (0.15 + 0.85 * std::clamp((double) appliedBrightness, 0.0, 1.0));
    const double kk = std::tan(kTwoPi * 0.5 * std::min(kShelfHz, 0.45 * fs) / fs);
    double total = 0.0;
    for (int i = 0; i < kLines; ++i) {
        const double d = cfg.lineDelay[(std::size_t) i];
        total += d;
        const double g0 = std::pow(10.0, -3.0 * d / (t * fs)), gh = std::pow(10.0, -3.0 * d / (th * fs));
        const double g = gh / g0;
        auto& a = absTarget[(std::size_t) i];
        a.gain = (float) g0;
        a.b0 = (float) ((g + kk) / (1.0 + kk));
        a.b1 = (float) ((kk - g) / (1.0 + kk));
        a.a1 = (float) ((kk - 1.0) / (1.0 + kk));
    }
    // The network's energy for a unit impulse on both inputs (an input as correlated as a piano's): the energy
    // stored (2), spread over the lines' samples, read through unit output vectors while it decays as
    // exp(-13.8 t / T60): 2 T60 fs / (13.8 sum(d)). The late energy is set to 3 sqrt(T60 / 2 s) times the early one.
    const double unit = 2.0 * t * fs / (13.8 * total);
    lateTarget = (float) std::sqrt(3.0 * std::sqrt(t / 2.0) / unit);
}

double Room::tailSeconds(const Settings& s) noexcept
{
    return std::clamp((double) s.predelayMs, 0.0, (double) kMaxPredelayMs) / 1000.0
         + std::clamp((double) s.decaySeconds, 0.1, 30.0) * 80.0 / 60.0 + 0.2;
}

void Room::process(float* left, float* right, int n) noexcept
{
    if (fs <= 0.0 || n <= 0 || lines[0].data.empty())
        return;
    readSettings();
    if (!on && cleared)
        return; // off: the input passes through, bit for bit
    bool silent = true;
    for (int k = 0; k < n && silent; ++k)
        silent = left[k] == 0.0f && right[k] == 0.0f;
    silentSamples = silent ? silentSamples + (std::uint64_t) n : 0;
    if (sleeping) {
        if (silent)
            return; // asleep: nothing rings, nothing comes in
        sleeping = false;
    }
    cleared = false;
    float wetPeak = 0.0f;
    NoDenormals noDenormals;
    const Vectors& v = kVectors;
    for (int k = 0; k < n; ++k) {
        // Glides, and the fade of a change of size or pre-delay.
        wet += (wetTarget - wet) * glide;
        widthNow += (widthTarget - widthNow) * glide;
        late += (lateTarget - late) * glide;
        for (int i = 0; i < kLines; ++i) {
            auto& now = absNow[(std::size_t) i];
            const auto& to = absTarget[(std::size_t) i];
            now.gain += (to.gain - now.gain) * glide;
            now.b0 += (to.b0 - now.b0) * glide;
            now.b1 += (to.b1 - now.b1) * glide;
            now.a1 += (to.a1 - now.a1) * glide;
        }
        if (hasPending) {
            fade -= fadeStep;
            if (fade <= 0.0f) { // faded out: the new lengths (the filters follow them at once)
                fade = 0.0f;
                cfg = pending;
                hasPending = false;
                updateTargets();
                absNow = absTarget;
                late = lateTarget;
            }
        } else if (fade < 1.0f) {
            fade = std::min(1.0f, fade + fadeStep);
        }

        // The input, high-passed, into the pre-delay (stereo) and the reflections' line (mono).
        const float inL = left[k], inR = right[k];
        float x[2] = { inL, inR };
        for (int c = 0; c < 2; ++c) {
            hpY[c] = hpCoef * (hpY[c] + x[c] - hpX[c]);
            hpX[c] = x[c];
            x[c] = hpY[c];
            pre[c].data[pos & pre[c].mask] = x[c];
        }
        mono.data[pos & mono.mask] = 0.5f * (x[0] + x[1]);

        // Early reflections.
        float er[2] = { 0.0f, 0.0f };
        for (int j = 0; j < kTaps; ++j) {
            er[0] += cfg.tapGainL[(std::size_t) j] * mono.at(pos, cfg.predelay + cfg.tapDelayL[(std::size_t) j]);
            er[1] += cfg.tapGainR[(std::size_t) j] * mono.at(pos, cfg.predelay + cfg.tapDelayR[(std::size_t) j]);
        }
        for (int c = 0; c < 2; ++c)
            lpY[c] += lpCoef * (er[c] - lpY[c]);

        // The network's input: pre-delayed, diffused (all-passes v = x + g v[-d], y = v[-d] - g v).
        float d[2] = { pre[0].at(pos, cfg.predelay), pre[1].at(pos, cfg.predelay) };
        for (int c = 0; c < 2; ++c)
            for (int j = 0; j < kDiffusers; ++j) {
                auto& ap = diffusers[c][(std::size_t) j];
                const float delayed = ap.at(pos, cfg.diffuserDelay[(std::size_t) j]);
                const float w = d[c] + kDiffuserGain[j] * delayed;
                ap.data[pos & ap.mask] = w;
                d[c] = delayed - kDiffuserGain[j] * w;
            }

        // The network: modulated reads, absorption, outputs, Hadamard mixing, writes.
        float y[kLines];
        for (int i = 0; i < kLines; ++i) {
            const auto ii = (std::size_t) i;
            const float c0 = lfoC[ii], s0 = lfoS[ii];
            lfoC[ii] = c0 * rotC[ii] - s0 * rotS[ii];
            lfoS[ii] = s0 * rotC[ii] + c0 * rotS[ii];
            // The fractional read through a first-order all-pass (unit gain at every frequency: a linear
            // interpolation, inside the loop, would damp the highs at every pass), its fraction kept in [0.5, 1.5).
            const float delay = (float) cfg.lineDelay[ii] + depth * s0;
            const float whole = std::floor(delay - 0.5f);
            const float frac = delay - whole;
            const float eta = (1.0f - frac) / (1.0f + frac);
            const auto di = (std::uint32_t) whole;
            const float read = eta * lines[ii].at(pos, di) + lines[ii].at(pos, di + 1) - eta * apY[ii];
            apY[ii] = read;
            const auto& a = absNow[ii];
            const float shelf = a.b0 * read + a.b1 * shelfX[ii] - a.a1 * shelfY[ii];
            shelfX[ii] = read;
            shelfY[ii] = shelf;
            y[i] = a.gain * shelf;
        }
        float lateL = 0.0f, lateR = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            lateL += v.outL[i] * y[i];
            lateR += v.outR[i] * y[i];
        }
        hadamard16(y);
        for (int i = 0; i < kLines; ++i) {
            auto& line = lines[(std::size_t) i];
            line.data[pos & line.mask] = y[i] + v.in[i] * d[i & 1];
        }

        // Wet: early and late, the width, the level.
        const float wl = lpY[0] + late * lateL, wr = lpY[1] + late * lateR;
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * widthNow;
        const float g = wet * fade;
        const float outL = g * (mid + side), outR = g * (mid - side);
        wetPeak = std::max(wetPeak, std::max(std::abs(outL), std::abs(outR)));
        left[k] = inL + outL;
        right[k] = inR + outR;

        ++pos;
        if ((++samples & 4095u) == 0) // the phasors back on the unit circle (by the sample count: block-independent)
            for (int i = 0; i < kLines; ++i) {
                const auto ii = (std::size_t) i;
                const float m = 1.0f / std::sqrt(lfoC[ii] * lfoC[ii] + lfoS[ii] * lfoS[ii]);
                lfoC[ii] *= m;
                lfoS[ii] *= m;
            }
    }
    if (!on && wet < 1e-6f) {
        reset(); // faded out: cleared, and bypassed from the next block
        return;
    }
    // Asleep once the input has been silent longer than anything takes to cross the room (pre-delay, reflections,
    // diffusion, the longest line) and a whole block of reverberation stayed under -120 dB.
    std::uint32_t longest = cfg.predelay + cfg.lineDelay[kLines - 1];
    for (int j = 0; j < kTaps; ++j)
        longest = std::max(longest, cfg.predelay + std::max(cfg.tapDelayL[(std::size_t) j], cfg.tapDelayR[(std::size_t) j]));
    for (int j = 0; j < kDiffusers; ++j)
        longest += cfg.diffuserDelay[(std::size_t) j];
    if (silent && !hasPending && silentSamples > 2 * (std::uint64_t) longest && wetPeak < 1e-6f) {
        reset();
        sleeping = true;
    }
}

} // namespace pf
