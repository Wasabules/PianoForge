// The plugin inside the installed libobs, without the OBS application: libobs started for audio only (48 kHz), the
// plugin loaded as OBS loads it, a "PianoForge" source created, and the feed written as the application writes it
// (a 1 kHz sine at 44.1 kHz in blocks of 128, on a clock 200 ppm fast). What libobs hands on (the source's audio, as
// the mixer gets it) must be the sine at 1 kHz, continuous, with timestamps that follow the frames; then the
// properties: the status, a latency changed on the fly, the resync button. The feed has a name of its own
// (PIANOFORGE_AUDIO_FEED): the test runs beside a PianoForge and an OBS in use.
//
//   pianoforge-obs-test <plugin dll> <report file> [feed rate, default 44100]    (OBS's bin/64bit on the PATH)
#include <obs-module.h>
#include <obs.h>

#include <pianoforge/audio_feed.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Capture {
    std::mutex lock;
    std::vector<float> left;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> blocks; // timestamp, frames
};

void onAudio(void* param, obs_source_t*, const struct audio_data* audio, bool)
{
    auto* c = static_cast<Capture*>(param);
    const std::lock_guard<std::mutex> guard(c->lock);
    const auto* l = reinterpret_cast<const float*>(audio->data[0]);
    c->left.insert(c->left.end(), l, l + audio->frames);
    c->blocks.emplace_back(audio->timestamp, audio->frames);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: pianoforge-obs-test <plugin dll> <report file>\n");
        return 2;
    }
    FILE* report = std::fopen(argv[2], "w");
    auto say = [&](const std::string& line) {
        std::printf("%s\n", line.c_str());
        if (report != nullptr)
            std::fprintf(report, "%s\n", line.c_str());
    };
    // A feed of the test's own: it runs beside a PianoForge (and an OBS) in use.
#if defined(_WIN32)
    _putenv_s("PIANOFORGE_AUDIO_FEED", "PianoForgeAudioFeedTest");
#else
    setenv("PIANOFORGE_AUDIO_FEED", "PianoForgeAudioFeedTest", 1);
