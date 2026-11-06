// The "PianoForge" audio source for OBS Studio: the sound of the PianoForge application, read from its audio feed
// (shared memory, pianoforge/audio_feed.hpp) and handed to OBS on OBS's own clock. No virtual cable, no system mixer.
//
// The feed runs on the audio device's clock (the DAC behind ASIO), OBS on the system's: the two drift apart by tens
// to hundreds of ppm. OBS, fed timestamps on the device's clock, would let the gap grow to its 70 ms threshold and
// then jump. So the source resamples the feed (pf::DriftResampler, ratio 1 corrected by the fill level) and stamps
// its output on OBS's clock, frame for frame: OBS sees a stream that never drifts.
#include <obs-module.h>
#include <util/platform.h>

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/drift_resampler.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

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

OBS_DECLARE_MODULE()

MODULE_EXPORT const char* obs_module_name(void)
{
    return "PianoForge";
}

MODULE_EXPORT const char* obs_module_description(void)
{
    return "The PianoForge audio source: the sound of the PianoForge application, straight from it.";
}

namespace {

constexpr int kMaxChunk = 256;          // frames handed to OBS at once
constexpr double kPeriodSeconds = 0.002; // the reader wakes this often
constexpr double kStaleSeconds = 0.25;   // no new frame for this long: the application stopped

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

struct Source {
    obs_source_t* source = nullptr;
    std::atomic<bool> running { true };
    std::thread worker;
};

void run(Source* s)
{
    pf::AudioFeedReader feed;
    pf::DriftResampler resampler;
    pf::AudioFeedReader::Format format;
    std::vector<float> scratch(2 * 8192), left(kMaxChunk), right(kMaxChunk);
    float* planes[2] = { left.data(), right.data() };
    Sleeper sleeper;
    std::uint64_t session = ~0ull, position = 0, lastWritten = 0, lastProgress = 0;
    std::uint64_t start = 0, framesOut = 0; // the timeline handed to OBS: start + framesOut / rate (ns)
    double rate = 0.0;
    bool connected = false;

    while (s->running.load()) {
        const std::uint64_t now = os_gettime_ns();
        if (!feed.isOpen() && !feed.open()) { // PianoForge is not running (yet)
            sleeper.sleep(0.25);
            continue;
        }
        if (!feed.format(format)) { // PianoForge closed, or its audio device restarts
            if (connected)
                blog(LOG_INFO, "[pianoforge] the application's audio stopped");
            connected = false;
            start = 0;
            session = ~0ull;
            sleeper.sleep(0.05);
            continue;
        }
        if (format.session != session) {
            session = format.session;
            rate = format.sampleRate;
            resampler.prepare(rate, rate, (int) std::max<std::uint32_t>(format.blockSize, 1), kMaxChunk, 2.0 * kPeriodSeconds);
            position = lastWritten = feed.written();
            lastProgress = now;
            start = framesOut = 0;
            connected = true;
            blog(LOG_INFO, "[pianoforge] reading the application's audio: %u Hz, blocks of %u", format.sampleRate, format.blockSize);
        }
        const std::uint64_t written = feed.written();
        if (written != lastWritten) {
            lastWritten = written;
            lastProgress = now;
        } else if ((double) (now - lastProgress) * 1e-9 > kStaleSeconds) { // stalled: the timeline stops
            if (start != 0)
                resampler.reset();
            start = 0;
            position = written;
            sleeper.sleep(0.01);
            continue;
        }
        // Everything new, into the resampler.
        while (position < written && resampler.room() > 0) {
            const int n = feed.read(position, scratch.data(), std::min((int) (scratch.size() / 2), resampler.room()));
            if (n < 0) { // fell behind (OBS stalled): start again from the newest frames
                resampler.reset();
                start = 0;
                position = written - std::min<std::uint64_t>(written, (std::uint64_t) resampler.targetFrames());
                continue;
            }
            if (n == 0)
                break;
            resampler.append(scratch.data(), n);
            position += (std::uint64_t) n;
        }
        // The timeline starts once enough is waiting; then every frame due by OBS's clock, stamped on it.
        if (start == 0) {
            if (!resampler.readyToStart()) {
                sleeper.sleep(kPeriodSeconds);
                continue;
            }
            start = now;
            framesOut = 0;
        }
        std::int64_t due = (std::int64_t) ((double) (now - start) * rate * 1e-9) - (std::int64_t) framesOut;
        while (due > 0) {
            const int n = (int) std::min<std::int64_t>(due, kMaxChunk);
            resampler.render(planes, 2, n);
            if (!resampler.running()) { // the feed ran dry: start again once it is back
                start = 0;
                break;
            }
            obs_source_audio audio = {};
            audio.data[0] = reinterpret_cast<const uint8_t*>(left.data());
            audio.data[1] = reinterpret_cast<const uint8_t*>(right.data());
            audio.frames = (uint32_t) n;
            audio.speakers = SPEAKERS_STEREO;
            audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
            audio.samples_per_sec = (uint32_t) rate;
            audio.timestamp = start + (std::uint64_t) ((double) framesOut * 1e9 / rate);
            obs_source_output_audio(s->source, &audio);
            framesOut += (std::uint64_t) n;
            due -= n;
        }
        sleeper.sleep(kPeriodSeconds);
    }
}

const char* sourceName(void*)
{
    return "PianoForge";
}

void* sourceCreate(obs_data_t*, obs_source_t* source)
{
    auto* s = new Source;
    s->source = source;
    s->worker = std::thread(run, s);
    return s;
}

void sourceDestroy(void* data)
{
    auto* s = static_cast<Source*>(data);
    s->running = false;
    if (s->worker.joinable())
        s->worker.join();
    delete s;
}

} // namespace

bool obs_module_load(void)
{
    obs_source_info info = {};
    info.id = "pianoforge_audio";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = sourceName;
    info.create = sourceCreate;
    info.destroy = sourceDestroy;
    info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
    obs_register_source(&info);
    blog(LOG_INFO, "[pianoforge] the PianoForge audio source is available");
    return true;
}
