// The "PianoForge keyboard" video source: the 88 keys as played in PianoForge, the pedals, and the notes rising from
// the keys as they sound. The events come from the feed with their frames; the source shows the frame the PianoForge
// audio source stamps at that instant (the image meets the sound), or, without one, the application's newest frame
// less 20 ms.
//
// The drawing is a shader of the plugin's own (data/pianoforge-keyboard.effect): keys with rounded fronts, gradients
// and rims, a front edge that sinks as the key goes down, black keys casting their shadow; notes as rounded bars with
// a bright rim and a halo, fading as they rise. Light is added over them: beams rising from the keys held, a burst
// and a flare at the strike, the keys lit from within; and sparkles thrown up by each strike in a short stream, as
// many and as high as it is strong (their height, spread and direction chosen), a few more rising while a key is
// held. Colours are chosen by hue: each hand's (estimated from the notes: overlay.hpp), the white and the black
// keys', one for all, or the rainbow along the keyboard; the black keys take a deeper shade of theirs.
//
// The velocity shows (against the keyboard's strongest): soft notes thinner, deeper and more transparent, strong
// ones wider with a white-hot core, a ripple of light from the key at the strike. Powerful chords (enough notes
// struck together, hard enough: overlay.hpp) are events: a wave of light along the keyboard, the room above the keys
// lit, fireworks, the chord's notes tied by a bar of light, the keyboard shaking if asked.
//
// The notes last as the sound does: a pedal holding a note after its key is up draws its tail, fainter, until the
// pedal lets it go; along its bar a note fades as a string dies away (slower in the bass). The melody (the right
// hand's top notes) is a line of light through the notes. The rail can be a lacquered band reflecting the notes and
// lights above it; curtains of light over the room follow how loud the playing is; the whole can glow (a bloom: the
// keyboard drawn into an image, its bright coloured parts blurred and added over it). The keys shown: all 88, a
// smaller keyboard's, a range of one's own, or following the playing (gliding to the keys played lately). Styles set
// the whole look at once.
#include "common.hpp"
#include "overlay.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace pfobs {

namespace {

constexpr double kBeatSeconds = 0.25;
constexpr double kFallbackLatency = 0.02; // without an audio source: this far behind the application's newest frame
constexpr double kFlashSeconds = 0.12;    // the strike's light, its decay
constexpr double kReleaseSeconds = 0.08;  // a key coming back up
constexpr double kRecentStrike = 0.25;    // a strike seen later than this (a catch-up): no sparkles
constexpr int kSparksPerDraw = 128;       // sparkles drawn at once (their vertices sent together)
constexpr double kRippleSeconds = 0.45;   // a strike's ripple
constexpr double kWashSeconds = 0.55;     // a powerful chord's light over the room
constexpr double kWaveSeconds = 0.7;      // its wave along the keyboard
constexpr double kShakeSeconds = 0.25;    // its shake
constexpr int kLinePoints = 64;           // the melody's line: its points at most (the shader's array)
constexpr int kLevelFrames = 1024;        // the loudness: over this many frames

// The keyboard's own colours, as OBS keeps colours (0xAABBGGRR).
constexpr std::uint32_t kBedTop = 0xFF161618, kBedBottom = 0xFF060607;
constexpr std::uint32_t kRailTop = 0xFF3C3C42, kRailBottom = 0xFF141417, kRailEdge = 0xFF74747C, kFelt = 0xFF22164C;
constexpr std::uint32_t kIvoryTop = 0xFFD2D2D6, kIvory = 0xFFFBFBFC, kLipTop = 0xFFCACACF, kLipBottom = 0xFF95959D;
constexpr std::uint32_t kEbonyTop = 0xFF2E2E34, kEbonyBottom = 0xFF09090B, kFaceBack = 0xFF1B1B1F, kFaceFront = 0xFF44444C;
constexpr std::uint32_t kBlack = 0xFF000000, kWhite = 0xFFFFFFFF, kPillTop = 0xFF1C1C21, kPillBottom = 0xFF0E0E11;
constexpr std::uint32_t kPillRim = 0xFF4A4A54;

enum class ColorMode : int { single = 0, keys = 1, hands = 2, rainbow = 3 };

struct Settings {
    int width = 1280, trailHeight = 240;
    double trailSeconds = 4.0, offsetMs = 0.0;
    bool pedals = true;
    ColorMode mode = ColorMode::hands;
    int hueLeft = 217, hueRight = 25, hueWhite = 200, hueBlack = 275, hue = 200; // degrees
    int saturation = 90;                                                       // %
    bool adaptive = true; // the hands followed; else a fixed split
    int split = 60;
    int glow = 60;     // the lights, %
    int sparkles = 50;      // %
    int sparkleHeight = 70; // % of the room above the keys
    int sparkleSpread = 20; // degrees each side of their direction
    int sparkleTilt = 0;    // degrees from the vertical, positive to the right
    int afterglowMs = 400;
    bool flash = true;
    int accent = 70;        // how much the velocity shows, %
    int velocityTop = 112;  // the velocity shown at full strength (the keyboard's strongest)
    bool chords = true;     // powerful chords as events
    int chordNotes = 3;     // notes at least
    int chordThreshold = 75; // their mean strength at least, %
    int chordIntensity = 80; // %
    bool chordWave = true, chordWash = true, chordFireworks = true, chordBracket = true, chordShake = false;
    int range = 88;                       // the keys shown: 88, 76, 61, 49 or 37; 0: rangeFirst to rangeLast; -1: following
    int rangeFirst = 36, rangeLast = 96;  // (C2 to C7)
    int followOctaves = 3;                // following: at least this wide
    bool pedalTails = true, decay = true, melody = true;
    int tailWidth = 35, tailOpacity = 50, tailFine = 80; // a pedal's tail: its width (% of its note's), opacity, end
    int pedalLine = 30;   // the line of light along the keyboard while the sustain pedal is down, %
    int pedalGlow = 20;   // the soft light rising from it over the notes' feet, %
    int auroraHeight = 80, auroraSpeed = 50, auroraRest = 0; // the ambiance's curtains: how high, how fast, when quiet
    int bloom = 40, reflection = 50, ambiance = 35; // %
};

/// A velocity's strength, 0 to 1, against the keyboard's strongest.
float strengthOf(int velocity, const Settings& s)
{
    return std::clamp((float) velocity / (float) std::max(1, s.velocityTop), 0.0f, 1.0f);
}

/// The shader and its parameters, the unit square every shape is drawn on and the sparkles' vertices, made once on
/// the graphics thread.
struct Drawing {
    gs_effect_t* effect = nullptr;
    gs_vertbuffer_t *quad = nullptr, *sparks = nullptr;
    gs_technique_t *box = nullptr, *glow = nullptr, *beam = nullptr, *ring = nullptr, *sparkle = nullptr, *line = nullptr,
                   *aurora = nullptr, *image = nullptr, *bright = nullptr, *blur = nullptr, *bloom = nullptr;
    gs_eparam_t *quadOrigin = nullptr, *quadSize = nullptr, *boxCenter = nullptr, *boxHalf = nullptr, *boxRadius = nullptr,
                *boxBorder = nullptr, *boxSoft = nullptr, *fillTop = nullptr, *fillBottom = nullptr, *fillCurve = nullptr,
                *borderColor = nullptr, *lightColor = nullptr, *lightStrength = nullptr, *lightFrom = nullptr, *boxTaper = nullptr,
                *lightReach = nullptr, *fadeBand = nullptr, *time = nullptr, *seed = nullptr, *linePoints = nullptr,
                *lineCount = nullptr, *auroraLeft = nullptr, *auroraRight = nullptr, *auroraHeight = nullptr,
                *auroraSpeed = nullptr, *texture = nullptr, *texel = nullptr,
                *bloomGain = nullptr;
    bool failed = false;

