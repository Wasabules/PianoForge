#pragma once

// The look of the "PianoForge keyboard" OBS source (obs/src/keyboard_source.cpp), shared by the plugin and
// PianoForge's "Stream look" window: every setting (its key in OBS's settings, kind, range, default, page, label,
// help, when it shows), the styles, and the plain text the two sides exchange through the look channel
// (look_channel.hpp): "key=value" lines, in this table's order, values written the same way on both sides.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pf::look {

enum class Kind { integer, toggle, choice, real };
enum class Page { keyboard, colours, notes, lights, sparkles, velocity, chords };

inline const char* pageName(Page p) noexcept
{
    switch (p) {
    case Page::keyboard: return "Keyboard";
    case Page::colours: return "Colours";
    case Page::notes: return "Notes";
    case Page::lights: return "Lights";
    case Page::sparkles: return "Sparkles";
    case Page::velocity: return "Velocity";
    case Page::chords: return "Powerful chords";
    }
    return "";
}
constexpr int kPages = 7;

struct Choice {
    int value;
    std::string label;
};

/// Shown when the setting `key` is `value` (or is not, `equal` false); no key: always.
struct Condition {
    const char* key = nullptr;
    int value = 0;
    bool equal = true;
};

struct Entry {
    const char* key;
    Kind kind;
    double min, max, step, def;
    Page page;
    const char* label;
    const char* help;
    const char* suffix;     // after the value ("%", " px"...)
    bool look;              // part of the look a style sets
    std::vector<Choice> choices;
    Condition when[2];
};

namespace detail {

inline std::vector<Choice> keyChoices()
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    std::vector<Choice> keys;
    for (int key = 21; key <= 108; ++key)
        keys.push_back({ key, std::string(names[key % 12]) + std::to_string(key / 12 - 1) + (key == 60 ? " (middle C)" : "") });
    return keys;
}

