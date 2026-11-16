// The plugin inside the installed libobs, without the OBS application: libobs started for audio (48 kHz) and video
// (OBS's Direct3D 11 module on Windows, its OpenGL one on Linux, offscreen), the plugin loaded as OBS loads it (its
// locale too), OBS's text source beside it, and the feed written as the application writes it: three mixes of a 1 kHz sine at 44.1 kHz in blocks of 128, on
// a clock 200 ppm fast, the application's volume at -6 dB, and MIDI events (middle C held, the sustain pedal down).
//   - the audio source: the speakers mix by default, at its level whatever the application's volume; continuous, with
//     timestamps that follow the frames, at 1 kHz on OBS's clock, clean; its status (latency, key to OBS); a new
//     latency, the resync button; another mix with the volume followed, reached through fades (no click);
//   - the readers the application sees (sound, keyboard, text, and the mix taken);
//   - the keyboard: rendered, C2 lit blue (left hand), middle C orange, D not, a note rising, the sustain pedal; a
//     phrase played (its images kept), sparkles thrown up by strikes (leaning as asked), none when off; the velocity
//     (a soft note thinner and duller than a strong one); a powerful chord (the room lit, fireworks; nothing for a
//     soft chord, nor with them off);
//   - the stream look shared with PianoForge's window (the test on the application's side of the channel): OBS's
//     look reaches it, a newer one from it reaches the keyboard source, an edit in OBS comes back; the preview's
//     frames while it asks for them;
//   - the text: its template filled with the application's state, wider for a longer text;
//   - the chapters: a mark while OBS does not record is said so in the log.
// The feed has a name of its own (PIANOFORGE_AUDIO_FEED): the test runs beside a PianoForge and an OBS in use.
//
//   pianoforge-obs-test <plugin dll> <plugin data> <report file> <OBS folder> [feed rate, default 44100]
//   Windows: OBS's bin/64bit on the PATH. Linux: the OBS folder is its prefix (/usr, or /app inside a Flatpak OBS:
//   flatpak run --devel --device=shm --command=... com.obsproject.Studio); OpenGL through the Wayland display when
//   there is one, else X11's.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <obs-nix-platform.h>
#endif

#include <obs-module.h>
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/base.h>

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/look_channel.hpp>
#include <pianoforge/stream_look.hpp>

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
#if defined(_WIN32)
    // libobs finds its effects from OBS's bin/64bit (../../data/libobs), and loads its modules (the graphics, the
    // plugin's frontend API) with a restricted search: OBS's folder added to it, as OBS's own executable there has it.
    std::filesystem::current_path(obsDir / "bin" / "64bit");
    AddDllDirectory((obsDir / "bin" / "64bit").wstring().c_str());
    const char* graphics = "Direct3D 11";
#else
    // OpenGL needs the display OBS would draw on (as OBS's own main sets it): Wayland's when there is one, else X11's
    // (EGL). Their client libraries are loaded here: the test links nothing more than libobs.
    const char* graphics = "OpenGL";
    void* display = nullptr;
    if (std::getenv("WAYLAND_DISPLAY") != nullptr)
        if (void* wayland = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL))
            if (auto connect = reinterpret_cast<void* (*)(const char*)>(dlsym(wayland, "wl_display_connect")))
                if ((display = connect(nullptr)) != nullptr) {
                    obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
                    graphics = "OpenGL, Wayland";
                }
    if (display == nullptr)
        if (void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL))
            if (auto open = reinterpret_cast<void* (*)(const char*)>(dlsym(x11, "XOpenDisplay")))
                if ((display = open(nullptr)) != nullptr) {
                    obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
                    graphics = "OpenGL, X11";
                }
    if (display != nullptr)
        obs_set_nix_platform_display(display);
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
#if defined(_WIN32)
    videoInfo.graphics_module = "libobs-d3d11";
#else
    videoInfo.graphics_module = "libobs-opengl";