    bool ready() const noexcept
    {
        return effect != nullptr && quad != nullptr && sparks != nullptr && box && glow && beam && ring && sparkle
               && quadOrigin && quadSize && boxCenter && boxHalf && boxRadius && boxBorder && boxSoft && fillTop
               && fillBottom && fillCurve && borderColor && lightColor && lightStrength && lightFrom && lightReach
               && fadeBand && time && seed && line && aurora && image && bright && blur && bloom && linePoints && lineCount
               && auroraLeft && auroraRight && auroraHeight && auroraSpeed && texture && texel && bloomGain && boxTaper;
    }
};

struct Keyboard {
    obs_source_t* source = nullptr;
    std::mutex lock; // the settings (written by the interface, read by the graphics thread)
    Settings settings;
    // The graphics thread's own; what is drawn also under `drawing` (OBS ticks and draws on its graphics thread, a
    // test draws from its own).
    std::mutex drawing;
    pf::AudioFeedReader feed;
    KeyboardState state;
    Sparkles sparkles;
    std::vector<PlayedNote> strikes;
    std::array<float, 128> owed {}; // the sparks owed to each key held
    ChordDetector chordFinder;
    ChordHit chordFound;
    struct ShownChord {
        ChordHit hit;
        float power = 0.0f; // how strongly it shows, 0 to 1
    };
    std::deque<ShownChord> chords; // the powerful chords still shown
    std::deque<std::pair<std::uint64_t, int>> recent; // the last minute's strikes: when (ns), their velocity
    std::atomic<int> strongest { 0 };                 // their strongest (read by the properties' button)
    ViewFollower follower; // the keys shown, following the playing
    KeyView view;
    float level = 0.0f; // how loud the playing is, 0 to 1 (the ambiance)
    std::vector<struct Bar> bars;
    gs_texrender_t *picture = nullptr, *glowA = nullptr, *glowB = nullptr; // the bloom's images
    std::uint64_t session = ~0ull, nextEvent = 0, lastTry = 0, lastBeat = 0;
    double frame = 0.0, rate = 0.0;
    int sparksWidth = 0; // the width the sparks were thrown over
    Drawing look;
    std::atomic<bool> following { false }, connected { false }; // (read by the properties)
};

/// The keys the settings show: a smaller keyboard's (76: E1 to G7, 61: C2 to C7, 49: C2 to C6, 37: C3 to C6), a range
/// of one's own, or all 88 (following the playing starts there).
KeyView rangeView(const Settings& s)
{
    switch (s.range) {
    case 76:
        return viewOf(28, 103);
    case 61:
        return viewOf(36, 96);
    case 49:
        return viewOf(36, 84);
    case 37:
        return viewOf(48, 84);
    case 0:
        return viewOf(s.rangeFirst, s.rangeLast);
    default:
        return KeyView {};
    }
}

/// The white keys the keys' length is made for: the range's (keys of a piano's proportions); following the playing,
/// between its narrowest view and the whole keyboard.
float referenceWhites(const Settings& s)
{
    if (s.range < 0)
        return 0.5f * ((float) kWhiteKeys + 7.0f * (float) s.followOctaves);
    return std::max(1.0f, rangeView(s).count);
}

/// The band of the keys (the rail and the keys, as long as a piano's for the width of the keys shown).
int keyboardHeight(const Settings& s)
{
    return std::max(24, (int) std::lround(6.5 * s.width / referenceWhites(s)));
}

int pedalHeight(const Settings& s)
{
    return s.pedals ? std::max(6, keyboardHeight(s) / 8) : 0;
}

/// Above the keys: the notes, or room for the lights, sparkles and fireworks (0.6 of a key's length).
int topHeight(const Settings& s)
{
    const bool room = s.glow > 0 || s.sparkles > 0 || (s.chords && s.chordFireworks) || s.ambiance > 0;
    return std::max(s.trailHeight, room ? (int) std::lround(0.6 * keyboardHeight(s)) : 0);
}

/// The rail over the keys: thin, or a lacquered band reflecting what is above it.
float thinRail(const Settings& s)
{
    return std::max(2.0f, std::round((float) keyboardHeight(s) * 0.05f));
}

float railHeight(const Settings& s)
{
    return s.reflection > 0 ? std::round(0.17f * (float) keyboardHeight(s)) : thinRail(s);
}

int totalHeight(const Settings& s)
{
    return topHeight(s) + (int) railHeight(s) + keyboardHeight(s) - (int) thinRail(s) + pedalHeight(s);
}

/// Where things are, in the source's pixels: the area above the keys (0 to `top`), the rail, the keys (from
/// `keysTop` to `bottom`), the pedals under them; the keys shown (`view`) and their shapes.
struct Layout {
    float width = 0, top = 0, rail = 0, keysTop = 0, keysHeight = 0, blackHeight = 0, white = 0, bottom = 0;
    float pedalTop = 0, pedalHeight = 0, height = 0;
    KeyView view;
    bool lacquer = false; // the rail a lacquered band
    KeyShape shape(int key) const noexcept { return keyShape(key, width, view); }
    bool shows(const KeyShape& k) const noexcept { return k.x + k.width > 0.0f && k.x < width; }
};

Layout layoutOf(const Settings& s, const KeyView& view)
{
    Layout l;
    const float band = (float) keyboardHeight(s), thin = thinRail(s);
    l.view = view;
    l.width = (float) s.width;
    l.top = (float) topHeight(s);
    l.lacquer = s.reflection > 0;
    l.rail = railHeight(s);
    l.keysTop = l.top + l.rail;
    l.keysHeight = band - thin;
    l.blackHeight = 0.64f * l.keysHeight;
    l.white = l.width / std::max(1.0f, view.count);
    l.bottom = l.keysTop + l.keysHeight;
    l.pedalTop = l.bottom;
    l.pedalHeight = (float) pedalHeight(s);
    l.height = l.bottom + l.pedalHeight;
    return l;
}

/// The keys shown now: the range asked, or the view following the playing.
KeyView viewNow(const Keyboard& k, const Settings& s)
{
    return s.range < 0 ? k.view : rangeView(s);
}

/// A key's hue, by the mode (the rainbow: red in the bass to violet in the treble).
double hueOf(const Settings& s, int key, int hand)
{
    switch (s.mode) {
    case ColorMode::single:
        return s.hue;
    case ColorMode::keys:
        return isBlack(key) ? s.hueBlack : s.hueWhite;
    case ColorMode::rainbow:
        return 280.0 * (key - kFirstKey) / (double) (kLastKey - kFirstKey);
    case ColorMode::hands:
    default:
        return hand == kLeftHand ? s.hueLeft : s.hueRight;
    }
}

/// A hue's colours: `main` a key pressed and its note (deeper for a black key), `deep` its shade, `light` the light it
/// gives, `pale` rims and sparkles.
struct Tint {
    std::uint32_t main = kWhite, deep = kWhite, light = kWhite, pale = kWhite;
};

Tint tintOf(double hue, const Settings& s, bool black)
{
    const double sat = s.saturation / 100.0;
    return { hslColor(hue, sat, black ? 0.42 : 0.55), hslColor(hue, sat, black ? 0.26 : 0.36), hslColor(hue, sat, 0.62),
             hslColor(hue, sat * 0.85, 0.84) };
}

Tint tintOf(const Settings& s, int key, int hand)
{
    return tintOf(hueOf(s, key, hand), s, isBlack(key));
}

/// The ambiance's colours, left and right: the hands', the white and the black keys', one, or the rainbow's ends.
std::pair<std::uint32_t, std::uint32_t> ambianceColors(const Settings& s)
{
    switch (s.mode) {
    case ColorMode::single:
        return { tintOf(s.hue, s, false).light, tintOf(s.hue, s, false).light };
    case ColorMode::keys:
        return { tintOf(s.hueWhite, s, false).light, tintOf(s.hueBlack, s, false).light };
    case ColorMode::rainbow:
        return { tintOf(0.0, s, false).light, tintOf(280.0, s, false).light };
    case ColorMode::hands:
    default:
        return { tintOf(s.hueLeft, s, false).light, tintOf(s.hueRight, s, false).light };
    }
}

/// The pedals' colours: soft, sostenuto, sustain (left to right).
Tint pedalTint(const Settings& s, int i)
{
    switch (s.mode) {
    case ColorMode::single:
        return tintOf(s.hue, s, false);
    case ColorMode::keys:
        return tintOf(s.hueWhite, s, false);
    case ColorMode::rainbow:
        return tintOf(40.0 + 100.0 * i, s, false);
    case ColorMode::hands:
    default:
        return tintOf(i == 0 ? s.hueLeft : s.hueRight, s, false);
    }
}

/// How lit a key is at `frame`: `level` (held: by its velocity; up: going out over the afterglow), `flash` (the
/// strike, by its velocity, decaying over kFlashSeconds) and `depth` (how far down it is: 1 held, back up over
/// kReleaseSeconds).
struct Lit {
    double level = 0.0, flash = 0.0, depth = 0.0;
};

Lit lightOf(const KeyLight& l, double frame, double rate, const Settings& s)
{
    Lit lit;
    if (!l.struck || rate <= 0.0)
        return lit;
    const double st = strengthOf(l.lastVelocity, s), accent = s.accent / 100.0;
    const double strength = (0.55 + 0.45 * st) * (1.0 - accent) + (0.35 + 0.65 * st) * accent;
    if (l.velocity > 0) {
        lit.level = strength;
        lit.depth = 1.0;
    } else {
        const double up = (frame - (double) l.released) / rate;
        if (up >= 0.0) {
            lit.depth = std::max(0.0, 1.0 - up / kReleaseSeconds);
            const double t = s.afterglowMs > 0 ? up / (s.afterglowMs * 1e-3) : 1.0;
            if (t < 1.0)
                lit.level = strength * (1.0 - t) * (1.0 - t);
        }
    }
    const double since = (frame - (double) l.pressed) / rate;
    if (s.flash && since >= 0.0 && since < 1.0)
        lit.flash = st * std::exp(-since / kFlashSeconds);
    return lit;
}

/// How strongly a powerful chord shows (0 to 1): the intensity chosen, more for a chord well over the threshold and
/// for one of more notes than asked.
float chordPower(const ChordHit& hit, const Settings& s)
{
    const float threshold = s.chordThreshold / 100.0f;
    const float over = std::clamp((hit.strength - threshold) / std::max(0.05f, 1.0f - threshold), 0.0f, 1.0f);
    const float more = std::clamp((float) ((int) hit.notes.size() - s.chordNotes) / 3.0f, 0.0f, 1.0f);
    return s.chordIntensity / 100.0f * (0.55f + 0.3f * over + 0.15f * more);
}

/// A chord's notes by hand: for each hand playing in it, the span of its keys (pixels) and the key at its middle.
struct HandPart {
    float x0 = 0.0f, x1 = 0.0f;
    int count = 0, key = 0, hand = kRightHand;
};

std::vector<HandPart> partsOf(const ChordHit& hit, const Layout& l)
{
    std::vector<HandPart> parts;
    for (int hand : { kLeftHand, kRightHand }) {
        HandPart part;
        part.hand = hand;
        int low = 128, high = -1;
        for (const auto& n : hit.notes) {
            if ((n.hand == kLeftHand) != (hand == kLeftHand) || n.key < kFirstKey || n.key > kLastKey)
                continue;
            const auto shape = l.shape(n.key);
            part.x0 = part.count == 0 ? shape.x : std::min(part.x0, shape.x);
            part.x1 = part.count == 0 ? shape.x + shape.width : std::max(part.x1, shape.x + shape.width);
            low = std::min(low, n.key);
            high = std::max(high, n.key);
            ++part.count;
        }
        if (part.count > 0) {
            part.key = (low + high) / 2;
            parts.push_back(part);
        }
    }
    return parts;
}

/// A powerful chord's fireworks: a rocket from each hand playing in it (one more from a chord of six notes or more),
/// rising to about half the room and bursting, in that hand's hue.
void launchFireworks(Keyboard& k, const Settings& s, const Layout& l, const ChordHit& hit, float power)
{
    if (l.top < 30.0f || power <= 0.0f)
        return;
    auto parts = partsOf(hit, l);
    if (hit.notes.size() >= 6 && !parts.empty()) {
        HandPart middle = parts.front();
        middle.x0 = middle.x1 = 0.5f * (parts.front().x0 + parts.back().x1);
        middle.key = (parts.front().key + parts.back().key) / 2;
        parts.push_back(middle);
    }
    int i = 0;
    for (const auto& part : parts) {
        const Tint tint = tintOf(s, part.key, part.hand);
        k.sparkles.rocket(0.5f * (part.x0 + part.x1), l.top, l.top * (0.42f + 0.12f * power), 0.45f, 0.09f * (float) i,
                          l.white * 0.45f, tint.light, (int) std::lround(18.0f + 24.0f * power), l.top * (0.25f + 0.12f * power),
                          0.9f + 0.3f * power, 0.35f * l.top);
        ++i;
    }
}

void tick(void* data, float seconds)
{
    auto* k = static_cast<Keyboard*>(data);
    Settings s;
    {
        const std::lock_guard<std::mutex> guard(k->lock);
        s = k->settings;
    }
    const std::uint64_t now = os_gettime_ns();
    if (!k->feed.isOpen()) {
        if (now - k->lastTry < 500'000'000ull)
            return;
        k->lastTry = now;
        if (!k->feed.open(pf::FeedReaderKind::keyboard)) {
            k->connected = false;
            return;
        }
    }
    pf::AudioFeedReader::Format format;
    if (!k->feed.format(format)) {
        k->connected = false;
        return;
    }
    k->connected = true;
    const std::lock_guard<std::mutex> drawing(k->drawing);
    if (format.session != k->session) { // a new format (or a first one): the keys as the last events leave them
        k->session = format.session;
        k->rate = format.sampleRate;
        k->state.reset();
        k->state.setRate(k->rate);
        k->sparkles.clear();
        k->chordFinder.reset();
        k->chordFinder.setRate(k->rate);
        k->chords.clear();
        k->follower.reset(KeyView {});
        k->level = 0.0f;
        const std::uint64_t written = k->feed.eventsWritten();
        k->nextEvent = written - std::min<std::uint64_t>(written, pf::AudioFeedReader::kEventCapacity / 4);
    }
    k->state.setFixedSplit(!s.adaptive, s.split);
    pf::FeedEvent events[256];
    for (int guard = 0; guard < 64; ++guard) {
        const int n = k->feed.readEvents(k->nextEvent, events, 256);
        if (n < 0) { // fell behind: the newest quarter of the ring again
            const std::uint64_t written = k->feed.eventsWritten();
            k->nextEvent = written - std::min<std::uint64_t>(written, pf::AudioFeedReader::kEventCapacity / 4);
            continue;
        }
        for (int i = 0; i < n; ++i)
            k->state.add(events[i]);
        k->nextEvent += (std::uint64_t) n;
        if (n < 256)
            break;
    }
    // The frame shown: the audio source's, now; else the application's newest less a little.
    Timeline t;
    if (latestTimeline(t) && t.session == format.session) {
        k->frame = t.feedFrame + (double) (now - std::min(now, t.obsNs)) * 1e-9 * t.feedRate;
        k->following = true;
    } else {
        std::uint64_t frames = 0, ns = 0;
        const std::uint64_t feedNow = pf::feedClockNs();
        if (k->feed.clock(frames, ns))
            k->frame = (double) frames + (double) (feedNow - std::min(feedNow, ns)) * 1e-9 * k->rate - kFallbackLatency * k->rate;
        k->following = false;
    }
    k->frame -= s.offsetMs * 1e-3 * k->rate;
    const auto keep = (std::uint64_t) std::max(0.0, (s.trailSeconds + 0.5) * k->rate);
    k->state.advance((std::uint64_t) std::max(0.0, k->frame), keep);

    const float dt = std::clamp(seconds, 0.0f, 0.1f);
    // The keys shown: the range asked, or those played lately (held, ringing, struck in the last 4 s).
    if (s.range < 0) {
        int low = 128, high = -1;
        for (const auto& n : k->state.notes())
            if (n.open() || n.ringing() || k->frame - (double) n.start < 4.0 * k->rate) {
                low = std::min(low, n.key);
                high = std::max(high, n.key);
            }
        k->view = k->follower.step(low, high, dt, 7.0f * (float) s.followOctaves);
    } else {
        k->view = rangeView(s);
        k->follower.reset(k->view);
    }
    const Layout l = layoutOf(s, k->view);
    const float amount = s.sparkles / 100.0f;
    // How loud the playing is (the ambiance): the heard mix over the last frames shown, quick to rise, slow to fall.
    if (s.ambiance > 0 && k->frame > (double) kLevelFrames) {
        float samples[2 * kLevelFrames];
        const int n = k->feed.read(pf::FeedMix::heard, (std::uint64_t) k->frame - kLevelFrames, samples, kLevelFrames);
        double sum = 0.0;
        for (int i = 0; i < 2 * std::max(0, n); ++i)
            sum += (double) samples[i] * samples[i];
        const double rms = n > 0 ? std::sqrt(sum / (2.0 * n)) : 0.0;
        const float target = std::clamp((float) ((20.0 * std::log10(std::max(rms, 1e-6)) + 48.0) / 34.0), 0.0f, 1.0f);
        const float tau = target > k->level ? 0.06f : 0.9f;
        k->level += (target - k->level) * (1.0f - std::exp(-dt / tau));
    } else {
        k->level = 0.0f;
    }
    const bool fireworks = s.chords && s.chordFireworks;
    k->state.takeStrikes(k->strikes);
    if (k->sparksWidth != s.width || (amount <= 0.0f && !fireworks)) {
        k->sparkles.clear();
        k->sparksWidth = s.width;
    }
    // The last minute's strongest strike (the velocity button).
    for (const auto& n : k->strikes)
        k->recent.emplace_back(now, n.velocity);
    while (!k->recent.empty() && now - k->recent.front().first > 60'000'000'000ull)
        k->recent.pop_front();
    int strongest = 0;
    for (const auto& r : k->recent)
        strongest = std::max(strongest, r.second);
    k->strongest = strongest;
    // The powerful chords: announced as they qualify (a fireworks for each), shown while their effects last.
    if (k->rate > 0.0) {
        if (s.chords) {
            k->chordFinder.configure(s.chordNotes, s.chordThreshold / 100.0f);
            for (const auto& n : k->strikes)
                if ((k->frame - (double) n.start) / k->rate <= kRecentStrike)
                    k->chordFinder.add(n, strengthOf(n.velocity, s));
            while (k->chordFinder.poll((std::uint64_t) std::max(0.0, k->frame), k->chordFound)) {
                k->chords.push_back({ k->chordFound, chordPower(k->chordFound, s) });
                if (fireworks)
                    launchFireworks(*k, s, l, k->chordFound, k->chords.back().power);
            }
        }
        const double shown = std::max({ kWaveSeconds, kWashSeconds, s.trailSeconds + 0.5 });
        while (!k->chords.empty() && (k->frame - (double) k->chords.front().hit.frame) / k->rate > shown)
            k->chords.pop_front();
        if (!s.chords)
            k->chords.clear();
    }
    // The sparkles: a stream at each strike, a few rising from the keys held, as high and in the direction chosen;
    // they move on, the fireworks with them.
    if (amount > 0.0f && l.top > 4.0f && k->rate > 0.0) {
        constexpr float radians = 3.14159265f / 180.0f;
        const float height = s.sparkleHeight / 100.0f * l.top, tilt = s.sparkleTilt * radians, spread = s.sparkleSpread * radians;
        for (const auto& n : k->strikes) {
            if (n.key < kFirstKey || n.key > kLastKey || (k->frame - (double) n.start) / k->rate > kRecentStrike)
                continue;
            const auto shape = l.shape(n.key);
            const Tint tint = tintOf(s, n.key, n.hand);
            const float v = strengthOf(n.velocity, s);
            SparkBurst b;
            b.x = shape.x + 0.5f * shape.width;
            b.y = l.top;
            b.width = shape.width * 0.8f;
            b.height = height * (0.55f + 0.45f * v); // a soft note's lower
            b.tilt = tilt;
            b.spread = spread;
            b.stagger = 0.25f;
            b.size = l.white * (0.4f + 0.35f * v);
            b.life = 1.0f + 0.8f * v;
            const int count = (int) std::lround(amount * (3.0f + 15.0f * v));
            b.count = count / 2; // half in the hue,
            b.color = tint.light;
            k->sparkles.burst(b);
            b.count = count - count / 2; // half paler
            b.color = tint.pale;
            k->sparkles.burst(b);
        }
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            const auto& light = k->state.light(key);
            auto& owed = k->owed[(std::size_t) key];
            if (light.velocity <= 0) {
                owed = 0.0f;
                continue;
            }
            owed += dt * amount * (0.8f + 2.5f * strengthOf(light.velocity, s));
            if (owed < 1.0f)
                continue;
            const auto shape = l.shape(key);
            SparkBurst b;
            b.x = shape.x + 0.5f * shape.width;
            b.y = l.top;
            b.width = shape.width * 0.7f;
            b.count = (int) owed;
            b.height = 0.4f * height;
            b.tilt = tilt;
            b.spread = 0.6f * spread;
            b.stagger = 0.0f;
            b.size = l.white * 0.3f;
            b.life = 1.1f;
            b.color = tintOf(s, key, light.hand).light;
            k->sparkles.burst(b);
            owed -= (float) b.count;
        }
    }
    if (l.top > 4.0f)
        k->sparkles.step(dt, 0.3f * l.white);
    if (now - k->lastBeat > (std::uint64_t) (kBeatSeconds * 1e9)) {
        k->feed.heartbeat(pf::FeedMix::heard, 0.0f);
        k->lastBeat = now;
    }
}

/// The unit square (0 to 1, its texture coordinates the same), drawn scaled to each shape's quad.
gs_vertbuffer_t* makeQuad()
{
    gs_vb_data* data = gs_vbdata_create();
    data->num = 4;
    data->points = static_cast<vec3*>(bmalloc(sizeof(vec3) * 4));
    data->num_tex = 1;
    data->tvarray = static_cast<gs_tvertarray*>(bzalloc(sizeof(gs_tvertarray)));
    data->tvarray[0].width = 2;
    data->tvarray[0].array = bmalloc(sizeof(vec2) * 4);
    auto* uv = static_cast<vec2*>(data->tvarray[0].array);
    const float corners[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } };
    for (int i = 0; i < 4; ++i) {
        vec3_set(&data->points[i], corners[i][0], corners[i][1], 0.0f);
        vec2_set(&uv[i], corners[i][0], corners[i][1]);
    }
    return gs_vertexbuffer_create(data, 0); // (it keeps the data)
}