inline std::vector<Entry> makeEntries()
{
    using K = Kind;
    using P = Page;
    const auto keys = keyChoices();
    std::vector<Entry> e;
    auto add = [&](Entry entry) { e.push_back(std::move(entry)); };
    // The keyboard: its size, the keys shown, the notes above it, the pedals, the timing.
    add({ "width", K::integer, 320, 7680, 1, 1280, P::keyboard, "Width", "The source's width in OBS, in pixels.", " px", false, {}, {} });
    add({ "range", K::choice, -1, 88, 1, 88, P::keyboard, "Keys shown",
          "All 88 keys, a smaller keyboard's, a range of your own, or following the playing (the view glides to the keys played lately).",
          "", false,
          { { 88, "88 keys (A0 to C8)" }, { 76, "76 keys (E1 to G7)" }, { 61, "61 keys (C2 to C7)" }, { 49, "49 keys (C2 to C6)" },
            { 37, "37 keys (C3 to C6)" }, { 0, "From one key to another" }, { -1, "Following the playing" } },
          {} });
    add({ "range_first", K::choice, 21, 108, 1, 36, P::keyboard, "From", "The first key shown.", "", false, keys, { { "range", 0 } } });
    add({ "range_last", K::choice, 21, 108, 1, 96, P::keyboard, "To", "The last key shown.", "", false, keys, { { "range", 0 } } });
    add({ "follow_octaves", K::integer, 1, 7, 1, 3, P::keyboard, "At least (octaves)",
          "Following the playing: the view is never narrower than this.", "", false, {}, { { "range", -1 } } });
    add({ "trail_height", K::integer, 0, 4320, 1, 240, P::keyboard, "Notes above the keys",
          "Each note rises from its key as a rounded bar, as long as it is held, fading toward the top. 0: the keyboard alone.",
          " px", false, {}, {} });
    add({ "trail_seconds", K::real, 0.5, 30, 0.5, 4, P::keyboard, "Time to the top", "How long a note takes to rise to the top.", " s",
          false, {}, {} });
    add({ "pedals", K::toggle, 0, 1, 1, 1, P::keyboard, "The pedals (soft, sostenuto, sustain)", "", "", false, {}, {} });
    add({ "offset_ms", K::real, -1000, 1000, 1, 0, P::keyboard, "Offset",
          "Moves the keys against the sound: later if positive. Zero meets the PianoForge audio source's sound.", " ms", false, {}, {} });
    // The colours.
    add({ "color_mode", K::choice, 0, 3, 1, 2, P::colours, "Colours",
          "Each hand's hue (estimated from the notes), one for the white keys and one for the black, one for all, or the rainbow along "
          "the keyboard. The black keys take a deeper shade of their hue.",
          "", true,
          { { 2, "Left and right hands (estimated)" }, { 1, "White and black keys" }, { 0, "One colour" }, { 3, "Rainbow along the keyboard" } },
          {} });
    const char* hueHelp = "The hue on the colour wheel: 0 red, 30 orange, 60 yellow, 120 green, 180 cyan, 215 blue, 270 violet, 320 pink.";
    add({ "hue_left", K::integer, 0, 360, 1, 217, P::colours, "Left hand (hue)", hueHelp, "\xC2\xB0", true, {}, { { "color_mode", 2 } } });
    add({ "hue_right", K::integer, 0, 360, 1, 25, P::colours, "Right hand (hue)", hueHelp, "\xC2\xB0", true, {}, { { "color_mode", 2 } } });
    add({ "hue_white", K::integer, 0, 360, 1, 200, P::colours, "White keys (hue)", hueHelp, "\xC2\xB0", true, {}, { { "color_mode", 1 } } });
    add({ "hue_black", K::integer, 0, 360, 1, 275, P::colours, "Black keys (hue)", hueHelp, "\xC2\xB0", true, {}, { { "color_mode", 1 } } });
    add({ "hue", K::integer, 0, 360, 1, 200, P::colours, "Hue", hueHelp, "\xC2\xB0", true, {}, { { "color_mode", 0 } } });
    add({ "saturation", K::integer, 0, 100, 1, 90, P::colours, "Saturation", "How vivid the colours are: 0 grey, 100 the purest.", "%",
          true, {}, {} });
    add({ "hand_adaptive", K::toggle, 0, 1, 1, 1, P::colours, "Follow the hands",
          "On: the hands estimated from what is played. Off: a fixed split, the keys under it to the left hand.", "", false, {},
          { { "color_mode", 2 } } });
    add({ "hand_split", K::choice, 21, 108, 1, 60, P::colours, "Split", "The keys under it go to the left hand.", "", false, keys,
          { { "color_mode", 2 }, { "hand_adaptive", 0 } } });
    // The notes.
    add({ "pedal_tails", K::toggle, 0, 1, 1, 1, P::notes, "Notes held by the pedals",
          "A note whose key is up but which a pedal still holds goes on as a fainter tail until the pedal lets it go; then its end "
          "draws in and fades out.",
          "", true, {}, {} });
    add({ "tail_width", K::integer, 10, 100, 1, 35, P::notes, "Tails: width", "A tail's width, as a share of its note's.", "%", true, {},
          { { "pedal_tails", 1 } } });
    add({ "tail_opacity", K::integer, 10, 100, 1, 50, P::notes, "Tails: opacity", "How much a tail shows, as a share of its note's opacity.",
          "%", true, {}, { { "pedal_tails", 1 } } });
    add({ "tail_fine", K::integer, 0, 100, 1, 80, P::notes, "Tails: fine end",
          "How thin and faint a tail's end becomes once the pedal has let it go: 0 a blunt end, 100 a point fading to nothing.", "%",
          true, {}, { { "pedal_tails", 1 } } });
    add({ "pedal_line", K::integer, 0, 100, 1, 30, P::notes, "Pedal line",
          "A thin line of light along the top of the keyboard while the sustain pedal is down. 0: none.", "%", true, {}, {} });
    add({ "pedal_glow", K::integer, 0, 100, 1, 20, P::notes, "Pedal halo",
          "A soft light rising from the top of the keyboard over the notes' feet while the sustain pedal is down. 0: none.", "%", true,
          {}, {} });
    add({ "note_decay", K::toggle, 0, 1, 1, 1, P::notes, "Notes die away",
          "Along its bar, a note dims as its sound would: slowly in the bass, quickly in the treble.", "", true, {}, {} });
    add({ "melody", K::toggle, 0, 1, 1, 1, P::notes, "Melody line", "A line of light through the right hand's top notes.", "", true, {},
          {} });
    // The lights.
    add({ "glow", K::integer, 0, 100, 1, 60, P::lights, "Lights",
          "Beams rising from the keys held, halos around the notes, the keys lit from within. 0: none.", "%", true, {}, {} });
    add({ "bloom", K::integer, 0, 100, 1, 40, P::lights, "Bloom",
          "The lights, the notes and the keys lit glow beyond themselves (the white keys do not). 0: none.", "%", true, {}, {} });
    add({ "reflection", K::integer, 0, 100, 1, 50, P::lights, "Reflection",
          "The band over the keys a black lacquer reflecting the notes and lights above it. 0: a thin rail.", "%", true, {}, {} });
    add({ "ambiance", K::integer, 0, 100, 1, 35, P::lights, "Aurora (ambiance)",
          "Curtains of light over the room, swaying, as strong as the playing is loud. 0: none.", "%", true, {}, {} });
    add({ "aurora_height", K::integer, 10, 100, 1, 80, P::lights, "Aurora: height",
          "How high its curtains rise, as a share of the room above the keys.", "%", true, {}, {} });
    add({ "aurora_speed", K::integer, 0, 100, 1, 50, P::lights, "Aurora: speed", "How fast its curtains sway. 0: still; 50: as made.", "%",
          true, {}, {} });
    add({ "aurora_rest", K::integer, 0, 100, 1, 0, P::lights, "Aurora: at rest",
          "How much of it stays when nothing sounds: 0 it follows the sound alone, 100 always at full strength.", "%", true, {}, {} });
    add({ "flash", K::toggle, 0, 1, 1, 1, P::lights, "Burst at the strike",
          "A burst of light where a key is struck, a flare along the keyboard, the key lit from within.", "", true, {}, {} });
    add({ "afterglow_ms", K::integer, 0, 3000, 10, 400, P::lights, "Afterglow", "How long a key keeps fading once it is up.", " ms", true,
          {}, {} });
    // The sparkles.
    add({ "sparkles", K::integer, 0, 100, 1, 50, P::sparkles, "Sparkles",
          "Sparkles thrown up by each strike in a short stream, the more and the higher the stronger it is. 0: none.", "%", true, {}, {} });
    add({ "sparkle_height", K::integer, 10, 100, 1, 70, P::sparkles, "Height", "How high they rise, as a share of the room above the keys.",
          "%", true, {}, {} });
    add({ "sparkle_spread", K::integer, 0, 90, 1, 20, P::sparkles, "Spread",
          "The angle they leave within: 0 a thread straight up, 90 a wide fountain.", "\xC2\xB0", true, {}, {} });
    add({ "sparkle_tilt", K::integer, -45, 45, 1, 0, P::sparkles, "Direction", "Leans them to the left (negative) or to the right.",
          "\xC2\xB0", true, {}, {} });
    // The velocity.
    add({ "velocity_accent", K::integer, 0, 100, 1, 70, P::velocity, "Accent",
          "How much each strike's strength shows: soft notes thinner and more transparent, strong ones wider with a white-hot core.", "%",
          true, {}, {} });
    add({ "velocity_top", K::integer, 40, 127, 1, 112, P::velocity, "Strongest velocity",
          "The velocity shown at full strength: your keyboard's strongest (a Korg B2 seldom goes past 112).", "", false, {}, {} });
    // The powerful chords.
    add({ "chords", K::toggle, 0, 1, 1, 1, P::chords, "Powerful chords",
          "A chord struck together and hard becomes an event. After one, the next waits 0.6 s.", "", true, {}, {} });
    add({ "chord_notes", K::integer, 2, 8, 1, 3, P::chords, "Notes at least", "", "", false, {}, { { "chords", 1 } } });
    add({ "chord_threshold", K::integer, 0, 100, 1, 75, P::chords, "Strength threshold",
          "The chord's mean strength, against the strongest velocity.", "%", false, {}, { { "chords", 1 } } });
    add({ "chord_intensity", K::integer, 0, 100, 1, 80, P::chords, "Intensity", "", "%", true, {}, { { "chords", 1 } } });
    add({ "chord_wave", K::toggle, 0, 1, 1, 1, P::chords, "A wave of light along the keyboard", "", "", true, {}, { { "chords", 1 } } });
    add({ "chord_wash", K::toggle, 0, 1, 1, 1, P::chords, "The room above the keys lit", "", "", true, {}, { { "chords", 1 } } });
    add({ "chord_fireworks", K::toggle, 0, 1, 1, 1, P::chords, "Fireworks", "", "", true, {}, { { "chords", 1 } } });
    add({ "chord_bracket", K::toggle, 0, 1, 1, 1, P::chords, "The chord's notes tied by a bar of light", "", "", true, {},
          { { "chords", 1 } } });
    add({ "chord_shake", K::toggle, 0, 1, 1, 0, P::chords, "The keyboard shakes", "", "", true, {}, { { "chords", 1 } } });
    return e;
}

} // namespace detail

