// The keyboard sources' look, shared with PianoForge's "Stream look" window through the look channel
// (pianoforge/look_channel.hpp; the settings: pianoforge/stream_look.hpp). An edit in OBS reaches PianoForge; one in
// PianoForge reaches every keyboard source synchronised with it ("sync_app", on by default): they all share one look.
// The newest edit wins, on whichever side (each look carries the wall-clock instant of its edit, kept in the source's
// settings as "sync_changed"; at a tie, OBS's).
//
// And the window's preview: while PianoForge asks for it, a private keyboard source (in no scene) with the shared look
// is rendered after OBS's main view, at most 30 times a second and as large as asked, and its frames written to the
// channel (staged on the GPU one frame, read back the next: no wait).
#include "common.hpp"

#include <pianoforge/look_channel.hpp>

#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pfobs {

namespace {

constexpr double kPollSeconds = 0.1;
constexpr std::uint64_t kFrameNs = 33'000'000ull;           // the preview: 30 frames a second at most
constexpr std::uint64_t kPreviewKeepNs = 10'000'000'000ull; // the preview's source kept 10 s after the last request

struct Sync {
    std::mutex lock;
    pf::LookChannel channel;
    std::string text;            // the shared look (stream_look's text)
    std::uint64_t changedMs = 0; // when it was edited
    bool fromObs = false;        // it came from OBS (at a tie, OBS's wins)
    bool pending = false;        // the synchronised sources to bring to it
    std::uint64_t appSerial = 0;
    obs_source_t* preview = nullptr;
    std::uint64_t wantedNs = 0;  // the last time the preview was asked for
    std::thread thread;
    std::atomic<bool> running { false };
    // The graphics thread's own.
    gs_texrender_t* target = nullptr;
    gs_stagesurf_t* stages[2] {};
    std::uint32_t stageWidth = 0, stageHeight = 0;
    int staged = -1;             // the stage surface holding last frame's image
    std::uint64_t lastFrameNs = 0;
};

Sync* sync = nullptr;

/// Brings `source` to the shared look, if it is synchronised and not there already.
void bringTo(obs_source_t* source, const std::string& text, std::uint64_t changedMs)
{
    obs_data_t* current = obs_source_get_settings(source);
    const bool synced = obs_data_get_bool(current, "sync_app");
    const bool there = lookOf(current).text() == text;
    obs_data_release(current);
    if (!synced || there)
        return;
    pf::look::Values values;
    values.read(text);
    obs_data_t* data = obs_data_create();
    writeLook(data, values, false);
    obs_data_set_int(data, "sync_changed", (long long) changedMs);
    obs_source_update(source, data);
    obs_data_release(data);
    obs_source_update_properties(source); // an open properties window shows it
}

/// Every synchronised keyboard source (the preview's too) brought to the shared look.
void bringAll()
{
    std::string text;
    std::uint64_t changed = 0;
    obs_source_t* preview = nullptr;
    {
        const std::lock_guard<std::mutex> guard(sync->lock);
        if (!sync->pending || sync->text.empty())
            return;
        sync->pending = false;
        text = sync->text;
        changed = sync->changedMs;
        preview = sync->preview != nullptr ? obs_source_get_ref(sync->preview) : nullptr;
    }
    std::vector<obs_source_t*> sources;
    obs_enum_sources(
        [](void* param, obs_source_t* source) {
            if (std::strcmp(obs_source_get_unversioned_id(source), "pianoforge_keyboard") == 0)
                if (obs_source_t* ref = obs_source_get_ref(source))
                    static_cast<std::vector<obs_source_t*>*>(param)->push_back(ref);
            return true;
        },
        &sources);
    if (preview != nullptr)
        sources.push_back(preview);
    for (obs_source_t* source : sources) {
        bringTo(source, text, changed);
        obs_source_release(source);
    }
}

/// The preview's source: made when PianoForge asks for it, let go 10 s after the last request.
void keepPreview()
{
    std::uint32_t w = 0, h = 0;
    const bool wanted = sync->channel.previewWanted(w, h);
    const std::uint64_t now = os_gettime_ns();
    obs_source_t* release = nullptr;
    std::string text;
    {
        const std::lock_guard<std::mutex> guard(sync->lock);
        if (wanted)
            sync->wantedNs = now;
        if (!wanted && sync->preview != nullptr && now - sync->wantedNs > kPreviewKeepNs) {
            release = sync->preview;
            sync->preview = nullptr;
        }
        if (!wanted || sync->preview != nullptr)
            text.clear();
        else
            text = sync->text.empty() ? pf::look::Values().text() : sync->text;
    }
    if (release != nullptr)
        obs_source_release(release);
    if (text.empty())
        return;
    pf::look::Values values;
    values.read(text);
    obs_data_t* data = obs_data_create();
    writeLook(data, values, false);
    obs_data_set_bool(data, "sync_app", true);
    obs_data_set_bool(data, "sync_preview", true);
    obs_source_t* preview = obs_source_create_private("pianoforge_keyboard", "PianoForge keyboard (Stream look preview)", data);
    obs_data_release(data);
    const std::lock_guard<std::mutex> guard(sync->lock);
    if (sync->preview == nullptr)
        sync->preview = preview;
    else if (preview != nullptr)
        obs_source_release(preview);
}

void poll()
{
    Sleeper sleeper;
    while (sync->running.load()) {
        std::string text;
        std::uint64_t changed = 0;
        if (sync->channel.other(sync->appSerial, text, changed)) {
            const std::lock_guard<std::mutex> guard(sync->lock);
            if (text != sync->text) {
                const bool newer = sync->text.empty() || changed > sync->changedMs;
                if (newer) { // PianoForge's edit is the newest: every synchronised source takes it
                    sync->text = text;
                    sync->changedMs = changed;
                    sync->fromObs = false;
                    sync->pending = true;
                } else { // OBS's is: PianoForge is told again (it takes it)
                    sync->channel.publish(sync->text, sync->changedMs);
                }
            }
        }
        bringAll();
        keepPreview();
        sleeper.sleep(kPollSeconds);
    }
}

/// The graphics thread, after OBS's main view: the preview's frame, while PianoForge asks for it.
void renderPreview(void*)
{
    if (sync == nullptr)
        return;
    std::uint32_t maxWidth = 0, maxHeight = 0;
    const std::uint64_t now = os_gettime_ns();
    if (!sync->channel.previewWanted(maxWidth, maxHeight) || now - sync->lastFrameNs < kFrameNs)
        return;
    obs_source_t* source = nullptr;
    {
        const std::lock_guard<std::mutex> guard(sync->lock);
        if (sync->preview != nullptr)
            source = obs_source_get_ref(sync->preview);
    }
    if (source == nullptr)
        return;
    sync->lastFrameNs = now;
    const std::uint32_t sw = obs_source_get_width(source), sh = obs_source_get_height(source);
    if (sw > 0 && sh > 0) {
        const double scale = std::min({ 1.0, (double) maxWidth / sw, (double) maxHeight / sh });
        const auto w = (std::uint32_t) std::max(1L, std::lround(sw * scale)), h = (std::uint32_t) std::max(1L, std::lround(sh * scale));
        if (sync->target == nullptr)
            sync->target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        gs_texrender_reset(sync->target);
        if (sync->target != nullptr && gs_texrender_begin(sync->target, w, h)) {
            vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
            gs_ortho(0.0f, (float) sw, 0.0f, (float) sh, -100.0f, 100.0f); // the whole source, scaled to the frame
            gs_blend_state_push();
            gs_reset_blend_state();
            obs_source_video_render(source);
            gs_blend_state_pop();
            gs_texrender_end(sync->target);
            if (w != sync->stageWidth || h != sync->stageHeight) {
                for (auto*& stage : sync->stages) {
                    if (stage != nullptr)
                        gs_stagesurface_destroy(stage);
                    stage = gs_stagesurface_create(w, h, GS_BGRA);
                }
                sync->stageWidth = w;
                sync->stageHeight = h;
                sync->staged = -1;
            }
            const int next = sync->staged == 0 ? 1 : 0;
            if (sync->stages[next] != nullptr) {
                gs_stage_texture(sync->stages[next], gs_texrender_get_texture(sync->target));
                if (sync->staged >= 0 && sync->stages[sync->staged] != nullptr) { // last frame's, ready by now
                    std::uint8_t* data = nullptr;
                    std::uint32_t lineBytes = 0;
                    if (gs_stagesurface_map(sync->stages[sync->staged], &data, &lineBytes)) {
                        sync->channel.putFrame(data, lineBytes, w, h);
                        gs_stagesurface_unmap(sync->stages[sync->staged]);
                    }
                }
                sync->staged = next;
            }
        }
    }
    obs_source_release(source);
}

} // namespace