#endif
    if (!obs_startup("en-US", nullptr, nullptr)) {
        say("[FAIL]  libobs did not start");
        return 1;
    }
    obs_audio_info audioInfo = {};
    audioInfo.samples_per_sec = 48000;
    audioInfo.speakers = SPEAKERS_STEREO;
    if (!obs_reset_audio(&audioInfo)) {
        say("[FAIL]  libobs audio did not start");
        return 1;
    }
    obs_module_t* module = nullptr;
    const int opened = obs_open_module(&module, argv[1], nullptr);
    if (opened != MODULE_SUCCESS || !obs_init_module(module)) {
        say("[FAIL]  the plugin did not load (" + std::to_string(opened) + ")");
        return 1;
    }
    say("[PASS]  the plugin loads into libobs " + std::string(obs_get_version_string()));

    // The application's side: the feed, written from a thread at the pace of a device running fast.
    constexpr double seconds = 40.0, clock = 1.0 + 200e-6;
    const double rate = argc > 3 ? std::atof(argv[3]) : 44100.0;
    pf::AudioFeedWriter feed;
    if (!feed.create()) {
        say("[FAIL]  the feed could not be created (another test running?)");
        return 1;
    }
    feed.setFormat((std::uint32_t) rate, 128);
    obs_source_t* source = obs_source_create("pianoforge_audio", "PianoForge test", nullptr, nullptr);
    if (source == nullptr) {
        say("[FAIL]  no source of type pianoforge_audio");
        return 1;
    }
    Capture capture;
    obs_source_add_audio_capture_callback(source, onAudio, &capture);

    std::atomic<bool> producing { true };
    std::thread producer([&] {
        std::vector<float> l(128), r(128);
        const float* channels[2] = { l.data(), r.data() };
        long long produced = 0;
        const auto start = std::chrono::steady_clock::now();
        while (producing.load() && produced < (long long) ((seconds + 30.0) * rate)) {
            for (int k = 0; k < 128; ++k)
                l[(std::size_t) k] = r[(std::size_t) k] = 0.25f * (float) std::sin(6.283185307179586 * 1000.0 * (double) (produced + k) / rate);
            feed.write(channels, 2, 128);
            produced += 128;
            std::this_thread::sleep_until(start + std::chrono::nanoseconds((long long) (1e9 * produced / (rate * clock))));
        }
    });
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds / clock));
    obs_source_remove_audio_capture_callback(source, onAudio, &capture);

    // The properties, while the audio still flows: the status, a new latency, the resync button.
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        say((ok ? "[PASS]  " : "[FAIL]  ") + what);
        failures += ok ? 0 : 1;
    };
    obs_properties_t* props = obs_source_properties(source);
    auto status = [&] {
        obs_property_button_clicked(obs_properties_get(props, "refresh"), source);
        const char* text = obs_property_description(obs_properties_get(props, "status"));
        return std::string(text != nullptr ? text : "");
    };
    auto number = [](const std::string& text, const std::string& after) {
        const auto at = text.find(after);
        return at == std::string::npos ? -1.0 : std::atof(text.c_str() + at + after.size());
    };
    check(props != nullptr && obs_properties_get(props, "status") != nullptr && obs_properties_get(props, "latency_ms") != nullptr
              && obs_properties_get(props, "refresh") != nullptr && obs_properties_get(props, "resync") != nullptr,
          "properties: status, latency, refresh and resync");
    const std::string first = status();
    say("        status: " + first);
    check(first.find("PianoForge connected") != std::string::npos && std::fabs(number(first, "Latency: ") - 20.0) < 0.5,
          "the status says connected, 20 ms of latency by default");
    obs_data_t* settings = obs_data_create();
    obs_data_set_int(settings, "latency_ms", 50);
    obs_source_update(source, settings);
    obs_data_release(settings);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const std::string second = status();
    check(std::fabs(number(second, "Latency: ") - 50.0) < 0.5 && std::fabs(number(second, "aimed at, ") - 50.0) < 10.0,
          "latency set to 50 ms: aimed at " + std::to_string(number(second, "Latency: ")) + " ms, "
              + std::to_string(number(second, "aimed at, ")) + " ms waiting");
    const double before = number(second, "created: ");
    obs_property_button_clicked(obs_properties_get(props, "resync"), source);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::string third = status();
    check(number(third, "created: ") == before + 1.0 && third.find("Running") != std::string::npos,
          "resynchronise: one resync more, and running again");
    obs_properties_destroy(props);
    producing = false;
    producer.join();
    obs_source_release(source);
    feed.close();

    // What libobs passed on.
    const std::lock_guard<std::mutex> guard(capture.lock);
    const auto& x = capture.left;
    const double received = (double) x.size() / 48000.0;
    say("        received " + std::to_string(received) + " s of audio at 48 kHz in " + std::to_string(capture.blocks.size())
        + " blocks, for " + std::to_string(seconds / clock) + " s produced");
    // Timestamps: each block where the previous one ended (within 1 ms), no jump.
    double worstJumpMs = 0.0;
    for (std::size_t i = 1; i < capture.blocks.size(); ++i) {
        const double expected = (double) capture.blocks[i - 1].first + 1e9 * capture.blocks[i - 1].second / 48000.0;
        worstJumpMs = std::fmax(worstJumpMs, std::fabs((double) capture.blocks[i].first - expected) * 1e-6);
    }
    // The sine over the last 20 s: frequency (from the phase over 1 s windows) and the residual after a fit.
    double worstResidual = -300.0, lowF = 1e9, highF = 0.0;
    const int window = 48000;
    for (std::size_t s = x.size() > (std::size_t) (21 * window) ? x.size() - (std::size_t) (20 * window) : x.size();
         s + (std::size_t) window <= x.size(); s += (std::size_t) window) {
        // Frequency: zero crossings with linear interpolation, first to last.
        double first = -1.0, last = -1.0;
        int crossings = 0;
        for (int k = 1; k < window; ++k) {
            const float a = x[s + (std::size_t) k - 1], b = x[s + (std::size_t) k];
            if (a < 0.0f && b >= 0.0f) {
                const double t = (k - 1) + (double) (-a / (b - a));
                if (first < 0.0)
                    first = t;
                last = t;
                ++crossings;
            }
        }
        const double f = crossings > 1 ? (crossings - 1) * 48000.0 / (last - first) : 0.0;
        lowF = std::fmin(lowF, f);
        highF = std::fmax(highF, f);
        // Least-squares fits at that frequency over 20 ms (the loop's ppm-level wander would show over a whole
        // second as phase, not as noise), the rest against the fit.
        const double w = 6.283185307179586 * f / 48000.0;
        constexpr int span = 960;
        for (int o = 0; o + span <= window; o += span) {
            double cc = 0, cs = 0, ss = 0, xc = 0, xs = 0;
            for (int k = o; k < o + span; ++k) {
                const double co = std::cos(w * k), si = std::sin(w * k);
                cc += co * co;
                cs += co * si;
                ss += si * si;
                xc += x[s + (std::size_t) k] * co;
                xs += x[s + (std::size_t) k] * si;
            }
            const double det = cc * ss - cs * cs, a = (xc * ss - xs * cs) / det, b = (xs * cc - xc * cs) / det;
            double fitted = 0.0, rest = 0.0;
            for (int k = o; k < o + span; ++k) {
                const double y = a * std::cos(w * k) + b * std::sin(w * k);
                fitted += y * y;
                rest += (x[s + (std::size_t) k] - y) * (x[s + (std::size_t) k] - y);
            }
            worstResidual = std::fmax(worstResidual, 10.0 * std::log10(rest / fitted + 1e-30));
        }
    }
    // Glitches: a sample far from what its neighbours predict (a sine obeys x[k] = 2 cos(w) x[k-1] - x[k-2]).
    int glitches = 0;
    {
        const double c2 = 2.0 * std::cos(6.283185307179586 * 1000.0 / 48000.0);
        for (std::size_t k = x.size() > (std::size_t) (21 * window) ? x.size() - (std::size_t) (20 * window) : 2; k < x.size(); ++k)
            glitches += std::fabs(x[k] - (c2 * x[k - 1] - x[k - 2])) > 0.01 ? 1 : 0;
    }
    const double expectedF = 1000.0 * clock; // the producer's clock runs fast: its 1 kHz is 1000.2 Hz on OBS's
    check(received > seconds / clock - 1.0, "the audio arrives (" + std::to_string(received) + " s)");
    check(worstJumpMs < 1.0, "timestamps continuous: largest jump " + std::to_string(worstJumpMs) + " ms");
    check(lowF > expectedF - 0.05 && highF < expectedF + 0.05,
          "frequency " + std::to_string(lowF) + " to " + std::to_string(highF) + " Hz on OBS's clock (expected "
              + std::to_string(expectedF) + ")");
    check(worstResidual < -55.0 && glitches == 0, "clean: residual after a sine fit " + std::to_string(worstResidual)
                                                     + " dB, " + std::to_string(glitches) + " glitches over the last 20 s");
    obs_shutdown();
    say(failures == 0 ? "All OBS plugin tests passed" : std::to_string(failures) + " failure(s)");
    if (report != nullptr)
        std::fclose(report);
    return failures == 0 ? 0 : 1;
}
