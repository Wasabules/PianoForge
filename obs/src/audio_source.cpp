// The "PianoForge" audio source: the application's sound, read from its feed and handed to OBS on OBS's own clock.
//
// The feed runs on the audio device's clock (the DAC behind ASIO), OBS on the system's: the two drift apart by tens
// to hundreds of ppm. OBS, fed timestamps on the device's clock, would let the gap grow to its 70 ms threshold and
// then jump. So the source resamples the feed (pf::DriftResampler, straight to OBS's rate: one resampling, not two),
// with its ratio corrected by the frames waiting, counted from the instants the application wrote them (the feed's
// clock: no sawtooth of the application's blocks in it), and stamps its output on OBS's clock, frame for frame: OBS
// sees a stream that never drifts.
//
// The mix: as heard in PianoForge, on speakers (the room without the head, for a stream heard on speakers while
// PianoForge is heard on headphones), or dry. The feed's mixes come before PianoForge's volume and limiter: the
// stream keeps its level whatever the volume heard (or follows it), takes its own gain, and its own limiter. Every
// start fades in, every planned stop (a new latency, mix or format, the resync button, the application's audio
// stopping) fades out: no click in the stream.
#include "common.hpp"

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/drift_resampler.hpp>
#include <pianoforge/limiter.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace pfobs {

namespace {

constexpr int kMaxChunk = 128;           // frames handed to OBS at once
constexpr double kPeriodSeconds = 0.002; // the reader wakes this often
constexpr double kStaleSeconds = 0.25;   // no new frame for this long: the application stopped
constexpr double kFadeSeconds = 0.01;    // fades in and out
constexpr double kBeatSeconds = 0.25;    // the reader's heartbeat in the feed
constexpr double kRateSeconds = 60.0;    // the application's rate: measured over about this long
constexpr double kRateWarmSeconds = 5.0; // and followed from then on
constexpr int kDefaultLatencyMs = 20, kMinLatencyMs = 10, kMaxLatencyMs = 100;

enum class State : int { noApplication, noAudio, starting, running };

struct Source {
    obs_source_t* source = nullptr;
    std::atomic<bool> running { true };
    std::atomic<int> latencyMs { kDefaultLatencyMs }, mix { (int) pf::FeedMix::speakers };
    std::atomic<bool> followVolume { false }, chapters { false };
    std::atomic<float> gainDb { 0.0f };
    std::atomic<bool> restart { false };
    std::thread worker;

