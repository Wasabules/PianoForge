// SPDX-License-Identifier: AGPL-3.0-or-later

// The overlays' logic without OBS (obs/src/overlay.hpp): the keyboard's geometry, the keys and pedals at a frame,
// the notes kept for the trails, the text template.
#include "../src/overlay.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

pf::FeedEvent event(std::uint64_t frame, int status, int data1, int data2)
{
    pf::FeedEvent e;
    e.frame = frame;
    e.status = (std::uint8_t) status;
    e.data1 = (std::uint8_t) data1;
    e.data2 = (std::uint8_t) data2;
    return e;
}

} // namespace

int main()
{
    using namespace pfobs;
    // Geometry: 52 white keys edge to edge over the width, each black key between its two white neighbours.
    {
        const float width = 1040.0f;
        bool whites = true, blacks = true;
        float x = 0.0f;
        int count = 0;
        for (int k = kFirstKey; k <= kLastKey; ++k) {
            const auto s = keyShape(k, width);
            if (!s.black) {
                whites = whites && std::fabs(s.x - x) < 1e-3f && std::fabs(s.width - 20.0f) < 1e-3f;
                x += s.width;
                ++count;
            } else {
                const auto below = keyShape(k - 1, width), above = keyShape(k + 1, width);
                blacks = blacks && s.x > below.x && s.x + s.width < above.x + above.width && s.width < below.width;
            }
        }
        check(whites && count == 52 && std::fabs(x - width) < 1e-2f, "52 white keys, edge to edge over the width");
        check(blacks, "each black key between its two white neighbours, narrower");
        check(!keyShape(60, width).black && keyShape(61, width).black && keyShape(60, width).x == 23 * 20.0f,
              "middle C is the 24th white key, C# a black one");
    }

    // The keys and pedals at a frame: events applied only once the frame shown reaches theirs.
    {
        KeyboardState k;
        k.reset();
        k.add(event(100, 0x90, 60, 90));
        k.add(event(150, 0xB0, 64, 127));
        k.add(event(200, 0x80, 60, 0));
        k.add(event(300, 0x90, 64, 50));
        k.add(event(400, 0x90, 64, 0));
        k.advance(99, 1000);
        check(k.velocity(60) == 0 && k.waiting() == 5, "before its frame, an event waits");
        k.advance(160, 1000);
        check(k.velocity(60) == 90 && k.pedal(0) == 1.0f && k.notes().size() == 1 && k.notes()[0].open(),
              "a key down at its frame, the pedal too, the note open");
        k.advance(250, 1000);
        check(k.velocity(60) == 0 && !k.notes()[0].open() && k.notes()[0].end == 200, "the key up: the note ends there");
        k.advance(450, 1000);
        check(k.velocity(64) == 0 && k.notes().size() == 2 && k.notes()[1].end == 400, "a note-on at velocity 0 ends a note");
        k.add(event(500, 0x90, 64, 70));
        k.add(event(520, 0x90, 64, 80)); // struck again while held
        k.advance(600, 1000);
        check(k.notes().size() == 4 && k.notes()[2].end == 520 && k.notes()[3].open() && k.velocity(64) == 80,
              "struck again while held: the first note ends where the second starts");
        k.add(event(8000, 0xB0, 64, 0)); // the pedal up: the sounds it held end
        k.advance(10000, 1000);
        check(k.notes().size() == 1 && k.notes()[0].open(), "notes whose sound ended long ago are forgotten, the one held kept");
        k.add(event(10001, 0xB0, 123, 0));
        k.advance(10002, 1000);
        check(k.velocity(64) == 0 && !k.notes()[0].open(), "all notes off: every key up");
    }

    // The hands, estimated: a bass under a melody, a chord too wide for a hand, a fixed split; the keys' lights.
    {
        KeyboardState k;
        k.reset();
        k.setRate(48000.0);
        std::uint64_t t = 0;
        auto strike = [&](std::initializer_list<int> keys, std::uint64_t length) {
            int i = 0;
            for (const int key : keys)
                k.add(event(t + (std::uint64_t) (10 * i++), 0x90, key, 80)); // a chord: within a few samples
            for (const int key : keys)
                k.add(event(t + length, 0x80, key, 0));
            t += length + 100;
        };
        // An alternating bass (C2, G2) under a melody (E5, G5, then down to C5).
        for (int bar = 0; bar < 4; ++bar) {
            strike({ 36 }, 4800);
            strike({ 76 }, 4800);
            strike({ 43 }, 4800);
            strike({ 79 - bar }, 4800);
        }
        k.advance(t, 1000000);
        bool alternating = true;
        for (const auto& n : k.notes())
            alternating = alternating && n.hand == (n.key < 60 ? kLeftHand : kRightHand);
        check(alternating && k.notes().size() == 16, "a bass under a melody: the bass to the left hand, the melody to the right");
        // A chord over three octaves and a half: split at its widest interval (C3 to E4).
        strike({ 36, 43, 48, 64, 67, 72 }, 9600);
        k.advance(t, 1000000);
        bool split = true;
        for (const auto& n : k.notes())
            if (n.start >= t - 9600 - 100 - 100)
                split = split && n.hand == (n.key <= 48 ? kLeftHand : kRightHand);
        check(split, "a chord too wide for a hand: split at its widest interval, the lower part to the left hand");
        // A fixed split: middle C and above, the right hand.
        KeyboardState f;
        f.reset();
        f.setFixedSplit(true, 60);
        f.add(event(0, 0x90, 59, 80));
        f.add(event(5000, 0x90, 60, 80));
        f.advance(10000, 100000);
        check(f.light(59).hand == kLeftHand && f.light(60).hand == kRightHand, "a fixed split: under it the left hand, from it the right");
        // The lights: pressed, released, the velocity kept for the afterglow.
        KeyboardState l;
        l.reset();
        l.add(event(100, 0x90, 64, 90));
        l.add(event(900, 0x80, 64, 0));
        l.advance(500, 100000);
        const auto held = l.light(64);
        l.advance(1000, 100000);
        const auto after = l.light(64);
        check(held.velocity == 90 && held.pressed == 100 && after.velocity == 0 && after.released == 900 && after.lastVelocity == 90
                  && after.struck && !l.light(65).struck,
              "a key's light: pressed at its frame, released at its frame, its velocity kept");
    }

    // Colours by hue: red, green, blue at full saturation; grey without; light and dark.
    {
        check(hslColor(0.0, 1.0, 0.5) == 0xFF0000FFu && hslColor(120.0, 1.0, 0.5) == 0xFF00FF00u && hslColor(240.0, 1.0, 0.5) == 0xFFFF0000u
                  && hslColor(360.0, 1.0, 0.5) == 0xFF0000FFu && hslColor(-120.0, 1.0, 0.5) == 0xFFFF0000u,
              "hues: 0 red, 120 green, 240 blue, around the circle both ways");
        check(hslColor(200.0, 0.0, 0.5) == 0xFF808080u && hslColor(30.0, 1.0, 1.0) == 0xFFFFFFFFu && hslColor(30.0, 1.0, 0.0) == 0xFF000000u,
              "no saturation: grey; full lightness white, none black");
        const std::uint32_t orange = hslColor(25.0, 0.9, 0.55);
        check((orange & 0xFF) > ((orange >> 8) & 0xFF) && ((orange >> 8) & 0xFF) > ((orange >> 16) & 0xFF),
              "an orange: more red than green, more green than blue");
        check(mixColor(0xFF000000u, 0xFFFFFFFFu, 0.5) == 0xFF808080u && mixColor(0xFF102030u, 0xFFFFFFFFu, 0.0) == 0xFF102030u,
              "colours mixed");
    }

    // The strikes, for the sparkles: each note struck once, then gone from the list.
    {
        KeyboardState k;
        k.reset();
        k.add(event(100, 0x90, 60, 90));
        k.add(event(110, 0x90, 64, 60));
        k.add(event(300, 0x80, 60, 0));
        k.advance(500, 100000);
        std::vector<PlayedNote> struck;
        k.takeStrikes(struck);
        const bool first = struck.size() == 2 && struck[0].key == 60 && struck[0].velocity == 90 && struck[1].key == 64;
        k.takeStrikes(struck);
        check(first && struck.empty(), "the notes struck, each once");
        for (int i = 0; i < 400; ++i)
            k.add(event(1000 + (std::uint64_t) i, 0x90, 60 + i % 12, 80));
        k.advance(2000, 100000);
        k.takeStrikes(struck);
        check(struck.size() == 256 && struck.back().start == 1399, "unread strikes: the newest kept");
    }

    // Sparkles: a stream rises from the key, no higher than asked, in the direction asked, and goes out.
    {
        SparkBurst b;
        b.x = 100.0f;
        b.y = 200.0f;
        b.width = 20.0f;
        b.count = 12;
        b.height = 80.0f;
        b.tilt = 0.0f;
        b.spread = 0.0f;
        b.stagger = 0.0f;
        b.size = 10.0f;
        b.life = 1.0f;
        Sparkles s;
        s.burst(b);
        bool upright = s.sparks().size() == 12;
        for (const auto& p : s.sparks())
            upright = upright && p.vy < 0.0f && std::fabs(p.vx) < 1e-3f && std::fabs(p.x - 100.0f) <= 10.0f && p.life <= 1.0f;
        float highest = 200.0f, widest = 0.0f;
        int steps = 0;
        while (!s.sparks().empty() && steps < 200) {
            s.step(0.01f, 0.0f);
            ++steps;
            for (const auto& p : s.sparks()) {
                highest = std::min(highest, p.y);
                widest = std::max(widest, std::fabs(p.x - 100.0f));
            }
        }
        check(upright && widest < 10.0f + 2.0f, "straight up: the sparks leave upward, within the key's width");
        check(highest < 200.0f - 0.5f * 80.0f * 0.8f && highest > 200.0f - 80.0f * 1.02f - 1.0f,
              "they rise toward the height asked, never above it");
        check(s.sparks().empty() && steps <= 101, "they go out within their life");

        b.tilt = 0.5f; // leaning to the right
        b.spread = 0.1f;
        s.burst(b);
        bool right = !s.sparks().empty();
        for (const auto& p : s.sparks())
            right = right && p.vx > 0.0f && p.vy < 0.0f;
        s.clear();
        b.tilt = 0.0f;
        b.spread = 1.2f; // a wide fountain: both ways
        s.burst(b);
        bool left = false, rightToo = false;
        for (const auto& p : s.sparks()) {
            left = left || p.vx < 0.0f;
            rightToo = rightToo || p.vx > 0.0f;
        }
        check(right && left && rightToo, "a tilt leans them all one way; a wide spread throws them both ways");

        s.clear();
        b.spread = 0.0f;
        b.stagger = 0.5f;
        s.burst(b);
        s.step(0.25f, 0.0f);
        s.step(0.01f, 0.0f);
        int moved = 0, waiting = 0;
        bool unseen = true;
        for (const auto& p : s.sparks()) {
            if (p.y < 200.0f) {
                ++moved;
            } else {
                ++waiting;
                unseen = unseen && (p.age >= 0.0f || p.brightness() == 0.0f);
            }
        }
        check(moved > 0 && waiting > 0 && unseen && moved + waiting == 12,
              "staggered: some have left, the others wait, unseen, at the key");

        s.clear();
        b.stagger = 0.0f;
        b.height = 300.0f;
        b.life = 5.0f;
        s.burst(b);
        for (int i = 0; i < 100; ++i)
            s.step(0.02f, 150.0f);
        check(s.sparks().empty(), "risen above the ceiling: gone");
        b.height = 80.0f;
        b.life = 1.0f;
        for (int i = 0; i < 100; ++i)
            s.burst(b);
        Spark young;
        young.life = 1.0f;
        young.age = 0.03f;
        Spark old = young;
        old.age = 0.9f;
        check(s.sparks().size() == Sparkles::kMax && young.brightness() > 0.4f && old.brightness() < 0.25f,
              "no more than kMax at once; lit at once, going out at the end");
    }

    // How long notes sound: to the key's release, or, held by a pedal, until the pedal is up or the key struck again.
    {
        KeyboardState k;
        k.reset();
        k.add(event(100, 0x90, 60, 90));
        k.add(event(200, 0x80, 60, 0)); // no pedal: its sound ends with the key
        k.add(event(300, 0xB0, 64, 127));
        k.add(event(400, 0x90, 64, 90));
        k.add(event(500, 0x80, 64, 0)); // the sustain pedal down: it rings on
        k.advance(600, 100000);
        const bool rings = k.notes().size() == 2 && !k.notes()[0].ringing() && k.notes()[0].sound == 200 && k.notes()[1].ringing()
                           && k.notes()[1].end == 500;
        k.add(event(700, 0xB0, 64, 0));
        k.advance(800, 100000);
        check(rings && !k.notes()[1].ringing() && k.notes()[1].sound == 700, "the sustain pedal holds a note's sound until it is up");
        k.add(event(900, 0xB0, 64, 127));
        k.add(event(1000, 0x90, 67, 90));
        k.add(event(1100, 0x80, 67, 0));
        k.add(event(1200, 0x90, 67, 80)); // struck again: the sound before ends
        k.advance(1300, 100000);
        const auto& first = k.notes()[2];
        check(first.key == 67 && first.sound == 1200 && k.notes()[3].open(), "a key struck again ends the sound the pedal held");
        k.add(event(1300, 0x80, 67, 0));
        k.add(event(1400, 0xB0, 64, 0));
        // The sostenuto holds the keys down when it goes down, and only those.
        k.add(event(1500, 0x90, 48, 90));
        k.add(event(1600, 0xB0, 66, 127));
        k.add(event(1700, 0x80, 48, 0));
        k.add(event(1800, 0x90, 50, 90));
        k.add(event(1900, 0x80, 50, 0));
        k.advance(2000, 100000);
        const PlayedNote* c3 = nullptr;
        const PlayedNote* d3 = nullptr;
        for (const auto& n : k.notes()) {
            c3 = n.key == 48 ? &n : c3;
            d3 = n.key == 50 ? &n : d3;
        }
        const bool held = c3 != nullptr && d3 != nullptr && c3->ringing() && d3->sound == 1900;
        k.add(event(2100, 0xB0, 66, 0));
        k.advance(2200, 100000);
        for (const auto& n : k.notes())
            c3 = n.key == 48 ? &n : c3;
        check(held && c3->sound == 2100, "the sostenuto holds the keys down as it goes down, until it is up");
        k.advance(2200 + 200000, 1000);
        check(k.notes().empty(), "notes are forgotten once their sound has ended long ago");
    }

    // The part of the keyboard shown: a range's keys across the width; the view following the playing.
    {
        const KeyView all;
        const KeyView v61 = viewOf(36, 96); // C2 to C7: 36 white keys
        const auto c2 = keyShape(36, 720.0f, v61), c7 = keyShape(96, 720.0f, v61), a0 = keyShape(21, 720.0f, v61);
        check(v61.first == 9.0f && v61.count == 36.0f && std::fabs(c2.x) < 1e-3f && std::fabs(c2.width - 20.0f) < 1e-3f
                  && std::fabs(c7.x + c7.width - 720.0f) < 1e-3f && a0.x + a0.width <= 0.0f
                  && std::fabs(keyShape(60, 1040.0f, all).x - keyShape(60, 1040.0f).x) < 1e-3f,
              "a range of keys: its first at the left edge, its last at the right, the others outside");
        const KeyView sharp = viewOf(37, 94); // black ends: widened to the white keys beside them
        check(sharp.first == 9.0f && sharp.count == 35.0f, "a range ending on black keys takes the white keys beside them");

        ViewFollower f;
        f.reset(all);
        for (int i = 0; i < 100; ++i)
            f.step(72, 84, 0.05f, 21.0f); // an octave around C6: three octaves shown at least
        const KeyView v = f.view();
        const float c5 = (float) whitesBefore(72), c6 = (float) whitesBefore(84) + 1.0f;
        check(std::fabs(v.count - 21.0f) < 0.1f && v.first < c5 && v.first + v.count > c6, "following: at least the width asked, around the keys played");
        const KeyView before = f.view();
        for (int i = 0; i < 20; ++i)
            f.step(74, 81, 0.05f, 21.0f); // inside, not much narrower: it stays
        check(std::fabs(f.view().first - before.first) < 0.01f, "keys played inside the view do not move it");
        for (int i = 0; i < 100; ++i)
            f.step(21, 30, 0.05f, 21.0f); // the bass: it glides there, within the keyboard
        check(f.view().first < 0.05f && f.view().count >= 20.9f, "keys played outside move it there, never beyond the keyboard");
    }

    // The melody: the highest note of each group the right hand strikes.
    {
        std::vector<PlayedNote> notes;
        auto add = [&](int key, int hand, std::uint64_t at) {
            PlayedNote n;
            n.key = key;
            n.hand = hand;
            n.start = at;
            notes.push_back(n);
        };
        add(48, kLeftHand, 1000);
        add(60, kRightHand, 1000);
        add(67, kRightHand, 1010);
        add(64, kRightHand, 1020); // a chord: its top, 67
        add(72, kRightHand, 5000);
        add(40, kLeftHand, 6000);
        add(71, kRightHand, 9000);
        std::vector<PlayedNote> melody;
        melodyOf(notes, 1680, melody);
        check(melody.size() == 3 && melody[0].key == 67 && melody[1].key == 72 && melody[2].key == 71,
              "the melody: the right hand's top notes, in order, the left hand left out");
    }

    // A firework: the rocket rises to its height, then bursts all around it.
    {
        Sparkles s;
        s.rocket(500.0f, 300.0f, 100.0f, 0.4f, 0.0f, 8.0f, 0xFFFFFFFFu, 20, 40.0f, 0.8f, 30.0f);
        bool rising = s.sparks().size() == 1 && s.sparks()[0].vy < 0.0f;
        float apex = 300.0f;
        for (int i = 0; i < 60 && s.sparks().size() == 1; ++i) {
            s.step(0.01f, 0.0f);
            if (s.sparks().size() == 1)
                apex = s.sparks()[0].y;
        }
        const auto& burst = s.sparks();
        bool around = burst.size() == 20, left = false, right = false, up = false, down = false;
        for (const auto& c : burst) {
            left = left || c.vx < -10.0f;
            right = right || c.vx > 10.0f;
            up = up || c.vy < -10.0f;
            down = down || c.vy > 10.0f;
            around = around && std::fabs(c.x - 500.0f) < 2.0f && std::fabs(c.y - apex) < 4.0f && c.burst == 0;
        }
        check(rising && std::fabs(apex - 200.0f) < 4.0f && around && left && right && up && down,
              "a firework: the rocket rises to its height, and bursts there into sparks going every way");
        float lowest = 0.0f, farthest = 0.0f;
        for (int i = 0; i < 100 && !s.sparks().empty(); ++i) {
            s.step(0.01f, 0.0f);
            for (const auto& c : s.sparks()) {
                farthest = std::max(farthest, std::fabs(c.x - 500.0f));
                lowest = std::max(lowest, c.y - apex);
            }
        }
        check(s.sparks().empty() && farthest > 25.0f && farthest < 45.0f && lowest > 0.0f,
              "its sparks spread about the radius asked, droop, and go out");
    }

    // Powerful chords: enough notes together, strong enough, known once their window has passed (all their notes in);
    // not an arpeggio, not too soon after another.
    {
        ChordDetector d;
        d.reset();
        d.setRate(48000.0);
        d.configure(3, 0.75f);
        auto note = [](int key, std::uint64_t at) {
            PlayedNote n;
            n.key = key;
            n.velocity = 100;
            n.start = at;
            return n;
        };
        ChordHit hit;
        d.add(note(48, 1000), 0.9f);
        d.add(note(52, 1200), 0.9f);
        d.add(note(55, 1500), 0.8f);
        const bool waiting = !d.poll(2000, hit); // still within its window: a fourth note may come
        d.add(note(60, 1900), 1.0f);
        const bool found = d.poll(2700, hit) && hit.frame == 1000 && hit.notes.size() == 4
                           && std::fabs(hit.strength - 3.6f / 4.0f) < 1e-4f && !d.poll(2800, hit);
        check(waiting && found, "a strong chord: known once its window has passed, all its notes in, once");
        for (int k = 0; k < 4; ++k) // 0.3 s later: resting
            d.add(note(60 + k, 1000 + 14400), 1.0f);
        const bool soon = !d.poll(1000 + 14400 + 2000, hit);
        for (int k = 0; k < 4; ++k) // 0.7 s later
            d.add(note(60 + k, 1000 + 33600 + (std::uint64_t) k), 1.0f);
        const bool again = d.poll(1000 + 33600 + 2000, hit) && hit.notes.size() == 4;
        check(soon && again, "another one too soon is not announced; once rested, it is");
        for (int k = 0; k < 4; ++k)
            d.add(note(40 + k, 200000 + (std::uint64_t) k), 0.5f);
        const bool soft = !d.poll(210000, hit);
        bool spread = true;
        for (int k = 0; k < 4; ++k) { // an arpeggio: 50 ms apart
            d.add(note(40 + k, 300000 + 2400 * (std::uint64_t) k), 1.0f);
            spread = spread && !d.poll(300000 + 2400 * (std::uint64_t) k + 100, hit);
        }
        spread = spread && !d.poll(320000, hit);
        d.add(note(40, 400000), 1.0f);
        d.add(note(41, 400001), 1.0f);
        const bool few = !d.poll(410000, hit);
        d.add(note(40, 500000), 1.0f); // a new group closes the one before
        d.add(note(44, 500010), 1.0f);
        d.add(note(47, 500020), 1.0f);
        d.add(note(80, 520000), 1.0f);
        const bool closed = d.poll(520001, hit) && hit.notes.size() == 3 && hit.frame == 500000;
        check(soft && spread && few && closed, "a soft chord, an arpeggio, two notes: nothing; a note after the window closes the chord");
    }

    // The text template.
    {
        pf::FeedInfo info;
        std::strcpy(info.engine, "Modal");
        std::strcpy(info.preset, "Concert");
        std::strcpy(info.room, "Concert hall");
        std::strcpy(info.file, "Clair de lune");
        info.filePlaying = 1;
        const std::string a = formatInfo("Now: {file} ({state})\n{preset} in {room}{recording}", info, " REC", "playing", "paused");
        check(a == "Now: Clair de lune (playing)\nConcert in Concert hall", "the fields replaced");
        info.file[0] = 0;
        info.recording = 1;
        const std::string b = formatInfo("Now: {file}\n{preset} in {room}{recording}", info, " REC", "playing", "paused");
        check(b == "Now: \nConcert in Concert hall REC", "a line with text of its own stays");
        const std::string c = formatInfo("{file} - {state}\n{preset}", info, "", "playing", "paused");
        check(c == "Concert", "a line its empty fields leave blank disappears");
    }

    std::printf(failures ? "%d failure(s)\n" : "All overlay tests passed\n", failures);
    return failures ? 1 : 0;
}
