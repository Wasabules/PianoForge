#pragma once

// The overlays' logic, without OBS (tests: obs/tests/overlay_test.cpp): the keyboard's geometry, the keys and
// pedals as the MIDI events of the feed leave them at a frame, the notes played (for the trails), and the text
// source's template.

#include <pianoforge/audio_feed.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

enum Hand : int { kNoHand = 0, kLeftHand = 1, kRightHand = 2 };

/// Which hand plays each key, from the notes alone: each hand has a place on the keyboard (where it played last) and
/// the keys it holds. Keys struck together (within a few tens of milliseconds) are taken as one group: one too wide
/// for a hand (more than kSpan semitones) is split at its widest interval, the lower part to the left hand; another
/// goes to the hand it costs least, by its distance from the hand's place, the stretch it would ask of the hand (the
/// keys it holds included) and how far it would cross over the other hand's keys. With a fixed split, the keys under
/// it are the left hand's. An estimate: crossed hands and wide leaps fool it, as they would a listener.
class HandEstimator {
public:
    static constexpr int kSpan = 14; // a ninth and more: what one hand holds at most, here

    void reset()
    {
        place[0] = 48.0;
        place[1] = 72.0;
        holder.fill(kNoHand);
    }
    void setFixed(bool on, int splitKey)
    {
        fixed = on;
        split = splitKey;
    }
    /// The hands of `keys` (ascending, distinct), struck together, into `hands`.
    void assign(const std::vector<int>& keys, std::vector<int>& hands)
    {
        const int n = (int) keys.size();
        hands.assign((std::size_t) n, kRightHand);
        if (n == 0)
            return;
        if (fixed) {
            for (int i = 0; i < n; ++i)
                hands[(std::size_t) i] = keys[(std::size_t) i] < split ? kLeftHand : kRightHand;
        } else if (keys[(std::size_t) n - 1] - keys[0] > kSpan) { // too wide for one hand: split at the widest interval
            int at = 1, widest = 0;
            for (int i = 1; i < n; ++i)
                if (keys[(std::size_t) i] - keys[(std::size_t) i - 1] > widest) {
                    widest = keys[(std::size_t) i] - keys[(std::size_t) i - 1];
                    at = i;
                }
            for (int i = 0; i < n; ++i)
                hands[(std::size_t) i] = i < at ? kLeftHand : kRightHand;
        } else {
            const int lo = keys[0], hi = keys[(std::size_t) n - 1];
            const double left = cost(kLeftHand, lo, hi), right = cost(kRightHand, lo, hi);
            const int hand = left < right ? kLeftHand : right < left ? kRightHand
                           : 0.5 * (lo + hi) < 0.5 * (place[0] + place[1]) ? kLeftHand : kRightHand;
            for (auto& h : hands)
                h = hand;
        }
        // The hands' places move toward what they just took; they do not cross.
        for (int hand : { kLeftHand, kRightHand }) {
            double sum = 0.0;
            int count = 0;
            for (int i = 0; i < n; ++i)
                if (hands[(std::size_t) i] == hand) {
                    sum += keys[(std::size_t) i];
                    ++count;
                }
            if (count > 0)
                place[hand - 1] += 0.6 * (sum / count - place[hand - 1]);
        }
        if (place[0] > place[1] - 4.0) {
            const double mid = 0.5 * (place[0] + place[1]);
            place[0] = mid - 2.0;
            place[1] = mid + 2.0;
        }
        for (int i = 0; i < n; ++i)
            holder[(std::size_t) keys[(std::size_t) i]] = hands[(std::size_t) i];
    }
    void release(int key) { holder[(std::size_t) key] = kNoHand; }
    double where(int hand) const { return place[hand - 1]; }

private:
    double cost(int hand, int lo, int hi) const
    {
        double c = std::abs(0.5 * (lo + hi) - place[hand - 1]);
        int heldLo = lo, heldHi = hi, otherLo = 128, otherHi = -1;
        for (int k = 0; k < 128; ++k) {
            if (holder[(std::size_t) k] == hand) {
                heldLo = std::min(heldLo, k);
                heldHi = std::max(heldHi, k);
            } else if (holder[(std::size_t) k] != kNoHand) {
                otherLo = std::min(otherLo, k);
                otherHi = std::max(otherHi, k);
            }
        }
        if (heldHi - heldLo > kSpan)
            c += 4.0 * (heldHi - heldLo - kSpan);
        if (hand == kLeftHand && otherHi >= 0 && hi > otherLo)
            c += 3.0 * (hi - otherLo);
        if (hand == kRightHand && otherHi >= 0 && lo < otherHi)
            c += 3.0 * (otherHi - lo);
        return c;
    }

    double place[2] = { 48.0, 72.0 }; // where each hand is (a key), left and right
    std::array<int, 128> holder {};   // the hand holding each key
    bool fixed = false;
    int split = 60;
};

/// A note played: its key, velocity, hand, and frames (end: open while the key is down).
struct PlayedNote {
    int key = 0, velocity = 0, hand = kNoHand;
    std::uint64_t start = 0, end = std::numeric_limits<std::uint64_t>::max();
    bool open() const noexcept { return end == std::numeric_limits<std::uint64_t>::max(); }
};

