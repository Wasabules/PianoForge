// SPDX-License-Identifier: AGPL-3.0-or-later

// Piano gestures (pianoforge/piano_gestures.hpp): chords of keys no piece asks for, double taps, music that must
// not trigger them, learning.
#include <pianoforge/piano_gestures.hpp>

#include <cstdio>

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
    using pf::PianoGesture;
    using pf::PianoGestures;
    enum { kRecord, kSave, kPlay };
    const auto low = PianoGesture::parse("chord:21,22,23"), twice = PianoGesture::parse("double:108");
    check(low.kind == PianoGesture::Kind::chord && low.keys == std::vector<int> { 21, 22, 23 } && low.text() == "chord:21,22,23"
              && low.describe() == "A0 + A#0 + B0 together" && twice.describe() == "C8 twice"
              && PianoGesture::parse("chord:60").empty() && PianoGesture::parse("double:1,2").empty() && PianoGesture::parse("").empty(),
          "gestures read and written as text, described by their keys; one-key chords and two-key taps refused");

    PianoGestures g;
    g.set(kRecord, low);
    g.set(kSave, twice);
    g.set(kPlay, PianoGesture::parse("double:21"));
    double start = 0.0;
    // The chord: three keys within 0.1 s, held.
    int a = g.noteOn(22, 10.00, start);
    a = a < 0 ? g.noteOn(21, 10.04, start) : a;
    a = a < 0 ? g.noteOn(23, 10.09, start) : a;
    check(a == kRecord && start == 10.00, "three keys struck together: the action, at the time of the first");
    for (const int k : { 21, 22, 23 })
        g.noteOff(k, 10.3);
    // Too slow, or with another key: not the gesture.
    int b = g.noteOn(21, 20.0, start);
    b = b < 0 ? g.noteOn(22, 20.1, start) : b;
    b = b < 0 ? g.noteOn(23, 20.2, start) : b;
    for (const int k : { 21, 22, 23 })
        g.noteOff(k, 20.5);
    int c = g.noteOn(21, 30.0, start);
    c = c < 0 ? g.noteOn(22, 30.02, start) : c;
    c = c < 0 ? g.noteOn(24, 30.03, start) : c;
    c = c < 0 ? g.noteOn(23, 30.05, start) : c;
    for (const int k : { 21, 22, 23, 24 })
        g.noteOff(k, 30.5);
    check(b < 0 && c < 0, "spread over 0.2 s, or with a fourth key: not the gesture");
    // A double tap: struck, released, struck again within 0.35 s.
    int d = g.noteOn(108, 40.0, start);
    g.noteOff(108, 40.08);
    d = d < 0 ? g.noteOn(108, 40.25, start) : d;
    g.noteOff(108, 40.3);
    check(d == kSave && start == 40.0, "a key struck twice quickly: the action, at the first strike");
    // Music: a repeated note too slow, a trill with another key in between, a note held and struck again.
    int e = g.noteOn(108, 50.0, start);
    g.noteOff(108, 50.2);
    e = e < 0 ? g.noteOn(108, 50.5, start) : e;
    g.noteOff(108, 50.6);
    int f = g.noteOn(21, 60.0, start);
    g.noteOff(21, 60.05);
    f = f < 0 ? g.noteOn(23, 60.1, start) : f;
    g.noteOff(23, 60.15);
    f = f < 0 ? g.noteOn(21, 60.2, start) : f;
    g.noteOff(21, 60.25);
    check(e < 0 && f < 0, "music: a note repeated slowly, a trill (another key in between): nothing");
    // A melody over the low keys does not make the chord either.
    int m = -1;
    double t = 70.0;
    for (const int k : { 21, 23, 22, 24, 21, 22, 23 }) {
        m = m < 0 ? g.noteOn(k, t, start) : m;
        g.noteOff(k, t + 0.1);
        t += 0.2;
    }
    check(m < 0, "a melody over the same keys, one at a time: nothing");

    // Learning: the next gesture played.
    g.learn();
    int during = g.noteOn(60, 80.0, start);
    during = during < 0 ? g.noteOn(64, 80.03, start) : during;
    during = during < 0 ? g.noteOn(67, 80.06, start) : during;
    const auto early = g.takeLearned(80.2);
    const auto chord = g.takeLearned(80.6);
    for (const int k : { 60, 64, 67 })
        g.noteOff(k, 80.7);
    g.learn();
    g.noteOn(72, 90.0, start);
    g.noteOff(72, 90.05);
    const auto single = g.takeLearned(90.6); // one note: not a gesture, learning goes on
    g.noteOn(72, 91.0, start);
    g.noteOff(72, 91.05);
    g.noteOn(72, 91.2, start);
    g.noteOff(72, 91.25);
    const auto tap = g.takeLearned(91.8);
    check(during < 0 && !early && chord && chord->text() == "chord:60,64,67" && !single && tap && tap->text() == "double:72"
              && !g.learning(),
          "learning: nothing recognized meanwhile; a chord taken once the keys are still, a single note ignored, a double tap");
    check(g.owner(twice, kRecord) == kSave && g.owner(twice, kSave) == -1, "the action a gesture already belongs to");

    std::printf(failures ? "%d failure(s)\n" : "All piano gesture tests passed\n", failures);
    return failures ? 1 : 0;
}