#endif
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
    std::string adapter; // the GPU it draws on, as libobs says it ("Loading up OpenGL on adapter ..."): the timings are its
    {
        const std::lock_guard<std::mutex> guard(logLock);
        if (const auto at = logText.find(" on adapter "); at != std::string::npos)
            adapter = ": " + logText.substr(at + 12, logText.find('\n', at) - at - 12);
    }
    check(video, "libobs video started (" + std::string(graphics) + ", offscreen" + adapter + ")");
    obs_module_t* module = nullptr;
    const int opened = obs_open_module(&module, pluginPath.c_str(), dataPath.c_str());
    if (opened != MODULE_SUCCESS || !obs_init_module(module)) {
        const std::lock_guard<std::mutex> guard(logLock);
        say("[FAIL]  the plugin did not load (" + std::to_string(opened) + ")\n" + logText);
        return 1;
    }
    say("[PASS]  the plugin loads into libobs " + std::string(obs_get_version_string()));
    obs_module_t* textModule = nullptr;
#if defined(_WIN32)
    const auto textDll = (obsDir / "core" / "obs-text" / "obs-text.dll").string();
    const auto textData = (obsDir / "core" / "obs-text" / "data").string();
#else // FreeType 2's text source: OBS 33's layout (obs-modules/core), or the older one (obs-plugins)
    auto textDll = (obsDir / "lib" / "obs-modules" / "core" / "text-freetype2.so").string();
    auto textData = (obsDir / "share" / "obs" / "obs-modules" / "core" / "text-freetype2").string();
    if (!std::filesystem::exists(textDll))
        for (const char* lib : { "lib/obs-plugins", "lib/x86_64-linux-gnu/obs-plugins", "lib64/obs-plugins" })
            if (std::filesystem::exists(obsDir / lib / "text-freetype2.so")) {
                textDll = (obsDir / lib / "text-freetype2.so").string();
                textData = (obsDir / "share" / "obs" / "obs-plugins" / "text-freetype2").string();
                break;
            }
