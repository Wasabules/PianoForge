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
constexpr double kHighPassHz = 60.0;           // the reverberation's input
constexpr double kShelfHz = 3000.0;            // crossover of the highs' faster decay
constexpr double kReflectionLowPassHz = 7000.0; // air and walls, on the early reflections
constexpr double kGlideS = 0.02, kFadeS = 0.01;
constexpr double kDepthS = 0.000125;           // modulation depth of the line lengths (+-)
constexpr double kImageS = 0.2;                // the input's mid and side energies, smoothed over this
// Line lengths at the largest size, ms (geometric, 29 to 113 ms; rounded to primes of samples).
constexpr double kLineMs[Room::kLines] = { 29.3, 32.1, 35.1, 38.4, 42.0, 45.9, 50.2, 54.9,
                                           60.1, 65.7, 71.9, 78.7, 86.1, 94.2, 103.0, 112.7 };
constexpr double kDiffuserMs[Room::kDiffusers] = { 4.77, 3.59, 12.73, 9.31 };
constexpr double kMinVolume = 60.0, kMaxVolume = 40000.0; // m3, at sizes 0 and 1
constexpr double kPi = 3.141592653589793;
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

/// The room (a shoebox of proportions 10.5 : 7.5 : 4.5), the piano in it and the listener's two microphones.
struct Geometry {
    double volume = 0.0;
    double dim[3] {};      // length, width, height
    double src[3] {};      // the piano: near one end, a little off the axis (the side walls differ)
    double mic[2][3] {};   // the listener, down the room from the piano, ears 1.2 m high, 0.6 m apart
    double distance = 0.0; // what the listener is at (the room may be shorter than asked)
};

double volumeFor(double size) noexcept
{
    return kMinVolume * std::pow(kMaxVolume / kMinVolume, std::clamp(size, 0.0, 1.0));
}

Geometry geometryFor(double size, double distance) noexcept
{
    Geometry g;
    g.volume = volumeFor(size);
    const double k = std::cbrt(g.volume / (10.5 * 7.5 * 4.5));
    g.dim[0] = 10.5 * k;
    g.dim[1] = 7.5 * k;
    g.dim[2] = 4.5 * k;
    g.src[0] = std::clamp(0.15 * g.dim[0], 1.5, 6.0);
    g.src[1] = 0.5 * g.dim[1] - std::min(1.0, 0.05 * g.dim[1]);
    g.src[2] = std::min(1.0, 0.4 * g.dim[2]);
    const double ear = std::min(1.2, 0.6 * g.dim[2]), dz = ear - g.src[2];
    const double wanted = std::clamp(distance, (double) Room::kReferenceDistance, (double) Room::kMaxDistance);
    const double room = std::max(0.0, g.dim[0] - 1.0 - g.src[0]); // how far down the room the listener can go
    double along = std::sqrt(std::max(0.0, wanted * wanted - dz * dz));
    g.distance = wanted;
    if (along > room) {
        along = room;
        g.distance = std::max((double) Room::kReferenceDistance, std::sqrt(along * along + dz * dz));
    }
    const double half = std::min(0.3, 0.1 * g.dim[1]);
    for (int m = 0; m < 2; ++m) {
        g.mic[m][0] = g.src[0] + along;
        g.mic[m][1] = g.src[1] + (m == 0 ? -half : half);
        g.mic[m][2] = ear;
    }
    return g;
}

/// Each image source of the first and second order but the floor's (the engine's sound already has its floor
/// reflection), with its number of reflections: f(image, reflections).
template <class F>
void forEachImage(const Geometry& g, F&& f)
{
    // Per axis, an image is (1 - 2u) s + 2 l L after |2 l - u| reflections; on the vertical axis, only the
    // ceiling's (u = 1, l = 1).
    struct Option {
        int u, l, reflections;
    };
    const Option horizontal[] = { { 0, 0, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 2 }, { 0, -1, 2 } };
    const Option vertical[] = { { 0, 0, 0 }, { 1, 1, 1 } };
    for (const auto& ox : horizontal)
        for (const auto& oy : horizontal)
            for (const auto& oz : vertical) {
                const int reflections = ox.reflections + oy.reflections + oz.reflections;
                if (reflections < 1 || reflections > 2)
                    continue;
                const Option* o[3] = { &ox, &oy, &oz };
                double image[3];
                for (int a = 0; a < 3; ++a)
                    image[a] = (1 - 2 * o[a]->u) * g.src[a] + 2.0 * o[a]->l * g.dim[a];
                f(image, reflections);
            }
}

