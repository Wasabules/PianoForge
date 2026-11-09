// The plugin inside the installed libobs, without the OBS application: libobs started for audio (48 kHz) and video
// (OBS's Direct3D 11 module, offscreen), the plugin loaded as OBS loads it (its locale too), OBS's text source beside
// it, and the feed written as the application writes it: three mixes of a 1 kHz sine at 44.1 kHz in blocks of 128, on
// a clock 200 ppm fast, the application's volume at -6 dB, and MIDI events (middle C held, the sustain pedal down).
//   - the audio source: the speakers mix by default, at its level whatever the application's volume; continuous, with
//     timestamps that follow the frames, at 1 kHz on OBS's clock, clean; its status (latency, key to OBS); a new
//     latency, the resync button; another mix with the volume followed, reached through fades (no click);
//   - the readers the application sees (sound, keyboard, text, and the mix taken);
//   - the keyboard: rendered, C2 lit blue (left hand), middle C orange, D not, a note rising, the sustain pedal; a
//     phrase played (its images kept), sparkles thrown up by strikes (leaning as asked), none when off; the velocity
//     (a soft note thinner and duller than a strong one); a powerful chord (the room lit, fireworks; nothing for a
//     soft chord, nor with them off);
//   - the text: its template filled with the application's state, wider for a longer text;
//   - the chapters: a mark while OBS does not record is said so in the log.
// The feed has a name of its own (PIANOFORGE_AUDIO_FEED): the test runs beside a PianoForge and an OBS in use.
//
//   pianoforge-obs-test <plugin dll> <plugin data> <report file> <OBS folder> [feed rate, default 44100]
//   (OBS's bin/64bit on the PATH)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <obs-module.h>
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/base.h>

#include <pianoforge/audio_feed.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Capture {
    std::mutex lock;
    std::vector<float> left;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> blocks; // timestamp, frames
    std::uint32_t rate = 0;
};

void onAudio(void* param, obs_source_t*, const struct audio_data* audio, bool)
{
    auto* c = static_cast<Capture*>(param);
    const std::lock_guard<std::mutex> guard(c->lock);
    const auto* l = reinterpret_cast<const float*>(audio->data[0]);
    c->left.insert(c->left.end(), l, l + audio->frames);
    c->blocks.emplace_back(audio->timestamp, audio->frames);
}

std::mutex logLock;
std::string logText;

void onLog(int level, const char* format, va_list args, void*)
{
    char line[4096];
    std::vsnprintf(line, sizeof(line), format, args);
    const std::lock_guard<std::mutex> guard(logLock);
    logText += line;
    logText += '\n';
    (void) level;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double s = 0.0;
    for (std::size_t k = from; k < to && k < x.size(); ++k)
        s += (double) x[k] * x[k];
    return to > from ? std::sqrt(s / (double) (to - from)) : 0.0;
}

/// A source rendered offscreen, BGRA rows.
std::vector<std::uint8_t> renderSource(obs_source_t* source, std::uint32_t& w, std::uint32_t& h)
{
    std::vector<std::uint8_t> pixels;
    w = obs_source_get_width(source);
    h = obs_source_get_height(source);
    if (w == 0 || h == 0)
        return pixels;
    obs_enter_graphics();
    gs_texrender_t* texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (gs_texrender_begin(texrender, w, h)) {
        vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
        gs_ortho(0.0f, (float) w, 0.0f, (float) h, -100.0f, 100.0f);
        obs_source_video_render(source);
        gs_texrender_end(texrender);
        gs_stagesurf_t* stage = gs_stagesurface_create(w, h, GS_BGRA);
        gs_stage_texture(stage, gs_texrender_get_texture(texrender));
        uint8_t* data = nullptr;
        uint32_t linesize = 0;
        if (gs_stagesurface_map(stage, &data, &linesize)) {
            pixels.resize((std::size_t) w * h * 4);
            for (std::uint32_t y = 0; y < h; ++y)
                std::memcpy(pixels.data() + (std::size_t) y * w * 4, data + (std::size_t) y * linesize, (std::size_t) w * 4);
            gs_stagesurface_unmap(stage);
        }
        gs_stagesurface_destroy(stage);
    }
    gs_texrender_destroy(texrender);
    obs_leave_graphics();
    return pixels;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: pianoforge-obs-test <plugin dll> <plugin data> <report file> <OBS folder> [feed rate]\n");
        return 2;
    }
    const std::string pluginPath = std::filesystem::absolute(argv[1]).string();
    const std::string dataPath = std::filesystem::absolute(argv[2]).string();
    const std::filesystem::path obsDir = argv[4];
    FILE* report = std::fopen(std::filesystem::absolute(argv[3]).string().c_str(), "w");
    const auto reportFolder = std::filesystem::absolute(argv[3]).parent_path(); // (before the folder changes, below)
    auto say = [&](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        if (report != nullptr)
            std::fprintf(report, "%s\n", line.c_str());
    };
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        say((ok ? "[PASS]  " : "[FAIL]  ") + what);
        failures += ok ? 0 : 1;
    };
    // A feed of the test's own: it runs beside a PianoForge (and an OBS) in use.