/// The sparkles' vertices, kSparksPerDraw at a time (two triangles each): positions, colours, texture coordinates
/// (-1 to 1 across a sparkle), and each one's brightness and phase.
gs_vertbuffer_t* makeSparkBuffer()
{
    constexpr std::size_t n = (std::size_t) kSparksPerDraw * 6;
    gs_vb_data* data = gs_vbdata_create();
    data->num = n;
    data->points = static_cast<vec3*>(bzalloc(sizeof(vec3) * n));
    data->colors = static_cast<uint32_t*>(bzalloc(sizeof(uint32_t) * n));
    data->num_tex = 2;
    data->tvarray = static_cast<gs_tvertarray*>(bzalloc(sizeof(gs_tvertarray) * 2));
    data->tvarray[0].width = 2;
    data->tvarray[0].array = bzalloc(sizeof(vec2) * n);
    data->tvarray[1].width = 4;
    data->tvarray[1].array = bzalloc(sizeof(vec4) * n);
    return gs_vertexbuffer_create(data, GS_DYNAMIC);
}

void freeDrawing(Drawing& d)
{
    if (d.effect != nullptr)
        gs_effect_destroy(d.effect);
    if (d.quad != nullptr)
        gs_vertexbuffer_destroy(d.quad);
    if (d.sparks != nullptr)
        gs_vertexbuffer_destroy(d.sparks);
    d.effect = nullptr;
    d.quad = nullptr;
    d.sparks = nullptr;
}

void loadDrawing(Drawing& d)
{
    char* path = obs_module_file("pianoforge-keyboard.effect");
    char* errors = nullptr;
    d.effect = path != nullptr ? gs_effect_create_from_file(path, &errors) : nullptr;
    if (d.effect != nullptr) {
        auto param = [&](const char* name) { return gs_effect_get_param_by_name(d.effect, name); };
        d.quad = makeQuad();
        d.sparks = makeSparkBuffer();
        d.box = gs_effect_get_technique(d.effect, "Box");
        d.glow = gs_effect_get_technique(d.effect, "Glow");
        d.beam = gs_effect_get_technique(d.effect, "Beam");
        d.ring = gs_effect_get_technique(d.effect, "Ring");
        d.sparkle = gs_effect_get_technique(d.effect, "Sparkles");
        d.line = gs_effect_get_technique(d.effect, "Line");
        d.aurora = gs_effect_get_technique(d.effect, "Aurora");
        d.image = gs_effect_get_technique(d.effect, "Image");
        d.bright = gs_effect_get_technique(d.effect, "Bright");
        d.blur = gs_effect_get_technique(d.effect, "Blur");
        d.bloom = gs_effect_get_technique(d.effect, "Bloom");
        d.linePoints = param("line_points");
        d.lineCount = param("line_count");
        d.auroraLeft = param("aurora_left");
        d.auroraRight = param("aurora_right");
        d.auroraHeight = param("aurora_height");
        d.auroraSpeed = param("aurora_speed");
        d.texture = param("image");
        d.texel = param("texel");
        d.bloomGain = param("bloom_gain");
        d.boxTaper = param("box_taper");
        d.quadOrigin = param("quad_origin");
        d.quadSize = param("quad_size");
        d.boxCenter = param("box_center");
        d.boxHalf = param("box_half");
        d.boxRadius = param("box_radius");
        d.boxBorder = param("box_border");
        d.boxSoft = param("box_soft");
        d.fillTop = param("fill_top");
        d.fillBottom = param("fill_bottom");
        d.fillCurve = param("fill_curve");
        d.borderColor = param("border_color");
        d.lightColor = param("light_color");
        d.lightStrength = param("light_strength");
        d.lightFrom = param("light_from");
        d.lightReach = param("light_reach");
        d.fadeBand = param("fade_band");
        d.time = param("time");
        d.seed = param("seed");
    }
    if (!d.ready()) {
        blog(LOG_WARNING, "[pianoforge] keyboard: its drawing (%s) did not load, plain keys instead: %s", path != nullptr ? path : "?",
             errors != nullptr ? errors : "a technique, a parameter or its vertices missing");
        freeDrawing(d);
        d = Drawing {};
        d.failed = true;
    }
    bfree(errors);
    bfree(path);
}

/// A rounded box's look: its gradient (top to bottom, `curve` its shape), its rim, a light within it (from its top,
/// `lightFrom` 0, or its bottom, 1), its corners and edge (soft: a shadow).
struct BoxStyle {
    std::uint32_t top = kWhite, bottom = kWhite;
    float alpha = 1.0f, curve = 1.0f, fadeTo = 1.0f; // the bottom's alpha: fadeTo of the top's
    float taper = 1.0f;                              // the bottom's width: taper of the top's
    std::uint32_t rim = kBlack;
    float rimAlpha = 0.0f, rimWidth = 0.0f;
    std::uint32_t light = kWhite;
    float lightStrength = 0.0f, lightFrom = 1.0f;
    float radius = 0.0f, soft = 1.0f;
};

/// Draws with the shader: boxes blended over, lights added; each shape on the unit square scaled to its quad, cut at
/// the source's edges. A technique stays begun while shapes of its kind follow one another, each shape sending only
/// what it changed; every shape sets all the parameters its shader reads (OBS clears an effect's values when a
/// technique ends, and Direct3D refuses a draw whose shader has a parameter never set).
class Painter {
public:
    Painter(const Drawing& d, float w, float h, bool linearColors)
        : look(d), width(w), height(h), linear(linearColors), clipX1(w), clipY1(h)
    {
    }
    ~Painter() { finish(); }
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    void over() const { gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); }
    void add() const { gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); }
    /// The shapes drawn next fade upward from y = `whole` to nothing at y = `none`.
    void fade(float none, float whole)
    {
        fadeNone = none;
        fadeWhole = whole;
    }
    void noFade() { fade(-2.0f, -1.0f); }
    void time(float seconds) { now = seconds; }
    /// What is drawn next is cut to this rectangle (the source's own pixels), until unclip().
    void clip(float x0, float y0, float x1, float y1)
    {
        clipX0 = x0;
        clipY0 = y0;
        clipX1 = x1;
        clipY1 = y1;
    }
    void unclip() { clip(0.0f, 0.0f, width, height); }
    /// Everything drawn moves by (x, y) pixels (the shake), still cut at the source's edges.
    void offset(float x, float y)
    {
        dx = x;
        dy = y;
    }

    /// A rounded box from (x, y), `w` by `h`; `squareTop`: its top corners square (it goes on under what is above).
    void box(float x, float y, float w, float h, const BoxStyle& st, bool squareTop = false)
    {
        if (w <= 0.0f || h <= 0.0f)
            return;
        use(look.box);
        const float r = std::min(st.radius, 0.5f * std::min(w, squareTop ? 2.0f * h : h)), extra = squareTop ? r : 0.0f;
        pair(look.boxCenter, x + 0.5f * w, y + 0.5f * (h - extra));
        pair(look.boxHalf, 0.5f * w, 0.5f * (h + extra));
        gs_effect_set_float(look.boxRadius, r);
        gs_effect_set_float(look.boxBorder, st.rimWidth);
        gs_effect_set_float(look.boxSoft, st.soft);
        gs_effect_set_float(look.boxTaper, st.taper);
        color(look.fillTop, st.top, st.alpha);
        color(look.fillBottom, st.bottom, st.alpha * st.fadeTo);
        gs_effect_set_float(look.fillCurve, st.curve);
        color(look.borderColor, st.rim, st.rimAlpha);
        color(look.lightColor, st.light, 1.0f);
        gs_effect_set_float(look.lightStrength, st.lightStrength);
        gs_effect_set_float(look.lightFrom, st.lightFrom);
        const float m = std::max(1.0f, st.soft);
        draw(x - m, squareTop ? y : y - m, x + w + m, y + h + m);
    }

    /// A halo around a rounded box centred on (cx, cy): `strength` within it, falling off over `reach` pixels.
    void glow(float cx, float cy, float halfW, float halfH, float radius, float reach, std::uint32_t c, float strength)
    {
        if (strength <= 0.003f)
            return;
        use(look.glow);
        pair(look.boxCenter, cx, cy);
        pair(look.boxHalf, halfW, halfH);
        gs_effect_set_float(look.boxRadius, std::min(radius, std::min(halfW, halfH)));
        gs_effect_set_float(look.lightReach, reach);
        color(look.lightColor, c, 1.0f);
        gs_effect_set_float(look.lightStrength, strength);
        const float m = 3.0f * reach;
        draw(cx - halfW - m, cy - halfH - m, cx + halfW + m, cy + halfH + m);
    }

    /// A beam rising `h` pixels from (cx, foot), `halfW` across each side.
    void beam(float cx, float foot, float halfW, float h, std::uint32_t c, float strength, float seed)
    {
        if (strength <= 0.003f || h <= 1.0f)
            return;
        use(look.beam);
        pair(look.boxCenter, cx, foot - 0.5f * h);
        pair(look.boxHalf, halfW, 0.5f * h);
        color(look.lightColor, c, 1.0f);
        gs_effect_set_float(look.lightStrength, strength);
        gs_effect_set_float(look.seed, seed);
        draw(cx - halfW, foot - h, cx + halfW, foot);
    }

    /// A ripple: a ring of light around the ellipse of radii (rx, ry) centred on (cx, cy), `thickness` pixels thick.
    void ring(float cx, float cy, float rx, float ry, float thickness, std::uint32_t c, float strength)
    {
        if (strength <= 0.003f || rx <= 0.5f || ry <= 0.5f)
            return;
        use(look.ring);
        pair(look.boxCenter, cx, cy);
        pair(look.boxHalf, rx, ry);
        gs_effect_set_float(look.lightReach, thickness);
        color(look.lightColor, c, 1.0f);
        gs_effect_set_float(look.lightStrength, strength);
        const float m = 3.0f * thickness;
        draw(cx - rx - m, cy - ry - m, cx + rx + m, cy + ry + m);
    }

    /// A line of light through `points` (x, y, brightness; kLinePoints at most): a thin core in a halo `reach` wide.
    void line(const std::vector<std::array<float, 3>>& points, std::uint32_t c, float strength, float reach)
    {
        const std::size_t n = std::min<std::size_t>(points.size(), kLinePoints);
        if (n < 2 || strength <= 0.003f)
            return;
        use(look.line);
        float data[kLinePoints * 4] = {};
        float x0 = points[0][0], y0 = points[0][1], x1 = x0, y1 = y0;
        for (std::size_t i = 0; i < n; ++i) {
            data[4 * i] = points[i][0];
            data[4 * i + 1] = points[i][1];
            data[4 * i + 2] = points[i][2];
            x0 = std::min(x0, points[i][0]);
            x1 = std::max(x1, points[i][0]);
            y0 = std::min(y0, points[i][1]);
            y1 = std::max(y1, points[i][1]);
        }
        gs_effect_set_val(look.linePoints, data, sizeof(data));
        gs_effect_set_float(look.lineCount, (float) n);
        color(look.lightColor, c, 1.0f);
        gs_effect_set_float(look.lightStrength, strength);
        gs_effect_set_float(look.lightReach, reach);
        const float m = 4.0f * reach + 2.0f;
        draw(x0 - m, y0 - m, x1 + m, y1 + m);
    }

    /// Curtains of light over the rectangle (x0, y0)-(x1, y1), rising from its bottom, `left` to `right` across,
    /// `height` of it high (1: up to 0.9), swaying at `speed` (1: as made).
    void aurora(float x0, float y0, float x1, float y1, std::uint32_t left, std::uint32_t right, float strength, float height,
                float speed)
    {
        if (strength <= 0.003f || x1 <= x0 || y1 <= y0)
            return;
        use(look.aurora);
        pair(look.boxCenter, 0.5f * (x0 + x1), 0.5f * (y0 + y1));
        pair(look.boxHalf, 0.5f * (x1 - x0), 0.5f * (y1 - y0));
        color(look.auroraLeft, left, 1.0f);
        color(look.auroraRight, right, 1.0f);
        gs_effect_set_float(look.lightStrength, strength);
        gs_effect_set_float(look.auroraHeight, height);
        gs_effect_set_float(look.auroraSpeed, speed);
        draw(x0, y0, x1, y1);
    }

    /// The sparkles, kSparksPerDraw to a draw: each a star reaching its size, cut at the source's edges.
    void sparkles(const std::vector<Spark>& list)
    {
        gs_vb_data* data = gs_vertexbuffer_get_data(look.sparks);
        if (data == nullptr || list.empty())
            return;
        auto* uv = static_cast<vec2*>(data->tvarray[0].array);
        auto* spark = static_cast<vec4*>(data->tvarray[1].array);
        std::size_t at = 0;
        int count = 0;
        auto flush = [&] {
            if (count == 0)
                return;
            gs_vertexbuffer_flush(look.sparks);
            use(look.sparkle);
            gs_effect_set_float(look.time, now);
            gs_effect_update_params(look.effect);
            if (current != nullptr) {
                gs_load_vertexbuffer(look.sparks);
                gs_load_indexbuffer(nullptr);
                gs_draw(GS_TRIS, 0, (uint32_t) count * 6);
            }
            at = 0;
            count = 0;
        };
        for (const auto& sp : list) {
            const float b = sp.brightness(), r = sp.size, cx = sp.x + dx, cy = sp.y + dy;
            const float x0 = std::max(cx - r, 0.0f), x1 = std::min(cx + r, width);
            const float y0 = std::max(cy - r, 0.0f), y1 = std::min(cy + r, height);
            if (b <= 0.003f || r <= 0.5f || x1 <= x0 || y1 <= y0)
                continue;
            const float u0 = (x0 - cx) / r, u1 = (x1 - cx) / r, v0 = (y0 - cy) / r, v1 = (y1 - cy) / r;
            const std::uint32_t c = vertexColor(sp.color);
            const float corners[6][4] = { { x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x0, y1, u0, v1 },
                                          { x1, y0, u1, v0 }, { x1, y1, u1, v1 }, { x0, y1, u0, v1 } };
            for (const auto& corner : corners) {
                vec3_set(&data->points[at], corner[0], corner[1], 0.0f);
                data->colors[at] = c;
                vec2_set(&uv[at], corner[2], corner[3]);
                vec4_set(&spark[at], b, sp.phase, 0.0f, 0.0f);
                ++at;
            }
            if (++count == kSparksPerDraw)
                flush();
        }
        flush();
    }

    /// Ends the technique begun (the destructor does it too).
    void finish()
    {
        if (current == nullptr)
            return;
        gs_technique_end_pass(current);
        gs_technique_end(current);
        current = nullptr;
    }

