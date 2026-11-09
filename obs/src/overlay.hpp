#pragma once

// The overlays' logic, without OBS (tests: obs/tests/overlay_test.cpp): the keyboard's geometry, the keys and
// pedals as the MIDI events of the feed leave them at a frame, the notes played (for the trails) and struck (for the
// sparkles) and how long they sound (the pedals holding them), the part of the keyboard shown (following the playing),
// the melody, the colours by hue, the sparkles and fireworks, the powerful chords, and the text source's template.

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

/// A colour from its hue (degrees), saturation and lightness (0 to 1), as OBS keeps colours (0xAABBGGRR), opaque.
inline std::uint32_t hslColor(double hue, double saturation, double lightness) noexcept
{
    const double h = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0) / 60.0;
    const double s = std::clamp(saturation, 0.0, 1.0), l = std::clamp(lightness, 0.0, 1.0);
    const double c = (1.0 - std::fabs(2.0 * l - 1.0)) * s, x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0)), m = l - 0.5 * c;
    double r = 0.0, g = 0.0, b = 0.0;
    switch ((int) h) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    auto byte = [m](double v) { return (std::uint32_t) std::lround(255.0 * std::clamp(v + m, 0.0, 1.0)); };
    return 0xFF000000u | byte(b) << 16 | byte(g) << 8 | byte(r);
}

/// Between two colours (0xAABBGGRR), `t` of the way from `a` to `b`.
inline std::uint32_t mixColor(std::uint32_t a, std::uint32_t b, double t) noexcept
{
    t = std::clamp(t, 0.0, 1.0);
    std::uint32_t out = 0;
    for (int c = 0; c < 32; c += 8) {
        const double x = (double) ((a >> c) & 0xFF), y = (double) ((b >> c) & 0xFF);
        out |= (std::uint32_t) std::lround(x + (y - x) * t) << c;
    }
    return out;
}

/// The part of the keyboard shown: from white key `first` (counted from A0, fractional while moving), `count` white
/// keys across the width.
struct KeyView {
    float first = 0.0f, count = (float) kWhiteKeys;
};

/// A key over a keyboard `width` wide showing `view` (keys outside it fall outside the width).
inline KeyShape keyShape(int key, float width, const KeyView& view) noexcept
{
    const float w = width / std::max(1.0f, view.count);
    KeyShape s = keyShape(key, w * (float) kWhiteKeys);
    s.x -= view.first * w;
    return s;
}

/// The view of the keys from `firstKey` to `lastKey` (a black end widened to the white key beside it).
inline KeyView viewOf(int firstKey, int lastKey) noexcept
{
    firstKey = std::clamp(firstKey, kFirstKey, kLastKey);
    lastKey = std::clamp(lastKey, firstKey, kLastKey);
    if (isBlack(firstKey))
        --firstKey;
    if (isBlack(lastKey))
        ++lastKey;
    KeyView v;
    v.first = (float) whitesBefore(firstKey);
    v.count = (float) (whitesBefore(lastKey) + 1) - v.first;
    return v;
}

/// The view following the playing: the keys played lately with a margin, at least `minWhites` wide, within the
/// keyboard. It moves only when a key played falls near its edge or when it could narrow by a quarter (a view that
/// moves at every note is tiring), and then glides there.
class ViewFollower {
public:
    static constexpr float kMargin = 1.5f, kSeconds = 0.6f;

