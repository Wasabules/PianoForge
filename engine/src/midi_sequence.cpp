#include <pianoforge/midi_sequence.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace pf {

namespace {

/// Big-endian reader over the file's bytes; past the end, it fails (and reads zeros) rather than overruns.
struct Bytes {
    const std::uint8_t* p;
    std::size_t size, at = 0;
    bool failed = false;

    bool has(std::size_t n) const noexcept { return at + n <= size; }
    std::uint8_t u8() noexcept
    {
        if (!has(1)) {
            failed = true;
            return 0;
        }
        return p[at++];
    }
    std::uint32_t be(int n) noexcept
    {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 8) | u8();
        return v;
    }
    std::uint32_t varlen() noexcept // at most 4 bytes
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const auto b = u8();
            v = (v << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0)
                return v;
        }
        failed = true;
        return v;
    }
};

struct TickEvent {
    std::uint64_t tick;
    int track;
    std::size_t order; // position in the track
    std::uint8_t status, data1, data2;
};

struct Tempo {
    std::uint64_t tick;
    double microsPerQuarter;
};

} // namespace

std::shared_ptr<MidiSequence> readMidiFile(const std::uint8_t* data, std::size_t size, std::string& error)
{
    Bytes b { data, size };
    if (size < 14 || std::memcmp(data, "MThd", 4) != 0) {
        error = "not a MIDI file";
        return nullptr;
    }
    b.at = 4;
    const auto headerLength = b.be(4);
    const auto format = b.be(2);
    const auto trackCount = b.be(2);
    const auto division = b.be(2);
    if (headerLength < 6 || format > 2) {
        error = "not a MIDI file (format " + std::to_string(format) + ")";
        return nullptr;
    }
    if ((division & 0x8000u) != 0) {
        error = "SMPTE time is not supported";
        return nullptr;
    }
    if (division == 0) {
        error = "no ticks per quarter note";
        return nullptr;
    }
    b.at = 8 + headerLength;

    std::vector<TickEvent> events;
    std::vector<Tempo> tempos;
    int track = 0;
    while (track < (int) trackCount && b.has(8)) {
        char id[4];
        for (auto& c : id)
            c = (char) b.u8();
        const auto length = b.be(4);
        const std::size_t end = std::min(size, b.at + (std::size_t) length);
        if (std::memcmp(id, "MTrk", 4) != 0) { // an unknown chunk: skipped
            b.at = end;
            continue;
        }
        std::uint64_t tick = 0;
        std::uint8_t running = 0;
        std::size_t order = 0;
        while (b.at < end && !b.failed) {
            tick += b.varlen();
            std::uint8_t status = b.u8();
            if (status < 0x80) { // running status: this byte is the first data byte
                if (running == 0)
                    break;
                --b.at;
                status = running;
            }
            if (status == 0xFF) { // meta event: tempo, end of track; running status cancelled
                const auto type = b.u8();
                const auto len = b.varlen();
                if (type == 0x51 && len == 3 && b.has(3)) {
                    tempos.push_back({ tick, (double) b.be(3) });
                } else {
                    if (!b.has(len))
                        break;
                    b.at += len;
                }
                running = 0;
                if (type == 0x2F)
                    break;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) { // system exclusive: skipped; running status cancelled
                const auto len = b.varlen();
                if (!b.has(len))
                    break;
                b.at += len;
                running = 0;
                continue;
            }
            if (status >= 0xF0) // other system messages have no place in a file
                break;
            running = status;
            const int type = status & 0xF0;
            const auto d1 = (std::uint8_t) (b.u8() & 0x7F);
            const auto d2 = (type == 0xC0 || type == 0xD0) ? (std::uint8_t) 0 : (std::uint8_t) (b.u8() & 0x7F);
            if (b.failed)
                break;
            if (type == 0x80 || type == 0x90 || type == 0xB0)
                events.push_back({ tick, track, order++, status, d1, d2 });
        }
        b.failed = false;
        b.at = end;
        ++track;
    }
    if (track == 0) {
        error = "no track in the MIDI file";
        return nullptr;
    }

    // General MIDI percussion (channel 10) would sound as piano notes: left out, unless it is all the file has.
    bool drumNotes = false, otherNotes = false;
    for (const auto& e : events)
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
            ((e.status & 0x0F) == 9 ? drumNotes : otherNotes) = true;
    auto seq = std::make_shared<MidiSequence>();
    seq->tracks = track;
    seq->drumsLeftOut = drumNotes && otherNotes;
    if (seq->drumsLeftOut)
        events.erase(std::remove_if(events.begin(), events.end(), [](const TickEvent& e) { return (e.status & 0x0F) == 9; }),
                     events.end());

    // Ticks to seconds through the tempo map (500000 us per quarter note until the first tempo).
    std::stable_sort(events.begin(), events.end(), [](const TickEvent& a, const TickEvent& c) {
        return a.tick != c.tick ? a.tick < c.tick : a.track != c.track ? a.track < c.track : a.order < c.order;
    });
    std::stable_sort(tempos.begin(), tempos.end(), [](const Tempo& a, const Tempo& c) { return a.tick < c.tick; });
    const double ticksPerQuarter = (double) division;
    std::size_t t = 0;
    std::uint64_t mapTick = 0;
    double mapSeconds = 0.0, micros = 500000.0;
    seq->events.reserve(events.size());
    for (const auto& e : events) {
        while (t < tempos.size() && tempos[t].tick <= e.tick) {
            mapSeconds += (double) (tempos[t].tick - mapTick) * micros / ticksPerQuarter * 1e-6;
            mapTick = tempos[t].tick;
            micros = tempos[t].microsPerQuarter > 0.0 ? tempos[t].microsPerQuarter : micros;
            ++t;
        }
        const double seconds = mapSeconds + (double) (e.tick - mapTick) * micros / ticksPerQuarter * 1e-6;
        seq->events.push_back({ seconds, e.status, e.data1, e.data2 });
        seq->notes += (e.status & 0xF0) == 0x90 && e.data2 > 0 ? 1 : 0;
    }
    seq->length = seq->events.empty() ? 0.0 : seq->events.back().seconds;
    if (seq->notes == 0) {
        error = "the MIDI file holds no note";
        return nullptr;
    }
    return seq;
}