/// Every setting, in the order they are shown and exchanged.
inline const std::vector<Entry>& entries()
{
    static const std::vector<Entry> table = detail::makeEntries();
    return table;
}

/// A setting's place in entries(), or -1.
inline int indexOf(const char* key)
{
    const auto& e = entries();
    for (std::size_t i = 0; i < e.size(); ++i)
        if (std::strcmp(e[i].key, key) == 0)
            return (int) i;
    return -1;
}

/// A setting's value made valid: within its range, on its step, one of its choices.
inline double valid(const Entry& e, double v)
{
    if (!std::isfinite(v))
        return e.def;
    if (e.kind == Kind::choice) {
        const int n = (int) std::lround(v);
        for (const auto& c : e.choices)
            if (c.value == n)
                return n;
        return e.def;
    }
    v = std::clamp(v, e.min, e.max);
    if (e.kind == Kind::toggle)
        return v >= 0.5 ? 1.0 : 0.0;
    if (e.kind == Kind::integer)
        return (double) std::lround(v);
    return std::round(v / e.step) * e.step;
}

/// All the settings' values, in entries()' order.
class Values {
public:
    Values()
    {
        for (const auto& e : entries())
            v.push_back(e.def);
    }
    double get(const char* key) const
    {
        const int i = indexOf(key);
        return i < 0 ? 0.0 : v[(std::size_t) i];
    }
    double at(std::size_t i) const { return v[i]; }
    void set(const char* key, double value)
    {
        const int i = indexOf(key);
        if (i >= 0)
            v[(std::size_t) i] = valid(entries()[(std::size_t) i], value);
    }
    void setAt(std::size_t i, double value) { v[i] = valid(entries()[i], value); }
    bool operator==(const Values& o) const { return v == o.v; }
    bool operator!=(const Values& o) const { return v != o.v; }