    void reset(const KeyView& v)
    {
        now = target = v;
    }
    /// The keys played lately (`low` > `high`: none), `dt` seconds on.
    const KeyView& step(int low, int high, float dt, float minWhites)
    {
        minWhites = std::clamp(minWhites, 1.0f, (float) kWhiteKeys);
        if (low <= high) {
            const float needLo = (float) whitesBefore(std::clamp(low, kFirstKey, kLastKey));
            const float needHi = (float) (whitesBefore(std::clamp(high, kFirstKey, kLastKey)) + 1);
            float lo = needLo - kMargin, hi = needHi + kMargin;
            const float span = std::min((float) kWhiteKeys, std::max(hi - lo, minWhites)), mid = 0.5f * (lo + hi);
            lo = std::clamp(mid - 0.5f * span, 0.0f, std::max(0.0f, (float) kWhiteKeys - span));
            hi = std::min((float) kWhiteKeys, lo + span);
            const bool outside = needLo < target.first + 0.5f || needHi > target.first + target.count - 0.5f;
            const bool narrower = hi - lo < 0.75f * target.count;
            if (outside || narrower || target.count < minWhites) {
                target.first = lo;
                target.count = hi - lo;
            }
        }
        const float k = 1.0f - std::exp(-std::max(0.0f, dt) / kSeconds);
        now.first += (target.first - now.first) * k;
        now.count += (target.count - now.count) * k;
        return now;
    }
    const KeyView& view() const noexcept { return now; }

private:
    KeyView now, target;
};

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
/// `sound`: when its sound ends (its key's release, or later: the pedal holding it lifted, the key struck again;
/// open while it sounds).
struct PlayedNote {
    int key = 0, velocity = 0, hand = kNoHand;
    std::uint64_t start = 0, end = std::numeric_limits<std::uint64_t>::max(), sound = std::numeric_limits<std::uint64_t>::max();
    bool open() const noexcept { return end == std::numeric_limits<std::uint64_t>::max(); }
    /// Its key up, a pedal holding its sound.
    bool ringing() const noexcept { return !open() && sound == std::numeric_limits<std::uint64_t>::max(); }
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
        strikes.clear();
        for (auto& l : lights)
            l = KeyLight {};
        open.fill(-1);
        pedals.fill(0.0f);
        sostenuto.fill(false);
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
                                    [&](const PlayedNote& n) { return !n.open() && !n.ringing() && n.sound < horizon; }),
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
    /// The notes struck since the last call (the newest kStrikesKept at most), into `out`.
    void takeStrikes(std::vector<PlayedNote>& out)
    {
        out.clear();
        out.swap(strikes);
    }
    std::size_t waiting() const noexcept { return pending.size(); }
    const HandEstimator& estimator() const noexcept { return hands; }

private:
    static constexpr double kTogetherSeconds = 0.035;
    static constexpr std::size_t kStrikesKept = 256;

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
            close(key, e.frame); // struck again: the earlier note ends here, and its sound
            silence(key, e.frame);
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
            if (strikes.size() >= kStrikesKept)
                strikes.erase(strikes.begin());
            strikes.push_back(n);
        } else if (type == 0x80 || (type == 0x90 && value == 0)) {
            release(key, e.frame);
        } else if (type == 0xB0) {
            if (key == 64) {
                const bool was = sustainDown();
                pedals[0] = value / 127.0f;
                if (was && !sustainDown()) // the sustain pedal up: what it held stops
                    for (auto& n : played)
                        if (n.ringing() && !(sostenutoDown() && sostenuto[(std::size_t) n.key]))
                            n.sound = e.frame;
            } else if (key == 66) {
                const bool was = sostenutoDown();
                pedals[1] = value / 127.0f;
                if (!was && sostenutoDown()) { // the sostenuto down: it holds the keys down now
                    for (int k = 0; k < 128; ++k)
                        sostenuto[(std::size_t) k] = lights[(std::size_t) k].velocity > 0;
                } else if (was && !sostenutoDown()) {
                    for (auto& n : played)
                        if (n.ringing() && sostenuto[(std::size_t) n.key] && !sustainDown())
                            n.sound = e.frame;
                    sostenuto.fill(false);
                }
            } else if (key == 67) {
                pedals[2] = value / 127.0f;
            } else if (key == 120 || key == 123) { // all sound off, all notes off
                for (int k = 0; k < 128; ++k)
                    if (lights[(std::size_t) k].velocity > 0)
                        release(k, e.frame);
                for (auto& n : played)
                    if (n.ringing())
                        n.sound = e.frame;
            }
        }
    }

    bool sustainDown() const noexcept { return pedals[0] >= 0.5f; }
    bool sostenutoDown() const noexcept { return pedals[1] >= 0.5f; }

    /// The sound of `key`'s notes still ringing ends at `frame`.
    void silence(int key, std::uint64_t frame)
    {
        for (auto& n : played)
            if (n.key == key && n.ringing())
                n.sound = std::max(frame, n.end);
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

    /// The note held on `key` ends at `frame`; its sound too, unless a pedal holds it.
    void close(int key, std::uint64_t frame)
    {
        const int i = open[(std::size_t) key];
        if (i >= 0 && i < (int) played.size() && played[(std::size_t) i].open()) {
            auto& n = played[(std::size_t) i];
            n.end = std::max(frame, n.start);
            const bool held = sustainDown() || (sostenutoDown() && sostenuto[(std::size_t) key]);
            n.sound = held ? std::numeric_limits<std::uint64_t>::max() : n.end;
        }
        open[(std::size_t) key] = -1;
    }

    std::deque<pf::FeedEvent> pending;
    std::vector<PlayedNote> played, strikes;
    std::array<KeyLight, 128> lights {};
    std::array<int, 128> open = [] {
        std::array<int, 128> a {};
        a.fill(-1);
        return a;
    }();
    std::array<float, 3> pedals {};
    std::array<bool, 128> sostenuto {}; // the keys the sostenuto pedal holds
    HandEstimator hands;
    std::uint64_t together = 1680; // 35 ms at 48 kHz
    std::vector<int> groupKeys, groupHands;
};