// ---- SequencePlayer ----

void SequencePlayer::load(std::shared_ptr<const MidiSequence> sequence)
{
    wantPlaying = false;
    // Once the pointer is withdrawn, a render() that starts sees it gone (sequentially consistent: the store comes
    // before the flag is read here, the flag is raised before the pointer is read there), and the one that may be
    // running is waited for: render() takes microseconds and comes back every block. (In a browser, everything
    // runs on the audio thread: nothing is ever running here.)
    published.store(nullptr);
#if !defined(__EMSCRIPTEN__)
    while (rendering.load())
        std::this_thread::yield();
#endif
    owned = std::move(sequence);
    positionSeconds = 0.0;
    seekTarget = 0.0;
    published.store(owned.get());
}

void SequencePlayer::play()
{
    if (owned == nullptr)
        return;
    if (positionSeconds.load() >= owned->length) // at the end: from the start again
        seek(0.0);
    wantPlaying = true;
}

void SequencePlayer::setSpeed(double factor)
{
    speed = std::clamp(factor, 0.25, 4.0);
}

void SequencePlayer::emit(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int offset) noexcept
{
    if (outCount < outCapacity)
        out[outCount++] = { (std::uint32_t) offset, status, data1, data2 };
}

void SequencePlayer::note(const MidiSequence::Event& e) noexcept
{
    const int channel = e.status & 0x0F, type = e.status & 0xF0;
    auto& count = notesOn[(std::size_t) (channel * 128 + (e.data1 & 0x7F))];
    if (type == 0x90 && e.data2 > 0)
        count = (std::uint8_t) std::min(255, count + 1);
    else if (type == 0x80 || type == 0x90)
        count = count > 0 ? (std::uint8_t) (count - 1) : (std::uint8_t) 0;
    else if (type == 0xB0 && e.data1 == 64)
        pedal[(std::size_t) channel] = e.data2;
    else if (type == 0xB0 && e.data1 == 66)
        sostenuto[(std::size_t) channel] = e.data2;
}

