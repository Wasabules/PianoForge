// The "PianoForge" audio source for OBS Studio: the sound of the PianoForge application, read from its audio feed
// (shared memory, pianoforge/audio_feed.hpp) and handed to OBS on OBS's own clock. No virtual cable, no system mixer.
//
// The feed runs on the audio device's clock (the DAC behind ASIO), OBS on the system's: the two drift apart by tens
// to hundreds of ppm. OBS, fed timestamps on the device's clock, would let the gap grow to its 70 ms threshold and
// then jump. So the source resamples the feed (pf::DriftResampler, ratio 1 corrected by the fill level) and stamps
// its output on OBS's clock, frame for frame: OBS sees a stream that never drifts.
//
// Properties: the connection's status (refreshed on demand), the latency (the audio kept waiting, 10 to 100 ms),
// and a button to start again from the newest audio.
#include <obs-module.h>
#include <util/platform.h>

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/drift_resampler.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
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

constexpr int kMaxChunk = 128;           // frames handed to OBS at once
constexpr double kPeriodSeconds = 0.002; // the reader wakes this often
constexpr double kStaleSeconds = 0.25;   // no new frame for this long: the application stopped
constexpr int kDefaultLatencyMs = 20, kMinLatencyMs = 10, kMaxLatencyMs = 100;

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

enum class State : int { noApplication, noAudio, starting, running };

struct Source {
    obs_source_t* source = nullptr;
    std::atomic<bool> running { true };
    std::atomic<int> latencyMs { kDefaultLatencyMs };
    std::atomic<bool> restart { false };
    std::thread worker;

    // Status, written by the worker, read by the properties.
    std::atomic<int> state { (int) State::noApplication };
    std::atomic<std::uint32_t> rate { 0 }, block { 0 }, resyncs { 0 };
    std::atomic<double> targetMs { 0.0 }, waitingMs { 0.0 }, driftPpm { 0.0 };
    std::atomic<std::uint64_t> runningSince { 0 };
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
    std::uint32_t seenResyncs = 0;          // the resampler's underruns and overruns already counted
    double rate = 0.0;
    int latency = -1;
    bool connected = false;

    auto stopTimeline = [&] {
        start = 0;
        s->state = (int) State::starting;
        s->runningSince = 0;
    };