/// The amplitude a wall keeps at each reflection: sqrt(1 - alpha), alpha the mean absorption that gives this decay
/// in this room (Sabine: A = 0.161 V / T60 = alpha S).
double reflectionFor(const Geometry& g, double t60) noexcept
{
    const double surface = 2.0 * (g.dim[0] * g.dim[1] + g.dim[0] * g.dim[2] + g.dim[1] * g.dim[2]);
    const double alpha = std::clamp(0.161 * g.volume / (t60 * surface), 0.02, 0.95);
    return std::sqrt(1.0 - alpha);
}

double distanceBetween(const double* a, const double* b) noexcept
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/// Barron's revised theory: the direct, early (first 80 ms) and late energies at `r`, against the direct sound at
/// the reference distance; `trim` scales the reflections.
struct Energies {
    double direct = 1.0, early = 0.0, late = 0.0;
    double total() const noexcept { return direct + early + late; }
};

Energies energiesAt(double volume, double t60, double r, double trim) noexcept
{
    const double area = 0.161 * volume / t60; // Sabine, m2
    const double ref = Room::kReferenceDistance;
    const double reflected = 16.0 * kPi * ref * ref / (Room::kDirectivity * area) * std::exp(-13.82 * r / (kSoundSpeed * t60));
    const double split = std::exp(-13.82 * 0.08 / t60);
    Energies e;
    e.direct = (ref / r) * (ref / r);
    e.early = trim * reflected * (1.0 - split);
    e.late = trim * reflected * split;
    return e;
}

double directWidthAt(double r) noexcept
{
    return std::atan(0.5 * Room::kSourceWidth / r) / std::atan(0.5 * Room::kSourceWidth / Room::kReferenceDistance);
}

} // namespace

double Room::airAbsorptionDbPerMetre(double f) noexcept
{
    // ISO 9613-1, at T = 20 degrees C, 50 % relative humidity, the reference pressure.
    const double t = 293.15, t0 = 293.15, t01 = 273.16, humidity = 50.0;
    const double c = -6.8346 * std::pow(t01 / t, 1.261) + 4.6151;
    const double h = humidity * std::pow(10.0, c); // molar concentration of water vapour, %
    const double frO = 24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h);
    const double frN = std::pow(t / t0, -0.5) * (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(t / t0, -1.0 / 3.0) - 1.0)));
    const double f2 = f * f;
    return 8.686 * f2
         * (1.84e-11 * std::pow(t / t0, 0.5)
            + std::pow(t / t0, -2.5) * (0.01275 * std::exp(-2239.1 / t) / (frO + f2 / frO) + 0.1068 * std::exp(-3352.0 / t) / (frN + f2 / frN)));
}

Room::Acoustics Room::acoustics(const Settings& s) noexcept
{
    const auto g = geometryFor(s.size, s.distance);
    Acoustics a;
    a.volume = g.volume;
    a.length = g.dim[0];
    a.widthM = g.dim[1];
    a.height = g.dim[2];
    a.distance = g.distance;
    const double t = std::clamp((double) s.decaySeconds, 0.1, 30.0);
    const double area = 0.161 * g.volume / t;
    a.criticalDistance = std::sqrt(kDirectivity * area / (16.0 * kPi));
    const double trim = std::pow(10.0, std::clamp((double) s.levelDb, -40.0, 20.0) / 10.0);
    const auto e = energiesAt(g.volume, t, g.distance, trim);
    a.directDb = 10.0 * std::log10(e.direct);
    a.earlyDb = 10.0 * std::log10(e.early + 1e-30);
    a.lateDb = 10.0 * std::log10(e.late + 1e-30);
    a.reflectionsOverDirectDb = 10.0 * std::log10((e.early + e.late) / e.direct + 1e-30);
    a.loudnessDb = s.keepLoudness ? 10.0 * std::log10(energiesAt(g.volume, t, kReferenceDistance, trim).total() / e.total()) : 0.0;
    double first = 1e9;
    forEachImage(g, [&](const double* image, int) {
        for (int m = 0; m < 2; ++m)
            first = std::min(first, (distanceBetween(image, g.mic[m]) - distanceBetween(g.src, g.mic[m])) / kSoundSpeed);
    });
    a.firstReflectionMs = 1000.0 * first;
    a.airDbAt10k = airAbsorptionDbPerMetre(10000.0) * std::max(0.0, g.distance - kReferenceDistance);
    a.directWidth = directWidthAt(g.distance);
    return a;
}

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
    s.distance = distance.load();
    s.keepLoudness = keepLoudness.load();
    s.width = width.load();
    return s;
}