private:
    /// The technique for the shapes that follow: the one begun, or that one begun after the last is ended.
    void use(gs_technique_t* technique)
    {
        if (technique == current)
            return;
        finish();
        if (gs_technique_begin(technique) == 0)
            return;
        if (!gs_technique_begin_pass(technique, 0)) {
            gs_technique_end(technique);
            return;
        }
        current = technique;
    }

    void pair(gs_eparam_t* param, float a, float b) const
    {
        vec2 v;
        vec2_set(&v, a, b);
        gs_effect_set_vec2(param, &v);
    }

    /// A vertex's colour (0xAABBGGRR), in the space the framebuffer takes.
    std::uint32_t vertexColor(std::uint32_t rgba) const
    {
        if (!linear)
            return rgba;
        std::uint32_t out = rgba & 0xFF000000u;
        for (int c = 0; c < 24; c += 8) {
            const float u = (float) ((rgba >> c) & 0xFF) / 255.0f;
            const float l = u <= 0.04045f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f);
            out |= (std::uint32_t) std::lround(255.0f * l) << c;
        }
        return out;
    }

    /// A colour (0xAABBGGRR), its alpha times `alpha`, in the space the framebuffer takes.
    void color(gs_eparam_t* param, std::uint32_t rgba, float alpha) const
    {
        vec4 c;
        if (linear)
            vec4_from_rgba_srgb(&c, rgba);
        else
            vec4_from_rgba(&c, rgba);
        c.w *= alpha;
        gs_effect_set_vec4(param, &c);
    }

    /// The quad (x0, y0)-(x1, y1), in the drawing's own pixels: moved by the offset, cut at the source's edges.
    void draw(float x0, float y0, float x1, float y1)
    {
        x0 = std::max({ x0 + dx, clipX0 + dx, 0.0f });
        y0 = std::max({ y0 + dy, clipY0 + dy, 0.0f });
        x1 = std::min({ x1 + dx, clipX1 + dx, width });
        y1 = std::min({ y1 + dy, clipY1 + dy, height });
        if (current == nullptr || x1 - x0 < 0.01f || y1 - y0 < 0.01f)
            return;
        pair(look.quadOrigin, x0 - dx, y0 - dy);
        pair(look.quadSize, x1 - x0, y1 - y0);
        pair(look.fadeBand, fadeNone, fadeWhole);
        gs_effect_set_float(look.time, now);
        gs_effect_update_params(look.effect);
        gs_matrix_push();
        gs_matrix_translate3f(x0, y0, 0.0f);
        gs_matrix_scale3f(x1 - x0, y1 - y0, 1.0f);
        gs_load_vertexbuffer(look.quad);
        gs_load_indexbuffer(nullptr);
        gs_draw(GS_TRISTRIP, 0, 0);
        gs_matrix_pop();
    }

    const Drawing& look;
    float width, height;
    bool linear;
    gs_technique_t* current = nullptr;
    float fadeNone = -2.0f, fadeWhole = -1.0f, now = 0.0f, dx = 0.0f, dy = 0.0f;
    float clipX0 = 0.0f, clipY0 = 0.0f, clipX1 = 0.0f, clipY1 = 0.0f;
};

/// A note as drawn: its bar (x, from y0 to y1, w wide), its tail (the sound a pedal held after its key was up, from
/// y1 to tailEnd), how strong it was struck, how bright its sound still is at y0, y1 and tailEnd, its hue.
struct Bar {
    float x = 0.0f, y0 = 0.0f, y1 = 0.0f, w = 0.0f, strength = 0.0f, tailEnd = 0.0f;
    float bright0 = 1.0f, bright1 = 1.0f, brightTail = 1.0f;
    bool open = false, black = false, ringing = false; // ringing: a pedal still holds it (its tail reaches the keys)
    int key = 0;
    Tint tint;
};

/// How bright a note's sound still is `t` seconds after its strike: as a string dies away, slower in the bass.
float decayOf(int key, double t, const Settings& s)
{
    if (!s.decay)
        return 1.0f;
    const double tau = 7.0 - 5.8 * (key - kFirstKey) / (double) (kLastKey - kFirstKey);
    return (float) (0.3 + 0.7 * std::exp(-std::max(0.0, t) / tau));
}

/// The notes' opacity, by the strength they were struck with and the accent.
float alphaOf(float strength, float accent)
{
    return (0.62f + 0.38f * strength) * (1.0f - accent) + (0.3f + 0.7f * strength) * accent;
}

/// The notes as they are drawn at the frame shown: rising from their keys at trailHeight per trailSeconds.
void barsOf(const Keyboard& k, const Settings& s, const Layout& l, std::vector<Bar>& bars)
{
    bars.clear();
    if (s.trailHeight <= 0)
        return;
    const double frame = k.frame, rate = k.rate > 0.0 ? k.rate : 48000.0;
    const double speed = s.trailHeight / std::max(0.5, s.trailSeconds); // pixels per second
    const float trailTop = l.top - (float) s.trailHeight, accent = s.accent / 100.0f;
    auto yAt = [&](std::uint64_t f) { return (float) (l.top - (frame - (double) f) / rate * speed); };
    for (const auto& n : k.state.notes()) {
        if (n.key < kFirstKey || n.key > kLastKey)
            continue;
        const auto shape = l.shape(n.key);
        if (!l.shows(shape))
            continue;
        const float st = strengthOf(n.velocity, s), base = shape.black ? shape.width * 0.92f : l.white * 0.78f;
        const float w = std::min(shape.black ? shape.width : l.white * 0.92f, base * (1.0f + accent * (0.6f * st - 0.45f)));
        const float y1 = n.open() ? l.top : yAt(n.end);
        const float y0 = std::min(yAt(n.start), y1 - 0.6f * w); // a short note: a pill
        float tail = y1;
        if (s.pedalTails && !n.open())
            tail = n.ringing() ? l.top : std::min(l.top, yAt(n.sound));
        if (std::max(y1, tail) <= trailTop || y0 >= l.top)
            continue;
        Bar b;
        b.x = shape.x + 0.5f * (shape.width - w);
        b.y0 = std::max(y0, trailTop - w);
        b.y1 = std::min(y1, l.top);
        b.tailEnd = std::max(b.y1, tail);
        b.w = w;
        b.strength = st;
        const double age = (frame - (double) n.start) / rate; // seconds since the strike, at the keys
        auto ageAt = [&](float y) { return age - (l.top - y) / speed; };
        b.bright0 = decayOf(n.key, ageAt(b.y0), s);
        b.bright1 = decayOf(n.key, ageAt(b.y1), s);
        b.brightTail = decayOf(n.key, ageAt(b.tailEnd), s);
        b.open = n.open();
        b.ringing = n.ringing();
        b.black = shape.black;
        b.key = n.key;
        b.tint = tintOf(s, n.key, n.hand);
        bars.push_back(b);
    }
}

/// A line through `points` smoothed (Catmull-Rom), kLinePoints points at most (the newest kept).
std::vector<std::array<float, 3>> smoothLine(const std::vector<std::array<float, 3>>& points)
{
    const std::size_t n = points.size();
    if (n < 3)
        return points;
    if (n >= (std::size_t) kLinePoints)
        return { points.end() - kLinePoints, points.end() };
    const int sub = std::clamp((int) ((kLinePoints - 1) / (n - 1)), 1, 6);
    std::vector<std::array<float, 3>> out;
    out.reserve((n - 1) * (std::size_t) sub + 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const auto& p0 = points[i == 0 ? 0 : i - 1];
        const auto& p1 = points[i];
        const auto& p2 = points[i + 1];
        const auto& p3 = points[std::min(i + 2, n - 1)];
        for (int j = 0; j < sub; ++j) {
            const float t = (float) j / (float) sub, t2 = t * t, t3 = t2 * t;
            std::array<float, 3> q {};
            for (int c = 0; c < 3; ++c)
                q[(std::size_t) c] = 0.5f * (2.0f * p1[(std::size_t) c] + (p2[(std::size_t) c] - p0[(std::size_t) c]) * t
                                             + (2.0f * p0[(std::size_t) c] - 5.0f * p1[(std::size_t) c] + 4.0f * p2[(std::size_t) c] - p3[(std::size_t) c]) * t2
                                             + (3.0f * p1[(std::size_t) c] - p0[(std::size_t) c] - 3.0f * p2[(std::size_t) c] + p3[(std::size_t) c]) * t3);
            out.push_back(q);
        }
    }
    out.push_back(points.back());
    return out;
}

/// The notes: rounded bars rising from their keys, as long as they were held; a halo around each (added), then the
/// bar (a gradient toward its hue, a pale rim, lit from the key while it is held), fading toward the top. Under each,
/// its tail while a pedal held its sound; along each, its sound dying away. The white keys' notes first, the black
/// keys' (narrower) over them. By the accent, a soft note thinner, deeper and more transparent, a strong one wider,
/// with a white-hot core and a wider halo. A powerful chord's notes tied, at their tops, by a bar of light; the
/// melody, a line of light through the right hand's top notes.
void drawNotes(const Keyboard& k, const Settings& s, const Layout& l, Painter& p, float lights, const std::vector<Bar>& bars)
{
    if (s.trailHeight <= 0)
        return;
    const double frame = k.frame, rate = k.rate > 0.0 ? k.rate : 48000.0;
    const double speed = s.trailHeight / std::max(0.5, s.trailSeconds); // pixels per second
    const float trailTop = l.top - (float) s.trailHeight;
    const float accent = s.accent / 100.0f;
    auto yAt = [&](std::uint64_t f) { return (float) (l.top - (frame - (double) f) / rate * speed); };
    p.fade(trailTop, trailTop + 0.3f * (float) s.trailHeight);
    // The tails, under the bars: narrower, fainter, dying away; once the pedal has let them go, their end drawing in
    // and fading out (the finer, the more).
    p.over();
    const float fine = s.tailFine / 100.0f;
    for (const auto& b : bars) {
        if (b.tailEnd <= b.y1 + 0.5f)
            continue;
        const float tw = std::max(1.5f, s.tailWidth / 100.0f * b.w);
        BoxStyle tail;
        tail.top = b.tint.main;
        tail.bottom = mixColor(b.tint.main, b.tint.deep, 0.4);
        tail.alpha = s.tailOpacity / 100.0f * alphaOf(b.strength, accent) * b.bright1;
        const float decay = b.brightTail / std::max(0.01f, b.bright1);
        if (b.ringing) { // still sounding: down to the keys
            tail.taper = 1.0f - 0.3f * fine;
            tail.fadeTo = decay;
        } else {
            tail.taper = 1.0f - 0.85f * fine;
            tail.fadeTo = (1.0f - fine) * decay;
            tail.curve = 1.0f + 0.6f * fine;
        }
        tail.radius = 0.5f * tw;
        p.box(b.x + 0.5f * (b.w - tw), b.y1 - 0.5f * tw, tw, b.tailEnd - b.y1 + 0.5f * tw, tail);
    }
    if (lights > 0.0f) {
        p.add();
        for (const auto& b : bars) {
            const float strong = 1.0f + accent * (0.4f + 1.2f * b.strength * b.strength - 1.0f);
            p.glow(b.x + 0.5f * b.w, 0.5f * (b.y0 + b.y1), 0.5f * b.w, 0.5f * (b.y1 - b.y0), 0.42f * b.w,
                   0.5f * l.white * (1.0f + accent * 0.6f * (b.strength - 0.5f)), b.tint.light,
                   lights * (0.22f + 0.3f * b.strength) * (b.open ? 1.0f : 0.6f) * strong * 0.5f * (b.bright0 + b.bright1));
        }
    }
    p.over();
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& b : bars) {
            if (b.black != (pass == 1) || b.y1 - b.y0 < 0.5f)
                continue;
            const double soft = accent * (1.0f - b.strength);
            BoxStyle st;
            st.top = mixColor(mixColor(b.tint.light, b.tint.pale, 0.35), b.tint.deep, 0.5 * soft);
            st.bottom = mixColor(mixColor(b.tint.main, b.tint.deep, 0.6 * soft), b.tint.deep, 0.5 * (1.0f - b.bright1));
            st.alpha = alphaOf(b.strength, accent) * b.bright0;
            st.fadeTo = b.bright1 / std::max(0.01f, b.bright0);
            st.curve = 0.8f;
            st.rim = b.tint.pale;
            st.rimAlpha = 0.95f;
            st.rimWidth = std::max(1.0f, 0.07f * l.white);
            st.light = b.tint.pale;
            st.lightStrength = b.open ? 0.3f : 0.0f;
            st.lightFrom = 1.0f;
            st.radius = 0.42f * b.w;
            p.box(b.x, b.y0, b.w, b.y1 - b.y0, st);
            const float hot = accent * std::pow(std::clamp((b.strength - 0.5f) / 0.5f, 0.0f, 1.0f), 1.5f) * 0.85f;
            const float inset = 0.15f * b.w;
            if (hot > 0.01f && b.y1 - b.y0 > 2.0f * inset + 2.0f) { // the white-hot core
                BoxStyle core;
                core.top = kWhite;
                core.bottom = b.tint.pale;
                core.alpha = hot * b.bright0;
                core.fadeTo = b.bright1 / std::max(0.01f, b.bright0);
                core.radius = 0.19f * b.w;
                p.box(b.x + 0.31f * b.w, b.y0 + inset, 0.38f * b.w, b.y1 - b.y0 - 2.0f * inset, core);
            }
        }
    // The powerful chords' notes tied at their tops: a bar of light across each hand's notes.
    if (s.chords && s.chordBracket)
        for (const auto& c : k.chords) {
            const float y = yAt(c.hit.frame), thick = std::max(2.0f, 0.14f * l.white);
            if (y >= l.top - thick || y < trailTop - thick)
                continue;
            for (const auto& part : partsOf(c.hit, l)) {
                if (part.count < 2)
                    continue;
                const Tint tint = tintOf(s, part.key, part.hand);
                const float x0 = part.x0 + 0.1f * l.white, x1 = part.x1 - 0.1f * l.white;
                p.add();
                p.glow(0.5f * (x0 + x1), y, 0.5f * (x1 - x0), 0.5f * thick, 0.5f * thick, 0.4f * l.white, tint.light, 0.7f * c.power);
                p.over();
                BoxStyle bar;
                bar.top = kWhite;
                bar.bottom = tint.pale;
                bar.alpha = std::min(1.0f, 0.4f + c.power);
                bar.radius = 0.5f * thick;
                p.box(x0, y - 0.5f * thick, x1 - x0, thick, bar);
            }
        }
    // The melody: a line of light through the right hand's top notes, at their strikes, brighter where struck harder.
    if (s.melody) {
        std::vector<PlayedNote> tune;
        melodyOf(k.state.notes(), (std::uint64_t) (0.035 * rate), tune);
        std::vector<std::array<float, 3>> points;
        for (const auto& n : tune) {
            const float y = yAt(n.start);
            const auto shape = l.shape(n.key);
            if (y <= l.top)
                points.push_back({ shape.x + 0.5f * shape.width, y, 0.55f + 0.45f * strengthOf(n.velocity, s) });
        }
        std::size_t from = 0; // from the last point above the notes' top on
        for (std::size_t i = 0; i < points.size(); ++i)
            if (points[i][1] < trailTop)
                from = i;
        points.erase(points.begin(), points.begin() + (std::ptrdiff_t) from);
        if (points.size() >= 2) {
            const Tint tint = tintOf(s, tune.back().key, kRightHand);
            p.add();
            p.line(smoothLine(points), mixColor(tint.light, tint.pale, 0.4), 0.85f, 0.3f * l.white + 2.0f);
        }
    }
    p.noFade();
}

