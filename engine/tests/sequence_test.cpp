// MIDI file reader and sequence player tests (shared by the lab and the browser module). Exits non-zero on a failure.
#include <pianoforge/midi_sequence.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

using Bytes = std::vector<std::uint8_t>;

void be(Bytes& b, std::uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; --i)
        b.push_back((std::uint8_t) (v >> (8 * i)));
}

void varlen(Bytes& b, std::uint32_t v)
{
    std::uint8_t tmp[4];
    int n = 0;
    do {
        tmp[n++] = (std::uint8_t) (v & 0x7F);
        v >>= 7;
    } while (v != 0);
    while (n-- > 0)
        b.push_back((std::uint8_t) (tmp[n] | (n > 0 ? 0x80 : 0)));
}

Bytes track(const Bytes& body)
{
    Bytes b { 'M', 'T', 'r', 'k' };
    be(b, (std::uint32_t) body.size(), 4);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

/// Format 1, 480 ticks per quarter: a tempo map (120, then 100 beats per minute from tick 960) and a track with a
/// system exclusive message, running status, a note-on of velocity 0 as a note-off, a program change and a drum note.
Bytes testFile()
{
    Bytes t0;
    varlen(t0, 0), t0.insert(t0.end(), { 0xFF, 0x51, 3 }), be(t0, 500000, 3);
    varlen(t0, 960), t0.insert(t0.end(), { 0xFF, 0x51, 3 }), be(t0, 600000, 3);
    varlen(t0, 0), t0.insert(t0.end(), { 0xFF, 0x2F, 0 });
    Bytes t1;
    varlen(t1, 0), t1.insert(t1.end(), { 0xF0, 3, 0x7E, 0x7F, 0xF7 });  // sysex
    varlen(t1, 480), t1.insert(t1.end(), { 0x90, 60, 90 });             // C4 at 0.5 s
    varlen(t1, 0), t1.insert(t1.end(), { 64, 70 });                     // E4, running status
    varlen(t1, 480), t1.insert(t1.end(), { 60, 0 });                    // C4 off (velocity 0) at 1.0 s
    varlen(t1, 480), t1.insert(t1.end(), { 0xB0, 64, 127 });            // pedal at 1.6 s
    varlen(t1, 0), t1.insert(t1.end(), { 0xC0, 5 });                    // program change (one data byte)
    varlen(t1, 60), t1.insert(t1.end(), { 0x99, 36, 100 });             // a drum note (channel 10)
    varlen(t1, 420), t1.insert(t1.end(), { 0x90, 67, 80 });             // G4 at 2.2 s
    varlen(t1, 480), t1.insert(t1.end(), { 0x80, 67, 0 });              // G4 off at 2.8 s
    varlen(t1, 0), t1.insert(t1.end(), { 0xB0, 64, 0 });                // pedal up at 2.8 s
    varlen(t1, 0), t1.insert(t1.end(), { 0xFF, 0x2F, 0 });
    Bytes b { 'M', 'T', 'h', 'd' };
    be(b, 6, 4), be(b, 1, 2), be(b, 2, 2), be(b, 480, 2);
    const auto a = track(t0), c = track(t1);
    b.insert(b.end(), a.begin(), a.end());
    b.insert(b.end(), c.begin(), c.end());
    return b;
}

struct Played {
    long long sample;
    int status, data1, data2;
};

std::vector<Played> play(pf::SequencePlayer& player, double seconds, long long& clock, double rate = 48000.0, int block = 128)
{
    std::vector<Played> out;
    pf::Event events[512];
    for (long long end = clock + (long long) (seconds * rate); clock < end; clock += block) {
        const int n = player.render(events, 512, block, rate);
        for (int i = 0; i < n; ++i)
            out.push_back({ clock + events[i].offset, events[i].status, events[i].data1, events[i].data2 });
    }
    return out;
}

std::string text(const std::vector<Played>& events)
{
    std::string s;
    char buffer[32];
    for (const auto& e : events) {
        std::snprintf(buffer, sizeof buffer, "%s%02x %d %d", s.empty() ? "" : ", ", e.status, e.data1, e.data2);
        s += buffer;
    }
    return s;
}

} // namespace