    /// Shown (its conditions met by these values).
    bool shows(const Entry& e) const
    {
        for (const auto& c : e.when)
            if (c.key != nullptr && ((std::lround(get(c.key)) == c.value) != c.equal))
                return false;
        return true;
    }

    /// "key=value" lines, in entries()' order (whole numbers plain, others to four decimals at most).
    std::string text() const
    {
        std::string out;
        const auto& e = entries();
        for (std::size_t i = 0; i < e.size(); ++i) {
            char value[48];
            if (e[i].kind == Kind::real) {
                std::snprintf(value, sizeof(value), "%.4f", v[i]);
                std::string s = value;
                while (!s.empty() && s.back() == '0')
                    s.pop_back();
                if (!s.empty() && s.back() == '.')
                    s.pop_back();
                std::snprintf(value, sizeof(value), "%s", s.c_str());
            } else {
                std::snprintf(value, sizeof(value), "%ld", std::lround(v[i]));
            }
            out += e[i].key;
            out += '=';
            out += value;
            out += '\n';
        }
        return out;
    }

    /// From such lines (unknown keys left out, missing ones as they were, values made valid).
    void read(const std::string& text)
    {
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t end = std::min(text.find('\n', at), text.size());
            const std::string line = text.substr(at, end - at);
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos)
                set(line.substr(0, eq).c_str(), std::strtod(line.c_str() + eq + 1, nullptr));
            at = end + 1;
        }
    }