/// A key's light: held at `velocity` (0: up), by `hand`, pressed at frame `pressed` and released at `released` (the
/// last time), at `lastVelocity`; `struck`: ever pressed since the reset.
struct KeyLight {
    int velocity = 0, lastVelocity = 0, hand = kNoHand;
    std::uint64_t pressed = 0, released = 0;
    bool struck = false;
};

/// The keyboard as the feed's events leave it: events are queued as they arrive (ahead of the sound), and applied
/// once the frame shown reaches theirs. The hands are estimated when a group of notes is applied, from the notes
/// queued with it (a few tens of milliseconds: they are there already, the sound being that much behind).
class KeyboardState {
public:
    void reset()
    {
        pending.clear();
        played.clear();
        for (auto& l : lights)
            l = KeyLight {};
        open.fill(-1);
        pedals.fill(0.0f);
        hands.reset();
    }
    /// The feed's rate (the window of notes struck together: kTogetherSeconds).
    void setRate(double rate) { together = (std::uint64_t) std::max(1.0, kTogetherSeconds * rate); }
    void setFixedSplit(bool on, int key) { hands.setFixed(on, key); }

    void add(const pf::FeedEvent& e)
    {
        pending.push_back(e);
        pending.back().reserved = kNoHand; // the hand, once estimated
    }

    /// Applies the events up to `frame`; forgets the notes ended more than `keep` frames before it.
    void advance(std::uint64_t frame, std::uint64_t keep)
    {
        while (!pending.empty() && pending.front().frame <= frame) {
            if (isNoteOn(pending.front()) && pending.front().reserved == kNoHand)
                estimateGroup();
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
    int velocity(int key) const noexcept { return key >= 0 && key < 128 ? lights[(std::size_t) key].velocity : 0; }
    const KeyLight& light(int key) const noexcept { return lights[(std::size_t) std::clamp(key, 0, 127)]; }
    /// 0 the sustain pedal, 1 the sostenuto, 2 the soft pedal: 0 (up) to 1 (down).
    float pedal(int which) const noexcept { return which >= 0 && which < 3 ? pedals[(std::size_t) which] : 0.0f; }
    const std::vector<PlayedNote>& notes() const noexcept { return played; }
    std::size_t waiting() const noexcept { return pending.size(); }
    const HandEstimator& estimator() const noexcept { return hands; }

private:
    static constexpr double kTogetherSeconds = 0.035;

    static bool isNoteOn(const pf::FeedEvent& e) { return (e.status & 0xF0) == 0x90 && (e.data2 & 0x7F) > 0; }

    /// The hands of the notes struck with the first queued one (within `together` frames of it).
    void estimateGroup()
    {
        const std::uint64_t until = pending.front().frame + together;
        groupKeys.clear();
        for (const auto& e : pending) {
            if (e.frame > until)
                break;
            if (isNoteOn(e) && e.reserved == kNoHand)
                groupKeys.push_back(e.data1 & 0x7F);
        }
        std::sort(groupKeys.begin(), groupKeys.end());
        groupKeys.erase(std::unique(groupKeys.begin(), groupKeys.end()), groupKeys.end());
        hands.assign(groupKeys, groupHands);
        for (auto& e : pending) {
            if (e.frame > until)
                break;
            if (!isNoteOn(e) || e.reserved != kNoHand)
                continue;
            const auto at = std::lower_bound(groupKeys.begin(), groupKeys.end(), e.data1 & 0x7F) - groupKeys.begin();
            e.reserved = (std::uint32_t) groupHands[(std::size_t) at];
        }
    }

    void apply(const pf::FeedEvent& e)
    {
        const int type = e.status & 0xF0, key = e.data1 & 0x7F, value = e.data2 & 0x7F;
        auto& light = lights[(std::size_t) key];
        if (type == 0x90 && value > 0) {
            close(key, e.frame); // struck again: the earlier note ends here
            light.velocity = light.lastVelocity = value;
            light.hand = e.reserved != kNoHand ? (int) e.reserved : kRightHand;
            light.pressed = e.frame;
            light.struck = true;
            PlayedNote n;
            n.key = key;
            n.velocity = value;
            n.hand = light.hand;
            n.start = e.frame;
            played.push_back(n);
            open[(std::size_t) key] = (int) played.size() - 1;
        } else if (type == 0x80 || (type == 0x90 && value == 0)) {
            release(key, e.frame);
        } else if (type == 0xB0) {
            if (key == 64)
                pedals[0] = value / 127.0f;
            else if (key == 66)
                pedals[1] = value / 127.0f;
            else if (key == 67)
                pedals[2] = value / 127.0f;
            else if (key == 120 || key == 123) // all sound off, all notes off
                for (int k = 0; k < 128; ++k)
                    if (lights[(std::size_t) k].velocity > 0)
                        release(k, e.frame);
        }
    }

    void release(int key, std::uint64_t frame)
    {
        auto& light = lights[(std::size_t) key];
        if (light.velocity > 0)
            light.released = frame;
        light.velocity = 0;
        hands.release(key);
        close(key, frame);
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
    std::array<KeyLight, 128> lights {};
    std::array<int, 128> open = [] {
        std::array<int, 128> a {};
        a.fill(-1);
        return a;
    }();
    std::array<float, 3> pedals {};
    HandEstimator hands;
    std::uint64_t together = 1680; // 35 ms at 48 kHz
    std::vector<int> groupKeys, groupHands;
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