void Room::applySettings(const Settings& s) noexcept
{
    levelDb = s.levelDb;
    size = s.size;
    decaySeconds = s.decaySeconds;
    brightness = s.brightness;
    distance = s.distance;
    keepLoudness = s.keepLoudness;
    width = s.width;
    enabled = s.enabled;
}

void Room::configure(Config& c, float newSize, float newDistance, float newDecay) const noexcept
{
    c.size = newSize;
    c.distance = newDistance;
    c.decay = newDecay;
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

    // Image sources, from the piano to the listener's two microphones: gains against each one's direct sound,
    // delays after it.
    const auto g = geometryFor(newSize, newDistance);
    const double reflection = reflectionFor(g, std::clamp((double) newDecay, 0.1, 30.0));
    c.heard = (float) g.distance;
    int count = 0;
    double energy[2] = { 0.0, 0.0 }, first = 1e9;
    double gains[2][kTaps] {}, delays[2][kTaps] {};
    forEachImage(g, [&](const double* image, int reflections) {
        if (count >= kTaps)
            return;
        for (int m = 0; m < 2; ++m) {
            const double r = distanceBetween(image, g.mic[m]), r0 = distanceBetween(g.src, g.mic[m]);
            gains[m][count] = std::pow(reflection, reflections) * r0 / r;
            delays[m][count] = (r - r0) / kSoundSpeed;
            energy[m] += gains[m][count] * gains[m][count];
            first = std::min(first, delays[m][count]);
        }
        ++count;
    });
    double before = 0.0; // the share of their energy that arrives in Barron's early window (80 ms)
    for (int j = 0; j < count; ++j)
        for (int m = 0; m < 2; ++m)
            if (delays[m][j] < 0.08)
                before += 0.5 * gains[m][j] * gains[m][j] / energy[m];
    c.earlyShare = (float) before;
    for (int j = 0; j < kTaps; ++j) { // the early energy normalized, per microphone
        c.tapGainL[(std::size_t) j] = j < count ? (float) (gains[0][j] / std::sqrt(energy[0])) : 0.0f;
        c.tapGainR[(std::size_t) j] = j < count ? (float) (gains[1][j] / std::sqrt(energy[1])) : 0.0f;
        c.tapDelayL[(std::size_t) j] = std::max<std::uint32_t>(1, (std::uint32_t) std::lround(delays[0][j] * fs));
        c.tapDelayR[(std::size_t) j] = std::max<std::uint32_t>(1, (std::uint32_t) std::lround(delays[1][j] * fs));
    }
    // The network's input waits for the first reflection.
    c.predelay = (std::uint32_t) std::lround(std::clamp(first, 0.0, kMaxPredelayMs / 1000.0) * fs);
}