private:
    std::vector<double> v;
};

/// The styles: whole looks at once (the size, the keys shown, the timing and the velocity scale kept).
enum Style : int { kCustom = 0, kPianoForge, kConcert, kNeon, kGalaxy, kRainbow, kMinimal };

inline const std::vector<Choice>& styles()
{
    static const std::vector<Choice> list = { { kCustom, "My own" }, { kPianoForge, "PianoForge (the defaults)" },
                                              { kConcert, "Concert (sober)" }, { kNeon, "Neon" }, { kGalaxy, "Galaxy" },
                                              { kRainbow, "Rainbow" }, { kMinimal, "Minimal" } };
    return list;
}

/// A style: the look back to its defaults, then the style's own values.
inline void applyStyle(Values& values, int style)
{
    const auto& e = entries();
    for (std::size_t i = 0; i < e.size(); ++i)
        if (e[i].look)
            values.setAt(i, e[i].def);
    struct Set {
        const char* key;
        double value;
    };
    std::vector<Set> sets;
    switch (style) {
    case kConcert: // sober: softer colours and lights, few sparkles, no fireworks
        sets = { { "hue_left", 210 }, { "hue_right", 35 }, { "saturation", 55 }, { "glow", 35 }, { "sparkles", 15 },
                 { "sparkle_height", 50 }, { "velocity_accent", 50 }, { "chord_intensity", 50 }, { "chord_fireworks", 0 },
                 { "pedal_line", 15 }, { "pedal_glow", 10 }, { "aurora_speed", 30 }, { "afterglow_ms", 600 }, { "bloom", 25 },
                 { "reflection", 60 }, { "ambiance", 15 } };
        break;
    case kNeon: // cyan and magenta, everything bright
        sets = { { "hue_left", 190 }, { "hue_right", 320 }, { "saturation", 100 }, { "glow", 90 }, { "sparkles", 70 },
                 { "velocity_accent", 80 }, { "chord_intensity", 100 }, { "bloom", 80 }, { "ambiance", 50 }, { "pedal_line", 60 },
                 { "pedal_glow", 40 }, { "aurora_speed", 75 } };
        break;
    case kGalaxy: // violet and turquoise, sparkles high, curtains of light
        sets = { { "hue_left", 265 }, { "hue_right", 185 }, { "saturation", 80 }, { "glow", 70 }, { "sparkles", 85 },
                 { "sparkle_height", 90 }, { "sparkle_spread", 35 }, { "chord_intensity", 90 }, { "bloom", 60 }, { "reflection", 40 },
                 { "ambiance", 80 }, { "aurora_height", 100 }, { "aurora_rest", 30 } };
        break;
    case kRainbow:
        sets = { { "color_mode", 3 }, { "saturation", 85 }, { "glow", 70 }, { "sparkles", 60 }, { "bloom", 50 }, { "ambiance", 40 } };
        break;
    case kMinimal: // the keys and their notes, nothing more
        sets = { { "color_mode", 0 }, { "hue", 205 }, { "saturation", 60 }, { "glow", 0 }, { "sparkles", 0 }, { "velocity_accent", 40 },
                 { "chords", 0 }, { "flash", 0 }, { "afterglow_ms", 250 }, { "note_decay", 0 }, { "melody", 0 }, { "bloom", 0 },
                 { "reflection", 0 }, { "ambiance", 0 }, { "pedal_line", 0 }, { "pedal_glow", 0 }, { "tail_opacity", 35 } };
        break;
    default: // PianoForge: the defaults
        break;
    }
    for (const auto& s : sets)
        values.set(s.key, s.value);
}

} // namespace pf::look
