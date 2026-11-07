// The "PianoForge keyboard" video source: the 88 keys as played in PianoForge, the pedals, and the notes rising from
// the keys as they sound (trails). The events come from the feed with their frames; the source shows the frame the
// PianoForge audio source stamps at that instant (the image meets the sound), or, without one, the application's
// newest frame less 20 ms.
#include "common.hpp"
#include "overlay.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <cmath>
#include <mutex>

namespace pfobs {

namespace {

constexpr double kBeatSeconds = 0.25;
constexpr double kFallbackLatency = 0.02; // without an audio source: this far behind the application's newest frame
// Colors as OBS keeps them (0xAABBGGRR).
constexpr std::uint32_t kWhite = 0xFFF2F2F2, kBlack = 0xFF1E1E1E, kGap = 0xFF3C3C3C, kPedalBack = 0xFF333333;
constexpr std::uint32_t kAccent = 0xFFE0A34F; // the application's accent, #4FA3E0

struct Settings {
    int width = 1280, trailHeight = 240;
    double trailSeconds = 4.0, offsetMs = 0.0;
    bool pedals = true;
    std::uint32_t keyColor = kAccent, trailColor = kAccent;
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

std::uint32_t mixColor(std::uint32_t a, std::uint32_t b, double t)
{
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
        const std::uint64_t written = k->feed.eventsWritten();
        k->nextEvent = written - std::min<std::uint64_t>(written, pf::AudioFeedReader::kEventCapacity / 4);
    }
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

struct Painter {
    gs_effect_t* solid = nullptr;
    gs_eparam_t* color = nullptr;
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
    p.linear = gs_get_linear_srgb();
    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(p.linear);
    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    const float width = (float) s.width, top = (float) s.trailHeight, keysHeight = (float) keyboardHeight(s);
    const float blackHeight = 0.62f * keysHeight;
    const double frame = k->frame, rate = k->rate > 0.0 ? k->rate : 48000.0;
    // The trails: each note rising from its key, as long as it was held, at trailHeight per trailSeconds.
    if (s.trailHeight > 0) {
        const double speed = top / std::max(0.5, s.trailSeconds); // pixels per second
        for (const auto& n : k->state.notes()) {
            if (n.key < kFirstKey || n.key > kLastKey)
                continue;
            const auto shape = keyShape(n.key, width);
            const float y1 = (float) (top - (frame - (double) n.start) / rate * speed);
            const float y2 = n.open() ? top : (float) (top - (frame - (double) n.end) / rate * speed);
            const float from = std::max(0.0f, y1), to = std::min(top, std::max(y2, y1 + 2.0f));
            if (to <= 0.0f || from >= top)
                continue;
            const float inset = shape.black ? 0.0f : std::min(2.0f, shape.width * 0.1f);
            p.rect(shape.x + inset, from, shape.width - 2.0f * inset, to - from, withAlpha(s.trailColor, 0.45 + 0.55 * n.velocity / 127.0));
        }
    }
    // The keys: the gaps behind, the white keys, then the black ones over them.
    p.rect(0.0f, top, width, keysHeight, kGap);
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        const auto shape = keyShape(key, width);
        if (shape.black)
            continue;
        const int v = k->state.velocity(key);
        const float gap = shape.width >= 6.0f ? 1.0f : 0.0f;
        p.rect(shape.x + 0.5f * gap, top, shape.width - gap, keysHeight - gap,
               v > 0 ? mixColor(kWhite, s.keyColor, 0.55 + 0.45 * v / 127.0) : kWhite);
    }
    for (int key = kFirstKey; key <= kLastKey; ++key) {
        const auto shape = keyShape(key, width);
        if (!shape.black)
            continue;
        const int v = k->state.velocity(key);
        p.rect(shape.x, top, shape.width, blackHeight, v > 0 ? mixColor(kBlack, s.keyColor, 0.55 + 0.45 * v / 127.0) : kBlack);
    }
    // The pedals: soft, sostenuto, sustain (as on a grand, left to right), each filled as far as it is down.
    if (s.pedals) {
        const float y = top + keysHeight, h = (float) pedalHeight(s), gap = std::max(2.0f, width * 0.004f);
        const float w = (width - 4.0f * gap) / 3.0f;
        const int which[3] = { 2, 1, 0 };
        for (int i = 0; i < 3; ++i) {
            const float x = gap + (float) i * (w + gap);
            p.rect(x, y + 0.2f * h, w, 0.6f * h, kPedalBack);
            p.rect(x, y + 0.2f * h, w * k->state.pedal(which[i]), 0.6f * h, s.keyColor);
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
    return (uint32_t) (k->settings.trailHeight + keyboardHeight(k->settings) + pedalHeight(k->settings));
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
    obs_data_set_default_int(settings, "key_color", kAccent);
    obs_data_set_default_int(settings, "trail_color", kAccent);
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
    s.keyColor = (std::uint32_t) obs_data_get_int(settings, "key_color");
    s.trailColor = (std::uint32_t) obs_data_get_int(settings, "trail_color");
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
    obs_properties_add_color_alpha(props, "key_color", text("Keyboard.KeyColor"));
    obs_properties_add_color_alpha(props, "trail_color", text("Keyboard.TrailColor"));
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