void drawKeyboard(Keyboard& k, const Settings& s, const Layout& l, Painter& p)
{
    const double frame = k.frame, rate = k.rate > 0.0 ? k.rate : 48000.0;
    const float wk = l.white, lights = s.glow / 100.0f;
    std::array<Lit, 128> lits {};
    std::array<Tint, 128> tints {};
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        lits[(std::size_t) key] = lightOf(k.state.light(key), frame, rate, s);
        tints[(std::size_t) key] = tintOf(s, key, k.state.light(key).hand);
    }
    p.time((float) std::fmod((double) os_gettime_ns() * 1e-9, 3600.0));
    p.noFade();
    const float accent = s.accent / 100.0f;
    barsOf(k, s, l, k.bars);

    // The ambiance, behind everything: curtains of light over the room, as strong as the playing is loud.
    // (at rest: the share of its strength it keeps when nothing sounds)
    if (const float rest = s.auroraRest / 100.0f, level = rest + (1.0f - rest) * k.level; s.ambiance > 0 && level > 0.01f && l.top > 1.0f) {
        const auto [left, right] = ambianceColors(s);
        p.add();
        p.aurora(0.0f, 0.0f, l.width, l.top, left, right, s.ambiance / 100.0f * level * 0.8f, s.auroraHeight / 100.0f,
                 s.auroraSpeed / 50.0f);
    }

    // The powerful chords' light over the room, behind everything: rising from each hand's keys, in its hue.
    if (s.chords && s.chordWash && l.top > 1.0f) {
        p.add();
        for (const auto& c : k.chords) {
            const double u = (frame - (double) c.hit.frame) / rate / kWashSeconds;
            if (u < 0.0 || u >= 1.0)
                continue;
            for (const auto& part : partsOf(c.hit, l))
                p.glow(0.5f * (part.x0 + part.x1), l.top, 0.5f * (part.x1 - part.x0) + wk, 1.0f, 1.0f, 0.55f * l.top,
                       tintOf(s, part.key, part.hand).light, c.power * 0.7f * (float) ((1.0 - u) * (1.0 - u)));
        }
    }

    // The beams, behind the notes: rising from each key lit, as high and as bright as it is.
    if (lights > 0.0f && l.top > 1.0f) {
        p.add();
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            const auto& lit = lits[(std::size_t) key];
            if (lit.level <= 0.0 && lit.flash <= 0.0)
                continue;
            const auto shape = l.shape(key);
            const float h = std::min(l.top, 1.4f * (l.bottom - l.top) * (float) (0.45 + 0.55 * lit.level + 0.3 * lit.flash));
            p.beam(shape.x + 0.5f * shape.width, l.top, wk * (shape.black ? 1.0f : 1.25f), h, tints[(std::size_t) key].light,
                   lights * (float) (0.32 * lit.level + 0.55 * lit.flash), (float) (key - kFirstKey) / 88.0f);
        }
    }
    drawNotes(k, s, l, p, lights, k.bars);

    // The keyboard: its bed (between the keys), the rail and its felt.
    p.over();
    BoxStyle bed;
    bed.top = kBedTop;
    bed.bottom = kBedBottom;
    bed.radius = 0.3f * wk;
    p.box(0.0f, l.top, l.width, l.bottom - l.top, bed, true);
    const float feltHeight = std::max(1.0f, std::round(0.3f * thinRail(s)));
    if (l.lacquer) { // a lacquered band: the notes and lights above it reflected, squashed, dimming with depth
        BoxStyle band;
        band.top = 0xFF18181C;
        band.bottom = 0xFF040405;
        p.box(0.0f, l.top, l.width, l.rail, band);
        const float refl = s.reflection / 100.0f, squash = 0.55f, depth = l.rail - feltHeight;
        p.clip(0.0f, l.top, l.width, l.keysTop - feltHeight);
        // A part of a note (from y `from` to `to`, x to x + w) seen in the lacquer, `alpha` at the band's top.
        auto mirrored = [&](const Bar& b, float from, float to, float x, float w, float alpha) {
            const float m0 = l.top + (l.top - to) * squash, m1 = l.top + (l.top - from) * squash;
            if (m0 >= l.top + depth || m1 <= m0)
                return;
            const float d0 = (m0 - l.top) / depth, d1 = std::min(1.0f, (m1 - l.top) / depth);
            BoxStyle mirror;
            mirror.top = b.tint.main;
            mirror.bottom = b.tint.deep;
            mirror.alpha = refl * 0.55f * (1.0f - d0) * alpha;
            mirror.fadeTo = (1.0f - d1) / std::max(0.01f, 1.0f - d0);
            mirror.radius = 0.42f * w;
            p.box(x, m0, w, std::max(1.0f, m1 - m0), mirror);
        };
        for (const auto& b : k.bars) {
            mirrored(b, b.y0, b.y1, b.x, b.w, b.bright1);
            if (b.tailEnd > b.y1 + 0.5f) { // its tail, as narrow and faint as it is
                const float tw = std::max(1.5f, s.tailWidth / 100.0f * b.w);
                mirrored(b, b.y1, b.tailEnd, b.x + 0.5f * (b.w - tw), tw, s.tailOpacity / 100.0f * b.brightTail);
            }
        }
        p.add();
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            const auto& lit = lits[(std::size_t) key];
            if (lit.level <= 0.0 && lit.flash <= 0.0)
                continue;
            const auto shape = l.shape(key);
            p.glow(shape.x + 0.5f * shape.width, l.top, 0.4f * shape.width, 1.0f, 1.0f, 0.45f * depth, tints[(std::size_t) key].light,
                   refl * (float) (0.5 * std::max(0.3f, lights) * lit.level + 0.8 * lit.flash));
        }
        p.unclip();
        p.over();
        BoxStyle edge;
        edge.top = edge.bottom = kRailEdge;
        edge.alpha = 0.6f;
        p.box(0.0f, l.top, l.width, 1.0f, edge);
        BoxStyle sheen;
        sheen.top = sheen.bottom = kWhite;
        sheen.alpha = 0.07f;
        sheen.fadeTo = 0.0f;
        p.box(0.0f, l.top + 1.0f, l.width, 0.4f * l.rail, sheen);
    } else {
        BoxStyle rail;
        rail.top = kRailTop;
        rail.bottom = kRailBottom;
        p.box(0.0f, l.top, l.width, l.rail, rail);
        BoxStyle edge;
        edge.top = edge.bottom = kRailEdge;
        edge.alpha = 0.8f;
        p.box(0.0f, l.top, l.width, 1.0f, edge);
    }
    BoxStyle felt;
    felt.top = felt.bottom = kFelt;
    p.box(0.0f, l.keysTop - feltHeight, l.width, feltHeight, felt);
    // The sustain pedal down: a line of light along the keyboard's top, and a soft light rising from it over the
    // notes' feet (its halo), each its own strength.
    if (const float down = k.state.pedal(0); down > 0.0f && (s.pedalLine > 0 || s.pedalGlow > 0)) {
        const Tint tint = pedalTint(s, 2);
        p.add();
        p.glow(0.5f * l.width, l.top, 0.5f * l.width, 0.5f, 0.5f, 0.12f * l.top + 4.0f, tint.light, 0.4f * down * s.pedalGlow / 100.0f);
        p.glow(0.5f * l.width, l.top, 0.5f * l.width, 0.5f, 0.5f, 2.0f + 0.1f * l.rail, tint.light, 0.9f * down * s.pedalLine / 100.0f);
        p.over();
    }

    // The white keys: a front edge (the lip), less of it as the key goes down, and the top over it; lit toward the
    // key's hue, from within at the strike.
    const float gap = wk >= 10.0f ? 1.0f : 0.5f, radius = 0.16f * wk;
    const float lipUp = std::max(2.0f, 0.075f * l.keysHeight), lipDown = std::max(1.0f, 0.025f * l.keysHeight);
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        if (isBlack(key))
            continue;
        const auto shape = l.shape(key);
        const auto& lit = lits[(std::size_t) key];
        const auto& tint = tints[(std::size_t) key];
        const double lv = lit.level;
        const float x = shape.x + 0.5f * gap, w = shape.width - gap, h = l.bottom - l.keysTop - 0.5f * gap;
        const float lip = lipUp + (lipDown - lipUp) * (float) lit.depth;
        BoxStyle front;
        front.top = mixColor(kLipTop, tint.deep, 0.9 * lv);
        front.bottom = mixColor(kLipBottom, tint.deep, 0.9 * lv);
        front.radius = radius;
        p.box(x, l.keysTop, w, h, front, true);
        BoxStyle body;
        body.top = mixColor(kIvoryTop, mixColor(tint.light, tint.pale, 0.45), 0.9 * lv);
        body.bottom = mixColor(kIvory, tint.main, 0.9 * lv);
        body.curve = lv > 0.0 ? 0.8f : 0.3f;
        body.light = tint.pale;
        body.lightStrength = (float) (0.3 * lv + 0.9 * lit.flash);
        body.lightFrom = 0.0f;
        body.radius = 0.8f * radius;
        p.box(x, l.keysTop, w, h - lip, body, true);
    }

    // The black keys: their shadow on the white ones, the body, its top face (longer as the key goes down), a gleam
    // at its front; lit toward the key's hue.
    const float blackRadius = 0.14f * wk;
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        if (!isBlack(key))
            continue;
        const auto shape = l.shape(key);
        BoxStyle shadow;
        shadow.top = shadow.bottom = kBlack;
        shadow.alpha = 0.5f;
        shadow.radius = blackRadius;
        shadow.soft = std::max(2.0f, 0.3f * wk);
        p.box(shape.x + 0.1f * wk, l.keysTop, shape.width, l.blackHeight + 0.05f * l.keysHeight, shadow, true);
    }
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        if (!isBlack(key))
            continue;
        const auto shape = l.shape(key);
        const auto& lit = lits[(std::size_t) key];
        const auto& tint = tints[(std::size_t) key];
        const double lv = lit.level;
        BoxStyle body;
        body.top = mixColor(kEbonyTop, tint.deep, 0.6 * lv);
        body.bottom = mixColor(kEbonyBottom, tint.deep, 0.5 * lv);
        body.rim = kBlack;
        body.rimAlpha = 0.7f;
        body.rimWidth = 1.0f;
        body.radius = blackRadius;
        p.box(shape.x, l.keysTop, shape.width, l.blackHeight, body, true);
        const float inset = 0.13f * shape.width;
        const float faceHeight = l.blackHeight * (0.86f + 0.1f * (float) lit.depth) - 0.5f * inset;
        BoxStyle face;
        face.top = mixColor(kFaceBack, tint.deep, 0.9 * lv);
        face.bottom = mixColor(kFaceFront, tint.light, 0.9 * lv);
        face.curve = 1.2f;
        face.rim = 0xFF64646E;
        face.rimAlpha = 0.35f;
        face.rimWidth = 1.0f;
        face.light = tint.pale;
        face.lightStrength = (float) (0.3 * lv + 0.8 * lit.flash);
        face.lightFrom = 0.0f;
        face.radius = 0.7f * blackRadius;
        p.box(shape.x + inset, l.keysTop, shape.width - 2.0f * inset, faceHeight, face, true);
        BoxStyle gleam;
        gleam.top = gleam.bottom = kWhite;
        gleam.alpha = 0.14f;
        gleam.radius = 1.0f;
        p.box(shape.x + 1.4f * inset, l.keysTop + faceHeight - 3.0f, shape.width - 2.8f * inset, 1.5f, gleam);
    }

    // Light where each key lit meets the rail; a burst and a flare along the rail at the strike.
    p.add();
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        const auto& lit = lits[(std::size_t) key];
        if (lit.level <= 0.0 && lit.flash <= 0.0)
            continue;
        const auto shape = l.shape(key);
        const auto& tint = tints[(std::size_t) key];
        const float cx = shape.x + 0.5f * shape.width, fl = (float) lit.flash;
        p.glow(cx, l.keysTop, 0.45f * shape.width, 1.0f, 1.0f, 0.35f * wk, tint.light, lights * (float) (0.45 * lit.level + 0.6 * fl));
        if (fl > 0.02f) {
            p.glow(cx, l.keysTop, 0.3f * shape.width, 0.5f, 0.5f, wk * (0.6f + 0.8f * fl), tint.pale, 0.9f * fl * (0.4f + 0.6f * lights));
            p.glow(cx, l.top + 0.5f * l.rail, wk * (1.0f + 3.0f * fl), 0.4f, 0.4f, 1.5f + 2.0f * fl, tint.pale, 0.8f * fl);
        }
    }

    // A ripple of light from each key at its strike, as wide and bright as the strike is strong (by the accent).
    if (accent > 0.0f)
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            const auto& light = k.state.light(key);
            if (!light.struck)
                continue;
            const double u = (frame - (double) light.pressed) / rate / kRippleSeconds;
            const float st = strengthOf(light.lastVelocity, s), amp = accent * std::pow(st, 1.5f);
            if (u < 0.0 || u >= 1.0 || amp < 0.02f)
                continue;
            const auto shape = l.shape(key);
            const float grow = (float) (1.0 - (1.0 - u) * (1.0 - u)), rx = wk * (0.5f + (1.0f + 3.5f * st) * grow);
            p.ring(shape.x + 0.5f * shape.width, l.keysTop, rx, 0.32f * rx, 1.2f + 1.8f * st, tints[(std::size_t) key].pale,
                   amp * 0.9f * (float) ((1.0 - u) * (1.0 - u)));
        }

    // The powerful chords' wave: light running from the chord along the keys and the rail, both ways.
    if (s.chords && s.chordWave)
        for (const auto& c : k.chords) {
            const double u = (frame - (double) c.hit.frame) / rate / kWaveSeconds;
            if (u < 0.0 || u >= 1.0 || c.hit.notes.empty())
                continue;
            float cx = 0.0f;
            for (const auto& n : c.hit.notes) {
                const auto shape = l.shape(n.key);
                cx += shape.x + 0.5f * shape.width;
            }
            cx /= (float) c.hit.notes.size();
            const float d = std::max(cx, l.width - cx) * (float) (1.0 - (1.0 - u) * (1.0 - u)), fade = c.power * (float) std::pow(1.0 - u, 1.5);
            const Tint tint = tintOf(s, c.hit.notes.front().key, c.hit.notes.front().hand);
            for (float side : { -1.0f, 1.0f }) {
                const float x = cx + side * d;
                p.glow(x, l.keysTop + 0.35f * l.keysHeight, 0.9f * wk, 0.3f * l.keysHeight, 0.5f * wk, 0.7f * wk, tint.pale, 0.45f * fade);
                p.glow(x, l.top + 0.5f * l.rail, 2.0f * wk, 0.5f, 0.5f, 2.5f, tint.pale, fade);
            }
        }

    // The pedals: soft, sostenuto, sustain (as on a grand, left to right), rounded, each filled as far as it is down.
    if (s.pedals) {
        const float g = std::max(3.0f, l.width * 0.006f), w = (l.width - 4.0f * g) / 3.0f;
        const float h = std::max(4.0f, 0.5f * l.pedalHeight), y = l.pedalTop + 0.5f * (l.pedalHeight - h);
        const int which[3] = { 2, 1, 0 };
        for (int i = 0; i < 3; ++i) {
            const float x = g + (float) i * (w + g), down = k.state.pedal(which[i]);
            p.over();
            BoxStyle back;
            back.top = kPillTop;
            back.bottom = kPillBottom;
            back.rim = kPillRim;
            back.rimAlpha = 0.9f;
            back.rimWidth = 1.0f;
            back.radius = 0.5f * h;
            p.box(x, y, w, h, back);
            if (down <= 0.0f)
                continue;
            const Tint tint = pedalTint(s, i);
            const float fw = std::max(h, w * down);
            p.add();
            p.glow(x + 0.5f * fw, y + 0.5f * h, 0.5f * fw, 0.5f * h, 0.5f * h, 0.9f * h, tint.light, lights * 0.6f * down);
            p.over();
            BoxStyle fill;
            fill.top = tint.light;
            fill.bottom = tint.main;
            fill.rim = tint.pale;
            fill.rimAlpha = 0.8f;
            fill.rimWidth = 1.0f;
            fill.radius = 0.5f * h;
            p.box(x, y, fw, h, fill);
        }
    }

    // The sparkles, over everything.
    p.add();
    p.sparkles(k.sparkles.sparks());
}

