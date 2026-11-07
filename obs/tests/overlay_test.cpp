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
        k.advance(10000, 1000);
        check(k.notes().size() == 1 && k.notes()[0].open(), "notes ended long ago are forgotten, the one held kept");
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