int main()
{
    const auto bytes = testFile();
    std::string error;
    const auto seq = pf::readMidiFile(bytes.data(), bytes.size(), error);
    const double expected[] = { 0.5, 0.5, 1.0, 1.6, 2.2, 2.8, 2.8 };
    bool times = seq != nullptr && seq->events.size() == 7;
    for (std::size_t i = 0; times && i < 7; ++i)
        times = std::abs(seq->events[i].seconds - expected[i]) < 1e-9;
    check(times && seq->notes == 3 && seq->tracks == 2 && seq->drumsLeftOut && std::abs(seq->length - 2.8) < 1e-9,
          "a format-1 file: tempo map, running status, sysex, program change, drums left out, the times exact");
    check(seq != nullptr && seq->events[2].status == 0x90 && seq->events[2].data2 == 0 && seq->events[1].data1 == 64,
          "running status read as such, a note-on of velocity 0 kept (a note-off)");

    Bytes smpte = bytes;
    smpte[12] = 0xE7, smpte[13] = 0x28; // -25 frames per second, 40 ticks per frame
    check(pf::readMidiFile(smpte.data(), smpte.size(), error) == nullptr && error.find("SMPTE") != std::string::npos,
          "SMPTE time refused");
    const std::uint8_t junk[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 0, 0 };
    check(pf::readMidiFile(junk, sizeof junk, error) == nullptr, "not a MIDI file: refused");
    check(pf::readMidiFile(bytes.data(), 30, error) == nullptr, "a truncated file without a note: refused, no overrun");

    // The player, to the sample, at 100 % and 150 %.
    for (const double speed : { 1.0, 1.5 }) {
        pf::SequencePlayer player;
        player.load(seq);
        player.setSpeed(speed);
        player.play();
        long long clock = 0;
        const auto got = play(player, 3.0, clock);
        double worst = got.size() == 7 ? 0.0 : 1e9;
        for (std::size_t i = 0; i < got.size() && i < 7; ++i)
            worst = std::max(worst, std::abs((double) got[i].sample - expected[i] / speed * 48000.0));
        std::printf("      at %.0f %%: %d events, at most %.2f samples from their time\n", speed * 100, (int) got.size(), worst);
        check(worst <= 0.5 && !player.wantsToPlay() && std::abs(player.position() - 2.8) < 0.01,
              "every event on its sample, stopped at the end");
    }

    // A seek releases what sounds and puts the pedal back; a pause releases the pedal; playing on puts it back.
    {
        pf::SequencePlayer player;
        player.load(seq);
        player.play();
        long long clock = 0;
        play(player, 1.7, clock);       // C4 released, E4 sounding, the pedal down
        player.seek(2.0);
        const auto seek = play(player, 0.1, clock);
        player.pause();
        const auto pause = play(player, 0.1, clock);
        player.play();
        const auto resume = play(player, 0.8, clock);
        const std::string s = text(seek), p = text(pause), r = text(resume);
        std::printf("      seek [%s], pause [%s], resume [%s]\n", s.c_str(), p.c_str(), r.c_str());
        check(s == "80 64 0, b0 64 0, b0 64 127" && p == "b0 64 0" && r == "b0 64 127, 90 67 80, 80 67 0, b0 64 0",
              "seek, pause and resume: releases and pedals as the sequence has them");
        check(player.sounding(67) == 0 && player.sounding(64) == 0, "nothing left sounding at the end");
    }

    // Loop: back to the start, everything released first.
    {
        pf::SequencePlayer player;
        player.load(seq);
        player.setLoop(true);
        player.play();
        long long clock = 0;
        const auto got = play(player, 3.4, clock);
        int strikes = 0;
        for (const auto& e : got)
            strikes += e.status == 0x90 && e.data1 == 60 && e.data2 > 0 ? 1 : 0;
        check(strikes == 2 && player.wantsToPlay(), "loop: the sequence starts again");
    }
    return failures ? 1 : 0;
}