#endif
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
        // (the band over them lacquered by default: 22 px)
        const int keysTop = 122, bottom = 245;
        bool ok = !px.empty() && w == 1040 && h == 100 + 22 + 123 + 16;
        if (ok) {
            save(px, w, h, "keyboard.bmp");
            const int y = keysTop + (int) (0.75 * (bottom - keysTop));
            const auto c2 = at(9 * 20 + 10, y), c4 = at(23 * 20 + 10, y), g4 = at(27 * 20 + 10, y); // (G: away from the bloom)
            const auto pedal = at(1040 - 20, bottom + 8), note = at(23 * 20 + 10, 60);
            say("        C2 " + show(c2) + "; middle C " + show(c4) + "; G " + show(g4) + "; pedal " + show(pedal) + "; note " + show(note));
            ok = c2[2] > c2[0] + 40 && c4[0] > c4[2] + 60 && c4[0] > 150 && g4[0] > 200 && g4[1] > 200 && g4[2] > 200
                 && std::abs(g4[0] - g4[2]) < 15 && pedal[0] > pedal[2] + 40 && note[0] > note[2] + 40;
        }
        check(ok, "the keyboard by hand: C2 to the left (blue), middle C to the right (orange), G not lit, its note rising, the sustain pedal");

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
                obs_data_set_int(s, "bloom", 0);
                obs_data_set_int(s, "reflection", 0);
                obs_data_set_int(s, "ambiance", 0);
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
            look({ { "color_mode", 0 }, { "trail_height", 100 }, { "glow", 60 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 0 },
                   { "ambiance", 0 } },
                 { { "chords", false }, { "melody", false } });
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
                look({ { "color_mode", 2 }, { "trail_height", 0 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 0 },
                       { "ambiance", 0 } },
                     { { "chords", on } });
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
            look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 60 }, { "sparkles", 50 }, { "bloom", 40 }, { "reflection", 50 },
                   { "ambiance", 35 } },
                 { { "chords", true }, { "melody", true } });
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

        // The notes held by the pedal: a tail under the bar while it holds them, ended when it is up; none without them.
        {
            auto quiet = [&](bool tails) {
                look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 0 },
                       { "ambiance", 0 }, { "velocity_accent", 0 }, { "pedal_line", 0 }, { "pedal_glow", 0 } },
                     { { "chords", false }, { "melody", false }, { "pedal_tails", tails }, { "note_decay", false } });
            };
            int widthTop = 0, widthEnd = 99, fadeTop = 0, fadeEnd = 255;
            auto alpha = [&](int x, int y) { return (int) px[((std::size_t) y * w + (std::size_t) x) * 4 + 3]; };
            auto across = [&](int y) { // how many pixels wide the tail is at row y (around E4's centre)
                int n = 0;
                for (int x = 500; x < 521 && !px.empty(); ++x)
                    n += alpha(x, y) > 40 ? 1 : 0;
                return n;
            };
            int held = 0, bar = 0, without = 0, after = 0;
            for (bool tails : { true, false }) {
                quiet(tails);
                play({ { 0, 0xB0, 64, 127, 0, 0 }, noteOn(64, 100) }); // E4, the pedal down
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                play({ noteOff(64) });
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                px = renderSource(keyboard, w, h);
                if (!px.empty() && tails) {
                    held = alpha(510, 96); // under the bar: the tail
                    bar = alpha(510, 84);
                    save(px, w, h, "keyboard-pedal.bmp");
                } else if (!px.empty()) {
                    without = alpha(510, 96);
                }
                play({ { 0, 0xB0, 64, 0, 0, 0 } });
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                if (tails) {
                    px = renderSource(keyboard, w, h);
                    after = px.empty() ? 255 : alpha(510, 97); // the tail ended where the pedal went up, a few pixels higher
                    widthTop = across(79);                     // just under the bar
                    widthEnd = across(88);                     // near its end: drawn in, faded
                    fadeTop = px.empty() ? 0 : alpha(510, 80);
                    fadeEnd = px.empty() ? 255 : alpha(510, 89);
                    if (!px.empty())
                        save(px, w, h, "keyboard-pedal-up.bmp");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            check(held > 60 && bar > 100 && without < 30 && after < 30,
                  "a note the pedal holds: its tail under the bar (" + std::to_string(held) + "), none without the option ("
                      + std::to_string(without) + "), ended once the pedal is up (" + std::to_string(after) + ")");
            check(widthTop >= 3 && widthEnd < widthTop && fadeEnd * 2 < fadeTop,
                  "the tail's end once the pedal is up, drawn in and faded: " + std::to_string(widthTop) + " px wide under its note ("
                      + std::to_string(fadeTop) + "), " + std::to_string(widthEnd) + " near its end (" + std::to_string(fadeEnd) + ")");
        }

        // The sustain pedal's line along the keyboard's top and its halo above it, each its own.
        {
            auto pedalLight = [&](int line, int halo) {
                look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 0 },
                       { "ambiance", 0 }, { "pedal_line", line }, { "pedal_glow", halo } },
                     { { "chords", false }, { "melody", false } });
                play({ { 0, 0xB0, 64, 127, 0, 0 } });
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                px = renderSource(keyboard, w, h);
                std::pair<std::array<int, 3>, std::array<int, 3>> out {}; // on the line, 10 px above it
                if (!px.empty())
                    out = { at(300, 100), at(300, 90) };
                play({ { 0, 0xB0, 64, 0, 0, 0 } });
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return out;
            };
            const auto none = pedalLight(0, 0), line = pedalLight(100, 0), halo = pedalLight(0, 100);
            check(std::abs(none.first[0] - none.first[2]) < 20 && line.first[0] > line.first[2] + 40
                      && line.second[0] - line.second[2] < 8 && halo.second[0] > halo.second[2] + 15,
                  "the pedal's line (" + show(line.first) + ", nothing above it: " + show(line.second) + ") and its halo above it ("
                      + show(halo.second) + "), each its own; neither at 0 (" + show(none.first) + ")");
        }

        // The melody: a line of light between the right hand's notes; none without it.
        {
            auto melodyAt = [&](bool on) {
                look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 0 },
                       { "ambiance", 0 }, { "velocity_accent", 0 } },
                     { { "chords", false }, { "melody", on }, { "pedal_tails", false } });
                for (int key : { 72, 76, 79, 84 }) {
                    play({ noteOn(key, 100) });
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                px = renderSource(keyboard, w, h);
                int brightest = 0; // between C5's and E5's notes (x 610 and 650), around their tops
                for (int y = 70; y < 96 && !px.empty(); ++y) {
                    const auto c = at(630, y);
                    brightest = std::max(brightest, c[0] + c[1] + c[2]);
                }
                if (on && !px.empty())
                    save(px, w, h, "keyboard-melody.bmp");
                play({ noteOff(72), noteOff(76), noteOff(79), noteOff(84) });
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return brightest;
            };
            const int on = melodyAt(true), off = melodyAt(false);
            check(on > 300 && off < 60, "the melody: a line of light between the notes (" + std::to_string(on) + "), none without it ("
                                            + std::to_string(off) + ")");
        }

        // The keys shown: 61 keys (C2 at the left edge, the keys wider, the keyboard taller); following the playing.
        {
            look({ { "range", 61 }, { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 },
                   { "reflection", 0 }, { "ambiance", 0 } },
                 { { "chords", false }, { "melody", false } });
            play({ noteOn(36, 100) });
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            px = renderSource(keyboard, w, h);
            // 36 white keys over 1040 px: the band 188 high (a rail of 9, the keys 179), the pedals 23.
            const bool sized = w == 1040 && h == 100 + 9 + 179 + 23;
            const auto c2 = px.empty() || !sized ? std::array<int, 3> {} : at(14, (int) h - 23 - 30);
            if (!px.empty())
                save(px, w, h, "keyboard-61.bmp");
            play({ noteOff(36) });
            look({ { "range", -1 }, { "follow_octaves", 3 } }, {});
            for (int key : { 84, 88, 91, 96 }) {
                play({ noteOn(key, 100) });
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            px = renderSource(keyboard, w, h);
            int longest = 0, run = 0; // the widest key lit (orange), along the keys' lower part
            for (int x = 0; x < (int) w && !px.empty(); ++x) {
                const auto c = at(x, (int) h - 23 - 20);
                run = c[0] > 180 && c[0] > c[2] + 60 ? run + 1 : 0;
                longest = std::max(longest, run);
            }
            if (!px.empty())
                save(px, w, h, "keyboard-follow.bmp");
            play({ noteOff(84), noteOff(88), noteOff(91), noteOff(96) });
            look({ { "range", 88 } }, {});
            check(sized && c2[2] > c2[0] + 30 && longest > 22, // (wider: the view narrows to the keys played lately)
                  "61 keys: C2 at the left edge (" + show(c2) + "), the keyboard " + std::to_string(h) + " px high; following the "
                      + "playing: keys " + std::to_string(longest) + " px wide (20 on 88 keys)");
        }

        // The bloom: the light around a lit key and its note grows; the white keys away from it do not change.
        {
            auto bloomAt = [&](int bloom) {
                look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 60 }, { "sparkles", 0 }, { "bloom", bloom }, { "reflection", 0 },
                       { "ambiance", 0 } },
                     { { "chords", false }, { "melody", false } });
                play({ noteOn(64, 110) });
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                px = renderSource(keyboard, w, h);
                std::pair<long long, long long> out { 0, 0 };
                for (int y = 60; y < 96 && !px.empty(); ++y)
                    for (int x : { 486, 490, 494, 526, 530, 534 }) {
                        const auto c = at(x, y);
                        out.first += c[0] + c[1] + c[2];
                    }
                for (int y = 167; y < 177 && !px.empty(); ++y)
                    for (int x = 202; x < 218; ++x) {
                        const auto c = at(x, y);
                        out.second += c[0] + c[1] + c[2];
                    }
                if (bloom > 0 && !px.empty())
                    save(px, w, h, "keyboard-bloom.bmp");
                play({ noteOff(64) });
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return out;
            };
            const auto off = bloomAt(0), on = bloomAt(100);
            check(on.first > off.first * 13 / 10 + 2000 && std::llabs(on.second - off.second) * 100 < off.second * 3,
                  "the bloom: the light around a lit note " + std::to_string(on.first) + " (" + std::to_string(off.first)
                      + " without), the white keys away " + std::to_string(on.second) + " (" + std::to_string(off.second) + ")");
        }

        // The reflection: under a note, the lacquered band takes its hue; elsewhere it stays dark.
        {
            look({ { "color_mode", 2 }, { "trail_height", 100 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 100 },
                   { "ambiance", 0 } },
                 { { "chords", false }, { "melody", false }, { "pedal_tails", false }, { "note_decay", false } });
            play({ noteOn(64, 100) });
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            px = renderSource(keyboard, w, h);
            const bool sized = h == 100 + 22 + 123 + 16;
            const auto under = px.empty() || !sized ? std::array<int, 3> {} : at(510, 104), away = px.empty() || !sized ? std::array<int, 3> {} : at(300, 104);
            if (!px.empty())
                save(px, w, h, "keyboard-reflection.bmp");
            play({ noteOff(64) });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const int hi = std::max({ under[0], under[1], under[2] }), lo = std::min({ under[0], under[1], under[2] });
            check(sized && hi > std::max({ away[0], away[1], away[2] }) + 40 && hi - lo > 40,
                  "the reflection: under the note, the lacquer in its hue (" + show(under) + "), elsewhere dark (" + show(away) + ")");
        }

        // The ambiance: curtains of light over the room while the sound plays (the test's sine); no room for them at 0.
        {
            look({ { "trail_height", 0 }, { "glow", 0 }, { "sparkles", 0 }, { "bloom", 0 }, { "reflection", 0 }, { "ambiance", 100 } },
                 { { "chords", false }, { "melody", false } });
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            px = renderSource(keyboard, w, h);
            long long light = 0;
            for (int y = 0; y + 146 < (int) h && !px.empty(); ++y)
                for (int x = 0; x < (int) w; x += 2) {
                    const auto c = at(x, y);
                    light += c[0] + c[1] + c[2];
                }
            if (!px.empty())
                save(px, w, h, "keyboard-ambiance.bmp");
            auto upper = [&] { // the light in the room's upper half
                long long sum = 0;
                for (int y = 0; y < 39 && !px.empty(); ++y)
                    for (int x = 0; x < (int) w; x += 2) {
                        const auto c = at(x, y);
                        sum += c[0] + c[1] + c[2];
                    }
                return sum;
            };
            look({ { "aurora_height", 100 } }, {});
            px = renderSource(keyboard, w, h);
            const long long high = upper();
            look({ { "aurora_height", 30 } }, {});
            px = renderSource(keyboard, w, h);
            const long long low = upper();
            look({ { "ambiance", 0 }, { "aurora_height", 80 } }, {});
            const std::uint32_t without = obs_source_get_height(keyboard);
            check(high > 2 * low + 1000, "the aurora's height: the room's upper half " + std::to_string(high) + " at 100 %, "
                                              + std::to_string(low) + " at 30 %");
            check(light > 100000 && h == 78 + 146 && without == 146,
                  "the ambiance: light over the room as the sound plays (" + std::to_string(light) + "); at 0, no room above the keys ("
                      + std::to_string(without) + " px high)");
        }

        // The styles: one chosen sets the look, once (the properties opened again do not apply it over what changed
        // since); back to the defaults keeps the width.
        {
            obs_data_t* settings = obs_source_get_settings(keyboard);
            obs_properties_t* props = obs_source_properties(keyboard);
            obs_property_t* style = obs_properties_get(props, "style");
            obs_data_set_int(settings, "style", 6); // minimal
            obs_property_modified(style, settings);
            const bool minimal = obs_data_get_int(settings, "sparkles") == 0 && obs_data_get_int(settings, "bloom") == 0
                                 && !obs_data_get_bool(settings, "chords") && obs_data_get_int(settings, "color_mode") == 0;
            obs_data_set_int(settings, "sparkles", 33);
            obs_property_modified(style, settings); // as the properties open again
            const bool kept = obs_data_get_int(settings, "sparkles") == 33;
            obs_data_set_int(settings, "style", 3); // neon
            obs_property_modified(style, settings);
            const bool neon = obs_data_get_int(settings, "hue_right") == 320 && obs_data_get_int(settings, "bloom") == 80
                              && obs_data_get_bool(settings, "chords") && obs_data_get_int(settings, "sparkles") == 70;
            obs_source_update(keyboard, settings);
            obs_property_button_clicked(obs_properties_get(props, "reset"), keyboard);
            const bool reset = obs_data_get_int(settings, "sparkles") == 50 && obs_data_get_int(settings, "bloom") == 40
                               && obs_data_get_int(settings, "width") == 1040 && obs_data_get_int(settings, "style") == 0;
            obs_properties_destroy(props);
            obs_data_release(settings);
            check(minimal && kept && neon && reset, "styles: minimal, then neon, each applied once; back to the defaults, the width kept");
        }

        // Images of the look by default (the notes 240 px high): a phrase, the pedal down, its melody, a chord.
        {
            play({ { 0, 0xB0, 64, 127, 0, 0 } });
            const int phrase[10] = { 48, 55, 60, 64, 67, 72, 76, 79, 77, 76 };
            for (int i = 0; i < 10; ++i) {
                play({ noteOn(phrase[i], 60 + 6 * i) });
                std::this_thread::sleep_for(std::chrono::milliseconds(160));
                if (i < 7)
                    play({ noteOff(phrase[i]) });
            }
            play({ noteOn(36, 118), noteOn(43, 116), noteOn(48, 117), noteOn(72, 120), noteOn(76, 119), noteOn(79, 121) });
            const char* lookFrames[3] = { "keyboard-look-1.bmp", "keyboard-look-2.bmp", "keyboard-look-3.bmp" };
            for (int i = 0; i < 3; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(i == 0 ? 120 : 350));
                px = renderSource(keyboard, w, h);
                if (!px.empty())
                    save(px, w, h, lookFrames[i]);
            }
            std::vector<pf::FeedEvent> up;
            for (int key = 21; key <= 108; ++key)
                up.push_back(noteOff(key));
            up.push_back({ 0, 0xB0, 64, 0, 0, 0 });
            const std::lock_guard<std::mutex> guard(playLock);
            played.insert(played.end(), up.begin(), up.end());
        }

        {   // The rainbow: the bass warm (C2 toward orange), the treble cool (E6 toward blue); the hues in the properties.
            obs_data_t* s = obs_data_create();
            obs_data_set_int(s, "color_mode", 3);
            obs_data_set_int(s, "trail_height", 100);
            obs_data_set_int(s, "glow", 60);
            obs_data_set_int(s, "sparkles", 50);
            obs_data_set_int(s, "sparkle_tilt", 0);
            obs_data_set_int(s, "reflection", 0);
            obs_data_set_int(s, "bloom", 0);
            obs_data_set_int(s, "ambiance", 0);
            obs_source_update(keyboard, s);
            obs_data_release(s);
            play({ { 0, 0x90, 36, 110, 0, 0 }, { 0, 0x90, 88, 110, 0, 0 } }); // C2 and E6 held
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            px = renderSource(keyboard, w, h);
            bool rainbow = !px.empty() && h == 246;
            if (rainbow) {
                save(px, w, h, "keyboard-rainbow.bmp");
                const auto low = at(9 * 20 + 10, 107 + 90), high = at(39 * 20 + 10, 107 + 90); // (a thin rail: the keys from 107)
                say("        rainbow: C2 " + show(low) + "; E6 " + show(high));
                rainbow = low[0] > low[2] + 60 && high[2] > high[0] + 60;
            }
            obs_properties_t* props = obs_source_properties(keyboard);
            for (const char* name : { "color_mode", "hue_left", "hue_right", "hue_white", "hue_black", "hue", "saturation", "glow", "sparkles",
                                     "sparkle_height", "sparkle_spread", "sparkle_tilt", "velocity_accent", "velocity_top", "velocity_measure",
                                     "chords", "chord_notes", "chord_threshold", "chord_intensity", "chord_fireworks", "style", "reset", "range",
                                     "range_first", "follow_octaves", "pedal_tails", "tail_width", "tail_fine", "pedal_line", "pedal_glow", "aurora_height", "aurora_speed", "aurora_rest", "note_decay", "melody", "bloom", "reflection",
                                     "ambiance" })
                rainbow = rainbow && props != nullptr && obs_properties_get(props, name) != nullptr;
            obs_properties_destroy(props);
            check(rainbow, "the rainbow along the keyboard (the bass warm, the treble cool); hues, saturation, lights and sparkles in the properties");
        }
        // The stream look, from PianoForge's side of the channel: OBS's look arrives; a newer one sent from here reaches
        // the keyboard source; an edit in OBS comes back; the preview's frames, while asked for.
        {
            pf::LookChannel app;
            const bool opened = app.open(pf::LookChannel::Side::app);
            std::uint64_t serial = 0, changed = 0;
            std::string text;
            auto wait = [&](auto done) { // (done() asked once a turn: a frame read is not read again)
                for (int i = 0; i < 40; ++i) {
                    if (done())
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                return false;
            };
            // (the source's settings changed many times above: the newest is there)
            const bool arrived = opened && wait([&] {
                                     while (app.other(serial, text, changed)) {
                                     }
                                     return text.find("width=1040\n") != std::string::npos;
                                 });
            pf::look::Values look;
            look.read(text);
            look.set("sparkles", 77);
            look.set("hue_right", 140);
            app.publish(look.text(), changed + 1000);
            auto setting = [&](const char* key) {
                obs_data_t* settings = obs_source_get_settings(keyboard);
                const long long value = obs_data_get_int(settings, key);
                obs_data_release(settings);
                return value;
            };
            const bool applied = wait([&] { return setting("sparkles") == 77 && setting("hue_right") == 140; });
            const std::uint64_t before = changed;
            obs_data_t* edit = obs_data_create();
            obs_data_set_int(edit, "bloom", 13);
            obs_source_update(keyboard, edit);
            obs_data_release(edit);
            const bool back = wait([&] {
                while (app.other(serial, text, changed)) {
                }
                return text.find("bloom=13\n") != std::string::npos && text.find("sparkles=77\n") != std::string::npos;
            });
            check(arrived && applied && back && changed > before + 1000,
                  "the stream look: OBS's reaches PianoForge, a newer one from PianoForge reaches the keyboard, an edit in OBS comes back");

            app.wantPreview(pf::feedClockNs() + 3'000'000'000ull, 520, 400);
            std::uint64_t frameSerial = 0, made = 0;
            std::vector<std::uint8_t> frame;
            std::uint32_t fw = 0, fh = 0;
            const bool framed = wait([&] { return app.frame(frameSerial, frame, fw, fh, made); });
            const std::uint32_t sw = obs_source_get_width(keyboard), sh = obs_source_get_height(keyboard);
            int lit = 0;
            for (std::size_t i = 0; i + 3 < frame.size(); i += 4)
                lit += frame[i + 3] > 128 ? 1 : 0;
            const std::uint64_t first = frameSerial;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            app.frame(frameSerial, frame, fw, fh, made);
            const bool flowing = frameSerial > first + 3;
            if (framed && fw > 0 && fh > 0)
                save(frame, fw, fh, "keyboard-preview.bmp");
            app.wantPreview(0, 520, 400);
            check(framed && flowing && fw == 520 && std::abs((double) fh / fw - (double) sh / sw) < 0.02 && lit > (int) (fw * fh / 4),
                  "the preview: frames while asked for, " + std::to_string(fw) + " by " + std::to_string(fh) + " (the keyboard's "
                      + std::to_string(sw) + " by " + std::to_string(sh) + " scaled), the keys in them");
        }
        {
            const std::lock_guard<std::mutex> guard(logLock);
            const auto at = logText.find("device_draw");
            check(at == std::string::npos && logText.find("pianoforge-keyboard.effect") == std::string::npos,
                  at == std::string::npos ? "the keyboard drawn without a word from " + std::string(graphics) + " or about its shader"
                                          : std::string(graphics) + ": " + logText.substr(at, logText.find('\n', at) - at));
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