/// The melody: the highest note of each group of notes the right hand struck together (within `window` frames of the
/// group's first), in order (`notes` in the order struck).
inline void melodyOf(const std::vector<PlayedNote>& notes, std::uint64_t window, std::vector<PlayedNote>& out)
{
    out.clear();
    std::uint64_t groupStart = 0;
    for (const auto& n : notes) {
        if (n.hand != kRightHand)
            continue;
        if (!out.empty() && n.start <= groupStart + window) {
            if (n.key > out.back().key)
                out.back() = n;
            continue;
        }
        groupStart = n.start;
        out.push_back(n);
    }
}

/// A spark: where it is and where it goes (pixels, pixels per second), how old it is (seconds; below 0 it has not
/// left yet) and how long it lives, its size (pixels: its star's reach), its colour (0xAABBGGRR) and its phase (it
/// twinkles in its own time).
struct Spark {
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f, age = 0.0f, life = 1.0f, size = 1.0f, phase = 0.0f;
    std::uint32_t color = 0xFFFFFFFFu;
    float gravity = 0.0f;                                // pixels per second per second, downward
    int burst = 0;                                       // a rocket: the sparks it bursts into at the end of its life,
    float burstRadius = 0.0f, burstLife = 0.0f, burstGravity = 0.0f; // how far they go, how long they live, how they fall

    /// How bright it is: nothing before it leaves, lit within a few hundredths of a second, going out toward its end.
    float brightness() const noexcept
    {
        if (age < 0.0f)
            return 0.0f;
        const float t = age / life;
        return std::clamp(age / 0.06f, 0.0f, 1.0f) * std::max(0.0f, 1.0f - t * t);
    }
};

/// How a strike throws its sparks: `count` of them from (x, y), spread over `width` pixels across, leaving one after
/// another over `stagger` seconds (a stream, not an explosion). Each rises up to `height` pixels by the end of its
/// life (up to `life` seconds), within a cone of half-angle `spread` leaning `tilt` from the vertical (radians,
/// positive to the right); up to `size` pixels, in `color`.
struct SparkBurst {
    float x = 0.0f, y = 0.0f, width = 0.0f;
    int count = 0;
    float height = 100.0f, tilt = 0.0f, spread = 0.35f, stagger = 0.25f, size = 10.0f, life = 1.2f;
    std::uint32_t color = 0xFFFFFFFFu;
};

/// The sparkles thrown up by the keys: a stream at each strike (as many, as high and as long-lived as the caller makes
/// it, by the velocity), a few rising while a key is held. Each leaves just fast enough to reach its height as it goes
/// out, slowing down in the air, swaying a little.
class Sparkles {
public:
    static constexpr std::size_t kMax = 800;
    static constexpr float kDrag = 1.6f; // how fast they slow down, per second

    void clear() { list.clear(); }

    void burst(const SparkBurst& b)
    {
        for (int i = 0; i < b.count && list.size() < kMax; ++i) {
            Spark s;
            s.x = b.x + (random() - 0.5f) * b.width;
            s.y = b.y;
            s.life = b.life * (0.65f + 0.35f * random());
            const float rise = b.height * (0.5f + 0.5f * random()); // the way it goes, by the end of its life
            const float speed = kDrag * rise / (1.0f - std::exp(-kDrag * s.life));
            const float angle = b.tilt + (2.0f * random() - 1.0f) * b.spread;
            s.vx = speed * std::sin(angle);
            s.vy = -speed * std::cos(angle);
            s.size = b.size * (0.55f + 0.45f * random());
            s.phase = random();
            s.age = -b.stagger * random();
            s.color = b.color;
            list.push_back(s);
        }
    }

    /// A firework: a rocket rising `height` pixels from (x, y) in `life` seconds (leaving after `delay`), bursting there
    /// into `count` sparks all around, which go `radius` pixels, droop by `gravity` and go out within `burstLife`.
    void rocket(float x, float y, float height, float life, float delay, float size, std::uint32_t color, int count, float radius,
                float burstLife, float gravity)
    {
        if (list.size() >= kMax)
            return;
        Spark s;
        s.x = x;
        s.y = y;
        s.life = life;
        s.vy = -kDrag * height / (1.0f - std::exp(-kDrag * life));
        s.size = size;
        s.phase = random();
        s.age = -delay;
        s.color = color;
        s.burst = count;
        s.burstRadius = radius;
        s.burstLife = burstLife;
        s.burstGravity = gravity;
        list.push_back(s);
    }

