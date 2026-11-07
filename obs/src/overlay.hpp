#pragma once

// The overlays' logic, without OBS (tests: obs/tests/overlay_test.cpp): the keyboard's geometry, the keys and
// pedals as the MIDI events of the feed leave them at a frame, the notes played (for the trails), and the text
// source's template.

#include <pianoforge/audio_feed.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <vector>

namespace pfobs {

constexpr int kFirstKey = 21, kLastKey = 108; // A0 to C8
constexpr int kWhiteKeys = 52;

inline bool isBlack(int key) noexcept
{
    const int pc = ((key % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

/// White keys before `key` from A0 (for a black key: those before the white key under it).
inline int whitesBefore(int key) noexcept
{
    int n = 0;
    for (int k = kFirstKey; k < key; ++k)
        n += isBlack(k) ? 0 : 1;
    return n;
}

struct KeyShape {
    float x = 0.0f, width = 0.0f;
    bool black = false;
};

/// A key over a keyboard `width` wide: the 52 white keys side by side, each black key 0.58 of a white one, between
/// its two white neighbours and set off a little as on a piano (C# and F# toward the left, D# and A# toward the
/// right).
inline KeyShape keyShape(int key, float width) noexcept
{
    const float w = width / (float) kWhiteKeys;
    KeyShape s;
    s.black = isBlack(key);
    if (!s.black) {
        s.x = (float) whitesBefore(key) * w;
        s.width = w;
        return s;
    }
    const int pc = ((key % 12) + 12) % 12;
    const float shift = pc == 1 || pc == 6 ? -0.12f : pc == 3 || pc == 10 ? 0.12f : 0.0f;
    s.width = 0.58f * w;
    const float boundary = (float) whitesBefore(key) * w; // between the white key below and the one above
    s.x = boundary + (shift - 0.29f) * w;
    return s;
}

/// A note played: its key, velocity, and frames (end: open while the key is down).
struct PlayedNote {
    int key = 0, velocity = 0;
    std::uint64_t start = 0, end = std::numeric_limits<std::uint64_t>::max();
    bool open() const noexcept { return end == std::numeric_limits<std::uint64_t>::max(); }
};

/// The keyboard as the feed's events leave it: events are queued as they arrive (ahead of the sound), and applied
/// once the frame shown reaches theirs.
class KeyboardState {
public:
    void reset()
    {
        pending.clear();
        played.clear();
        down.fill(0);
        open.fill(-1);
        pedals.fill(0.0f);
    }

    void add(const pf::FeedEvent& e) { pending.push_back(e); }

    /// Applies the events up to `frame`; forgets the notes ended more than `keep` frames before it.
    void advance(std::uint64_t frame, std::uint64_t keep)
    {
        while (!pending.empty() && pending.front().frame <= frame) {
            apply(pending.front());
            pending.pop_front();
        }
        const std::uint64_t horizon = frame > keep ? frame - keep : 0;
        played.erase(std::remove_if(played.begin(), played.end(),
                                    [&](const PlayedNote& n) { return !n.open() && n.end < horizon; }),
                     played.end());
        for (int k = 0; k < 128; ++k) // the indices of the notes still open, after the erase
            open[(std::size_t) k] = -1;
        for (int i = 0; i < (int) played.size(); ++i)
            if (played[(std::size_t) i].open())
                open[(std::size_t) played[(std::size_t) i].key] = i;
    }

    /// The velocity of a key held (0: up).
    int velocity(int key) const noexcept { return key >= 0 && key < 128 ? down[(std::size_t) key] : 0; }
    /// 0 the sustain pedal, 1 the sostenuto, 2 the soft pedal: 0 (up) to 1 (down).
    float pedal(int which) const noexcept { return which >= 0 && which < 3 ? pedals[(std::size_t) which] : 0.0f; }
    const std::vector<PlayedNote>& notes() const noexcept { return played; }
    std::size_t waiting() const noexcept { return pending.size(); }

private:
    void apply(const pf::FeedEvent& e)
    {
        const int type = e.status & 0xF0, key = e.data1 & 0x7F, value = e.data2 & 0x7F;
        if (type == 0x90 && value > 0) {
            close(key, e.frame); // struck again: the earlier note ends here
            down[(std::size_t) key] = value;
            PlayedNote n;
            n.key = key;
            n.velocity = value;
            n.start = e.frame;
            played.push_back(n);
            open[(std::size_t) key] = (int) played.size() - 1;
        } else if (type == 0x80 || (type == 0x90 && value == 0)) {
            down[(std::size_t) key] = 0;
            close(key, e.frame);
        } else if (type == 0xB0) {
            if (key == 64)
                pedals[0] = value / 127.0f;
            else if (key == 66)
                pedals[1] = value / 127.0f;
            else if (key == 67)
                pedals[2] = value / 127.0f;
            else if (key == 120 || key == 123) { // all sound off, all notes off
                for (int k = 0; k < 128; ++k) {
                    down[(std::size_t) k] = 0;
                    close(k, e.frame);
                }
            }
        }
    }

    void close(int key, std::uint64_t frame)
    {
        const int i = open[(std::size_t) key];
        if (i >= 0 && i < (int) played.size() && played[(std::size_t) i].open())
            played[(std::size_t) i].end = std::max(frame, played[(std::size_t) i].start);
        open[(std::size_t) key] = -1;
    }

    std::deque<pf::FeedEvent> pending;
    std::vector<PlayedNote> played;
    std::array<int, 128> down {};
    std::array<int, 128> open = [] {
        std::array<int, 128> a {};
        a.fill(-1);
        return a;
    }();
    std::array<float, 3> pedals {};
};

/// The text source's template, its fields replaced: {file} the MIDI file, {state} playing or stopped, {preset} the
/// instrument preset, {room} the room, {engine}, {recording} (`recordingText` while PianoForge records, else
/// nothing). A line left empty by its fields disappears.
inline std::string formatInfo(const std::string& tmpl, const pf::FeedInfo& info, const std::string& recordingText,
                              const std::string& playingText, const std::string& stoppedText)
{
    const std::string file = info.file;
    const std::pair<std::string, std::string> fields[] = {
        { "{file}", file },
        { "{state}", file.empty() ? std::string() : info.filePlaying ? playingText : stoppedText },
        { "{preset}", info.preset },
        { "{room}", info.room },
        { "{engine}", info.engine },
        { "{recording}", info.recording ? recordingText : std::string() },
    };
    std::string out;
    std::size_t lineStart = 0;
    while (lineStart <= tmpl.size()) {
        const std::size_t lineEnd = std::min(tmpl.find('\n', lineStart), tmpl.size());
        std::string line = tmpl.substr(lineStart, lineEnd - lineStart);
        bool hadField = false, allEmpty = true;
        for (const auto& [name, value] : fields)
            for (std::size_t at = line.find(name); at != std::string::npos; at = line.find(name, at + value.size())) {
                line.replace(at, name.size(), value);
                hadField = true;
                allEmpty = allEmpty && value.empty();
            }
        auto blank = [](const std::string& s) { return s.find_first_not_of(" \t\r-:|\xE2\x80\x94") == std::string::npos; };
        if (!(hadField && allEmpty && blank(line))) {
            if (!out.empty())
                out += '\n';
            out += line;
        }
        if (lineEnd >= tmpl.size())
            break;
        lineStart = lineEnd + 1;
    }
    return out;
}

} // namespace pfobs