/// Without the shader (its file missing): the keys and notes in plain rectangles.
void drawPlain(const Keyboard& k, const Settings& s, const Layout& l, bool linear)
{
    gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (solid == nullptr)
        return;
    gs_eparam_t* color = gs_effect_get_param_by_name(solid, "color");
    auto rect = [&](float x, float y, float w, float h, std::uint32_t rgba) {
        if (w <= 0.0f || h <= 0.0f)
            return;
        vec4 c;
        if (linear)
            vec4_from_rgba_srgb(&c, rgba);
        else
            vec4_from_rgba(&c, rgba);
        gs_effect_set_vec4(color, &c);
        gs_matrix_push();
        gs_matrix_translate3f(x, y, 0.0f);
        gs_matrix_scale3f(w, h, 1.0f);
        while (gs_effect_loop(solid, "Solid"))
            gs_draw_sprite(nullptr, 0, 1, 1);
        gs_matrix_pop();
    };
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    const double frame = k.frame, rate = k.rate > 0.0 ? k.rate : 48000.0;
    if (s.trailHeight > 0) {
        const double speed = s.trailHeight / std::max(0.5, s.trailSeconds);
        for (const auto& n : k.state.notes()) {
            if (n.key < kFirstKey || n.key > kLastKey)
                continue;
            const auto shape = l.shape(n.key);
            const float y1 = (float) (l.top - (frame - (double) n.start) / rate * speed);
            const float y2 = n.open() ? l.top : (float) (l.top - (frame - (double) n.end) / rate * speed);
            const float from = std::max(l.top - (float) s.trailHeight, y1), to = std::min(l.top, std::max(y2, y1 + 2.0f));
            rect(shape.x, from, shape.width, to - from, tintOf(s, n.key, n.hand).main);
        }
    }
    rect(0.0f, l.top, l.width, l.bottom - l.top, kBedBottom);
    for (int pass = 0; pass < 2; ++pass)
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            if (isBlack(key) != (pass == 1))
                continue;
            const auto shape = l.shape(key);
            const double lv = lightOf(k.state.light(key), frame, rate, s).level;
            const std::uint32_t base = pass == 1 ? kEbonyBottom : kIvory;
            const std::uint32_t c = mixColor(base, tintOf(s, key, k.state.light(key).hand).main, lv);
            rect(shape.x + (pass == 0 ? 0.5f : 0.0f), l.keysTop, shape.width - (pass == 0 ? 1.0f : 0.0f),
                 pass == 1 ? l.blackHeight : l.bottom - l.keysTop, c);
        }
}

/// One pass of the bloom: `source` through `technique` into `target` (w by h), `texel` its step.
void bloomPass(const Drawing& d, gs_texrender_t* target, std::uint32_t w, std::uint32_t h, gs_technique_t* technique,
               gs_texture_t* source, float tx, float ty)
{
    gs_texrender_reset(target);
    if (source == nullptr || !gs_texrender_begin(target, w, h))
        return;
    gs_ortho(0.0f, (float) w, 0.0f, (float) h, -100.0f, 100.0f);
    gs_effect_set_texture(d.texture, source);
    vec2 step;
    vec2_set(&step, tx, ty);
    gs_effect_set_vec2(d.texel, &step);
    if (gs_technique_begin(technique) > 0) {
        if (gs_technique_begin_pass(technique, 0)) {
            gs_draw_sprite(source, 0, w, h);
            gs_technique_end_pass(technique);
        }
        gs_technique_end(technique);
    }
    gs_texrender_end(target);
}

/// A texture drawn w by h with `technique` (read as sRGB when the output is linear).
void drawTexture(const Drawing& d, gs_technique_t* technique, gs_texture_t* texture, std::uint32_t w, std::uint32_t h, bool linear)
{
    if (texture == nullptr)
        return;
    if (linear)
        gs_effect_set_texture_srgb(d.texture, texture);
    else
        gs_effect_set_texture(d.texture, texture);
    if (gs_technique_begin(technique) > 0) {
        if (gs_technique_begin_pass(technique, 0)) {
            gs_draw_sprite(texture, 0, w, h);
            gs_technique_end_pass(technique);
        }
        gs_technique_end(technique);
    }
}

/// The keyboard drawn into an image, its bright coloured parts taken at a quarter of its size, blurred twice each
/// way, and added over it. false when the images cannot be made (then it is drawn straight).
template <class Paint> bool drawWithBloom(Keyboard& k, const Settings& s, const Layout& l, bool linear, Paint paint)
{
    const auto w = (std::uint32_t) std::lround(l.width), h = (std::uint32_t) std::lround(l.height);
    if (w == 0 || h == 0)
        return false;
    for (gs_texrender_t** t : { &k.picture, &k.glowA, &k.glowB })
        if (*t == nullptr)
            *t = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (k.picture == nullptr || k.glowA == nullptr || k.glowB == nullptr)
        return false;
    gs_texrender_reset(k.picture);
    if (!gs_texrender_begin(k.picture, w, h))
        return false;
    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, (float) w, 0.0f, (float) h, -100.0f, 100.0f);
    gs_enable_framebuffer_srgb(false);
    paint();
    gs_texrender_end(k.picture);

    const Drawing& d = k.look;
    gs_texture_t* picture = gs_texrender_get_texture(k.picture);
    const std::uint32_t qw = std::max(1u, w / 4), qh = std::max(1u, h / 4);
    gs_enable_blending(false);
    bloomPass(d, k.glowA, qw, qh, d.bright, picture, 1.5f / (float) w, 1.5f / (float) h);
    for (int i = 0; i < 2; ++i) {
        bloomPass(d, k.glowB, qw, qh, d.blur, gs_texrender_get_texture(k.glowA), 1.2f / (float) qw, 0.0f);
        bloomPass(d, k.glowA, qw, qh, d.blur, gs_texrender_get_texture(k.glowB), 0.0f, 1.2f / (float) qh);
    }
    gs_enable_blending(true);
    gs_enable_framebuffer_srgb(linear);
    gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); // premultiplied
    drawTexture(d, d.image, picture, w, h, linear);
    gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_effect_set_float(d.bloomGain, 1.6f * (float) s.bloom / 100.0f);
    drawTexture(d, d.bloom, gs_texrender_get_texture(k.glowA), w, h, linear);
    return true;
}

void render(void* data, gs_effect_t*)
{
    auto* k = static_cast<Keyboard*>(data);
    Settings s;
    {
        const std::lock_guard<std::mutex> guard(k->lock);
        s = k->settings;
    }
    if (k->look.effect == nullptr && !k->look.failed)
        loadDrawing(k->look);
    const std::lock_guard<std::mutex> drawing(k->drawing);
    const Layout l = layoutOf(s, viewNow(*k, s));
    const bool linear = gs_get_linear_srgb(), previous = gs_framebuffer_srgb_enabled();
    gs_blend_state_push();
    if (k->look.ready()) {
        auto paint = [&](bool linearColors) {
            Painter painter(k->look, l.width, l.height, linearColors);
            if (s.chords && s.chordShake && k->rate > 0.0) { // a powerful chord shakes the keyboard, a few pixels, briefly
                float x = 0.0f, y = 0.0f;
                for (const auto& c : k->chords) {
                    const double t = (k->frame - (double) c.hit.frame) / k->rate;
                    if (t < 0.0 || t >= kShakeSeconds)
                        continue;
                    const float a = l.white * 0.18f * c.power * (float) std::exp(-t / 0.07);
                    x += a * (float) std::sin(t * 6.2831853 * 23.0);
                    y += 0.6f * a * (float) std::sin(t * 6.2831853 * 31.0 + 1.3);
                }
                painter.offset(x, y);
            }
            drawKeyboard(*k, s, l, painter);
        };
        if (s.bloom <= 0 || !drawWithBloom(*k, s, l, linear, [&] { paint(false); })) {
            gs_enable_framebuffer_srgb(linear);
            paint(linear);
        }
    } else {
        gs_enable_framebuffer_srgb(linear);
        drawPlain(*k, s, l, linear);
    }
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous);
}

uint32_t width(void* data)
{
    auto* k = static_cast<Keyboard*>(data);
    const std::lock_guard<std::mutex> guard(k->lock);
    return (uint32_t) k->settings.width;
}

uint32_t height(void* data)
{
    auto* k = static_cast<Keyboard*>(data);
    const std::lock_guard<std::mutex> guard(k->lock);
    return (uint32_t) totalHeight(k->settings);
}

const char* name(void*)
{
    return text("Keyboard.Name");
}

