// Chapter markers in OBS's recordings at PianoForge's marks (a take saved, a recording started or stopped, a MIDI file
// played): while a PianoForge audio source has them on and OBS records. OBS 30.2 and later, and only recordings in
// Hybrid MP4 keep chapters (OBS says so: the marker is then not added, and the log says why).
#include "common.hpp"

#include <obs-frontend-api.h>
#include <pianoforge/audio_feed.hpp>

#include <string>
#include <thread>

namespace pfobs {

namespace {

constexpr double kPollSeconds = 0.25;

std::atomic<bool> polling { false };
std::thread poller;

void addChapter(void* param)
{
    auto* name = static_cast<std::string*>(param);
    if (obs_frontend_recording_add_chapter(name->c_str()))
        blog(LOG_INFO, "[pianoforge] chapter marker \"%s\" added to the recording", name->c_str());
    else
        blog(LOG_INFO, "[pianoforge] chapter marker \"%s\" not added: the recording's format keeps no chapters (Hybrid MP4 does)",
             name->c_str());
    delete name;
}

void poll()
{
    pf::AudioFeedReader feed;
    Sleeper sleeper;
    std::uint32_t seen = 0;
    bool fresh = true; // the marks already there when the feed opens are not this session's
    while (polling.load()) {
        sleeper.sleep(kPollSeconds);
        if (!feed.isOpen()) {
            if (!feed.open())
                continue;
            fresh = true;
        }
        const std::uint32_t count = feed.marks();
        if (fresh || count < seen) {
            seen = count;
            fresh = false;
            continue;
        }
        for (; seen < count; ++seen) {
            char name[128];
            if (!feed.mark(seen, name, sizeof(name)) || chapterSources.load() <= 0)
                continue;
            if (!obs_frontend_recording_active()) {
                blog(LOG_INFO, "[pianoforge] PianoForge's mark \"%s\": OBS is not recording, no chapter", name);
                continue;
            }
            obs_queue_task(OBS_TASK_UI, addChapter, new std::string(name), false);
        }
    }
}

} // namespace

void startChapters()
{
    if (polling.exchange(true))
        return;
    poller = std::thread(poll);
}

void stopChapters()
{
    polling = false;
    if (poller.joinable())
        poller.join();
}

} // namespace pfobs
