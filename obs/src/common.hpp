#pragma once

// What the plugin's sources share: a sleeper on a high-resolution timer, the audio source's timeline (which frame of
// the feed it stamps at which OBS instant: the keyboard display follows the sound with it), the sources wanting
// chapter markers, and the locale's texts.

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace pfobs {

/// The locale's text for `key` (data/locale/<language>.ini).
inline const char* text(const char* key)
{
    return obs_module_text(key);
}

/// Sleeps on a high-resolution waitable timer where there is one (the default Windows timer ticks every 15.6 ms).
class Sleeper {
public:
    Sleeper()
    {
#if defined(_WIN32)
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    }
    ~Sleeper()
    {
#if defined(_WIN32)
        if (timer != nullptr)
            CloseHandle(timer);
#endif
    }
    Sleeper(const Sleeper&) = delete;
    Sleeper& operator=(const Sleeper&) = delete;

    void sleep(double seconds)
    {
#if defined(_WIN32)
        if (timer != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG) (seconds * 1e7); // relative, in 100 ns
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
#endif
        os_sleep_ms((uint32_t) std::max(1.0, seconds * 1000.0));
    }

private:
#if defined(_WIN32)
    HANDLE timer = nullptr;
#endif
};

/// The audio source's timeline: the feed's frame it stamped at an OBS instant (os_gettime_ns), at the feed's rate.
struct Timeline {
    std::uint64_t obsNs = 0, session = 0;
    double feedFrame = 0.0, feedRate = 0.0;
};
void publishTimeline(const Timeline& t);
/// The latest timeline, if one was published in the last half second.
bool latestTimeline(Timeline& out);

/// Audio sources with the chapter markers on (the chapters follow PianoForge's marks only while one is).
extern std::atomic<int> chapterSources;

void registerAudioSource();
void registerKeyboardSource();
void registerTextSource();
void startChapters();
void stopChapters();

} // namespace pfobs