void Room::prepare(double sampleRate)
{
    fs = sampleRate;
    depth = (float) (kDepthS * fs);
    glide = (float) (1.0 - std::exp(-1.0 / (kGlideS * fs)));
    fadeStep = (float) (1.0 / (kFadeS * fs));
    imageCoef = (float) (1.0 - std::exp(-1.0 / (kImageS * fs)));
    hpCoef = (float) (1.0 / (1.0 + kTwoPi * kHighPassHz / fs));
    lpCoef = (float) (1.0 - std::exp(-kTwoPi * kReflectionLowPassHz / fs));
    for (int i = 0; i < kLines; ++i) {
        const double w = kTwoPi * (0.25 + 0.05 * i) / fs;
        rotC[(std::size_t) i] = (float) std::cos(w);
        rotS[(std::size_t) i] = (float) std::sin(w);
    }
    // The largest room: every ring sized for it (a second-order image is at most twice the room's diagonal away).
    Config largest;
    configure(largest, 1.0f, kMaxDistance, 30.0f);
    for (int i = 0; i < kLines; ++i)
        lines[(std::size_t) i].allocate(largest.lineDelay[(std::size_t) i] + (std::size_t) std::ceil(depth) + 4);
    const auto big = geometryFor(1.0, kMaxDistance);
    const double diagonal = std::sqrt(big.dim[0] * big.dim[0] + big.dim[1] * big.dim[1] + big.dim[2] * big.dim[2]);
    for (auto& d : pre)
        d.allocate((std::size_t) std::ceil(kMaxPredelayMs / 1000.0 * fs) + 2);
    mono.allocate((std::size_t) std::ceil(2.0 * diagonal / kSoundSpeed * fs) + 4);
    for (auto& channel : diffusers)
        for (int j = 0; j < kDiffusers; ++j)
            channel[(std::size_t) j].allocate(largest.diffuserDelay[(std::size_t) j] + 2);
    reset();
    // Start on the current settings, without gliding to them.
    configure(cfg, std::clamp(size.load(), 0.0f, 1.0f), std::clamp(distance.load(), kReferenceDistance, kMaxDistance),
              std::clamp(decaySeconds.load(), 0.1f, 30.0f));
    hasPending = false;
    fade = 1.0f;
    readSettings();
    updateTargets();
    wet = wetTarget;
    widthNow = widthTarget;
    early = earlyTarget;
    late = lateTarget;
    direct = directTarget;
    directWidth = directWidthTarget;
    air = airTarget;
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
    hpX[0] = hpX[1] = hpY[0] = hpY[1] = lpY[0] = lpY[1] = airY[0] = airY[1] = airZ[0] = airZ[1] = 0.0f;
    midEnergy = sideEnergy = 0.0f;
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
    const bool wasOn = on;
    on = enabled.load(std::memory_order_relaxed);
    wetTarget = on ? 1.0f : 0.0f;
    widthTarget = std::clamp(width.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float newSize = std::clamp(size.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float newDistance = std::clamp(distance.load(std::memory_order_relaxed), kReferenceDistance, kMaxDistance);
    const float decay = std::clamp(decaySeconds.load(std::memory_order_relaxed), 0.1f, 30.0f);
    const Config& wanted = hasPending ? pending : cfg;
    if (newSize != wanted.size || newDistance != wanted.distance || decay != wanted.decay) {
        configure(pending, newSize, newDistance, decay);
        hasPending = true;
    }
    const float bright = std::clamp(brightness.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float level = std::clamp(levelDb.load(std::memory_order_relaxed), -40.0f, 20.0f);
    const bool keep = keepLoudness.load(std::memory_order_relaxed);
    if (decay != appliedDecay || bright != appliedBrightness || level != appliedLevel || keep != appliedKeep || on != wasOn) {
        appliedDecay = decay;
        appliedBrightness = bright;
        appliedLevel = level;
        appliedKeep = keep;
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
    // The energies at the listening position (Barron), and keep loudness: the total of those at the keyboard.
    const double volume = volumeFor(cfg.size), r = cfg.heard;
    const double trim = std::pow(10.0, (double) appliedLevel / 10.0);
    const auto e = energiesAt(volume, t, r, trim);
    const double norm = appliedKeep ? std::sqrt(energiesAt(volume, t, kReferenceDistance, trim).total() / e.total()) : 1.0;
    // The early reflections have unit energy (normalized taps); the network's energy for a unit impulse on both
    // inputs (an input as correlated as a piano's): the energy stored (2), spread over the lines' samples, read
    // through unit output vectors while it decays as exp(-13.8 t / T60): 2 T60 fs / (13.8 sum(d)).
    // The decay at 1 kHz, where the high shelf already shortens it a little: each line's gain there, its T60, the mean.
    double tMid = 0.0;
    {
        const double w = kTwoPi * 1000.0 / fs, cw = std::cos(w), sw = std::sin(w);
        for (int i = 0; i < kLines; ++i) {
            const auto& ab = absTarget[(std::size_t) i];
            const double nr = ab.b0 + ab.b1 * cw, ni = -ab.b1 * sw, dr = 1.0 + ab.a1 * cw, di = -ab.a1 * sw;
            const double gain = ab.gain * std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
            tMid += -3.0 * cfg.lineDelay[(std::size_t) i] / (fs * std::log10(std::min(gain, 0.999999))) / kLines;
        }
    }
    // The estimate supposes the lines' energy spread evenly over their samples; measured, the network gives 0.7 dB
    // less (room_test, 250 Hz - 2 kHz, sizes 0.15 to 1, decays 2.2 to 5 s): corrected by that.
    const double unit = 0.85 * 2.0 * tMid * fs / (13.8 * total);
    // Barron's windows: the reflections arriving in the first 80 ms, and after. The early reflections of a large room
    // arrive partly after 80 ms (their share before is known from their delays); the network's start, partly before
    // in a small room: its energy is taken to arrive from the centroid of its first pass (the pre-delay, the
    // diffusers' (each one's energy centroid is its delay), the lines' mean) and to decay from there. The two gains
    // are those that meet both energies: x a + y f = early, x (1 - a) + y (1 - f) = late (x, y: the two parts'
    // energies), the nearest within x, y >= 0 otherwise.
    double diffusion = 0.0;
    for (const auto d : cfg.diffuserDelay)
        diffusion += d;
    const double start = (cfg.predelay + diffusion + total / kLines) / fs;
    const double f = start < 0.08 ? 1.0 - std::exp(-13.82 * (0.08 - start) / tMid) : 0.0;
    const double a = cfg.earlyShare;
    double x = e.early + e.late, y = 0.0;
    if (const double det = a - f; det > 1e-6) {
        x = (e.early * (1.0 - f) - e.late * f) / det;
        y = (a * e.late - (1.0 - a) * e.early) / det;
        if (x < 0.0) {
            x = 0.0;
            y = e.early + e.late;
        } else if (y < 0.0) {
            y = 0.0;
            x = e.early + e.late;
        }
    }
    earlyTarget = (float) (norm * std::sqrt(x));
    lateTarget = (float) (norm * std::sqrt(y / unit));
    // The direct sound: 1 / r, narrower, and the air: two first-order low-passes in cascade, each with half the
    // attenuation ISO 9613-1 gives at 10 kHz (a low-pass's attenuation grows as f^2 under its corner, as the air's
    // does over the piano's range; two of them keep to it an octave further). At the keyboard, or off: the engine's
    // sound, untouched.
    if (on && r > kReferenceDistance) {
        directTarget = (float) (norm * kReferenceDistance / r);
        directWidthTarget = (float) directWidthAt(r);
        const double f0 = std::min(10000.0, 0.4 * fs), w = kTwoPi * f0 / fs;
        const double g2 = std::pow(10.0, 0.5 * airAbsorptionDbPerMetre(f0) * (r - kReferenceDistance) / 10.0); // |H(f0)|^-2
        // (1 - a)^2 / (1 + a^2 - 2 a cos w) = 1 / g2, for a in [0, 1).
        const double bq = g2 - std::cos(w), cq = g2 - 1.0;
        airTarget = cq > 1e-9 ? (float) ((bq - std::sqrt(std::max(0.0, bq * bq - cq * cq))) / cq) : 0.0f;
    } else {
        directTarget = on ? (float) norm : 1.0f;
        directWidthTarget = 1.0f;
        airTarget = 0.0f;
    }
}

double Room::tailSeconds(const Settings& s) noexcept
{
    return acoustics(s).firstReflectionMs / 1000.0 + std::clamp((double) s.decaySeconds, 0.1, 30.0) * 80.0 / 60.0 + 0.2;
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
    [[maybe_unused]] NoDenormals noDenormals;
    const Vectors& v = kVectors;
    for (int k = 0; k < n; ++k) {
        // Glides, and the fade of a change of size or pre-delay.
        auto approach = [this](float& v, float to) noexcept { // gliding, then exactly there
            v += (to - v) * glide;
            if (std::abs(to - v) < 1e-6f)
                v = to;
        };
        approach(wet, wetTarget);
        approach(direct, directTarget);
        approach(directWidth, directWidthTarget);
        approach(air, airTarget);
        widthNow += (widthTarget - widthNow) * glide;
        early += (earlyTarget - early) * glide;
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
                early = earlyTarget;
                late = lateTarget;
            }
        } else if (fade < 1.0f) {
            fade = std::min(1.0f, fade + fadeStep);
        }

        // The input's mid and side energies (200 ms): a mono sum or a narrowed image of an input whose sides are not
        // fully correlated loses energy; the reflections and a distant direct sound carry the piano's whole power
        // (each ear gets all of it from a point): the gains that restore it, 1 for a mono input, sqrt(2) at most.
        const float inL = left[k], inR = right[k];
        const float midIn = 0.5f * (inL + inR), sideIn = 0.5f * (inL - inR);
        midEnergy += imageCoef * (midIn * midIn - midEnergy);
        sideEnergy += imageCoef * (sideIn * sideIn - sideEnergy);
        const float monoKeep = std::min(1.41421356f, std::sqrt((midEnergy + sideEnergy + 1e-12f) / (midEnergy + 1e-12f)));

        // The input, high-passed, into the pre-delay (stereo) and the reflections' line (mono).
        float x[2] = { inL, inR };
        for (int c = 0; c < 2; ++c) {
            hpY[c] = hpCoef * (hpY[c] + x[c] - hpX[c]);
            hpX[c] = x[c];
            x[c] = hpY[c];
            pre[c].data[pos & pre[c].mask] = x[c];
        }
        mono.data[pos & mono.mask] = monoKeep * 0.5f * (x[0] + x[1]);

        // Early reflections (each tap at its delay after the direct sound).
        float er[2] = { 0.0f, 0.0f };
        for (int j = 0; j < kTaps; ++j) {
            er[0] += cfg.tapGainL[(std::size_t) j] * mono.at(pos, cfg.tapDelayL[(std::size_t) j]);
            er[1] += cfg.tapGainR[(std::size_t) j] * mono.at(pos, cfg.tapDelayR[(std::size_t) j]);
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

        // The reflections: early and late, the width, on / off.
        const float wl = early * lpY[0] + late * lateL, wr = early * lpY[1] + late * lateR;
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * widthNow;
        const float g = wet * fade;
        const float outL = g * (mid + side), outR = g * (mid - side);
        wetPeak = std::max(wetPeak, std::max(std::abs(outL), std::abs(outR)));

        // The direct sound at the listening position: its level, its width (each ear keeping its energy), the air.
        // At the keyboard, the input.
        float dl = inL, dr = inR;
        if (direct != 1.0f || directWidth != 1.0f || air != 0.0f) {
            const float keep = std::min(1.41421356f, std::sqrt((midEnergy + sideEnergy + 1e-12f)
                                                               / (midEnergy + directWidth * directWidth * sideEnergy + 1e-12f)));
            const float mk = keep * midIn, sk = keep * sideIn * directWidth;
            airY[0] += (1.0f - air) * (mk + sk - airY[0]);
            airY[1] += (1.0f - air) * (mk - sk - airY[1]);
            airZ[0] += (1.0f - air) * (airY[0] - airZ[0]);
            airZ[1] += (1.0f - air) * (airY[1] - airZ[1]);
            dl = direct * airZ[0];
            dr = direct * airZ[1];
        } else {
            airY[0] = airZ[0] = inL; // the low-passes follow: no step when they start
            airY[1] = airZ[1] = inR;
        }
        left[k] = dl + outL;
        right[k] = dr + outR;

        ++pos;
        if ((++samples & 4095u) == 0) // the phasors back on the unit circle (by the sample count: block-independent)
            for (int i = 0; i < kLines; ++i) {
                const auto ii = (std::size_t) i;
                const float m = 1.0f / std::sqrt(lfoC[ii] * lfoC[ii] + lfoS[ii] * lfoS[ii]);
                lfoC[ii] *= m;
                lfoS[ii] *= m;
            }
    }
    if (!on && wet < 1e-6f && direct == 1.0f && directWidth == 1.0f && air == 0.0f) {
        reset(); // faded out, the direct sound back to the input: cleared, and bypassed from the next block
        return;
    }
    // States that decay once the input is silent, set to zero before they go denormal (WebAssembly has no
    // flush-to-zero; see ModeBank::flushDead).
    for (int c = 0; c < 2; ++c) {
        if (std::abs(hpY[c]) < 1e-20f)
            hpY[c] = 0.0f;
        if (std::abs(lpY[c]) < 1e-20f)
            lpY[c] = 0.0f;
        if (std::abs(airY[c]) < 1e-20f)
            airY[c] = 0.0f;
        if (std::abs(airZ[c]) < 1e-20f)
            airZ[c] = 0.0f;
    }
    if (midEnergy < 1e-30f)
        midEnergy = 0.0f;
    if (sideEnergy < 1e-30f)
        sideEnergy = 0.0f;
    // Asleep once the input has been silent longer than anything takes to cross the room (pre-delay, reflections,
    // diffusion, the longest line) and a whole block of reverberation stayed under -120 dB.
    std::uint32_t longest = cfg.predelay + cfg.lineDelay[kLines - 1];
    for (int j = 0; j < kTaps; ++j)
        longest = std::max({ longest, cfg.tapDelayL[(std::size_t) j], cfg.tapDelayR[(std::size_t) j] });
    for (int j = 0; j < kDiffusers; ++j)
        longest += cfg.diffuserDelay[(std::size_t) j];
    if (silent && !hasPending && silentSamples > 2 * (std::uint64_t) longest && wetPeak < 1e-6f) {
        reset();
        sleeping = true;
    }
}

} // namespace pf
