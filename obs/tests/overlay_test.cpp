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
