// The stream look (stream_look.hpp: the settings, their text, the styles) and its channel between PianoForge and the
// OBS plugin (look_channel.hpp: both looks, the preview's request and frames), the two sides here in one process.
#include <pianoforge/audio_feed.hpp>
#include <pianoforge/look_channel.hpp>
#include <pianoforge/stream_look.hpp>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

} // namespace

int main()
{
#if defined(_WIN32)
    _putenv_s("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedLookTest");
#else
    setenv("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedLookTest", 1);
#endif
    using namespace pf::look;

    // The table: keys once each, defaults valid, conditions on keys that exist.
    {
        std::set<std::string> keys;
        bool sound = true;
        for (const auto& e : entries()) {
            sound = sound && keys.insert(e.key).second && valid(e, e.def) == e.def;
            for (const auto& c : e.when)
                sound = sound && (c.key == nullptr || indexOf(c.key) >= 0);
        }
        check(sound && entries().size() >= 50, "the settings: each key once, defaults valid, conditions on settings that exist");
    }

    // Values: made valid; their text, read back the same.
    {
        Values v;
        v.set("sparkles", 150);
        v.set("trail_seconds", 2.74);
        v.set("range", 50);  // not a choice
        v.set("pedals", 0.2);
        v.set("hue_left", 12.6);
        const bool valid = v.get("sparkles") == 100 && v.get("trail_seconds") == 2.5 && v.get("range") == 88 && v.get("pedals") == 0
                           && v.get("hue_left") == 13;
        Values back;
        back.read(v.text());
        Values partial;
        partial.read("bloom=12\nunknown=4\nnot a line\ntrail_seconds=7.5\n");
        check(valid && back == v && partial.get("bloom") == 12 && partial.get("trail_seconds") == 7.5 && partial.get("glow") == 60,
              "values made valid (range, step, choices); their text read back the same; unknown keys left out");
        check(v.text().find("trail_seconds=2.5\n") != std::string::npos && v.text().find("width=1280\n") != std::string::npos,
              "the text: key=value lines, whole numbers plain");
    }

    // When settings show: the hues by the colour mode, the split only when the hands are not followed.
    {
        Values v;
        const auto& e = entries();
        auto shows = [&](const char* key) { return v.shows(e[(std::size_t) indexOf(key)]); };
        const bool hands = shows("hue_left") && !shows("hue") && !shows("hand_split");
        v.set("hand_adaptive", 0);
        const bool split = shows("hand_split");
        v.set("color_mode", 0);
        check(hands && split && shows("hue") && !shows("hue_left") && !shows("hand_split"), "settings shown by the others (hues, split)");
    }

    // Styles: the look reset then set; the size and the keys shown kept.
    {
        Values v;
        v.set("width", 1600);
        v.set("range", 61);
        v.set("sparkles", 33);
        applyStyle(v, kMinimal);
        const bool minimal = v.get("sparkles") == 0 && v.get("chords") == 0 && v.get("color_mode") == 0 && v.get("width") == 1600
                             && v.get("range") == 61;
        applyStyle(v, kNeon);
        const bool neon = v.get("hue_right") == 320 && v.get("chords") == 1 && v.get("sparkles") == 70 && v.get("width") == 1600;
        applyStyle(v, kPianoForge);
        check(minimal && neon && v.get("sparkles") == 50 && v.get("bloom") == 40, "styles: minimal, neon, then the defaults; the size kept");
    }

    // The channel: each side's look reaches the other, once; the preview's request and frames.
    {
        pf::LookChannel app, obs;
        const bool opened = app.open(pf::LookChannel::Side::app) && obs.open(pf::LookChannel::Side::obs);
        std::uint64_t serialApp = 0, serialObs = 0, changed = 0;
        std::string text;
        const bool nothing = !app.other(serialApp, text, changed) && !obs.other(serialObs, text, changed);
        app.publish("bloom=10\n", 1000);
        const bool reached = obs.other(serialObs, text, changed) && text == "bloom=10\n" && changed == 1000;
        const bool once = !obs.other(serialObs, text, changed) && !app.other(serialApp, text, changed);
        obs.publish("bloom=20\n", 2000);
        const bool back = app.other(serialApp, text, changed) && text == "bloom=20\n" && changed == 2000;
        check(opened && nothing && reached && once && back, "the channel: each side's look reaches the other, once, with its instant");

        std::uint32_t w = 0, h = 0;
        const bool idle = !obs.previewWanted(w, h);
        app.wantPreview(pf::feedClockNs() + 1'000'000'000ull, 640, 400);
        const bool wanted = obs.previewWanted(w, h) && w == 640 && h == 400;
        std::vector<std::uint8_t> rows(3 * 8 * 4 + 3 * 8); // 3 rows of 8 pixels, 8 bytes of padding each
        for (std::size_t i = 0; i < rows.size(); ++i)
            rows[i] = (std::uint8_t) i;
        obs.putFrame(rows.data(), 8 * 4 + 8, 8, 3);
        std::uint64_t frameSerial = 0, made = 0;
        std::vector<std::uint8_t> pixels;
        std::uint32_t fw = 0, fh = 0;
        const bool got = app.frame(frameSerial, pixels, fw, fh, made) && fw == 8 && fh == 3 && pixels.size() == 96 && pixels[0] == 0
                         && pixels[32] == 40 && pixels[95] == 40 + 40 + 31 && made > 0;
        const bool newOnly = !app.frame(frameSerial, pixels, fw, fh, made);
        app.wantPreview(pf::feedClockNs() - 1, 640, 400);
        check(idle && wanted && got && newOnly && !obs.previewWanted(w, h),
              "the preview: asked for until an instant, at most so large; its frames, rows packed, each read once");
    }

    std::printf(failures ? "%d failure(s)\n" : "All stream look tests passed\n", failures);
    return failures ? 1 : 0;
}