void defaults(obs_data_t* settings)
{
    const Settings s;
    obs_data_set_default_int(settings, "width", s.width);
    obs_data_set_default_int(settings, "trail_height", s.trailHeight);
    obs_data_set_default_double(settings, "trail_seconds", s.trailSeconds);
    obs_data_set_default_bool(settings, "pedals", s.pedals);
    obs_data_set_default_int(settings, "color_mode", (int) s.mode);
    obs_data_set_default_int(settings, "hue_left", s.hueLeft);
    obs_data_set_default_int(settings, "hue_right", s.hueRight);
    obs_data_set_default_int(settings, "hue_white", s.hueWhite);
    obs_data_set_default_int(settings, "hue_black", s.hueBlack);
    obs_data_set_default_int(settings, "hue", s.hue);
    obs_data_set_default_int(settings, "saturation", s.saturation);
    obs_data_set_default_bool(settings, "hand_adaptive", s.adaptive);
    obs_data_set_default_int(settings, "hand_split", s.split);
    obs_data_set_default_int(settings, "glow", s.glow);
    obs_data_set_default_int(settings, "sparkles", s.sparkles);
    obs_data_set_default_int(settings, "sparkle_height", s.sparkleHeight);
    obs_data_set_default_int(settings, "sparkle_spread", s.sparkleSpread);
    obs_data_set_default_int(settings, "sparkle_tilt", s.sparkleTilt);
    obs_data_set_default_int(settings, "afterglow_ms", s.afterglowMs);
    obs_data_set_default_bool(settings, "flash", s.flash);
    obs_data_set_default_double(settings, "offset_ms", s.offsetMs);
    obs_data_set_default_int(settings, "velocity_accent", s.accent);
    obs_data_set_default_int(settings, "velocity_top", s.velocityTop);
    obs_data_set_default_bool(settings, "chords", s.chords);
    obs_data_set_default_int(settings, "chord_notes", s.chordNotes);
    obs_data_set_default_int(settings, "chord_threshold", s.chordThreshold);
    obs_data_set_default_int(settings, "chord_intensity", s.chordIntensity);
    obs_data_set_default_bool(settings, "chord_wave", s.chordWave);
    obs_data_set_default_bool(settings, "chord_wash", s.chordWash);
    obs_data_set_default_bool(settings, "chord_fireworks", s.chordFireworks);
    obs_data_set_default_bool(settings, "chord_bracket", s.chordBracket);
    obs_data_set_default_bool(settings, "chord_shake", s.chordShake);
    obs_data_set_default_int(settings, "range", s.range);
    obs_data_set_default_int(settings, "range_first", s.rangeFirst);
    obs_data_set_default_int(settings, "range_last", s.rangeLast);
    obs_data_set_default_int(settings, "follow_octaves", s.followOctaves);
    obs_data_set_default_bool(settings, "pedal_tails", s.pedalTails);
    obs_data_set_default_int(settings, "tail_width", s.tailWidth);
    obs_data_set_default_int(settings, "tail_opacity", s.tailOpacity);
    obs_data_set_default_int(settings, "tail_fine", s.tailFine);
    obs_data_set_default_int(settings, "pedal_line", s.pedalLine);
    obs_data_set_default_int(settings, "pedal_glow", s.pedalGlow);
    obs_data_set_default_int(settings, "aurora_height", s.auroraHeight);
    obs_data_set_default_int(settings, "aurora_speed", s.auroraSpeed);
    obs_data_set_default_int(settings, "aurora_rest", s.auroraRest);
    obs_data_set_default_bool(settings, "note_decay", s.decay);
    obs_data_set_default_bool(settings, "melody", s.melody);
    obs_data_set_default_int(settings, "bloom", s.bloom);
    obs_data_set_default_int(settings, "reflection", s.reflection);
    obs_data_set_default_int(settings, "ambiance", s.ambiance);
    obs_data_set_default_int(settings, "style", 0);
    obs_data_set_default_int(settings, "style_applied", 0);
}

void update(void* data, obs_data_t* settings)
{
    auto* k = static_cast<Keyboard*>(data);
    auto hue = [&](const char* name) { return (int) std::clamp<long long>(obs_data_get_int(settings, name), 0, 360); };
    Settings s;
    s.width = (int) std::clamp<long long>(obs_data_get_int(settings, "width"), 320, 7680);
    s.trailHeight = (int) std::clamp<long long>(obs_data_get_int(settings, "trail_height"), 0, 4320);
    s.trailSeconds = std::clamp(obs_data_get_double(settings, "trail_seconds"), 0.5, 30.0);
    s.pedals = obs_data_get_bool(settings, "pedals");
    s.mode = (ColorMode) std::clamp<long long>(obs_data_get_int(settings, "color_mode"), 0, 3);
    s.hueLeft = hue("hue_left");
    s.hueRight = hue("hue_right");
    s.hueWhite = hue("hue_white");
    s.hueBlack = hue("hue_black");
    s.hue = hue("hue");
    s.saturation = (int) std::clamp<long long>(obs_data_get_int(settings, "saturation"), 0, 100);
    s.adaptive = obs_data_get_bool(settings, "hand_adaptive");
    s.split = (int) std::clamp<long long>(obs_data_get_int(settings, "hand_split"), kFirstKey, kLastKey);
    s.glow = (int) std::clamp<long long>(obs_data_get_int(settings, "glow"), 0, 100);
    s.sparkles = (int) std::clamp<long long>(obs_data_get_int(settings, "sparkles"), 0, 100);
    s.sparkleHeight = (int) std::clamp<long long>(obs_data_get_int(settings, "sparkle_height"), 10, 100);
    s.sparkleSpread = (int) std::clamp<long long>(obs_data_get_int(settings, "sparkle_spread"), 0, 90);
    s.sparkleTilt = (int) std::clamp<long long>(obs_data_get_int(settings, "sparkle_tilt"), -45, 45);
    s.afterglowMs = (int) std::clamp<long long>(obs_data_get_int(settings, "afterglow_ms"), 0, 3000);
    s.flash = obs_data_get_bool(settings, "flash");
    s.offsetMs = std::clamp(obs_data_get_double(settings, "offset_ms"), -1000.0, 1000.0);
    s.accent = (int) std::clamp<long long>(obs_data_get_int(settings, "velocity_accent"), 0, 100);
    s.velocityTop = (int) std::clamp<long long>(obs_data_get_int(settings, "velocity_top"), 40, 127);
    s.chords = obs_data_get_bool(settings, "chords");
    s.chordNotes = (int) std::clamp<long long>(obs_data_get_int(settings, "chord_notes"), 2, 8);
    s.chordThreshold = (int) std::clamp<long long>(obs_data_get_int(settings, "chord_threshold"), 0, 100);
    s.chordIntensity = (int) std::clamp<long long>(obs_data_get_int(settings, "chord_intensity"), 0, 100);
    s.chordWave = obs_data_get_bool(settings, "chord_wave");
    s.chordWash = obs_data_get_bool(settings, "chord_wash");
    s.chordFireworks = obs_data_get_bool(settings, "chord_fireworks");
    s.chordBracket = obs_data_get_bool(settings, "chord_bracket");
    s.chordShake = obs_data_get_bool(settings, "chord_shake");
    const long long range = obs_data_get_int(settings, "range");
    s.range = range == 76 || range == 61 || range == 49 || range == 37 || range == 0 || range == -1 ? (int) range : 88;
    s.rangeFirst = (int) std::clamp<long long>(obs_data_get_int(settings, "range_first"), kFirstKey, kLastKey);
    s.rangeLast = (int) std::clamp<long long>(obs_data_get_int(settings, "range_last"), s.rangeFirst, kLastKey);
    s.followOctaves = (int) std::clamp<long long>(obs_data_get_int(settings, "follow_octaves"), 1, 7);
    s.pedalTails = obs_data_get_bool(settings, "pedal_tails");
    s.tailWidth = (int) std::clamp<long long>(obs_data_get_int(settings, "tail_width"), 10, 100);
    s.tailOpacity = (int) std::clamp<long long>(obs_data_get_int(settings, "tail_opacity"), 10, 100);
    s.tailFine = (int) std::clamp<long long>(obs_data_get_int(settings, "tail_fine"), 0, 100);
    s.pedalLine = (int) std::clamp<long long>(obs_data_get_int(settings, "pedal_line"), 0, 100);
    s.pedalGlow = (int) std::clamp<long long>(obs_data_get_int(settings, "pedal_glow"), 0, 100);
    s.auroraHeight = (int) std::clamp<long long>(obs_data_get_int(settings, "aurora_height"), 10, 100);
    s.auroraSpeed = (int) std::clamp<long long>(obs_data_get_int(settings, "aurora_speed"), 0, 100);
    s.auroraRest = (int) std::clamp<long long>(obs_data_get_int(settings, "aurora_rest"), 0, 100);
    s.decay = obs_data_get_bool(settings, "note_decay");
    s.melody = obs_data_get_bool(settings, "melody");
    s.bloom = (int) std::clamp<long long>(obs_data_get_int(settings, "bloom"), 0, 100);
    s.reflection = (int) std::clamp<long long>(obs_data_get_int(settings, "reflection"), 0, 100);
    s.ambiance = (int) std::clamp<long long>(obs_data_get_int(settings, "ambiance"), 0, 100);
    const std::lock_guard<std::mutex> guard(k->lock);
    k->settings = s;
}

void* create(obs_data_t* settings, obs_source_t* source)
{
    auto* k = new Keyboard;
    k->source = source;
    k->state.reset();
    update(k, settings);
    return k;
}

void destroy(void* data)
{
    auto* k = static_cast<Keyboard*>(data);
    obs_enter_graphics();
    freeDrawing(k->look);
    for (gs_texrender_t* t : { k->picture, k->glowA, k->glowB })
        if (t != nullptr)
            gs_texrender_destroy(t);
    obs_leave_graphics();
    delete k;
}

/// The strongest velocity of the last minute becomes the keyboard's strongest.
bool measureVelocity(obs_properties_t*, obs_property_t*, void* data)
{
    auto* k = static_cast<Keyboard*>(data);
    const int strongest = k->strongest.load();
    if (strongest <= 0)
        return false;
    obs_data_t* settings = obs_source_get_settings(k->source);
    obs_data_set_int(settings, "velocity_top", std::max(40, strongest));
    obs_source_update(k->source, settings);
    obs_data_release(settings);
    return true;
}

/// The settings of the look (what a style sets; the size, range, timing and velocity scale are not).
constexpr const char* kLookKeys[] = { "color_mode", "hue_left", "hue_right", "hue_white", "hue_black", "hue", "saturation",
                                      "glow", "sparkles", "sparkle_height", "sparkle_spread", "sparkle_tilt", "afterglow_ms",
                                      "flash", "velocity_accent", "chords", "chord_intensity", "chord_wave", "chord_wash",
                                      "chord_fireworks", "chord_bracket", "chord_shake", "pedal_tails", "tail_width", "tail_opacity",
                                      "tail_fine", "pedal_line", "pedal_glow", "aurora_height", "aurora_speed",
                                      "aurora_rest", "note_decay", "melody",
                                      "bloom", "reflection", "ambiance" };

enum Style : int { kStyleCustom = 0, kStylePianoForge, kStyleConcert, kStyleNeon, kStyleGalaxy, kStyleRainbow, kStyleMinimal };

/// A style: the look back to the defaults, then its own values.
void applyStyle(obs_data_t* settings, int style)
{
    for (const char* key : kLookKeys)
        obs_data_unset_user_value(settings, key);
    auto set = [&](const char* key, long long value) { obs_data_set_int(settings, key, value); };
    auto flag = [&](const char* key, bool value) { obs_data_set_bool(settings, key, value); };
    switch (style) {
    case kStyleConcert: // sober: softer colours and lights, few sparkles, no fireworks
        set("hue_left", 210);
        set("hue_right", 35);
        set("saturation", 55);
        set("glow", 35);
        set("sparkles", 15);
        set("sparkle_height", 50);
        set("velocity_accent", 50);
        set("chord_intensity", 50);
        flag("chord_fireworks", false);
        set("pedal_line", 15);
        set("pedal_glow", 10);
        set("aurora_speed", 30);
        set("afterglow_ms", 600);
        set("bloom", 25);
        set("reflection", 60);
        set("ambiance", 15);
        break;
    case kStyleNeon: // cyan and magenta, everything bright
        set("hue_left", 190);
        set("hue_right", 320);
        set("saturation", 100);
        set("glow", 90);
        set("sparkles", 70);
        set("velocity_accent", 80);
        set("chord_intensity", 100);
        set("bloom", 80);
        set("ambiance", 50);
        set("pedal_line", 60);
        set("pedal_glow", 40);
        set("aurora_speed", 75);
        break;
    case kStyleGalaxy: // violet and turquoise, sparkles high, curtains of light
        set("hue_left", 265);
        set("hue_right", 185);
        set("saturation", 80);
        set("glow", 70);
        set("sparkles", 85);
        set("sparkle_height", 90);
        set("sparkle_spread", 35);
        set("chord_intensity", 90);
        set("bloom", 60);
        set("reflection", 40);
        set("ambiance", 80);
        set("aurora_height", 100);
        set("aurora_rest", 30);
        break;
    case kStyleRainbow:
        set("color_mode", (long long) ColorMode::rainbow);
        set("saturation", 85);
        set("glow", 70);
        set("sparkles", 60);
        set("bloom", 50);
        set("ambiance", 40);
        break;
    case kStyleMinimal: // the keys and their notes, nothing more
        set("color_mode", (long long) ColorMode::single);
        set("hue", 205);
        set("saturation", 60);
        set("glow", 0);
        set("sparkles", 0);
        set("velocity_accent", 40);
        flag("chords", false);
        flag("flash", false);
        set("afterglow_ms", 250);
        flag("note_decay", false);
        flag("melody", false);
        set("bloom", 0);
        set("reflection", 0);
        set("ambiance", 0);
        set("pedal_line", 0);
        set("pedal_glow", 0);
        set("tail_opacity", 35);
        break;
    default: // PianoForge: the defaults
        break;
    }
}

bool modeChanged(obs_properties_t* props, obs_property_t*, obs_data_t* settings);

/// A style chosen: applied once (opening the properties again does not apply it over what was changed since).
bool styleChanged(obs_properties_t* props, obs_property_t*, obs_data_t* settings)
{
    const long long style = obs_data_get_int(settings, "style");
    if (style == obs_data_get_int(settings, "style_applied"))
        return false;
    obs_data_set_int(settings, "style_applied", style);
    if (style != kStyleCustom)
        applyStyle(settings, (int) style);
    modeChanged(props, nullptr, settings);
    return true;
}

/// Every setting back to its default, the width kept.
bool resetAll(obs_properties_t* props, obs_property_t*, void* data)
{
    auto* k = static_cast<Keyboard*>(data);
    obs_data_t* settings = obs_source_get_settings(k->source);
    for (obs_data_item_t* item = obs_data_first(settings); item != nullptr; obs_data_item_next(&item)) {
        const char* key = obs_data_item_get_name(item);
        if (std::strcmp(key, "width") != 0)
            obs_data_item_unset_user_value(item);
    }
    obs_source_update(k->source, settings);
    modeChanged(props, nullptr, settings);
    obs_data_release(settings);
    return true;
}

