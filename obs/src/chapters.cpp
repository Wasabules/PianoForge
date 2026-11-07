// What PianoForge asks of OBS's frontend, read from the feed:
// - chapter markers in OBS's recordings at PianoForge's marks (a take saved, a recording started or stopped, a MIDI
//   file played), while a PianoForge audio source has them on and OBS records. OBS 30.2 and later; only recordings
//   in Hybrid MP4 keep chapters (OBS says so: the marker is then not added, and the log says why);
// - commands from gestures played on PianoForge's keyboard (start or stop the recording or the stream, a chapter, the
//   next or previous scene), while a PianoForge audio source lets PianoForge control OBS.
// Both are carried out on OBS's interface thread.
#include "common.hpp"

#include <obs-frontend-api.h>
#include <pianoforge/audio_feed.hpp>

#include <string>
#include <thread>

namespace pfobs {

std::atomic<int> controlSources { 0 };

namespace {

constexpr double kPollSeconds = 0.1;

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

/// The scene `step` places after (or before) the current one, in OBS's list, made current.
void stepScene(int step)
{
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    obs_source_t* current = obs_frontend_get_current_scene();
    const int n = (int) scenes.sources.num;
    int at = -1;
    for (int i = 0; i < n; ++i)
        if (scenes.sources.array[i] == current)
            at = i;
    if (n > 0) {
        const int next = ((at < 0 ? 0 : at + step) % n + n) % n;
        obs_frontend_set_current_scene(scenes.sources.array[next]);
        blog(LOG_INFO, "[pianoforge] gesture: scene \"%s\"", obs_source_get_name(scenes.sources.array[next]));
    }
    obs_source_release(current);
    obs_frontend_source_list_free(&scenes);
}

void runCommand(void* param)
{
    const auto command = (pf::FeedCommand) (std::uintptr_t) param;
    switch (command) {
    case pf::FeedCommand::obsRecordToggle:
        if (obs_frontend_recording_active()) {
            obs_frontend_recording_stop();
            blog(LOG_INFO, "[pianoforge] gesture: OBS's recording stopped");
        } else {
            obs_frontend_recording_start();
            blog(LOG_INFO, "[pianoforge] gesture: OBS's recording started");
        }
        break;
    case pf::FeedCommand::obsStreamToggle:
        if (obs_frontend_streaming_active()) {
            obs_frontend_streaming_stop();
            blog(LOG_INFO, "[pianoforge] gesture: OBS's stream stopped");
        } else {
            obs_frontend_streaming_start();
            blog(LOG_INFO, "[pianoforge] gesture: OBS's stream started");
        }
        break;
    case pf::FeedCommand::obsChapter:
        addChapter(new std::string("PianoForge"));
        break;
    case pf::FeedCommand::obsNextScene:
        stepScene(1);
        break;
    case pf::FeedCommand::obsPreviousScene:
        stepScene(-1);
        break;
    default:
        break;
    }
}

const char* commandName(pf::FeedCommand c)
{
    switch (c) {
    case pf::FeedCommand::obsRecordToggle: return "start or stop the recording";
    case pf::FeedCommand::obsStreamToggle: return "start or stop the stream";
    case pf::FeedCommand::obsChapter: return "a chapter marker";
    case pf::FeedCommand::obsNextScene: return "the next scene";
    case pf::FeedCommand::obsPreviousScene: return "the previous scene";
    default: return "nothing";
    }
}

void poll()
{
    pf::AudioFeedReader feed;
    Sleeper sleeper;
    std::uint32_t seenMarks = 0, seenCommands = 0;
    bool fresh = true; // the marks and commands already there when the feed opens are not this session's
    while (polling.load()) {
        sleeper.sleep(kPollSeconds);
        if (!feed.isOpen()) {
            if (!feed.open())
                continue;
            fresh = true;
        }
        const std::uint32_t marks = feed.marks(), commands = feed.commands();
        if (fresh || marks < seenMarks || commands < seenCommands) {
            seenMarks = marks;
            seenCommands = commands;
            fresh = false;
            continue;
        }
        for (; seenMarks < marks; ++seenMarks) {
            char name[128];
            if (!feed.mark(seenMarks, name, sizeof(name)) || chapterSources.load() <= 0)
                continue;
            if (!obs_frontend_recording_active()) {
                blog(LOG_INFO, "[pianoforge] PianoForge's mark \"%s\": OBS is not recording, no chapter", name);
                continue;
            }
            obs_queue_task(OBS_TASK_UI, addChapter, new std::string(name), false);
        }
        for (; seenCommands < commands; ++seenCommands) {
            const auto command = feed.command(seenCommands);
            if (command == pf::FeedCommand::none)
                continue;
            if (controlSources.load() <= 0) {
                blog(LOG_INFO, "[pianoforge] PianoForge asks for %s: no PianoForge audio source lets it control OBS", commandName(command));
                continue;
            }
            blog(LOG_INFO, "[pianoforge] PianoForge asks for %s", commandName(command));
            obs_queue_task(OBS_TASK_UI, runCommand, (void*) (std::uintptr_t) command, false);
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
