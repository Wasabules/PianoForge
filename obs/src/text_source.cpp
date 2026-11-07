// The "PianoForge text" source: a template whose fields ({file}, {state}, {preset}, {room}, {engine}, {recording})
// take PianoForge's state, drawn by OBS's own text source (GDI+ on Windows, FreeType elsewhere), whose font, colors,
// outline and background stay its own (its properties, but its text, are this source's).
#include "common.hpp"
#include "overlay.hpp"

#include <mutex>
#include <string>

namespace pfobs {

namespace {

constexpr double kPollSeconds = 0.25;

#if defined(_WIN32)
constexpr const char* kTextType = "text_gdiplus";
#else
constexpr const char* kTextType = "text_ft2_source";
#endif

const char* childType()
{
    const char* latest = obs_get_latest_input_type_id(kTextType);
    return latest != nullptr ? latest : kTextType;
}

struct TextSource {
    obs_source_t* source = nullptr;
    obs_source_t* child = nullptr; // OBS's text source, private
    std::mutex lock;               // the template (written by the interface, read by the graphics thread)
    std::string tmpl;
    // The graphics thread's own.
    pf::AudioFeedReader feed;
    std::uint64_t lastPoll = 0, lastTry = 0;
    std::string shown; // the text the child draws
    bool dirty = true;
};

/// The child's settings: ours (font, colors...), with the text this source makes.
void forward(TextSource* t, obs_data_t* settings)
{
    if (t->child == nullptr)
        return;
    obs_data_t* copy = obs_data_create();
    obs_data_apply(copy, settings);
    obs_data_set_string(copy, "text", t->shown.c_str());
    obs_data_set_bool(copy, "read_from_file", false);
    obs_data_set_bool(copy, "from_file", false);
    obs_source_update(t->child, copy);
    obs_data_release(copy);
}

void tick(void* data, float)
{
    auto* t = static_cast<TextSource*>(data);
    const std::uint64_t now = os_gettime_ns();
    if (now - t->lastPoll < (std::uint64_t) (kPollSeconds * 1e9) && !t->dirty)
        return;
    t->lastPoll = now;
    if (!t->feed.isOpen() && now - t->lastTry > 1'000'000'000ull) {
        t->lastTry = now;
        t->feed.open(pf::FeedReaderKind::text);
    }
    pf::FeedInfo info;
    pf::AudioFeedReader::Format format;
    const bool live = t->feed.isOpen() && t->feed.format(format);
    if (live) {
        for (int attempt = 0; attempt < 4 && !t->feed.info(info); ++attempt) {
        }
        t->feed.heartbeat(pf::FeedMix::heard, 0.0f);
    }
    std::string tmpl;
    {
        const std::lock_guard<std::mutex> guard(t->lock);
        tmpl = t->tmpl;
    }
    const std::string out = live ? formatInfo(tmpl, info, text("Text.Recording"), text("Text.Playing"), text("Text.Stopped")) : std::string();
    if (out != t->shown || t->dirty) {
        t->shown = out;
        t->dirty = false;
        if (t->child != nullptr) {
            obs_data_t* s = obs_data_create();
            obs_data_set_string(s, "text", out.c_str());
            obs_source_update(t->child, s);
            obs_data_release(s);
        }
    }
}

void render(void* data, gs_effect_t*)
{
    auto* t = static_cast<TextSource*>(data);
    if (t->child != nullptr)
        obs_source_video_render(t->child);
}

uint32_t width(void* data)
{
    auto* t = static_cast<TextSource*>(data);
    return t->child != nullptr ? obs_source_get_width(t->child) : 0;
}

uint32_t height(void* data)
{
    auto* t = static_cast<TextSource*>(data);
    return t->child != nullptr ? obs_source_get_height(t->child) : 0;
}

const char* name(void*)
{
    return text("Text.Name");
}

/// The text source's defaults, as ours (so that its properties show them), and the template's.
void defaults(obs_data_t* settings)
{
    if (obs_data_t* d = obs_get_source_defaults(childType())) {
        for (obs_data_item_t* item = obs_data_first(d); item != nullptr; obs_data_item_next(&item)) {
            const char* key = obs_data_item_get_name(item);
            switch (obs_data_item_gettype(item)) {
            case OBS_DATA_STRING:
                obs_data_set_default_string(settings, key, obs_data_item_get_default_string(item));
                break;
            case OBS_DATA_NUMBER:
                if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
                    obs_data_set_default_int(settings, key, obs_data_item_get_default_int(item));
                else
                    obs_data_set_default_double(settings, key, obs_data_item_get_default_double(item));
                break;
            case OBS_DATA_BOOLEAN:
                obs_data_set_default_bool(settings, key, obs_data_item_get_default_bool(item));
                break;
            case OBS_DATA_OBJECT:
                if (obs_data_t* o = obs_data_item_get_default_obj(item)) {
                    obs_data_set_default_obj(settings, key, o);
                    obs_data_release(o);
                }
                break;
            default:
                break;
            }
        }
        obs_data_release(d);
    }
    obs_data_set_default_string(settings, "template", text("Text.DefaultTemplate"));
}

void update(void* data, obs_data_t* settings)
{
    auto* t = static_cast<TextSource*>(data);
    {
        const std::lock_guard<std::mutex> guard(t->lock);
        t->tmpl = obs_data_get_string(settings, "template");
    }
    t->dirty = true;
    forward(t, settings);
}

void* create(obs_data_t* settings, obs_source_t* source)
{
    auto* t = new TextSource;
    t->source = source;
    t->child = obs_source_create_private(childType(), "PianoForge text (drawing)", nullptr);
    if (t->child == nullptr)
        blog(LOG_WARNING, "[pianoforge] no text source of type %s to draw with: the PianoForge text stays empty", childType());
    update(t, settings);
    return t;
}

void destroy(void* data)
{
    auto* t = static_cast<TextSource*>(data);
    if (t->child != nullptr)
        obs_source_release(t->child);
    delete t;
}

void enumActive(void* data, obs_source_enum_proc_t callback, void* param)
{
    auto* t = static_cast<TextSource*>(data);
    if (t->child != nullptr)
        callback(t->source, t->child, param);
}

/// The text source's properties (its text and file taken out), under the template.
obs_properties_t* properties(void* data)
{
    auto* t = static_cast<TextSource*>(data);
    obs_properties_t* props = obs_properties_create();
    obs_property_t* tmpl = obs_properties_add_text(props, "template", text("Text.Template"), OBS_TEXT_MULTILINE);
    obs_property_set_long_description(tmpl, text("Text.Template.Description"));
    obs_property_t* help = obs_properties_add_text(props, "fields", text("Text.Fields"), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(help, true);
    if (t->child != nullptr) {
        obs_properties_t* own = obs_source_properties(t->child);
        for (const char* gone : { "text", "read_from_file", "from_file", "file", "text_file", "chatlog", "chatlog_lines", "log_mode", "log_lines" })
            obs_properties_remove_by_name(own, gone);
        obs_properties_add_group(props, "look", text("Text.Look"), OBS_GROUP_NORMAL, own);
    }
    return props;
}

} // namespace

void registerTextSource()
{
    obs_source_info info = {};
    info.id = "pianoforge_text";
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
    info.enum_active_sources = enumActive;
    info.icon_type = OBS_ICON_TYPE_TEXT;
    obs_register_source(&info);
}

} // namespace pfobs
