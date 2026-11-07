// The "PianoForge keyboard" video source: the 88 keys as played in PianoForge, the pedals, and the notes rising from
// the keys as they sound (trails). The events come from the feed with their frames; the source shows the frame the
// PianoForge audio source stamps at that instant (the image meets the sound), or, without one, the application's
// newest frame less 20 ms.
//
// Colours: one for every key pressed, one for the white keys and one for the black, or the hands' (estimated from
// the notes: overlay.hpp), each with a shade for the black keys. Light: a glow rising from the keys pressed, a flash
// at the strike (as strong as the velocity), an afterglow fading once a key is up.
#include "common.hpp"
#include "overlay.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <cmath>
#include <cstdio>
#include <mutex>

namespace pfobs {

namespace {

constexpr double kBeatSeconds = 0.25;
constexpr double kFallbackLatency = 0.02; // without an audio source: this far behind the application's newest frame
constexpr double kFlashSeconds = 0.09;    // the flash's decay
// Colours as OBS keeps them (0xAABBGGRR).
constexpr std::uint32_t kWhite = 0xFFF2F2F2, kBlack = 0xFF1E1E1E, kGap = 0xFF3C3C3C, kPedalBack = 0xFF333333;
constexpr std::uint32_t kAccent = 0xFFE0A34F, kAccentDark = 0xFFB06C2B;   // the application's accent, #4FA3E0, #2B6CB0
constexpr std::uint32_t kBlue = 0xFFF6823B, kDarkBlue = 0xFFAF401E;       // #3B82F6, #1E40AF
constexpr std::uint32_t kOrange = 0xFF1673F9, kDarkOrange = 0xFF0C41C2;   // #F97316, #C2410C

enum class ColorMode : int { single = 0, keys = 1, hands = 2 };

struct Settings {
    int width = 1280, trailHeight = 240;
    double trailSeconds = 4.0, offsetMs = 0.0;
    bool pedals = true;
    ColorMode mode = ColorMode::hands;
    std::uint32_t keyColor = kAccent, trailColor = kAccent;   // one colour
    std::uint32_t whiteColor = kAccent, blackColor = kAccentDark; // the white keys, the black ones
    std::uint32_t leftColor = kBlue, leftBlackColor = kDarkBlue, rightColor = kOrange, rightBlackColor = kDarkOrange;
    bool adaptive = true; // the hands followed; else a fixed split
    int split = 60;
    int glow = 50;          // %
    int afterglowMs = 300;
    bool flash = true;
};

struct Keyboard {
    obs_source_t* source = nullptr;
    std::mutex lock; // the settings (written by the interface, read by the graphics thread)
    Settings settings;
    // The graphics thread's own.
    pf::AudioFeedReader feed;
    KeyboardState state;
    std::uint64_t session = ~0ull, nextEvent = 0, lastTry = 0, lastBeat = 0;
    double frame = 0.0, rate = 0.0;
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

/// Above the keys: the trails, or room for the glow (half a key's length).
int topHeight(const Settings& s)
{
    return std::max(s.trailHeight, s.glow > 0 ? keyboardHeight(s) / 2 : 0);
}

std::uint32_t mixColor(std::uint32_t a, std::uint32_t b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    std::uint32_t out = 0;
    for (int c = 0; c < 32; c += 8) {
        const double x = (double) ((a >> c) & 0xFF), y = (double) ((b >> c) & 0xFF);
        out |= (std::uint32_t) std::lround(x + (y - x) * t) << c;
    }
    return out;
}

std::uint32_t withAlpha(std::uint32_t color, double alpha)
{
    return (color & 0x00FFFFFFu) | ((std::uint32_t) std::lround(std::clamp(alpha, 0.0, 1.0) * ((color >> 24) & 0xFF)) << 24);
}

/// The colour of a key pressed (or of its trail), by the mode.
std::uint32_t litColor(const Settings& s, int key, int hand)
{
    const bool black = isBlack(key);
    switch (s.mode) {
    case ColorMode::single:
        return s.keyColor;
    case ColorMode::keys:
        return black ? s.blackColor : s.whiteColor;
    case ColorMode::hands:
    default:
        if (hand == kLeftHand)
            return black ? s.leftBlackColor : s.leftColor;
        return black ? s.rightBlackColor : s.rightColor;
    }
}

/// How lit a key is at `frame`: `level` toward its colour (held: by its velocity; up: fading over the afterglow), and
/// `flash` toward white (the strike, by its velocity, decaying over kFlashSeconds).
struct Lit {
    double level = 0.0, flash = 0.0;
};

Lit lightOf(const KeyLight& l, double frame, double rate, const Settings& s)
{
    Lit lit;
    if (!l.struck || rate <= 0.0)
        return lit;
    if (l.velocity > 0) {
        lit.level = 0.55 + 0.45 * l.velocity / 127.0;
    } else if (s.afterglowMs > 0) {
        const double since = (frame - (double) l.released) / rate, t = since / (s.afterglowMs * 1e-3);
        if (since >= 0.0 && t < 1.0)
            lit.level = (0.55 + 0.45 * l.lastVelocity / 127.0) * (1.0 - t) * (1.0 - t);
    }
    if (s.flash && lit.level > 0.0) {
        const double since = (frame - (double) l.pressed) / rate;
        if (since >= 0.0)
            lit.flash = 0.75 * (l.lastVelocity / 127.0) * std::exp(-since / kFlashSeconds);
    }
    return lit;
}

void tick(void* data, float)
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
    if (format.session != k->session) { // a new format (or a first one): the keys as the last events leave them
        k->session = format.session;
        k->rate = format.sampleRate;
        k->state.reset();
        k->state.setRate(k->rate);
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
    if (now - k->lastBeat > (std::uint64_t) (kBeatSeconds * 1e9)) {
        k->feed.heartbeat(pf::FeedMix::heard, 0.0f);
        k->lastBeat = now;
    }
}

float srgbToLinear(float u)
{
    return u <= 0.04045f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f);
}

struct Painter {
    gs_effect_t* solid = nullptr;
    gs_eparam_t* color = nullptr;
    gs_technique_t* colored = nullptr;
    bool linear = false;