#if defined(_WIN32)
    _putenv_s("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedTest");
#else
    setenv("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedTest", 1);
#endif
    // libobs finds its effects from OBS's bin/64bit (../../data/libobs), and loads its modules (the graphics, the
    // plugin's frontend API) with a restricted search: OBS's folder added to it, as OBS's own executable there has it.
    std::filesystem::current_path(obsDir / "bin" / "64bit");
#if defined(_WIN32)
    AddDllDirectory((obsDir / "bin" / "64bit").wstring().c_str());
#endif
    base_set_log_handler(onLog, nullptr);
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
    obs_video_info videoInfo = {};
    videoInfo.graphics_module = "libobs-d3d11";
    videoInfo.fps_num = 30;
    videoInfo.fps_den = 1;
    videoInfo.base_width = videoInfo.output_width = 1280;
    videoInfo.base_height = videoInfo.output_height = 720;
    videoInfo.output_format = VIDEO_FORMAT_NV12;
    videoInfo.colorspace = VIDEO_CS_709;
    videoInfo.range = VIDEO_RANGE_PARTIAL;
    videoInfo.gpu_conversion = true;
    videoInfo.scale_type = OBS_SCALE_BICUBIC;
    const bool video = obs_reset_video(&videoInfo) == OBS_VIDEO_SUCCESS;
    if (!video) {
        const std::lock_guard<std::mutex> guard(logLock);
        say(logText);
    }
    check(video, "libobs video started (Direct3D 11, offscreen)");
    obs_module_t* module = nullptr;
    const int opened = obs_open_module(&module, pluginPath.c_str(), dataPath.c_str());
    if (opened != MODULE_SUCCESS || !obs_init_module(module)) {
        const std::lock_guard<std::mutex> guard(logLock);
        say("[FAIL]  the plugin did not load (" + std::to_string(opened) + ")\n" + logText);
        return 1;
    }
    say("[PASS]  the plugin loads into libobs " + std::string(obs_get_version_string()));
    obs_module_t* textModule = nullptr;
    const auto textDll = (obsDir / "core" / "obs-text" / "obs-text.dll").string();
    const auto textData = (obsDir / "core" / "obs-text" / "data").string();
    const bool haveText = obs_open_module(&textModule, textDll.c_str(), textData.c_str()) == MODULE_SUCCESS && obs_init_module(textModule);
    check(haveText, "OBS's text source loads beside it");

    // The application's side: the feed, written from a thread at the pace of a device running fast. Three mixes of
    // the sine (speakers 0.25, heard 0.1, dry 0.05), the volume at -6 dB, middle C and the sustain pedal down at 1 s.
    constexpr double seconds = 40.0, clock = 1.0 + 200e-6;
    const double rate = argc > 5 ? std::atof(argv[5]) : 44100.0;
    pf::AudioFeedWriter feed;
    if (!feed.create()) {
        say("[FAIL]  the feed could not be created (another test running?)");
        return 1;
    }
    feed.setFormat((std::uint32_t) rate, 128);
    pf::FeedInfo info;
    std::snprintf(info.engine, sizeof(info.engine), "Modal");
    std::snprintf(info.preset, sizeof(info.preset), "Concert");
    std::snprintf(info.file, sizeof(info.file), "Prelude");
    feed.setInfo(info);
    obs_source_t* source = obs_source_create("pianoforge_audio", "PianoForge test", nullptr, nullptr);
    if (source == nullptr) {
        say("[FAIL]  no source of type pianoforge_audio");
        return 1;
    }
    Capture capture;
    obs_source_add_audio_capture_callback(source, onAudio, &capture);

    std::atomic<bool> producing { true };
    std::mutex playLock; // events the test plays, written with the next block
    std::vector<pf::FeedEvent> played;
    auto play = [&](std::initializer_list<pf::FeedEvent> events) {
        const std::lock_guard<std::mutex> guard(playLock);
        played.insert(played.end(), events.begin(), events.end());
    };
    std::thread producer([&] {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); // as an audio driver's callback runs
#endif
        std::vector<float> mixes[3][2];
        for (auto& m : mixes)
            for (auto& c : m)
                c.assign(128, 0.0f);
        const float amplitude[3] = { 0.1f, 0.25f, 0.05f };
        long long produced = 0;
        bool struck = false;
        std::vector<pf::FeedEvent> blockEvents;
        blockEvents.reserve(64);
        const auto start = std::chrono::steady_clock::now();
        while (producing.load() && produced < (long long) ((seconds + 60.0) * rate)) {
            for (int k = 0; k < 128; ++k) {
                const float s = (float) std::sin(6.283185307179586 * 1000.0 * (double) (produced + k) / rate);
                for (int m = 0; m < 3; ++m)
                    mixes[m][0][(std::size_t) k] = mixes[m][1][(std::size_t) k] = amplitude[m] * s;
            }
            pf::AudioFeedWriter::Block b;
            for (int m = 0; m < 3; ++m) {
                b.left[m] = mixes[m][0].data();
                b.right[m] = mixes[m][1].data();
            }
            b.frames = 128;
            b.volume = 0.5f;
            const pf::FeedEvent events[3] = { { 0, 0x90, 36, 100, 0, 0 }, { 0, 0x90, 60, 100, 0, 0 }, { 10, 0xB0, 64, 127, 0, 0 } };
            blockEvents.clear();
            if (!struck && produced >= (long long) rate) {
                blockEvents.assign(events, events + 3);
                struck = true;
            }
            {
                const std::lock_guard<std::mutex> guard(playLock);
                blockEvents.insert(blockEvents.end(), played.begin(), played.end());
                played.clear();
            }
            b.events = blockEvents.data();
            b.numEvents = (int) blockEvents.size();
            feed.write(b);
            produced += 128;
            std::this_thread::sleep_until(start + std::chrono::nanoseconds((long long) (1e9 * produced / (rate * clock))));
        }
    });
    obs_source_t* keyboard = obs_source_create("pianoforge_keyboard", "PianoForge keyboard test", nullptr, nullptr);
    obs_source_t* textSource = obs_source_create("pianoforge_text", "PianoForge text test", nullptr, nullptr);
    if (textSource != nullptr) {
        obs_data_t* s = obs_data_create();
        obs_data_set_string(s, "template", "{preset}: {file}");
        obs_source_update(textSource, s);
        obs_data_release(s);
    }
    if (keyboard != nullptr) {
        obs_data_t* s = obs_data_create();
        obs_data_set_int(s, "width", 1040); // white keys of 20 pixels
        obs_data_set_int(s, "trail_height", 100);
        obs_source_update(keyboard, s);
        obs_data_release(s);
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds / clock));
    obs_source_remove_audio_capture_callback(source, onAudio, &capture);

    // The properties, while the audio still flows: the status, a new latency, the resync button.
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
              && obs_properties_get(props, "refresh") != nullptr && obs_properties_get(props, "resync") != nullptr
              && obs_properties_get(props, "mix") != nullptr && obs_properties_get(props, "follow_volume") != nullptr
              && obs_properties_get(props, "gain_db") != nullptr && obs_properties_get(props, "chapters") != nullptr,
          "properties: status, mix, volume followed, gain, latency, refresh, resync, chapters");
    const std::string first = status();
    say("        status: " + first);
    const double keyToObs = number(first, "from the key to OBS: ");
    check(first.find("PianoForge connected") != std::string::npos && std::fabs(number(first, "Latency: ") - 20.0) < 0.5
              && first.find("into OBS at 48.0 kHz") != std::string::npos,
          "the status says connected, 20 ms of latency by default, straight to OBS's 48 kHz");
    check(keyToObs > 15.0 && keyToObs < 45.0, "from the key to OBS: " + std::to_string(keyToObs) + " ms (the latency and a block)");

    // The readers the application sees.
    {
        pf::AudioFeedWriter::Reader readers[8];
        const int n = feed.readers(readers, 8);
        int audio = 0, keys = 0, texts = 0;
        bool speakers = false;
        for (int i = 0; i < n; ++i) {
            audio += readers[i].kind == pf::FeedReaderKind::audio;
            keys += readers[i].kind == pf::FeedReaderKind::keyboard;
            texts += readers[i].kind == pf::FeedReaderKind::text;
            speakers = speakers || (readers[i].kind == pf::FeedReaderKind::audio && readers[i].mix == pf::FeedMix::speakers
                                    && std::fabs(readers[i].latencyMs - keyToObs) < 10.0f);
        }
        for (int i = 0; i < n; ++i)
            say("        reader: kind " + std::to_string((int) readers[i].kind) + ", mix " + std::to_string((int) readers[i].mix)
                + ", " + std::to_string(readers[i].latencyMs) + " ms");
        check(audio == 1 && keys == 1 && texts == 1 && speakers,
              "the application sees its readers: the sound (speakers mix, its latency), the keyboard, the text");
    }
    obs_data_t* settings = obs_data_create();
    obs_data_set_int(settings, "latency_ms", 50);
    obs_source_update(source, settings);
    obs_data_release(settings);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const std::string second = status();
    check(std::fabs(number(second, "Latency: ") - 50.0) < 0.5 && std::fabs(number(second, "aimed at, ") - 50.0) < 10.0,
          "latency set to 50 ms: aimed at " + std::to_string(number(second, "Latency: ")) + " ms, "
              + std::to_string(number(second, "aimed at, ")) + " ms waiting");
    const double before = number(second, "resyncs: ");
    obs_property_button_clicked(obs_properties_get(props, "resync"), source);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::string third = status();
    check(number(third, "resyncs: ") == before + 1.0 && third.find("Running") != std::string::npos,
          "resynchronise: one resync more, and running again");

    // Another mix (as heard) with the application's volume followed: through a fade out and a fade in.
    Capture change;
    obs_source_add_audio_capture_callback(source, onAudio, &change);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    settings = obs_data_create();
    obs_data_set_int(settings, "mix", (int) pf::FeedMix::heard);
    obs_data_set_bool(settings, "follow_volume", true);
    obs_source_update(source, settings);
    obs_data_release(settings);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    obs_source_remove_audio_capture_callback(source, onAudio, &change);
    {
        const std::lock_guard<std::mutex> guard(change.lock);
        const auto& y = change.left;
        double worstStep = 0.0;
        for (std::size_t k = 1; k < y.size(); ++k)
            worstStep = std::fmax(worstStep, std::fabs((double) y[k] - y[k - 1]));
        const double startLevel = rms(y, 0, 4800) * std::sqrt(2.0), endLevel = rms(y, y.size() - 9600, y.size()) * std::sqrt(2.0);
        check(std::fabs(startLevel - 0.25) < 0.01 && std::fabs(endLevel - 0.05) < 0.003,
              "the speakers mix at its own level (" + std::to_string(startLevel) + "), then the heard one at the application's volume ("
                  + std::to_string(endLevel) + ", 0.1 at -6 dB)");
        check(worstStep < 0.04, "the change of mix fades out and in: no step larger than the sine's own ("
                                    + std::to_string(worstStep) + ", the sine moves 0.033 at most)");
    }
    obs_properties_destroy(props);

    // The keyboard, its colours by hand (the default): C2 and middle C struck together, too far apart for one hand:
    // C2 the left hand's (blue), middle C the right's (orange), D not lit, middle C's note rising, the sustain pedal.
    // Then a phrase played, its images kept (keyboard-1.bmp...); and sparkles thrown up by strikes, none when off.
    if (video && keyboard != nullptr) {
        auto save = [&](const std::vector<std::uint8_t>& px, std::uint32_t w, std::uint32_t h, const char* name) {
            // 32-bit, bottom-up rows
            if (FILE* f = std::fopen((reportFolder / name).string().c_str(), "wb")) {
                const std::uint32_t size = 54 + w * h * 4;
                const std::uint8_t header[54] = { 'B', 'M', (std::uint8_t) size, (std::uint8_t) (size >> 8), (std::uint8_t) (size >> 16),
                                                  (std::uint8_t) (size >> 24), 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                                                  (std::uint8_t) w, (std::uint8_t) (w >> 8), 0, 0, (std::uint8_t) h, (std::uint8_t) (h >> 8), 0, 0,
                                                  1, 0, 32, 0 };
                std::fwrite(header, 1, 54, f);
                for (std::uint32_t y = h; y-- > 0;)
                    std::fwrite(px.data() + (std::size_t) y * w * 4, 1, (std::size_t) w * 4, f);
                std::fclose(f);
            }
        };
        std::uint32_t w = 0, h = 0;
        auto px = renderSource(keyboard, w, h);
        auto at = [&](int x, int y) {
            const std::size_t i = ((std::size_t) y * w + (std::size_t) x) * 4;
            return std::array<int, 3> { px[i + 2], px[i + 1], px[i] }; // r, g, b
        };
        auto show = [](const std::array<int, 3>& c) { return std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]); };
        // 1040 wide: white keys of 20 pixels; the band of keys 130 high (a rail of 7, the keys from 107 to 230), the
        // pedals under it (16).
        const int keysTop = 107, bottom = 230;
        bool ok = !px.empty() && w == 1040 && h == 100 + 130 + 16;
        if (ok) {
            save(px, w, h, "keyboard.bmp");
            const int y = keysTop + (int) (0.75 * (bottom - keysTop));
            const auto c2 = at(9 * 20 + 10, y), c4 = at(23 * 20 + 10, y), d4 = at(24 * 20 + 10, y);
            const auto pedal = at(1040 - 20, bottom + 8), note = at(23 * 20 + 10, 60);
            say("        C2 " + show(c2) + "; middle C " + show(c4) + "; D " + show(d4) + "; pedal " + show(pedal) + "; note " + show(note));
            ok = c2[2] > c2[0] + 40 && c4[0] > c4[2] + 60 && c4[0] > 150 && d4[0] > 200 && d4[1] > 200 && d4[2] > 200
                 && std::abs(d4[0] - d4[2]) < 15 && pedal[0] > pedal[2] + 40 && note[0] > note[2] + 40;
        }
        check(ok, "the keyboard by hand: C2 to the left (blue), middle C to the right (orange), D not lit, its note rising, the sustain pedal");

        // A phrase: the keys up, then a rising arpeggio and a chord, struck harder and harder.
        play({ { 0, 0x80, 36, 0, 0, 0 }, { 0, 0x80, 60, 0, 0, 0 }, { 0, 0xB0, 64, 0, 0, 0 } });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int arpeggio[8] = { 48, 52, 55, 60, 64, 67, 72, 76 };
        for (int i = 0; i < 8; ++i) {
            play({ { 0, 0x90, (std::uint8_t) arpeggio[i], (std::uint8_t) (60 + 7 * i), 0, 0 } });
            std::this_thread::sleep_for(std::chrono::milliseconds(110));
        }
        play({ { 0, 0xB0, 64, 127, 0, 0 } });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        play({ { 0, 0x90, 36, 120, 0, 0 }, { 0, 0x90, 43, 115, 0, 0 }, { 0, 0x90, 79, 125, 0, 0 }, { 0, 0x90, 84, 127, 0, 0 },
               { 0, 0x90, 88, 120, 0, 0 } });
        const char* frames[3] = { "keyboard-1.bmp", "keyboard-2.bmp", "keyboard-3.bmp" };
        const int waits[3] = { 90, 250, 600 };
        bool rendered = true;
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waits[i]));
            px = renderSource(keyboard, w, h);
            rendered = rendered && !px.empty();
            if (!px.empty())
                save(px, w, h, frames[i]);
        }
        check(rendered, "a phrase played: its images kept (keyboard-1.bmp to keyboard-3.bmp)");
        {   // What a busy image costs: the time to draw it (the CPU's part, the GPU running behind), over 40 draws.
            obs_enter_graphics();
            gs_texrender_t* target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
            int drawn = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 40; ++i) {
                gs_texrender_reset(target);
                if (!gs_texrender_begin(target, w, h))
                    continue;
                vec4 clear;
                vec4_zero(&clear);
                gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
                gs_ortho(0.0f, (float) w, 0.0f, (float) h, -100.0f, 100.0f);
                obs_source_video_render(keyboard);
                gs_texrender_end(target);
                ++drawn;
            }
            gs_flush();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / std::max(1, drawn);
            gs_texrender_destroy(target);
            obs_leave_graphics();
            check(drawn == 40 && ms < 5.0, "a busy image (15 notes, their lights, sparkles) drawn in " + std::to_string(ms) + " ms");
        }

        // Sparkles: above the keys, with nothing else drawn there (no notes, no lights), bright points after strikes;
        // none when they are off.
        // Every key up first (the phrase's held ones would throw sparkles of their own).
        {
            std::vector<pf::FeedEvent> up;
            for (int key = 21; key <= 108; ++key)
                up.push_back({ 0, 0x80, (std::uint8_t) key, 0, 0, 0 });
            up.push_back({ 0, 0xB0, 64, 0, 0, 0 });
            const std::lock_guard<std::mutex> guard(playLock);
            played.insert(played.end(), up.begin(), up.end());
        }
        struct Sparkled {
            int bright = 0;
            double meanX = 0.0;
        };
        auto sparkleTest = [&](int amount, int key, int tilt) {
            auto set = [&](int sparkles) {
                obs_data_t* s = obs_data_create();
                obs_data_set_int(s, "trail_height", 0);
                obs_data_set_int(s, "glow", 0);
                obs_data_set_int(s, "sparkles", sparkles);
                obs_data_set_int(s, "sparkle_spread", 10);
                obs_data_set_int(s, "sparkle_tilt", tilt);
                obs_data_set_bool(s, "chords", false);
                obs_source_update(keyboard, s);
                obs_data_release(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            };
            set(0); // the sparks of a test before gone
            set(amount);
            play({ { 0, 0x90, (std::uint8_t) key, 120, 0, 0 }, { 0, 0x90, (std::uint8_t) (key + 4), 120, 0, 0 },
                   { 0, 0x90, (std::uint8_t) (key + 7), 120, 0, 0 } });
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            px = renderSource(keyboard, w, h);
            const int top = (int) h - 130 - 16; // the room above the keys
            Sparkled out;
            for (int y = 0; y + 12 < top && !px.empty(); ++y)
                for (int x = 0; x < (int) w; ++x) {
                    const auto c = at(x, y);
                    if (c[0] + c[1] + c[2] > 180) {
                        ++out.bright;
                        out.meanX += x;
                    }
                }
            out.meanX /= std::max(1, out.bright);
            play({ { 0, 0x80, (std::uint8_t) key, 0, 0, 0 }, { 0, 0x80, (std::uint8_t) (key + 4), 0, 0, 0 },
                   { 0, 0x80, (std::uint8_t) (key + 7), 0, 0, 0 } });
            return out;
        };
        const auto without = sparkleTest(0, 69, 0), with = sparkleTest(100, 69, 0);
        if (!px.empty())
            save(px, w, h, "keyboard-sparkles.bmp");
        const auto leaning = sparkleTest(100, 69, 40);
        if (!px.empty())
            save(px, w, h, "keyboard-sparkles-tilted.bmp");
        check(without.bright == 0 && with.bright > 30, "sparkles thrown up by strikes (" + std::to_string(with.bright)
                                                           + " bright pixels above the keys), none when off ("
                                                           + std::to_string(without.bright) + ")");
        check(leaning.bright > 30 && leaning.meanX > with.meanX + 4.0,
              "sparkles leaning to the right: around x " + std::to_string((int) leaning.meanX) + ", straight up around x "
                  + std::to_string((int) with.meanX));
        // The keyboard's look, set for a test (what is not given keeps its value).
        auto look = [&](std::initializer_list<std::pair<const char*, long long>> numbers, std::initializer_list<std::pair<const char*, bool>> flags) {
            obs_data_t* s = obs_data_create();
            for (const auto& [name, value] : numbers)
                obs_data_set_int(s, name, value);
            for (const auto& [name, value] : flags)
                obs_data_set_bool(s, name, value);
            obs_source_update(keyboard, s);
            obs_data_release(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        };
        auto noteOn = [](int key, int velocity) { return pf::FeedEvent { 0, 0x90, (std::uint8_t) key, (std::uint8_t) velocity, 0, 0 }; };
        auto noteOff = [](int key) { return pf::FeedEvent { 0, 0x80, (std::uint8_t) key, 0, 0, 0 }; };

        {   // The velocity shows: a soft note thinner and duller than a strong one (its white-hot core).
            look({ { "color_mode", 0 }, { "trail_height", 100 }, { "glow", 60 }, { "sparkles", 0 } }, { { "chords", false } });
            play({ noteOn(50, 25), noteOn(69, 110) });
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
            px = renderSource(keyboard, w, h);
            auto measure = [&](int from, int to, int& width, int& peak) {
                width = peak = 0;
                for (int x = from; x < to && !px.empty(); ++x) {
                    const std::size_t i = ((std::size_t) 96 * w + (std::size_t) x) * 4; // 4 pixels above the keys
                    width += px[i + 3] > 110 ? 1 : 0;
                    peak = std::max(peak, (int) px[i] + px[i + 1] + px[i + 2]);
                }
            };
            int softWidth = 0, softPeak = 0, strongWidth = 0, strongPeak = 0;
            measure(330, 370, softWidth, softPeak); // D3: white key 17
            measure(550, 590, strongWidth, strongPeak); // A4: white key 28
            if (!px.empty())
                save(px, w, h, "keyboard-velocity.bmp");
            play({ noteOff(50), noteOff(69) });
            check(strongWidth >= softWidth + 3 && strongPeak > softPeak + 60,
                  "the velocity shows: a soft note " + std::to_string(softWidth) + " px wide (brightest " + std::to_string(softPeak)
                      + "), a strong one " + std::to_string(strongWidth) + " px (" + std::to_string(strongPeak) + ")");
        }

        {   // A powerful chord: the room above the keys lit, then fireworks; nothing for a soft chord, nor with them off.
            struct Chorded {
                long long light = 0;
                int bright = 0;
            };
            auto chordTest = [&](bool on, int velocity) {
                look({ { "color_mode", 2 }, { "trail_height", 0 }, { "glow", 0 }, { "sparkles", 0 } }, { { "chords", on } });
                play({ noteOn(60, velocity), noteOn(64, velocity), noteOn(67, velocity), noteOn(72, velocity) });
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                Chorded out;
                px = renderSource(keyboard, w, h);
                const int top = (int) h - 130 - 16;
                for (int y = 8; y + 8 < top && !px.empty(); ++y)
                    for (int x = 0; x < (int) w; ++x) {
                        const auto c = at(x, y);
                        out.light += c[0] + c[1] + c[2];
                    }
                std::this_thread::sleep_for(std::chrono::milliseconds(630));
                px = renderSource(keyboard, w, h);
                for (int y = 0; y + 20 < top && !px.empty(); ++y)
                    for (int x = 0; x < (int) w; ++x) {
                        const auto c = at(x, y);
                        out.bright += c[0] + c[1] + c[2] > 180 ? 1 : 0;
                    }
                play({ noteOff(60), noteOff(64), noteOff(67), noteOff(72) });
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return out;
            };
            const auto on = chordTest(true, 110), off = chordTest(false, 110), soft = chordTest(true, 50);
            check(on.light > off.light * 13 / 10 && on.bright > 20 && off.bright == 0 && soft.bright == 0,
                  "a powerful chord: the room lit (" + std::to_string(on.light) + " against " + std::to_string(off.light)
                      + " without), fireworks (" + std::to_string(on.bright) + " bright pixels; " + std::to_string(off.bright)
                      + " without, " + std::to_string(soft.bright) + " for a soft chord)");

            // Its images, the look as by default: a big chord of both hands.
            look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 60 }, { "sparkles", 50 } }, { { "chords", true } });
            play({ noteOn(36, 118), noteOn(43, 116), noteOn(48, 117), noteOn(72, 120), noteOn(76, 119), noteOn(79, 121) });
            const char* chordFrames[3] = { "keyboard-chord-1.bmp", "keyboard-chord-2.bmp", "keyboard-chord-3.bmp" };
            const int chordWaits[3] = { 90, 260, 450 };
            for (int i = 0; i < 3; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(chordWaits[i]));
                px = renderSource(keyboard, w, h);
                if (!px.empty())
                    save(px, w, h, chordFrames[i]);
            }
            play({ noteOff(36), noteOff(43), noteOff(48), noteOff(72), noteOff(76), noteOff(79) });
        }

        {   // The rainbow: the bass warm (C2 toward orange), the treble cool (E6 toward blue); the hues in the properties.
            obs_data_t* s = obs_data_create();
            obs_data_set_int(s, "color_mode", 3);
            obs_data_set_int(s, "trail_height", 100);
            obs_data_set_int(s, "glow", 60);
            obs_data_set_int(s, "sparkles", 50);
            obs_data_set_int(s, "sparkle_tilt", 0);
            obs_source_update(keyboard, s);
            obs_data_release(s);
            play({ { 0, 0x90, 36, 110, 0, 0 }, { 0, 0x90, 88, 110, 0, 0 } }); // C2 and E6 held
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            px = renderSource(keyboard, w, h);
            bool rainbow = !px.empty() && h == 246;
            if (rainbow) {
                save(px, w, h, "keyboard-rainbow.bmp");
                const auto low = at(9 * 20 + 10, keysTop + 90), high = at(39 * 20 + 10, keysTop + 90);
                say("        rainbow: C2 " + show(low) + "; E6 " + show(high));
                rainbow = low[0] > low[2] + 60 && high[2] > high[0] + 60;
            }
            obs_properties_t* props = obs_source_properties(keyboard);
            for (const char* name : { "color_mode", "hue_left", "hue_right", "hue_white", "hue_black", "hue", "saturation", "glow", "sparkles",
                                     "sparkle_height", "sparkle_spread", "sparkle_tilt", "velocity_accent", "velocity_top", "velocity_measure",
                                     "chords", "chord_notes", "chord_threshold", "chord_intensity", "chord_fireworks" })
                rainbow = rainbow && props != nullptr && obs_properties_get(props, name) != nullptr;
            obs_properties_destroy(props);
            check(rainbow, "the rainbow along the keyboard (the bass warm, the treble cool); hues, saturation, lights and sparkles in the properties");
        }
        {
            const std::lock_guard<std::mutex> guard(logLock);
            const auto at = logText.find("device_draw");
            check(at == std::string::npos && logText.find("pianoforge-keyboard.effect") == std::string::npos,
                  at == std::string::npos ? std::string("the keyboard drawn without a word from Direct3D or about its shader")
                                          : "Direct3D: " + logText.substr(at, logText.find('\n', at) - at));
        }
    } else {
        check(false, "the keyboard source exists and renders");
    }

    // The text: the template filled, wider for a longer text.
    if (video && haveText && textSource != nullptr) {
        const std::uint32_t narrow = obs_source_get_width(textSource);
        std::snprintf(info.file, sizeof(info.file), "Prelude and fugue in C sharp minor");
        feed.setInfo(info);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const std::uint32_t wide = obs_source_get_width(textSource);
        check(narrow > 50 && wide > narrow + 100, "the text: the template filled with the application's state (" + std::to_string(narrow)
                                                      + " px), wider for a longer file name (" + std::to_string(wide) + " px)");
    } else {
        check(false, "the text source draws with OBS's text source");
    }

    // A mark while OBS does not record: no chapter, and the log says so.
    feed.addMark("Take test-mark");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    {
        const std::lock_guard<std::mutex> guard(logLock);
        check(logText.find("mark \"Take test-mark\": OBS is not recording") != std::string::npos,
              "a mark while OBS does not record: no chapter, said in the log");
    }

    producing = false;
    producer.join();
    obs_source_release(keyboard);
    obs_source_release(textSource);
    obs_source_release(source);
    feed.close();

    // What libobs passed on in the first 40 s.
    const std::lock_guard<std::mutex> guard(capture.lock);
    const auto& x = capture.left;
    const double received = (double) x.size() / 48000.0;
    say("        received " + std::to_string(received) + " s of audio at 48 kHz in " + std::to_string(capture.blocks.size())
        + " blocks, for " + std::to_string(seconds / clock) + " s produced");
    double worstJumpMs = 0.0;
    for (std::size_t i = 1; i < capture.blocks.size(); ++i) {
        const double expected = (double) capture.blocks[i - 1].first + 1e9 * capture.blocks[i - 1].second / 48000.0;
        worstJumpMs = std::fmax(worstJumpMs, std::fabs((double) capture.blocks[i].first - expected) * 1e-6);
    }
    // The sine over the last 20 s: frequency (zero crossings over 1 s windows) and the residual after a fit.
    double worstResidual = -300.0, lowF = 1e9, highF = 0.0;
    const int window = 48000;
    for (std::size_t s = x.size() > (std::size_t) (21 * window) ? x.size() - (std::size_t) (20 * window) : x.size();
         s + (std::size_t) window <= x.size(); s += (std::size_t) window) {
        double firstCrossing = -1.0, last = -1.0;
        int crossings = 0;
        for (int k = 1; k < window; ++k) {
            const float a = x[s + (std::size_t) k - 1], b = x[s + (std::size_t) k];
            if (a < 0.0f && b >= 0.0f) {
                const double t = (k - 1) + (double) (-a / (b - a));
                if (firstCrossing < 0.0)
                    firstCrossing = t;
                last = t;
                ++crossings;
            }
        }
        const double f = crossings > 1 ? (crossings - 1) * 48000.0 / (last - firstCrossing) : 0.0;
        lowF = std::fmin(lowF, f);
        highF = std::fmax(highF, f);
        // Least-squares fits at that frequency over 20 ms, the rest against the fit.
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
    int glitches = 0;
    {
        const double c2 = 2.0 * std::cos(6.283185307179586 * 1000.0 / 48000.0);
        for (std::size_t k = x.size() > (std::size_t) (21 * window) ? x.size() - (std::size_t) (20 * window) : 2; k < x.size(); ++k)
            glitches += std::fabs(x[k] - (c2 * x[k - 1] - x[k - 2])) > 0.01 ? 1 : 0;
    }
    const double expectedF = 1000.0 * clock; // the producer's clock runs fast: its 1 kHz is 1000.2 Hz on OBS's
    const double level = rms(x, x.size() - 48000, x.size()) * std::sqrt(2.0);
    check(received > seconds / clock - 1.0, "the audio arrives (" + std::to_string(received) + " s)");
    check(std::fabs(level - 0.25) < 0.005, "the speakers mix by default, independent of the application's volume (amplitude "
                                               + std::to_string(level) + ")");
    check(worstJumpMs < 1.0, "timestamps continuous: largest jump " + std::to_string(worstJumpMs) + " ms");
    check(lowF > expectedF - 0.05 && highF < expectedF + 0.05,
          "frequency " + std::to_string(lowF) + " to " + std::to_string(highF) + " Hz on OBS's clock (expected "
              + std::to_string(expectedF) + ")");
    check(worstResidual < -55.0 && glitches == 0, "clean: residual after a sine fit " + std::to_string(worstResidual)
                                                     + " dB, " + std::to_string(glitches) + " glitches over the last 20 s");
    {   // The plugin's log, for the record.
        const std::lock_guard<std::mutex> guard(logLock);
        std::size_t at = 0;
        while (at < logText.size()) {
            const std::size_t end = std::min(logText.find('\n', at), logText.size());
            const std::string line = logText.substr(at, end - at);
            if (line.find("[pianoforge]") != std::string::npos)
                say("        log: " + line);
            at = end + 1;
        }
    }
    obs_shutdown();
    say(failures == 0 ? "All OBS plugin tests passed" : std::to_string(failures) + " failure(s)");
    if (report != nullptr)
        std::fclose(report);
    return failures == 0 ? 0 : 1;
}