    while (s->running.load()) {
        const std::uint64_t now = os_gettime_ns();
        if (!feed.isOpen() && !feed.open()) { // PianoForge is not running (yet)
            s->state = (int) State::noApplication;
            sleeper.sleep(0.25);
            continue;
        }
        if (!feed.format(format)) { // PianoForge closed, or its audio device restarts
            if (connected)
                blog(LOG_INFO, "[pianoforge] the application's audio stopped");
            connected = false;
            start = 0;
            session = ~0ull;
            s->state = (int) State::noAudio;
            s->runningSince = 0;
            sleeper.sleep(0.05);
            continue;
        }
        const int wanted = std::clamp(s->latencyMs.load(), kMinLatencyMs, kMaxLatencyMs);
        if (format.session != session || wanted != latency || s->restart.exchange(false)) {
            const bool resync = format.session == session;
            session = format.session;
            latency = wanted;
            rate = format.sampleRate;
            // The latency asked for, above what the two block sizes call for (DriftResampler's target).
            const int inBlock = (int) std::max<std::uint32_t>(format.blockSize, 1);
            const double least = 1.5 * (inBlock + kMaxChunk) / rate + 0.002;
            resampler.prepare(rate, rate, inBlock, kMaxChunk, std::max(0.0, latency / 1000.0 - least));
            seenResyncs = 0;
            position = lastWritten = feed.written();
            lastProgress = now;
            framesOut = 0;
            stopTimeline();
            s->rate = format.sampleRate;
            s->block = format.blockSize;
            s->targetMs = 1000.0 * resampler.targetSeconds();
            if (resync)
                s->resyncs.fetch_add(1);
            if (!connected || !resync)
                blog(LOG_INFO, "[pianoforge] reading the application's audio: %u Hz, blocks of %u, latency %.1f ms",
                     format.sampleRate, format.blockSize, 1000.0 * resampler.targetSeconds());
            connected = true;
        }
        const std::uint64_t written = feed.written();
        if (written != lastWritten) {
            lastWritten = written;
            lastProgress = now;
        } else if ((double) (now - lastProgress) * 1e-9 > kStaleSeconds) { // stalled: the timeline stops
            if (start != 0)
                resampler.reset();
            stopTimeline();
            s->state = (int) State::noAudio;
            position = written;
            sleeper.sleep(0.01);
            continue;
        }
        // Everything new, into the resampler.
        while (position < written && resampler.room() > 0) {
            const int n = feed.read(position, scratch.data(), std::min((int) (scratch.size() / 2), resampler.room()));
            if (n < 0) { // fell behind (OBS stalled): start again from the newest frames
                resampler.reset();
                stopTimeline();
                s->resyncs.fetch_add(1);
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
            s->state = (int) State::running;
            s->runningSince = now;
        }
        std::int64_t due = (std::int64_t) ((double) (now - start) * rate * 1e-9) - (std::int64_t) framesOut;
        while (due > 0) {
            const int n = (int) std::min<std::int64_t>(due, kMaxChunk);
            resampler.render(planes, 2, n);
            if (!resampler.running()) { // the feed ran dry: start again once it is back
                stopTimeline();
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
        const std::uint32_t dropouts = resampler.underruns() + resampler.overruns();
        if (dropouts != seenResyncs) {
            s->resyncs.fetch_add(dropouts - seenResyncs);
            seenResyncs = dropouts;
        }
        s->waitingMs = 1000.0 * resampler.waiting() / rate;
        s->driftPpm = resampler.correctionPpm();
        sleeper.sleep(kPeriodSeconds);
    }
}

std::string statusText(const Source* s)
{
    char text[512];
    const auto state = (State) s->state.load();
    if (state == State::noApplication)
        return "Waiting for PianoForge: start the application.";
    if (state == State::noAudio)
        return "PianoForge is open, but its audio is stopped (no audio device, or the device restarting).";
    const double rate = s->rate.load(), block = s->block.load();
    std::snprintf(text, sizeof(text),
                  "PianoForge connected: %.1f kHz, blocks of %.0f (%.1f ms)\n"
                  "Latency: %.1f ms aimed at, %.1f ms waiting now\n"
                  "Clock drift compensated: %+.0f ppm (PianoForge's audio clock against the system's)\n"
                  "Resyncs since the source was created: %u\n"
                  "%s",
                  rate / 1000.0, block, rate > 0 ? 1000.0 * block / rate : 0.0, s->targetMs.load(), s->waitingMs.load(),
                  s->driftPpm.load(), s->resyncs.load(),
                  state == State::starting ? "Starting (filling the buffer)..." : "Running.");
    std::string out = text;
    if (state == State::running && s->runningSince.load() != 0) {
        const double minutes = (double) (os_gettime_ns() - s->runningSince.load()) * 1e-9 / 60.0;
        std::snprintf(text, sizeof(text), " For %.0f min %.0f s.", std::floor(minutes), std::fmod(minutes * 60.0, 60.0));
        out += text;
    }
    return out;
}

const char* sourceName(void*)
{
    return "PianoForge";
}

void sourceDefaults(obs_data_t* settings)
{
    obs_data_set_default_int(settings, "latency_ms", kDefaultLatencyMs);
}

void sourceUpdate(void* data, obs_data_t* settings)
{
    static_cast<Source*>(data)->latencyMs = (int) obs_data_get_int(settings, "latency_ms");
}

void* sourceCreate(obs_data_t* settings, obs_source_t* source)
{
    auto* s = new Source;
    s->source = source;
    sourceUpdate(s, settings);
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

bool refreshStatus(obs_properties_t* props, obs_property_t*, void* data)
{
    obs_property_set_description(obs_properties_get(props, "status"), statusText(static_cast<Source*>(data)).c_str());
    return true; // redraw the properties
}

bool resynchronise(obs_properties_t* props, obs_property_t* property, void* data)
{
    static_cast<Source*>(data)->restart = true;
    os_sleep_ms(100); // the worker starts again; the status then says so
    return refreshStatus(props, property, data);
}

obs_properties_t* sourceProperties(void* data)
{
    obs_properties_t* props = obs_properties_create();
    obs_property_t* status = obs_properties_add_text(props, "status", statusText(static_cast<Source*>(data)).c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(status, true);
    obs_properties_add_button2(props, "refresh", "Refresh the status", refreshStatus, data);
    obs_property_t* latency = obs_properties_add_int_slider(props, "latency_ms", "Latency", kMinLatencyMs, kMaxLatencyMs, 1);
    obs_property_int_set_suffix(latency, " ms");
    obs_property_set_long_description(latency,
                                      "Audio kept waiting between PianoForge and OBS. Lower: closer to the sound you hear; "
                                      "higher: safer on a busy machine. Below the two sides' block sizes it cannot go "
                                      "(about 10 ms with 128-sample blocks).");
    obs_properties_add_button2(props, "resync", "Resynchronise", resynchronise, data);
    obs_properties_add_text(props, "help",
                            "Volume, mono, sync offset and monitoring: in OBS's Advanced Audio Properties. Effects: in "
                            "the source's Filters. Turn monitoring off so that PianoForge is not heard twice.",
                            OBS_TEXT_INFO);
    return props;
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
    info.get_defaults = sourceDefaults;
    info.get_properties = sourceProperties;
    info.update = sourceUpdate;
    info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
    obs_register_source(&info);
    blog(LOG_INFO, "[pianoforge] the PianoForge audio source is available");
    return true;
}
