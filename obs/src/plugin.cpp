// SPDX-License-Identifier: AGPL-3.0-or-later

// The PianoForge plugin for OBS Studio: the application's sound (audio source), its keyboard as played (video
// source), its state as text (text source), and chapter markers in OBS's recordings at PianoForge's marks. All of it
// read from the application's feed (shared memory, pianoforge/audio_feed.hpp): no virtual cable, no system mixer. The
// keyboard's look is shared with PianoForge's "Stream look" window, which shows its preview (look_sync.cpp).
#include "common.hpp"

#include <mutex>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("pianoforge-obs", "en-US")

MODULE_EXPORT const char* obs_module_name(void)
{
    return "PianoForge";
}

MODULE_EXPORT const char* obs_module_description(void)
{
    return pfobs::text("Module.Description");
}

namespace pfobs {

std::atomic<int> chapterSources { 0 };

namespace {
std::mutex timelineLock;
Timeline timeline;
} // namespace

void publishTimeline(const Timeline& t)
{
    const std::lock_guard<std::mutex> guard(timelineLock);
    timeline = t;
}

bool latestTimeline(Timeline& out)
{
    const std::lock_guard<std::mutex> guard(timelineLock);
    const std::uint64_t now = os_gettime_ns();
    if (timeline.obsNs == 0 || timeline.feedRate <= 0.0 || (now > timeline.obsNs && now - timeline.obsNs > 500'000'000ull))
        return false;
    out = timeline;
    return true;
}

} // namespace pfobs

bool obs_module_load(void)
{
    pfobs::registerAudioSource();
    pfobs::registerKeyboardSource();
    pfobs::registerTextSource();
    pfobs::startChapters();
    pfobs::startLookSync();
    blog(LOG_INFO, "[pianoforge] the PianoForge sources are available (audio, keyboard, text)");
    return true;
}

void obs_module_unload(void)
{
    pfobs::stopLookSync();
    pfobs::stopChapters();
}
