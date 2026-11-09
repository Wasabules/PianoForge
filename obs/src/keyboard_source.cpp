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
#include "common.hpp"
#include "overlay.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <array>
#include <cmath>
#include <cstdio>
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
};

/// The shader and its parameters, the unit square every shape is drawn on and the sparkles' vertices, made once on
/// the graphics thread.
struct Drawing {
    gs_effect_t* effect = nullptr;
    gs_vertbuffer_t *quad = nullptr, *sparks = nullptr;
    gs_technique_t *box = nullptr, *glow = nullptr, *beam = nullptr, *sparkle = nullptr;
    gs_eparam_t *quadOrigin = nullptr, *quadSize = nullptr, *boxCenter = nullptr, *boxHalf = nullptr, *boxRadius = nullptr,
                *boxBorder = nullptr, *boxSoft = nullptr, *fillTop = nullptr, *fillBottom = nullptr, *fillCurve = nullptr,
                *borderColor = nullptr, *lightColor = nullptr, *lightStrength = nullptr, *lightFrom = nullptr,
                *lightReach = nullptr, *fadeBand = nullptr, *time = nullptr, *seed = nullptr;
    bool failed = false;

    bool ready() const noexcept
    {
        return effect != nullptr && quad != nullptr && sparks != nullptr && box && glow && beam && sparkle && quadOrigin && quadSize && boxCenter && boxHalf && boxRadius
               && boxBorder && boxSoft && fillTop && fillBottom && fillCurve && borderColor && lightColor && lightStrength
               && lightFrom && lightReach && fadeBand && time && seed;
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
    std::uint64_t session = ~0ull, nextEvent = 0, lastTry = 0, lastBeat = 0;
    double frame = 0.0, rate = 0.0;
    int sparksWidth = 0; // the width the sparks were thrown over
    Drawing look;
    std::atomic<bool> following { false }, connected { false }; // (read by the properties)
};

int keyboardHeight(const Settings& s)
{
    return std::max(24, (int) std::lround(s.width * 0.125));
}

int pedalHeight(const Settings& s)
{
    return s.pedals ? std::max(6, keyboardHeight(s) / 8) : 0;
}

/// Above the keys: the notes, or room for the lights and sparkles (0.6 of a key's length).
int topHeight(const Settings& s)
{
    return std::max(s.trailHeight, s.glow > 0 || s.sparkles > 0 ? (int) std::lround(0.6 * keyboardHeight(s)) : 0);
}

/// Where things are, in the source's pixels: the area above the keys (0 to `top`), the rail, the keys (from
/// `keysTop` to `bottom`), the pedals under them.
struct Layout {
    float width = 0, top = 0, rail = 0, keysTop = 0, keysHeight = 0, blackHeight = 0, white = 0, bottom = 0;
    float pedalTop = 0, pedalHeight = 0, height = 0;
};

Layout layoutOf(const Settings& s)
{
    Layout l;
    const float band = (float) keyboardHeight(s);
    l.width = (float) s.width;
    l.top = (float) topHeight(s);
    l.rail = std::max(2.0f, std::round(band * 0.05f));
    l.keysTop = l.top + l.rail;
    l.keysHeight = band - l.rail;
    l.blackHeight = 0.64f * l.keysHeight;
    l.white = l.width / (float) kWhiteKeys;
    l.bottom = l.top + band;
    l.pedalTop = l.bottom;
    l.pedalHeight = (float) pedalHeight(s);
    l.height = l.bottom + l.pedalHeight;
    return l;
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
    const double strength = 0.55 + 0.45 * l.lastVelocity / 127.0;
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
        lit.flash = (l.lastVelocity / 127.0) * std::exp(-since / kFlashSeconds);
    return lit;
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

    // The sparkles: a stream at each strike, a few rising from the keys held, as high and in the direction chosen;
    // they move on.
    const Layout l = layoutOf(s);
    const float amount = s.sparkles / 100.0f, dt = std::clamp(seconds, 0.0f, 0.1f);
    k->state.takeStrikes(k->strikes);
    if (k->sparksWidth != s.width || amount <= 0.0f) {
        k->sparkles.clear();
        k->sparksWidth = s.width;
    }
    if (amount > 0.0f && l.top > 4.0f && k->rate > 0.0) {
        constexpr float radians = 3.14159265f / 180.0f;
        const float height = s.sparkleHeight / 100.0f * l.top, tilt = s.sparkleTilt * radians, spread = s.sparkleSpread * radians;
        for (const auto& n : k->strikes) {
            if (n.key < kFirstKey || n.key > kLastKey || (k->frame - (double) n.start) / k->rate > kRecentStrike)
                continue;
            const auto shape = keyShape(n.key, l.width);
            const Tint tint = tintOf(s, n.key, n.hand);
            const float v = n.velocity / 127.0f;
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
            owed += dt * amount * (0.8f + 2.5f * light.velocity / 127.0f);
            if (owed < 1.0f)
                continue;
            const auto shape = keyShape(key, l.width);
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
        k->sparkles.step(dt, 0.3f * l.white);
    }
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
        d.sparkle = gs_effect_get_technique(d.effect, "Sparkles");
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
    float alpha = 1.0f, curve = 1.0f;
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
    Painter(const Drawing& d, float w, float h, bool linearColors) : look(d), width(w), height(h), linear(linearColors) {}
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
        color(look.fillTop, st.top, st.alpha);
        color(look.fillBottom, st.bottom, st.alpha);
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
            const float b = sp.brightness(), r = sp.size;
            const float x0 = std::max(sp.x - r, 0.0f), x1 = std::min(sp.x + r, width);
            const float y0 = std::max(sp.y - r, 0.0f), y1 = std::min(sp.y + r, height);
            if (b <= 0.003f || r <= 0.5f || x1 <= x0 || y1 <= y0)
                continue;
            const float u0 = (x0 - sp.x) / r, u1 = (x1 - sp.x) / r, v0 = (y0 - sp.y) / r, v1 = (y1 - sp.y) / r;
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

    void draw(float x0, float y0, float x1, float y1)
    {
        x0 = std::max(x0, 0.0f);
        y0 = std::max(y0, 0.0f);
        x1 = std::min(x1, width);
        y1 = std::min(y1, height);
        if (current == nullptr || x1 - x0 < 0.01f || y1 - y0 < 0.01f)
            return;
        pair(look.quadOrigin, x0, y0);
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
    float fadeNone = -2.0f, fadeWhole = -1.0f, now = 0.0f;
};

/// The notes: rounded bars rising from their keys, as long as they were held, at trailHeight per trailSeconds; a
/// halo around each (added), then the bar (a gradient toward its hue, a pale rim, lit from the key while it is
/// held), fading toward the top. The white keys' notes first, the black keys' (narrower) over them.
void drawNotes(const Keyboard& k, const Settings& s, const Layout& l, Painter& p, float lights)
{
    if (s.trailHeight <= 0)
        return;
    const double frame = k.frame, rate = k.rate > 0.0 ? k.rate : 48000.0;
    const double speed = s.trailHeight / std::max(0.5, s.trailSeconds); // pixels per second
    const float trailTop = l.top - (float) s.trailHeight;
    struct Bar {
        float x, y0, y1, w, velocity;
        bool open, black;
        Tint tint;
    };
    std::vector<Bar> bars;
    bars.reserve(k.state.notes().size());
    for (const auto& n : k.state.notes()) {
        if (n.key < kFirstKey || n.key > kLastKey)
            continue;
        const auto shape = keyShape(n.key, l.width);
        const float w = shape.black ? shape.width * 0.92f : l.white * 0.78f;
        float y0 = (float) (l.top - (frame - (double) n.start) / rate * speed);
        const float y1 = n.open() ? l.top : (float) (l.top - (frame - (double) n.end) / rate * speed);
        y0 = std::min(y0, y1 - 0.6f * w); // a short note: a pill
        if (y1 <= trailTop || y0 >= l.top)
            continue;
        bars.push_back({ shape.x + 0.5f * (shape.width - w), std::max(y0, trailTop - w), std::min(y1, l.top), w, n.velocity / 127.0f,
                         n.open(), shape.black, tintOf(s, n.key, n.hand) });
    }
    p.fade(trailTop, trailTop + 0.3f * (float) s.trailHeight);
    if (lights > 0.0f) {
        p.add();
        for (const auto& b : bars)
            p.glow(b.x + 0.5f * b.w, 0.5f * (b.y0 + b.y1), 0.5f * b.w, 0.5f * (b.y1 - b.y0), 0.42f * b.w, 0.5f * l.white,
                   b.tint.light, lights * (0.22f + 0.3f * b.velocity) * (b.open ? 1.0f : 0.6f));
    }
    p.over();
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& b : bars) {
            if (b.black != (pass == 1))
                continue;
            BoxStyle st;
            st.top = mixColor(b.tint.light, b.tint.pale, 0.35);
            st.bottom = b.tint.main;
            st.alpha = 0.62f + 0.38f * b.velocity;
            st.curve = 0.8f;
            st.rim = b.tint.pale;
            st.rimAlpha = 0.95f;
            st.rimWidth = std::max(1.0f, 0.07f * l.white);
            st.light = b.tint.pale;
            st.lightStrength = b.open ? 0.3f : 0.0f;
            st.lightFrom = 1.0f;
            st.radius = 0.42f * b.w;
            p.box(b.x, b.y0, b.w, b.y1 - b.y0, st);
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

    // The beams, behind the notes: rising from each key lit, as high and as bright as it is.
    if (lights > 0.0f && l.top > 1.0f) {
        p.add();
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            const auto& lit = lits[(std::size_t) key];
            if (lit.level <= 0.0 && lit.flash <= 0.0)
                continue;
            const auto shape = keyShape(key, l.width);
            const float h = std::min(l.top, 1.4f * (l.bottom - l.top) * (float) (0.45 + 0.55 * lit.level + 0.3 * lit.flash));
            p.beam(shape.x + 0.5f * shape.width, l.top, wk * (shape.black ? 1.0f : 1.25f), h, tints[(std::size_t) key].light,
                   lights * (float) (0.32 * lit.level + 0.55 * lit.flash), (float) (key - kFirstKey) / 88.0f);
        }
    }
    drawNotes(k, s, l, p, lights);

    // The keyboard: its bed (between the keys), the rail and its felt.
    p.over();
    BoxStyle bed;
    bed.top = kBedTop;
    bed.bottom = kBedBottom;
    bed.radius = 0.3f * wk;
    p.box(0.0f, l.top, l.width, l.bottom - l.top, bed, true);
    BoxStyle rail;
    rail.top = kRailTop;
    rail.bottom = kRailBottom;
    p.box(0.0f, l.top, l.width, l.rail, rail);
    BoxStyle edge;
    edge.top = edge.bottom = kRailEdge;
    edge.alpha = 0.8f;
    p.box(0.0f, l.top, l.width, 1.0f, edge);
    BoxStyle felt;
    felt.top = felt.bottom = kFelt;
    const float feltHeight = std::max(1.0f, std::round(0.3f * l.rail));
    p.box(0.0f, l.keysTop - feltHeight, l.width, feltHeight, felt);

    // The white keys: a front edge (the lip), less of it as the key goes down, and the top over it; lit toward the
    // key's hue, from within at the strike.
    const float gap = wk >= 10.0f ? 1.0f : 0.5f, radius = 0.16f * wk;
    const float lipUp = std::max(2.0f, 0.075f * l.keysHeight), lipDown = std::max(1.0f, 0.025f * l.keysHeight);
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        if (isBlack(key))
            continue;
        const auto shape = keyShape(key, l.width);
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
        const auto shape = keyShape(key, l.width);
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
        const auto shape = keyShape(key, l.width);
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
        const auto shape = keyShape(key, l.width);
        const auto& tint = tints[(std::size_t) key];
        const float cx = shape.x + 0.5f * shape.width, fl = (float) lit.flash;
        p.glow(cx, l.keysTop, 0.45f * shape.width, 1.0f, 1.0f, 0.35f * wk, tint.light, lights * (float) (0.45 * lit.level + 0.6 * fl));
        if (fl > 0.02f) {
            p.glow(cx, l.keysTop, 0.3f * shape.width, 0.5f, 0.5f, wk * (0.6f + 0.8f * fl), tint.pale, 0.9f * fl * (0.4f + 0.6f * lights));
            p.glow(cx, l.top + 0.5f * l.rail, wk * (1.0f + 3.0f * fl), 0.4f, 0.4f, 1.5f + 2.0f * fl, tint.pale, 0.8f * fl);
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
            const auto shape = keyShape(n.key, l.width);
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
            const auto shape = keyShape(key, l.width);
            const double lv = lightOf(k.state.light(key), frame, rate, s).level;
            const std::uint32_t base = pass == 1 ? kEbonyBottom : kIvory;
            const std::uint32_t c = mixColor(base, tintOf(s, key, k.state.light(key).hand).main, lv);
            rect(shape.x + (pass == 0 ? 0.5f : 0.0f), l.keysTop, shape.width - (pass == 0 ? 1.0f : 0.0f),
                 pass == 1 ? l.blackHeight : l.bottom - l.keysTop, c);
        }
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
    const Layout l = layoutOf(s);
    const std::lock_guard<std::mutex> drawing(k->drawing);
    const bool linear = gs_get_linear_srgb(), previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(linear);
    gs_blend_state_push();
    if (k->look.ready()) {
        Painter painter(k->look, l.width, l.height, linear);
        drawKeyboard(*k, s, l, painter);
    } else
        drawPlain(*k, s, l, linear);
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
    return (uint32_t) (topHeight(k->settings) + keyboardHeight(k->settings) + pedalHeight(k->settings));
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
    if (k->look.effect != nullptr || k->look.quad != nullptr) {
        obs_enter_graphics();
        freeDrawing(k->look);
        obs_leave_graphics();
    }
    delete k;
}

/// The hues shown for the mode chosen; the split, for a fixed one.
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
    obs_property_t* w = obs_properties_add_int(props, "width", text("Keyboard.Width"), 320, 7680, 1);
    obs_property_int_set_suffix(w, " px");
    obs_property_t* trail = obs_properties_add_int(props, "trail_height", text("Keyboard.TrailHeight"), 0, 4320, 1);
    obs_property_int_set_suffix(trail, " px");
    obs_property_set_long_description(trail, text("Keyboard.TrailHeight.Description"));
    obs_property_t* seconds = obs_properties_add_float_slider(props, "trail_seconds", text("Keyboard.TrailSeconds"), 0.5, 30.0, 0.5);
    obs_property_float_set_suffix(seconds, " s");
    obs_properties_add_bool(props, "pedals", text("Keyboard.Pedals"));

    obs_property_t* mode = obs_properties_add_list(props, "color_mode", text("Keyboard.ColorMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Hands"), (long long) ColorMode::hands);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Keys"), (long long) ColorMode::keys);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Single"), (long long) ColorMode::single);
    obs_property_list_add_int(mode, text("Keyboard.ColorMode.Rainbow"), (long long) ColorMode::rainbow);
    obs_property_set_long_description(mode, text("Keyboard.ColorMode.Description"));
    obs_property_set_modified_callback(mode, modeChanged);
    auto addHue = [&](const char* name, const char* label) {
        obs_property_t* p = obs_properties_add_int_slider(props, name, text(label), 0, 360, 1);
        obs_property_int_set_suffix(p, "\xC2\xB0");
        obs_property_set_long_description(p, text("Keyboard.Hue.Description"));
    };
    addHue("hue_left", "Keyboard.HueLeft");
    addHue("hue_right", "Keyboard.HueRight");
    addHue("hue_white", "Keyboard.HueWhite");
    addHue("hue_black", "Keyboard.HueBlack");
    addHue("hue", "Keyboard.HueSingle");
    obs_property_t* saturation = obs_properties_add_int_slider(props, "saturation", text("Keyboard.Saturation"), 0, 100, 1);
    obs_property_int_set_suffix(saturation, " %");
    obs_property_set_long_description(saturation, text("Keyboard.Saturation.Description"));
    obs_property_t* adaptive = obs_properties_add_bool(props, "hand_adaptive", text("Keyboard.HandAdaptive"));
    obs_property_set_long_description(adaptive, text("Keyboard.HandAdaptive.Description"));
    obs_property_set_modified_callback(adaptive, modeChanged);
    obs_property_t* split = obs_properties_add_list(props, "hand_split", text("Keyboard.HandSplit"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        char label[48];
        std::snprintf(label, sizeof(label), key == 60 ? "%s%d (%s)" : "%s%d", names[key % 12], key / 12 - 1, text("Keyboard.MiddleC"));
        obs_property_list_add_int(split, label, key);
    }

    obs_property_t* glow = obs_properties_add_int_slider(props, "glow", text("Keyboard.Glow"), 0, 100, 1);
    obs_property_int_set_suffix(glow, " %");
    obs_property_set_long_description(glow, text("Keyboard.Glow.Description"));
    obs_property_t* sparkles = obs_properties_add_int_slider(props, "sparkles", text("Keyboard.Sparkles"), 0, 100, 1);
    obs_property_int_set_suffix(sparkles, " %");
    obs_property_set_long_description(sparkles, text("Keyboard.Sparkles.Description"));
    obs_property_t* sparkleHeight = obs_properties_add_int_slider(props, "sparkle_height", text("Keyboard.SparkleHeight"), 10, 100, 1);
    obs_property_int_set_suffix(sparkleHeight, " %");
    obs_property_set_long_description(sparkleHeight, text("Keyboard.SparkleHeight.Description"));
    obs_property_t* sparkleSpread = obs_properties_add_int_slider(props, "sparkle_spread", text("Keyboard.SparkleSpread"), 0, 90, 1);
    obs_property_int_set_suffix(sparkleSpread, "\xC2\xB0");
    obs_property_set_long_description(sparkleSpread, text("Keyboard.SparkleSpread.Description"));
    obs_property_t* sparkleTilt = obs_properties_add_int_slider(props, "sparkle_tilt", text("Keyboard.SparkleTilt"), -45, 45, 1);
    obs_property_int_set_suffix(sparkleTilt, "\xC2\xB0");
    obs_property_set_long_description(sparkleTilt, text("Keyboard.SparkleTilt.Description"));
    obs_property_t* flash = obs_properties_add_bool(props, "flash", text("Keyboard.Flash"));
    obs_property_set_long_description(flash, text("Keyboard.Flash.Description"));
    obs_property_t* afterglow = obs_properties_add_int_slider(props, "afterglow_ms", text("Keyboard.Afterglow"), 0, 3000, 10);
    obs_property_int_set_suffix(afterglow, " ms");
    obs_property_set_long_description(afterglow, text("Keyboard.Afterglow.Description"));
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