/// The hues shown for the mode chosen; the split, for a fixed one; the range's keys for a range of one's own, the
/// width followed for following the playing.
bool modeChanged(obs_properties_t* props, obs_property_t*, obs_data_t* settings)
{
    const auto mode = (ColorMode) obs_data_get_int(settings, "color_mode");
    const bool adaptive = obs_data_get_bool(settings, "hand_adaptive");
    auto show = [&](const char* name, bool on) {
        if (obs_property_t* p = obs_properties_get(props, name))
            obs_property_set_visible(p, on);
    };
    show("hue", mode == ColorMode::single);
    show("hue_white", mode == ColorMode::keys);
    show("hue_black", mode == ColorMode::keys);
    for (const char* name : { "hue_left", "hue_right", "hand_adaptive" })
        show(name, mode == ColorMode::hands);
    show("hand_split", mode == ColorMode::hands && !adaptive);
    const long long range = obs_data_get_int(settings, "range");
    show("range_first", range == 0);
    show("range_last", range == 0);
    show("follow_octaves", range == -1);
    const bool tails = obs_data_get_bool(settings, "pedal_tails");
    for (const char* name : { "tail_width", "tail_opacity", "tail_fine" })
        show(name, tails);
    return true;
}

obs_properties_t* properties(void* data)
{
    auto* k = static_cast<Keyboard*>(data);
    obs_properties_t* props = obs_properties_create();
    const char* status = !k->connected ? text("Keyboard.Status.Waiting")
                         : k->following ? text("Keyboard.Status.Following") : text("Keyboard.Status.Alone");
    obs_property_t* info = obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(info, true);

    obs_property_t* style = obs_properties_add_list(props, "style", text("Keyboard.Style"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(style, text("Keyboard.Style.Custom"), kStyleCustom);
    obs_property_list_add_int(style, text("Keyboard.Style.PianoForge"), kStylePianoForge);
    obs_property_list_add_int(style, text("Keyboard.Style.Concert"), kStyleConcert);
    obs_property_list_add_int(style, text("Keyboard.Style.Neon"), kStyleNeon);
    obs_property_list_add_int(style, text("Keyboard.Style.Galaxy"), kStyleGalaxy);
    obs_property_list_add_int(style, text("Keyboard.Style.Rainbow"), kStyleRainbow);
    obs_property_list_add_int(style, text("Keyboard.Style.Minimal"), kStyleMinimal);
    obs_property_set_long_description(style, text("Keyboard.Style.Description"));
    obs_property_set_modified_callback(style, styleChanged);
    obs_property_t* reset = obs_properties_add_button2(props, "reset", text("Keyboard.Reset"), resetAll, k);
    obs_property_set_long_description(reset, text("Keyboard.Reset.Description"));

    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    auto keyList = [&](obs_properties_t* group, const char* name, const char* label) {
        obs_property_t* list = obs_properties_add_list(group, name, text(label), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            char line[48];
            std::snprintf(line, sizeof(line), key == 60 ? "%s%d (%s)" : "%s%d", names[key % 12], key / 12 - 1, text("Keyboard.MiddleC"));
            obs_property_list_add_int(list, line, key);
        }
        return list;
    };

    // The keyboard: its size, the keys shown, the notes above it, the pedals.
    obs_properties_t* board = obs_properties_create();
    obs_property_t* w = obs_properties_add_int(board, "width", text("Keyboard.Width"), 320, 7680, 1);
    obs_property_int_set_suffix(w, " px");
    obs_property_t* range = obs_properties_add_list(board, "range", text("Keyboard.Range"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(range, text("Keyboard.Range.88"), 88);
    obs_property_list_add_int(range, text("Keyboard.Range.76"), 76);
    obs_property_list_add_int(range, text("Keyboard.Range.61"), 61);
    obs_property_list_add_int(range, text("Keyboard.Range.49"), 49);
    obs_property_list_add_int(range, text("Keyboard.Range.37"), 37);
    obs_property_list_add_int(range, text("Keyboard.Range.Custom"), 0);
    obs_property_list_add_int(range, text("Keyboard.Range.Follow"), -1);
    obs_property_set_long_description(range, text("Keyboard.Range.Description"));
    obs_property_set_modified_callback(range, modeChanged);
    keyList(board, "range_first", "Keyboard.RangeFirst");
    keyList(board, "range_last", "Keyboard.RangeLast");
    obs_property_t* octaves = obs_properties_add_int_slider(board, "follow_octaves", text("Keyboard.FollowOctaves"), 1, 7, 1);
    obs_property_set_long_description(octaves, text("Keyboard.FollowOctaves.Description"));
    obs_property_t* trail = obs_properties_add_int(board, "trail_height", text("Keyboard.TrailHeight"), 0, 4320, 1);
    obs_property_int_set_suffix(trail, " px");
    obs_property_set_long_description(trail, text("Keyboard.TrailHeight.Description"));
    obs_property_t* seconds = obs_properties_add_float_slider(board, "trail_seconds", text("Keyboard.TrailSeconds"), 0.5, 30.0, 0.5);
    obs_property_float_set_suffix(seconds, " s");
    obs_properties_add_bool(board, "pedals", text("Keyboard.Pedals"));
    obs_properties_add_group(props, "board_group", text("Keyboard.Board"), OBS_GROUP_NORMAL, board);

    // The colours.
    obs_properties_t* colours = obs_properties_create();
    obs_property_t* mode = obs_properties_add_list(colours, "color_mode", text("Keyboard.ColorMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Hands"), (long long) ColorMode::hands);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Keys"), (long long) ColorMode::keys);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Single"), (long long) ColorMode::single);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Rainbow"), (long long) ColorMode::rainbow);
    obs_property_set_long_description(mode, text("Keyboard.ColorMode.Description"));
    obs_property_set_modified_callback(mode, modeChanged);
    auto addHue = [&](const char* name, const char* label) {
        obs_property_t* p = obs_properties_add_int_slider(colours, name, text(label), 0, 360, 1);
        obs_property_int_set_suffix(p, "\xC2\xB0");
        obs_property_set_long_description(p, text("Keyboard.Hue.Description"));
    };
    addHue("hue_left", "Keyboard.HueLeft");
    addHue("hue_right", "Keyboard.HueRight");
    addHue("hue_white", "Keyboard.HueWhite");
    addHue("hue_black", "Keyboard.HueBlack");
    addHue("hue", "Keyboard.HueSingle");
    obs_property_t* saturation = obs_properties_add_int_slider(colours, "saturation", text("Keyboard.Saturation"), 0, 100, 1);
    obs_property_int_set_suffix(saturation, " %");
    obs_property_set_long_description(saturation, text("Keyboard.Saturation.Description"));
    obs_property_t* adaptive = obs_properties_add_bool(colours, "hand_adaptive", text("Keyboard.HandAdaptive"));
    obs_property_set_long_description(adaptive, text("Keyboard.HandAdaptive.Description"));
    obs_property_set_modified_callback(adaptive, modeChanged);
    keyList(colours, "hand_split", "Keyboard.HandSplit");
    obs_properties_add_group(props, "colours_group", text("Keyboard.Colours"), OBS_GROUP_NORMAL, colours);

    // The notes.
    obs_properties_t* notes = obs_properties_create();
    obs_property_t* tails = obs_properties_add_bool(notes, "pedal_tails", text("Keyboard.PedalTails"));
    obs_property_set_long_description(tails, text("Keyboard.PedalTails.Description"));
    obs_property_set_modified_callback(tails, modeChanged);
    auto share = [&](const char* name, const char* label, const char* description, int low) {
        obs_property_t* p = obs_properties_add_int_slider(notes, name, text(label), low, 100, 1);
        obs_property_int_set_suffix(p, " %");
        obs_property_set_long_description(p, text(description));
    };
    share("tail_width", "Keyboard.TailWidth", "Keyboard.TailWidth.Description", 10);
    share("tail_opacity", "Keyboard.TailOpacity", "Keyboard.TailOpacity.Description", 10);
    share("tail_fine", "Keyboard.TailFine", "Keyboard.TailFine.Description", 0);
    share("pedal_line", "Keyboard.PedalLine", "Keyboard.PedalLine.Description", 0);
    share("pedal_glow", "Keyboard.PedalGlow", "Keyboard.PedalGlow.Description", 0);
    obs_property_t* decay = obs_properties_add_bool(notes, "note_decay", text("Keyboard.NoteDecay"));
    obs_property_set_long_description(decay, text("Keyboard.NoteDecay.Description"));
    obs_property_t* melody = obs_properties_add_bool(notes, "melody", text("Keyboard.Melody"));
    obs_property_set_long_description(melody, text("Keyboard.Melody.Description"));
    obs_properties_add_group(props, "notes_group", text("Keyboard.Notes"), OBS_GROUP_NORMAL, notes);

    // The lights.
    obs_properties_t* lights = obs_properties_create();
    auto percent = [&](obs_properties_t* group, const char* name, const char* label, const char* description) {
        obs_property_t* p = obs_properties_add_int_slider(group, name, text(label), 0, 100, 1);
        obs_property_int_set_suffix(p, " %");
        obs_property_set_long_description(p, text(description));
        return p;
    };
    percent(lights, "glow", "Keyboard.Glow", "Keyboard.Glow.Description");
    percent(lights, "bloom", "Keyboard.Bloom", "Keyboard.Bloom.Description");
    percent(lights, "reflection", "Keyboard.Reflection", "Keyboard.Reflection.Description");
    percent(lights, "ambiance", "Keyboard.Ambiance", "Keyboard.Ambiance.Description");
    obs_property_t* auroraHeight = obs_properties_add_int_slider(lights, "aurora_height", text("Keyboard.AuroraHeight"), 10, 100, 1);
    obs_property_int_set_suffix(auroraHeight, " %");
    obs_property_set_long_description(auroraHeight, text("Keyboard.AuroraHeight.Description"));
    percent(lights, "aurora_speed", "Keyboard.AuroraSpeed", "Keyboard.AuroraSpeed.Description");
    percent(lights, "aurora_rest", "Keyboard.AuroraRest", "Keyboard.AuroraRest.Description");
    obs_property_t* flash = obs_properties_add_bool(lights, "flash", text("Keyboard.Flash"));
    obs_property_set_long_description(flash, text("Keyboard.Flash.Description"));
    obs_property_t* afterglow = obs_properties_add_int_slider(lights, "afterglow_ms", text("Keyboard.Afterglow"), 0, 3000, 10);
    obs_property_int_set_suffix(afterglow, " ms");
    obs_property_set_long_description(afterglow, text("Keyboard.Afterglow.Description"));
    obs_properties_add_group(props, "lights_group", text("Keyboard.Lights"), OBS_GROUP_NORMAL, lights);

    // The sparkles.
    obs_properties_t* sparkles = obs_properties_create();
    percent(sparkles, "sparkles", "Keyboard.Sparkles", "Keyboard.Sparkles.Description");
    obs_property_t* sparkleHeight = obs_properties_add_int_slider(sparkles, "sparkle_height", text("Keyboard.SparkleHeight"), 10, 100, 1);
    obs_property_int_set_suffix(sparkleHeight, " %");
    obs_property_set_long_description(sparkleHeight, text("Keyboard.SparkleHeight.Description"));
    obs_property_t* sparkleSpread = obs_properties_add_int_slider(sparkles, "sparkle_spread", text("Keyboard.SparkleSpread"), 0, 90, 1);
    obs_property_int_set_suffix(sparkleSpread, "\xC2\xB0");
    obs_property_set_long_description(sparkleSpread, text("Keyboard.SparkleSpread.Description"));
    obs_property_t* sparkleTilt = obs_properties_add_int_slider(sparkles, "sparkle_tilt", text("Keyboard.SparkleTilt"), -45, 45, 1);
    obs_property_int_set_suffix(sparkleTilt, "\xC2\xB0");
    obs_property_set_long_description(sparkleTilt, text("Keyboard.SparkleTilt.Description"));
    obs_properties_add_group(props, "sparkles_group", text("Keyboard.SparklesGroup"), OBS_GROUP_NORMAL, sparkles);

    obs_properties_t* velocity = obs_properties_create();
    obs_property_t* accent = obs_properties_add_int_slider(velocity, "velocity_accent", text("Keyboard.Accent"), 0, 100, 1);
    obs_property_int_set_suffix(accent, " %");
    obs_property_set_long_description(accent, text("Keyboard.Accent.Description"));
    obs_property_t* top = obs_properties_add_int_slider(velocity, "velocity_top", text("Keyboard.VelocityTop"), 40, 127, 1);
    obs_property_set_long_description(top, text("Keyboard.VelocityTop.Description"));
    obs_property_t* measure = obs_properties_add_button2(velocity, "velocity_measure", text("Keyboard.VelocityMeasure"), measureVelocity, k);
    obs_property_set_long_description(measure, text("Keyboard.VelocityMeasure.Description"));
    obs_properties_add_group(props, "velocity_group", text("Keyboard.Velocity"), OBS_GROUP_NORMAL, velocity);

    obs_properties_t* chords = obs_properties_create();
    obs_properties_add_int_slider(chords, "chord_notes", text("Keyboard.ChordNotes"), 2, 8, 1);
    obs_property_t* threshold = obs_properties_add_int_slider(chords, "chord_threshold", text("Keyboard.ChordThreshold"), 0, 100, 1);
    obs_property_int_set_suffix(threshold, " %");
    obs_property_set_long_description(threshold, text("Keyboard.ChordThreshold.Description"));
    obs_property_t* intensity = obs_properties_add_int_slider(chords, "chord_intensity", text("Keyboard.ChordIntensity"), 0, 100, 1);
    obs_property_int_set_suffix(intensity, " %");
    obs_properties_add_bool(chords, "chord_wave", text("Keyboard.ChordWave"));
    obs_properties_add_bool(chords, "chord_wash", text("Keyboard.ChordWash"));
    obs_properties_add_bool(chords, "chord_fireworks", text("Keyboard.ChordFireworks"));
    obs_properties_add_bool(chords, "chord_bracket", text("Keyboard.ChordBracket"));
    obs_properties_add_bool(chords, "chord_shake", text("Keyboard.ChordShake"));
    obs_property_t* group = obs_properties_add_group(props, "chords", text("Keyboard.Chords"), OBS_GROUP_CHECKABLE, chords);
    obs_property_set_long_description(group, text("Keyboard.Chords.Description"));

    obs_property_t* offset = obs_properties_add_float_slider(props, "offset_ms", text("Keyboard.Offset"), -1000.0, 1000.0, 1.0);
    obs_property_float_set_suffix(offset, " ms");
    obs_property_set_long_description(offset, text("Keyboard.Offset.Description"));
    return props;
}

} // namespace

void registerKeyboardSource()
{
    obs_source_info info = {};
    info.id = "pianoforge_keyboard";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.update = update;
    info.video_tick = tick;
    info.video_render = render;
    info.get_width = width;
    info.get_height = height;
    info.icon_type = OBS_ICON_TYPE_CUSTOM;
    obs_register_source(&info);
}

} // namespace pfobs