    // Status, written by the worker, read by the properties.
    std::atomic<int> state { (int) State::noApplication };
    std::atomic<std::uint32_t> feedRate { 0 }, obsRate { 0 }, block { 0 }, resyncs { 0 };
    std::atomic<double> targetMs { 0.0 }, waitingMs { 0.0 }, driftPpm { 0.0 }, keyToObsMs { 0.0 }, volumeDb { 0.0 };
    std::atomic<float> limiterDb { 0.0f };
    std::atomic<std::uint64_t> runningSince { 0 };
};

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

void run(Source* s)
{
    pf::AudioFeedReader feed;
    pf::DriftResampler resampler;
    pf::Limiter limiter;
    pf::AudioFeedReader::Format format;
    std::vector<float> scratch(2 * 8192), left(kMaxChunk), right(kMaxChunk);
    float* planes[2] = { left.data(), right.data() };
    Sleeper sleeper;
    std::uint64_t session = ~0ull, position = 0, lastWritten = 0, lastProgress = 0;
    std::uint64_t start = 0, framesOut = 0; // the timeline handed to OBS: start + framesOut / obsRate (ns)
    std::uint64_t lastBeat = 0, lastRefresh = 0;
    std::uint32_t seenDropouts = 0;         // the resampler's underruns and overruns already counted
    double feedRate = 0.0, obsRate = 0.0, fade = 0.0, gainNow = 1.0, keyToObs = 0.0, limiterLow = 0.0, longestGap = 0.0;
    double offsetAverage = 0.0; // the frames made since the application's last block, on average (about a second)
    // The application's rate on this machine's clock: a least-squares line through its blocks' (instant, frames),
    // exponentially weighted (kRateSeconds): means, variance and covariance, in seconds and frames from the session's
    // first block.
    std::uint64_t rateFirstNs = 0, rateFirstFrames = 0;
    double meanT = 0.0, meanF = 0.0, varT = 0.0, covTF = 0.0, lastT = 0.0;
    std::uint64_t lastClockNs = 0;
    std::uint64_t limiterSince = 0;
    int latency = -1, mix = -1, inBlock = 1;
    bool connected = false;
    std::string signature; // what the status shows that is not a measure: a change refreshes the properties

    auto stopTimeline = [&] {
        start = 0;
        fade = 0.0;
        s->state = (int) State::starting;
        s->runningSince = 0;
    };
    // Hands `n` frames (planes) to OBS at the timeline's next position, through the gain, the fade (from `fade`
    // toward `fadeTo` at kFadeSeconds' pace) and the limiter.
    auto output = [&](int n, double fadeTo, double gainTarget) {
        const double fadeStep = 1.0 / std::max(1.0, kFadeSeconds * obsRate);
        const double gainStep = (gainTarget - gainNow) / std::max(1, n);
        for (int k = 0; k < n; ++k) {
            fade = fadeTo > fade ? std::min(fadeTo, fade + fadeStep) : std::max(fadeTo, fade - fadeStep);
            gainNow += gainStep;
            const float g = (float) (gainNow * fade * fade); // the fade's square: smooth at both ends
            left[(std::size_t) k] *= g;
            right[(std::size_t) k] *= g;
        }
        limiter.process(left.data(), right.data(), n);
        obs_source_audio audio = {};
        audio.data[0] = reinterpret_cast<const uint8_t*>(left.data());
        audio.data[1] = reinterpret_cast<const uint8_t*>(right.data());
        audio.frames = (uint32_t) n;
        audio.speakers = SPEAKERS_STEREO;
        audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec = (uint32_t) obsRate;
        audio.timestamp = start + (std::uint64_t) ((double) framesOut * 1e9 / obsRate);
        obs_source_output_audio(s->source, &audio);
        framesOut += (std::uint64_t) n;
    };
    auto gainTarget = [&] {
        double g = dbToGain(std::clamp((double) s->gainDb.load(), -60.0, 24.0));
        if (s->followVolume.load())
            g *= std::clamp((double) feed.volume(), 0.0, 16.0);
        return g;
    };
    // What is left waiting, faded out (a planned stop: no click), then nothing.
    auto fadeOut = [&] {
        if (start == 0 || !resampler.running())
            return;
        int left_ = std::min(resampler.renderable(), (int) std::ceil(kFadeSeconds * obsRate));
        const double g = gainTarget();
        while (left_ > 0) {
            const int n = std::min(left_, kMaxChunk);
            resampler.render(planes, 2, n);
            output(n, 0.0, g);
            left_ -= n;
        }
    };

    while (s->running.load()) {
        const std::uint64_t now = os_gettime_ns();
        const std::uint64_t feedNow = pf::feedClockNs();
        if (feed.isOpen() && now - lastBeat > (std::uint64_t) (kBeatSeconds * 1e9)) {
            feed.heartbeat((pf::FeedMix) std::clamp(s->mix.load(), 0, pf::kFeedMixes - 1),
                           s->state.load() == (int) State::running ? (float) keyToObs : 0.0f);
            lastBeat = now;
        }
        // The properties follow the status: at once on a change of what it says, and every few seconds for the measures.
        {
            char sig[160];
            std::snprintf(sig, sizeof(sig), "%d %u %u %u %u %d %d %d", s->state.load(), s->feedRate.load(), s->obsRate.load(),
                          s->block.load(), s->resyncs.load(), s->mix.load(), (int) s->followVolume.load(), s->limiterDb.load() < -0.05f);
            const double since = (double) (now - lastRefresh) * 1e-9;
            if ((signature != sig && since > 0.5) || (s->state.load() == (int) State::running && since > 5.0)) {
                signature = sig;
                lastRefresh = now;
                obs_source_update_properties(s->source);
            }
        }
        if (!feed.isOpen() && !feed.open(pf::FeedReaderKind::audio)) { // PianoForge is not running (yet)
            s->state = (int) State::noApplication;
            sleeper.sleep(0.25);
            continue;
        }
        if (!feed.format(format)) { // PianoForge closed, or its audio device restarts
            if (connected)
                blog(LOG_INFO, "[pianoforge] the application's audio stopped");
            fadeOut();
            connected = false;
            start = 0;
            session = ~0ull;
            s->state = (int) State::noAudio;
            s->runningSince = 0;
            sleeper.sleep(0.05);
            continue;
        }
        const int wanted = std::clamp(s->latencyMs.load(), kMinLatencyMs, kMaxLatencyMs);
        const int wantedMix = std::clamp(s->mix.load(), 0, pf::kFeedMixes - 1);
        const audio_t* audioOut = obs_get_audio();
        const double wantedObsRate = audioOut != nullptr && audio_output_get_sample_rate(audioOut) > 0
                                         ? (double) audio_output_get_sample_rate(audioOut) : 48000.0;
        if (format.session != session || wanted != latency || wantedMix != mix || wantedObsRate != obsRate
            || s->restart.exchange(false)) {
            const bool resync = format.session == session;
            fadeOut();
            session = format.session;
            latency = wanted;
            mix = wantedMix;
            feedRate = format.sampleRate;
            obsRate = wantedObsRate;
            // The latency asked for, above what the two block sizes call for (DriftResampler's target).
            inBlock = (int) std::max<std::uint32_t>(format.blockSize, 1);
            const double least = 1.5 * (inBlock / feedRate + kMaxChunk / obsRate) + 0.002;
            resampler.prepare(feedRate, obsRate, inBlock, kMaxChunk, std::max(0.0, latency / 1000.0 - least));
            limiter.prepare(obsRate);
            gainNow = gainTarget();
            longestGap = 3.0 * inBlock / feedRate;
            lastClockNs = 0;
            offsetAverage = 0.5 * inBlock;
            rateFirstNs = 0;
            seenDropouts = 0;
            position = lastWritten = feed.written();
            lastProgress = now;
            framesOut = 0;
            stopTimeline();
            s->feedRate = format.sampleRate;
            s->obsRate = (std::uint32_t) obsRate;
            s->block = format.blockSize;
            s->targetMs = 1000.0 * resampler.targetSeconds();
            if (resync)
                s->resyncs.fetch_add(1);
            if (!connected || !resync)
                blog(LOG_INFO, "[pianoforge] reading the application's audio: %u Hz, blocks of %u, into OBS at %.0f Hz, latency %.1f ms",
                     format.sampleRate, format.blockSize, obsRate, 1000.0 * resampler.targetSeconds());
            connected = true;
        }
        const std::uint64_t written = feed.written();
        if (written != lastWritten) {
            lastWritten = written;
            lastProgress = now;
        } else if ((double) (now - lastProgress) * 1e-9 > kStaleSeconds) { // stalled: the timeline stops
            fadeOut();
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
            const int n = feed.read((pf::FeedMix) mix, position, scratch.data(), std::min((int) (scratch.size() / 2), resampler.room()));
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
        // The frames the application has made since its last block (the feed's clock): the fill without its sawtooth.
        double offset = 0.0;
        std::uint64_t clockFrames = 0, clockNs = 0;
        bool overdue = false;
        if (feed.clock(clockFrames, clockNs)) {
            const double since = feedNow > clockNs ? (double) (feedNow - clockNs) * 1e-9 : 0.0;
            offset = std::clamp(since * feedRate + ((double) clockFrames - (double) position), 0.0, 4.0 * inBlock + 0.05 * feedRate);
            // The application's blocks come at intervals as regular as its device and its system make them: the
            // longest lately (held, slowly let go) sets when the next one is late. (A timer of 15.6 ms, a busy
            // machine: blocks in bursts, a stop told apart from the next burst only once that is late.)
            if (clockNs != lastClockNs) {
                if (lastClockNs != 0 && clockNs > lastClockNs)
                    longestGap = std::max((double) (clockNs - lastClockNs) * 1e-9, longestGap - 0.001 * (longestGap - inBlock / feedRate));
                lastClockNs = clockNs;
                // The application's rate: the line through its blocks (after kRateWarmSeconds, the ratio follows it).
                if (rateFirstNs == 0 || clockNs < rateFirstNs || clockFrames < rateFirstFrames) {
                    rateFirstNs = clockNs;
                    rateFirstFrames = clockFrames;
                    meanT = meanF = varT = covTF = lastT = 0.0;
                } else {
                    const double t = (double) (clockNs - rateFirstNs) * 1e-9, f = (double) (clockFrames - rateFirstFrames);
                    // Weighted alike over the time elapsed until that reaches kRateSeconds, then exponentially.
                    const double a = t > 0.0 ? std::max(1.0 - std::exp(-(t - lastT) / kRateSeconds), (t - lastT) / t) : 1.0;
                    const double dt = t - meanT, df = f - meanF;
                    meanT += a * dt;
                    meanF += a * df;
                    varT = (1.0 - a) * (varT + a * dt * dt);
                    covTF = (1.0 - a) * (covTF + a * dt * df);
                    lastT = t;
                    if (t > kRateWarmSeconds && varT > 0.0)
                        resampler.setProducerRate(covTF / varT);

                }
            }
            overdue = since > 1.5 * longestGap + 0.003;
            offsetAverage += (1.0 - std::exp(-kPeriodSeconds)) * (offset - offsetAverage);
        }
        const double sawtooth = offset - offsetAverage; // what the fill level's sawtooth is, now
        // The timeline starts once enough is waiting; then every frame due by OBS's clock, stamped on it.
        if (start == 0) {
            if (!resampler.readyToStart()) {
                sleeper.sleep(kPeriodSeconds);
                continue;
            }
            start = now;
            framesOut = 0;
            fade = 0.0;
            s->state = (int) State::running;
            s->runningSince = now;
        }
        std::int64_t due = (std::int64_t) ((double) (now - start) * obsRate * 1e-9) - (std::int64_t) framesOut;
        const int fadeFrames = (int) std::ceil(kFadeSeconds * obsRate);
        while (due > 0) {
            const int n = (int) std::min<std::int64_t>(due, kMaxChunk);
            // The application's next block is late and too little is left: fade out what is (it stopped, most
            // likely; if not, the sound fades back in once enough is waiting again).
            if (overdue && resampler.renderable() < n + fadeFrames) {
                const int m = std::min(n, resampler.renderable());
                if (m > 0) {
                    resampler.render(planes, 2, m, sawtooth);
                    output(m, 0.0, gainTarget());
                }
                if (resampler.renderable() == 0 || fade <= 0.0) {
                    resampler.reset();
                    stopTimeline();
                    break;
                }
                due -= m;
                continue;
            }
            const double feedFrame = (double) position - resampler.waiting(); // the frame this chunk starts at
            resampler.render(planes, 2, n, sawtooth);
            if (!resampler.running()) { // the feed ran dry: start again once it is back
                stopTimeline();
                break;
            }
            Timeline t;
            t.obsNs = start + (std::uint64_t) ((double) framesOut * 1e9 / obsRate);
            t.session = session;
            t.feedFrame = feedFrame;
            t.feedRate = feedRate;
            output(n, 1.0, gainTarget());
            publishTimeline(t);
            due -= n;
        }
        const std::uint32_t dropouts = resampler.underruns() + resampler.overruns();
        if (dropouts != seenDropouts) {
            s->resyncs.fetch_add(dropouts - seenDropouts);
            seenDropouts = dropouts;
        }
        // From the key to OBS: the age of the frames stamped now (their instants by the feed's clock).
        const double age = 1000.0 * (resampler.waiting() + offset) / feedRate;
        keyToObs = keyToObs <= 0.0 ? age : keyToObs + 0.02 * (age - keyToObs);
        s->keyToObsMs = keyToObs;
        s->waitingMs = 1000.0 * resampler.waiting() / feedRate;
        s->driftPpm = resampler.ratioPpm();
        s->volumeDb = 20.0 * std::log10(std::max(1e-6, (double) feed.volume()));
        if (const float r = limiter.takeReduction(); r < -0.05f) {
            limiterLow = std::min(limiterLow, (double) r);
            limiterSince = now;
        } else if (now - limiterSince > 3'000'000'000ull) {
            limiterLow = 0.0;
        }
        s->limiterDb = (float) limiterLow;
        sleeper.sleep(kPeriodSeconds);
    }
    fadeOut();
}

std::string statusText(const Source* s)
{
    char line[512];
    const auto state = (State) s->state.load();
    if (state == State::noApplication)
        return text("Status.NoApplication");
    if (state == State::noAudio)
        return text("Status.NoAudio");
    const double rate = s->feedRate.load(), block = s->block.load();
    std::string out;
    std::snprintf(line, sizeof(line), text("Status.Connected"), rate / 1000.0, block, rate > 0 ? 1000.0 * block / rate : 0.0,
                  s->obsRate.load() / 1000.0);
    out += line;
    const char* mixes[] = { "Mix.Heard", "Mix.Speakers", "Mix.Dry" };
    char volume[160];
    if (s->followVolume.load())
        std::snprintf(volume, sizeof(volume), text("Status.VolumeFollowed"), s->volumeDb.load());
    else
        std::snprintf(volume, sizeof(volume), "%s", text("Status.VolumeIndependent"));
    std::snprintf(line, sizeof(line), text("Status.Mix"), text(mixes[std::clamp(s->mix.load(), 0, 2)]), volume, s->gainDb.load());
    out += "\n" + std::string(line);
    std::snprintf(line, sizeof(line), text("Status.Latency"), s->targetMs.load(), s->waitingMs.load());
    out += "\n" + std::string(line);
    if (state == State::running) {
        std::snprintf(line, sizeof(line), text("Status.KeyToObs"), s->keyToObsMs.load());
        out += "\n" + std::string(line);
    }
    std::snprintf(line, sizeof(line), text("Status.Drift"), s->driftPpm.load());
    out += "\n" + std::string(line);
    std::snprintf(line, sizeof(line), text("Status.Resyncs"), s->resyncs.load());
    out += "\n" + std::string(line);
    if (s->limiterDb.load() < -0.05f) {
        std::snprintf(line, sizeof(line), text("Status.Limiter"), (double) s->limiterDb.load());
        out += "\n" + std::string(line);
    }
    out += "\n" + std::string(text(state == State::starting ? "Status.Starting" : "Status.Running"));
    if (state == State::running && s->runningSince.load() != 0) {
        const double minutes = (double) (os_gettime_ns() - s->runningSince.load()) * 1e-9 / 60.0;
        std::snprintf(line, sizeof(line), text("Status.For"), std::floor(minutes), std::fmod(minutes * 60.0, 60.0));
        out += line;
    }
    return out;
}

const char* sourceName(void*)
{
    return text("Audio.Name");
}

void sourceDefaults(obs_data_t* settings)
{
    obs_data_set_default_int(settings, "latency_ms", kDefaultLatencyMs);
    obs_data_set_default_int(settings, "mix", (int) pf::FeedMix::speakers);
    obs_data_set_default_bool(settings, "follow_volume", false);
    obs_data_set_default_double(settings, "gain_db", 0.0);
    obs_data_set_default_bool(settings, "chapters", true);
}

void sourceUpdate(void* data, obs_data_t* settings)
{
    auto* s = static_cast<Source*>(data);
    s->latencyMs = (int) obs_data_get_int(settings, "latency_ms");
    s->mix = (int) obs_data_get_int(settings, "mix");
    s->followVolume = obs_data_get_bool(settings, "follow_volume");
    s->gainDb = (float) obs_data_get_double(settings, "gain_db");
    const bool chapters = obs_data_get_bool(settings, "chapters");
    if (chapters != s->chapters.exchange(chapters))
        chapterSources.fetch_add(chapters ? 1 : -1);
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
    if (s->chapters.load())
        chapterSources.fetch_sub(1);
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
    obs_properties_add_button2(props, "refresh", text("Refresh"), refreshStatus, data);
    obs_property_t* mix = obs_properties_add_list(props, "mix", text("Mix"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mix, text("Mix.Speakers"), (long long) pf::FeedMix::speakers);
    obs_property_list_add_int(mix, text("Mix.Heard"), (long long) pf::FeedMix::heard);
    obs_property_list_add_int(mix, text("Mix.Dry"), (long long) pf::FeedMix::dry);
    obs_property_set_long_description(mix, text("Mix.Description"));
    obs_property_t* follow = obs_properties_add_bool(props, "follow_volume", text("FollowVolume"));
    obs_property_set_long_description(follow, text("FollowVolume.Description"));
    obs_property_t* gain = obs_properties_add_float_slider(props, "gain_db", text("Gain"), -30.0, 12.0, 0.5);
    obs_property_float_set_suffix(gain, " dB");
    obs_property_set_long_description(gain, text("Gain.Description"));
    obs_property_t* latency = obs_properties_add_int_slider(props, "latency_ms", text("Latency"), kMinLatencyMs, kMaxLatencyMs, 1);
    obs_property_int_set_suffix(latency, " ms");
    obs_property_set_long_description(latency, text("Latency.Description"));
    obs_properties_add_button2(props, "resync", text("Resync"), resynchronise, data);
    obs_property_t* chapters = obs_properties_add_bool(props, "chapters", text("Chapters"));
    obs_property_set_long_description(chapters, text("Chapters.Description"));
    obs_properties_add_text(props, "help", text("Help"), OBS_TEXT_INFO);
    return props;
}

} // namespace

void registerAudioSource()
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
}

} // namespace pfobs