    void rect(float x, float y, float w, float h, std::uint32_t rgba) const
    {
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
    }

    /// A vertex colour, in the space the framebuffer takes (linear when it converts to sRGB itself).
    std::uint32_t vertexColor(std::uint32_t rgba) const
    {
        if (!linear)
            return rgba;
        std::uint32_t out = rgba & 0xFF000000u;
        for (int c = 0; c < 24; c += 8)
            out |= (std::uint32_t) std::lround(255.0f * srgbToLinear((float) ((rgba >> c) & 0xFF) / 255.0f)) << c;
        return out;
    }

    /// A rectangle of `rgba`, at `alphaBottom` along its lower edge, fading to nothing at its top.
    void glow(float x, float y, float w, float h, std::uint32_t rgba, double alphaBottom) const
    {
        if (w <= 0.0f || h <= 0.0f || alphaBottom <= 0.0 || colored == nullptr)
            return;
        vec4 one;
        vec4_set(&one, 1.0f, 1.0f, 1.0f, 1.0f);
        gs_effect_set_vec4(color, &one);
        const std::uint32_t bottom = vertexColor(withAlpha(rgba, alphaBottom)), top = vertexColor(withAlpha(rgba, 0.0));
        gs_technique_begin(colored);
        gs_technique_begin_pass(colored, 0);
        gs_render_start(true);
        gs_color(top);
        gs_vertex2f(x, y);
        gs_color(top);
        gs_vertex2f(x + w, y);
        gs_color(bottom);
        gs_vertex2f(x, y + h);
        gs_color(bottom);
        gs_vertex2f(x + w, y + h);
        gs_render_stop(GS_TRISTRIP);
        gs_technique_end_pass(colored);
        gs_technique_end(colored);
    }
};

void render(void* data, gs_effect_t*)
{
    auto* k = static_cast<Keyboard*>(data);
    Settings s;
    {
        const std::lock_guard<std::mutex> guard(k->lock);
        s = k->settings;
    }
    Painter p;
    p.solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (p.solid == nullptr)
        return;
    p.color = gs_effect_get_param_by_name(p.solid, "color");
    p.colored = gs_effect_get_technique(p.solid, "SolidColored");
    p.linear = gs_get_linear_srgb();
    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(p.linear);
    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    const float width = (float) s.width, top = (float) topHeight(s), keysHeight = (float) keyboardHeight(s);
    const float blackHeight = 0.62f * keysHeight;
    const double frame = k->frame, rate = k->rate > 0.0 ? k->rate : 48000.0;
    Lit lits[128];
    for (int key = kFirstKey; key <= kLastKey; ++key)
        lits[key] = lightOf(k->state.light(key), frame, rate, s);
    // The glow: rising from each key lit, behind the trails, as strong and as high as the key is lit.
    if (s.glow > 0) {
        const double strength = s.glow / 100.0;
        for (int key = kFirstKey; key <= kLastKey; ++key) {
            const double g = std::min(1.0, lits[key].level + lits[key].flash);
            if (g <= 0.0)
                continue;
            const auto shape = keyShape(key, width);
            const float spread = shape.width * (shape.black ? 2.6f : 2.0f), h = (float) (0.5 * keysHeight * (0.55 + 0.45 * g));
            const auto& light = k->state.light(key);
            p.glow(shape.x + 0.5f * (shape.width - spread), top - h, spread, h, litColor(s, key, light.hand), 0.85 * strength * g);
        }
    }
    // The trails: each note rising from its key, as long as it was held, at trailHeight per trailSeconds.
    if (s.trailHeight > 0) {
        const double speed = s.trailHeight / std::max(0.5, s.trailSeconds); // pixels per second
        const float trailTop = top - (float) s.trailHeight;
        for (const auto& n : k->state.notes()) {
            if (n.key < kFirstKey || n.key > kLastKey)
                continue;
            const auto shape = keyShape(n.key, width);
            const float y1 = (float) (top - (frame - (double) n.start) / rate * speed);
            const float y2 = n.open() ? top : (float) (top - (frame - (double) n.end) / rate * speed);
            const float from = std::max(trailTop, y1), to = std::min(top, std::max(y2, y1 + 2.0f));
            if (to <= trailTop || from >= top)
                continue;
            const float inset = shape.black ? 0.0f : std::min(2.0f, shape.width * 0.1f);
            const std::uint32_t c = s.mode == ColorMode::single ? s.trailColor : litColor(s, n.key, n.hand);
            p.rect(shape.x + inset, from, shape.width - 2.0f * inset, to - from, withAlpha(c, 0.45 + 0.55 * n.velocity / 127.0));
        }
    }
    // The keys: the gaps behind, the white keys, then the black ones over them; each lit toward its colour, and
    // toward white at the strike.
    auto keyColor = [&](int key, std::uint32_t base) {
        const auto& lit = lits[key];
        if (lit.level <= 0.0)
            return base;
        const std::uint32_t c = mixColor(base, litColor(s, key, k->state.light(key).hand), lit.level);
        return lit.flash > 0.0 ? mixColor(c, 0xFFFFFFFFu, std::min(0.8, lit.flash)) : c;
    };
    p.rect(0.0f, top, width, keysHeight, kGap);
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        const auto shape = keyShape(key, width);
        if (shape.black)
            continue;
        const float gap = shape.width >= 6.0f ? 1.0f : 0.0f;
        p.rect(shape.x + 0.5f * gap, top, shape.width - gap, keysHeight - gap, keyColor(key, kWhite));
    }
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        const auto shape = keyShape(key, width);
        if (shape.black)
            p.rect(shape.x, top, shape.width, blackHeight, keyColor(key, kBlack));
    }
    // The pedals: soft, sostenuto, sustain (as on a grand, left to right), each filled as far as it is down.
    if (s.pedals) {
        const float y = top + keysHeight, h = (float) pedalHeight(s), gap = std::max(2.0f, width * 0.004f);
        const float w = (width - 4.0f * gap) / 3.0f;
        const int which[3] = { 2, 1, 0 };
        const std::uint32_t fill = s.mode == ColorMode::single ? s.keyColor : s.mode == ColorMode::keys ? s.whiteColor : s.rightColor;
        for (int i = 0; i < 3; ++i) {
            const float x = gap + (float) i * (w + gap);
            p.rect(x, y + 0.2f * h, w, 0.6f * h, kPedalBack);
            p.rect(x, y + 0.2f * h, w * k->state.pedal(which[i]), 0.6f * h, fill);
        }
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
    return (uint32_t) (topHeight(k->settings) + keyboardHeight(k->settings) + pedalHeight(k->settings));
}

const char* name(void*)
{
    return text("Keyboard.Name");
}

void defaults(obs_data_t* settings)
{
    obs_data_set_default_int(settings, "width", 1280);
    obs_data_set_default_int(settings, "trail_height", 240);
    obs_data_set_default_double(settings, "trail_seconds", 4.0);
    obs_data_set_default_bool(settings, "pedals", true);
    obs_data_set_default_int(settings, "color_mode", (int) ColorMode::hands);
    obs_data_set_default_int(settings, "key_color", kAccent);
    obs_data_set_default_int(settings, "trail_color", kAccent);
    obs_data_set_default_int(settings, "white_color", kAccent);
    obs_data_set_default_int(settings, "black_color", kAccentDark);
    obs_data_set_default_int(settings, "left_color", kBlue);
    obs_data_set_default_int(settings, "left_black_color", kDarkBlue);
    obs_data_set_default_int(settings, "right_color", kOrange);
    obs_data_set_default_int(settings, "right_black_color", kDarkOrange);
    obs_data_set_default_bool(settings, "hand_adaptive", true);
    obs_data_set_default_int(settings, "hand_split", 60);
    obs_data_set_default_int(settings, "glow", 50);
    obs_data_set_default_int(settings, "afterglow_ms", 300);
    obs_data_set_default_bool(settings, "flash", true);
    obs_data_set_default_double(settings, "offset_ms", 0.0);
}

void update(void* data, obs_data_t* settings)
{
    auto* k = static_cast<Keyboard*>(data);
    Settings s;
    s.width = (int) std::clamp<long long>(obs_data_get_int(settings, "width"), 320, 7680);
    s.trailHeight = (int) std::clamp<long long>(obs_data_get_int(settings, "trail_height"), 0, 4320);
    s.trailSeconds = std::clamp(obs_data_get_double(settings, "trail_seconds"), 0.5, 30.0);
    s.pedals = obs_data_get_bool(settings, "pedals");
    s.mode = (ColorMode) std::clamp<long long>(obs_data_get_int(settings, "color_mode"), 0, 2);
    s.keyColor = (std::uint32_t) obs_data_get_int(settings, "key_color");
    s.trailColor = (std::uint32_t) obs_data_get_int(settings, "trail_color");
    s.whiteColor = (std::uint32_t) obs_data_get_int(settings, "white_color");
    s.blackColor = (std::uint32_t) obs_data_get_int(settings, "black_color");
    s.leftColor = (std::uint32_t) obs_data_get_int(settings, "left_color");
    s.leftBlackColor = (std::uint32_t) obs_data_get_int(settings, "left_black_color");
    s.rightColor = (std::uint32_t) obs_data_get_int(settings, "right_color");
    s.rightBlackColor = (std::uint32_t) obs_data_get_int(settings, "right_black_color");
    s.adaptive = obs_data_get_bool(settings, "hand_adaptive");
    s.split = (int) std::clamp<long long>(obs_data_get_int(settings, "hand_split"), kFirstKey, kLastKey);
    s.glow = (int) std::clamp<long long>(obs_data_get_int(settings, "glow"), 0, 100);
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
    delete static_cast<Keyboard*>(data);
}

/// The colours shown for the mode chosen; the split, for a fixed one.
bool modeChanged(obs_properties_t* props, obs_property_t*, obs_data_t* settings)
{
    const auto mode = (ColorMode) obs_data_get_int(settings, "color_mode");
    const bool adaptive = obs_data_get_bool(settings, "hand_adaptive");
    auto show = [&](const char* name, bool on) {
        if (obs_property_t* p = obs_properties_get(props, name))
            obs_property_set_visible(p, on);
    };
    show("key_color", mode == ColorMode::single);
    show("trail_color", mode == ColorMode::single);
    show("white_color", mode == ColorMode::keys);
    show("black_color", mode == ColorMode::keys);
    for (const char* name : { "left_color", "left_black_color", "right_color", "right_black_color", "hand_adaptive" })
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
    obs_property_set_long_description(mode, text("Keyboard.ColorMode.Description"));
    obs_property_set_modified_callback(mode, modeChanged);
    obs_properties_add_color_alpha(props, "key_color", text("Keyboard.KeyColor"));
    obs_properties_add_color_alpha(props, "trail_color", text("Keyboard.TrailColor"));
    obs_properties_add_color_alpha(props, "white_color", text("Keyboard.WhiteColor"));
    obs_properties_add_color_alpha(props, "black_color", text("Keyboard.BlackColor"));
    obs_properties_add_color_alpha(props, "left_color", text("Keyboard.LeftColor"));
    obs_properties_add_color_alpha(props, "left_black_color", text("Keyboard.LeftBlackColor"));
    obs_properties_add_color_alpha(props, "right_color", text("Keyboard.RightColor"));
    obs_properties_add_color_alpha(props, "right_black_color", text("Keyboard.RightBlackColor"));
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