pf::look::Values lookOf(obs_data_t* settings)
{
    pf::look::Values values;
    const auto& entries = pf::look::entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        switch (e.kind) {
        case pf::look::Kind::toggle:
            values.setAt(i, obs_data_get_bool(settings, e.key) ? 1.0 : 0.0);
            break;
        case pf::look::Kind::real:
            values.setAt(i, obs_data_get_double(settings, e.key));
            break;
        default:
            values.setAt(i, (double) obs_data_get_int(settings, e.key));
            break;
        }
    }
    return values;
}

void writeLook(obs_data_t* settings, const pf::look::Values& values, bool lookOnly)
{
    const auto& entries = pf::look::entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (lookOnly && !e.look)
            continue;
        switch (e.kind) {
        case pf::look::Kind::toggle:
            obs_data_set_bool(settings, e.key, values.at(i) >= 0.5);
            break;
        case pf::look::Kind::real:
            obs_data_set_double(settings, e.key, values.at(i));
            break;
        default:
            obs_data_set_int(settings, e.key, std::lround(values.at(i)));
            break;
        }
    }
}

void lookChanged(obs_data_t* settings, bool loading)
{
    if (sync == nullptr || !obs_data_get_bool(settings, "sync_app") || obs_data_get_bool(settings, "sync_preview"))
        return;
    const std::string text = lookOf(settings).text();
    const std::lock_guard<std::mutex> guard(sync->lock);
    if (text == sync->text)
        return;
    std::uint64_t changed = 0;
    if (loading) { // a source as saved: its look is the shared one if it is the newest (at a tie, OBS's wins)
        changed = (std::uint64_t) std::max(0LL, obs_data_get_int(settings, "sync_changed"));
        const bool newest = sync->text.empty() || changed > sync->changedMs || (changed == sync->changedMs && !sync->fromObs);
        if (!newest) {
            sync->pending = true; // it takes the shared one
            return;
        }
    } else { // an edit in OBS: the newest
        changed = std::max(pf::LookChannel::nowMs(), sync->changedMs + 1);
        obs_data_set_int(settings, "sync_changed", (long long) changed);
    }
    sync->text = text;
    sync->changedMs = changed;
    sync->fromObs = true;
    sync->pending = true; // the other synchronised sources take it
    sync->channel.publish(text, changed);
}

void startLookSync()
{
    sync = new Sync;
    if (!sync->channel.open(pf::LookChannel::Side::obs))
        blog(LOG_WARNING, "[pianoforge] the stream look's channel could not be opened: no sync with PianoForge's window");
    sync->running = true;
    sync->thread = std::thread(poll);
    obs_add_main_rendered_callback(renderPreview, nullptr);
}

void stopLookSync()
{
    if (sync == nullptr)
        return;
    obs_remove_main_rendered_callback(renderPreview, nullptr);
    sync->running = false;
    if (sync->thread.joinable())
        sync->thread.join();
    obs_source_t* preview = nullptr;
    {
        const std::lock_guard<std::mutex> guard(sync->lock);
        preview = sync->preview;
        sync->preview = nullptr;
    }
    if (preview != nullptr)
        obs_source_release(preview);
    obs_enter_graphics();
    if (sync->target != nullptr)
        gs_texrender_destroy(sync->target);
    for (auto* stage : sync->stages)
        if (stage != nullptr)
            gs_stagesurface_destroy(stage);
    obs_leave_graphics();
    sync->channel.close();
    delete sync;
    sync = nullptr;
}

} // namespace pfobs