void SequencePlayer::releaseAll(int at) noexcept
{
    for (int channel = 0; channel < 16; ++channel) {
        for (int key = 0; key < 128; ++key)
            for (auto& count = notesOn[(std::size_t) (channel * 128 + key)]; count > 0; --count)
                emit((std::uint8_t) (0x80 | channel), (std::uint8_t) key, 0, at);
        if (pedal[(std::size_t) channel] != 0) {
            emit((std::uint8_t) (0xB0 | channel), 64, 0, at);
            pedal[(std::size_t) channel] = 0;
        }
        if (sostenuto[(std::size_t) channel] != 0) {
            emit((std::uint8_t) (0xB0 | channel), 66, 0, at);
            sostenuto[(std::size_t) channel] = 0;
        }
    }
}

void SequencePlayer::restorePedals(int at) noexcept
{
    // The sustain pedal of each channel as the sequence has it at the position (events before it).
    std::array<int, 16> state;
    state.fill(-1);
    for (std::size_t i = 0; i < next && i < current->events.size(); ++i) {
        const auto& e = current->events[i];
        if ((e.status & 0xF0) == 0xB0 && e.data1 == 64)
            state[(std::size_t) (e.status & 0x0F)] = e.data2;
    }
    for (int channel = 0; channel < 16; ++channel)
        if (state[(std::size_t) channel] > 0) {
            emit((std::uint8_t) (0xB0 | channel), 64, (std::uint8_t) state[(std::size_t) channel], at);
            pedal[(std::size_t) channel] = (std::uint8_t) state[(std::size_t) channel];
        }
}

int SequencePlayer::render(Event* events, int capacity, int numSamples, double sampleRate) noexcept
{
    rendering.store(true);
    out = events;
    outCapacity = capacity;
    outCount = 0;
    renderBlock(numSamples, sampleRate);
    rendering.store(false);
    return outCount;
}

int SequencePlayer::sounding(int key) const noexcept
{
    if (key < 0 || key > 127)
        return 0;
    int n = 0;
    for (int channel = 0; channel < 16; ++channel)
        n += notesOn[(std::size_t) (channel * 128 + key)];
    return n;
}

void SequencePlayer::renderBlock(int numSamples, double sampleRate) noexcept
{
    const auto* s = published.load();
    if (s != current) { // a new sequence (or none): what the old one had sounding is released (the old one is not read)
        releaseAll(0);
        current = s;
        next = 0;
        pos = 0.0;
        playing = false;
        seekSeen = seekSerial.load();
    }
    if (current == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;
    if (const auto serial = seekSerial.load(); serial != seekSeen) {
        seekSeen = serial;
        releaseAll(0);
        playing = false; // if it plays on, the pedals are put back below, as at a start
        pos = std::clamp(seekTarget.load(), 0.0, current->length);
        next = (std::size_t) (std::lower_bound(current->events.begin(), current->events.end(), pos,
                                               [](const MidiSequence::Event& e, double t) { return e.seconds < t; })
                              - current->events.begin());
    }
    if (!wantPlaying.load()) {
        if (playing.load()) { // paused: the notes and the pedals are released
            releaseAll(0);
            playing = false;
        }
        positionSeconds = pos;
        return;
    }
    if (!playing.load()) { // starting (or resuming): the pedals as the sequence has them here
        restorePedals(0);
        playing = true;
    }
    // The block covers [pos, pos + n / fs x speed) of the sequence's time; each event at its nearest sample, one that
    // rounds past the block waits for the next.
    const double factor = speed.load(), span = numSamples / sampleRate * factor;
    const auto& events = current->events;
    while (next < events.size()) {
        const auto& e = events[next];
        const double offset = (e.seconds - pos) / factor * sampleRate;
        if (offset >= numSamples - 0.5)
            break;
        emit(e.status, e.data1, e.data2, std::clamp((int) std::lround(offset), 0, numSamples - 1));
        note(e);
        ++next;
    }
    pos += span;
    if (next >= events.size() && pos >= current->length) {
        if (loop.load()) { // back to the start: what is sounding is released
            releaseAll(numSamples - 1);
            pos = 0.0;
            next = 0;
        } else {
            wantPlaying = false;
            playing = false;
            pos = current->length;
        }
    }
    positionSeconds = pos;
}

} // namespace pf