    /// Moves them on by `dt` seconds; those gone out, or risen above `ceiling`, are gone (a rocket at the end of its
    /// life bursts).
    void step(float dt, float ceiling)
    {
        const float drag = std::exp(-kDrag * dt);
        for (auto& s : list) {
            if (s.age < 0.0f) { // not left yet
                s.age += dt;
                continue;
            }
            s.x += (s.vx + std::sin(6.2831853f * (s.phase + 0.6f * s.age)) * 0.25f * s.size) * dt;
            s.y += s.vy * dt;
            s.vx *= drag;
            s.vy = s.vy * drag + s.gravity * dt;
            s.age += dt;
        }
        bursting.clear();
        for (const auto& s : list)
            if (s.burst > 0 && s.age >= s.life && s.y >= ceiling)
                bursting.push_back(s);
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Spark& s) { return s.age >= s.life || s.y < ceiling; }),
                   list.end());
        for (const auto& r : bursting)
            for (int i = 0; i < r.burst && list.size() < kMax; ++i) {
                Spark c;
                c.x = r.x;
                c.y = r.y;
                c.life = r.burstLife * (0.7f + 0.3f * random());
                const float angle = 6.2831853f * ((float) i + random()) / (float) r.burst;
                const float speed = kDrag * r.burstRadius * (0.25f + 0.75f * random()) / (1.0f - std::exp(-kDrag * c.life));
                c.vx = speed * std::cos(angle);
                c.vy = speed * std::sin(angle);
                c.gravity = r.burstGravity;
                c.size = r.size * (0.6f + 0.4f * random());
                c.phase = random();
                c.color = r.color;
                list.push_back(c);
            }
    }

    const std::vector<Spark>& sparks() const noexcept { return list; }

private:
    float random() noexcept // xorshift: the same sparks for the same strikes
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (float) (seed >> 8) * (1.0f / 16777216.0f);
    }

    std::uint32_t seed = 0x9E3779B9u;
    std::vector<Spark> list, bursting;
};

/// A powerful chord: its notes (struck within the window of the first), its first note's frame, how strong they are
/// together (the mean strength of their velocities, 0 to 1).
struct ChordHit {
    std::uint64_t frame = 0;
    std::vector<PlayedNote> notes;
    float strength = 0.0f;
};

/// Finds the powerful chords among the notes struck: at least `minNotes` struck within kWindowSeconds of the first,
/// their mean strength at least `threshold`. A chord is known once its window has passed (all its notes in, a few
/// hundredths of a second after the first); after one, another waits kRestSeconds (a fortissimo passage is not a
/// firework at every chord).
class ChordDetector {
public:
    static constexpr double kWindowSeconds = 0.035, kRestSeconds = 0.6;

    void reset()
    {
        group.clear();
        found.clear();
        lastFired = std::numeric_limits<std::uint64_t>::max();
    }
    void setRate(double rate)
    {
        window = (std::uint64_t) std::max(1.0, kWindowSeconds * rate);
        rest = (std::uint64_t) std::max(1.0, kRestSeconds * rate);
    }
    void configure(int notes, float strength)
    {
        minNotes = std::max(1, notes);
        threshold = strength;
    }

    /// A note struck (in order), its strength (0 to 1).
    void add(const PlayedNote& n, float strength)
    {
        if (!group.empty() && n.start > group.front().note.start + window)
            close();
        group.push_back({ n, strength });
    }
    /// The chord found once the frame shown is past its window, if any (`hit`).
    bool poll(std::uint64_t frame, ChordHit& hit)
    {
        if (!group.empty() && frame > group.front().note.start + window)
            close();
        if (found.empty())
            return false;
        hit = std::move(found.front());
        found.erase(found.begin());
        return true;
    }

private:
    struct Struck {
        PlayedNote note;
        float strength;
    };

    void close()
    {
        float sum = 0.0f;
        for (const auto& g : group)
            sum += g.strength;
        const float mean = sum / (float) group.size();
        const std::uint64_t first = group.front().note.start;
        const bool rested = lastFired == std::numeric_limits<std::uint64_t>::max() || first >= lastFired + rest;
        if ((int) group.size() >= minNotes && mean >= threshold && rested) {
            ChordHit hit;
            hit.frame = first;
            hit.strength = mean;
            for (const auto& g : group)
                hit.notes.push_back(g.note);
            found.push_back(std::move(hit));
            lastFired = first;
        }
        group.clear();
    }

    std::vector<Struck> group;
    std::vector<ChordHit> found;
    std::uint64_t lastFired = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t window = 1680, rest = 28800; // at 48 kHz
    int minNotes = 3;
    float threshold = 0.75f;
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
