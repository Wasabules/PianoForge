#include <pianoforge/midi_sequence.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

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

constexpr const char* kGeneralMidi[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1",
    "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
    "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)",
    "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar",
    "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)",
    "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello",
    "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani", "String Ensemble 1",
    "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1",
    "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon",
    "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)",
    "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)",
    "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)",
    "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
    "Gunshot",
};

/// Printable text of a meta event (names), control characters left out.
std::string text(const std::uint8_t* p, std::size_t n)
{
    std::string s;
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] >= 0x20)
            s += (char) p[i];
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

} // namespace

const char* generalMidiName(int program) noexcept
{
    return program >= 0 && program < 128 ? kGeneralMidi[program] : "";
}

std::string MidiSequence::Part::instrument() const
{
    return drums ? std::string("Drums") : std::string(generalMidiName(program));
}

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
    std::vector<std::string> trackNames, instrumentNames;
    std::map<int, int> firstProgram; // track x 16 + channel -> program
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
        trackNames.emplace_back();
        instrumentNames.emplace_back();
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
            if (status == 0xFF) { // meta event: tempo, names, end of track; running status cancelled
                const auto type = b.u8();
                const auto len = b.varlen();
                if (!b.has(len))
                    break;
                if (type == 0x51 && len == 3)
                    tempos.push_back({ tick, (double) ((data[b.at] << 16) | (data[b.at + 1] << 8) | data[b.at + 2]) });
                else if (type == 0x03 && trackNames.back().empty())
                    trackNames.back() = text(data + b.at, len);
                else if (type == 0x04 && instrumentNames.back().empty())
                    instrumentNames.back() = text(data + b.at, len);
                b.at += len;
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
            else if (type == 0xC0)
                firstProgram.emplace(track * 16 + (status & 0x0F), d1);
        }
        b.failed = false;
        b.at = end;
        ++track;
    }
    if (track == 0) {
        error = "no track in the MIDI file";
        return nullptr;
    }

    auto seq = std::make_shared<MidiSequence>();
    seq->tracks = track;
    // The parts: the (track, channel) pairs that hold notes, by track then channel.
    std::map<int, int> notesIn; // track x 16 + channel -> notes
    for (const auto& e : events)
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
            ++notesIn[e.track * 16 + (e.status & 0x0F)];
    std::map<int, int> partOf; // track x 16 + channel -> part
    for (const auto& [key, notes] : notesIn) {
        if ((int) seq->parts.size() >= MidiSequence::kMaxParts)
            break;
        MidiSequence::Part part;
        part.track = key / 16;
        part.channel = key % 16;
        part.notes = notes;
        part.drums = part.channel == 9;
        part.name = !trackNames[(std::size_t) part.track].empty() ? trackNames[(std::size_t) part.track]
                                                                  : instrumentNames[(std::size_t) part.track];
        if (const auto p = firstProgram.find(key); p != firstProgram.end())
            part.program = p->second;
        partOf[key] = (int) seq->parts.size();
        seq->parts.push_back(part);
    }
    // A controller on a track without notes (a pedal track) goes with the first part of its channel.
    std::array<int, 16> firstOfChannel;
    firstOfChannel.fill(-1);
    for (int i = (int) seq->parts.size() - 1; i >= 0; --i)
        firstOfChannel[(std::size_t) seq->parts[(std::size_t) i].channel] = i;
    auto partFor = [&](const TickEvent& e) {
        const int key = e.track * 16 + (e.status & 0x0F);
        if (const auto p = partOf.find(key); p != partOf.end())
            return p->second;
        return (e.status & 0xF0) == 0xB0 ? firstOfChannel[(std::size_t) (e.status & 0x0F)] : -1;
    };

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
        const int part = partFor(e);
        if (part < 0)
            continue; // no part to play it (a controller on a channel without notes, or past kMaxParts)
        const double seconds = mapSeconds + (double) (e.tick - mapTick) * micros / ticksPerQuarter * 1e-6;
        seq->events.push_back({ seconds, e.status, e.data1, e.data2, (std::uint8_t) part });
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
    transpose = 0;
    // Every part plays, but the percussion (it would sound as piano notes).
    std::uint64_t mask[2] = { 0, 0 };
    if (owned != nullptr)
        for (std::size_t i = 0; i < owned->parts.size(); ++i)
            if (!owned->parts[i].drums)
                mask[i >> 6] |= std::uint64_t { 1 } << (i & 63);
    enabled[0] = mask[0];
    enabled[1] = mask[1];
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

void SequencePlayer::setPartEnabled(int part, bool on) noexcept
{
    if (part < 0 || part >= MidiSequence::kMaxParts)
        return;
    const auto bit = std::uint64_t { 1 } << (part & 63);
    if (on)
        enabled[(std::size_t) (part >> 6)].fetch_or(bit);
    else
        enabled[(std::size_t) (part >> 6)].fetch_and(~bit);
}

bool SequencePlayer::partEnabled(int part) const noexcept
{
    return part >= 0 && part < MidiSequence::kMaxParts && ((enabled[(std::size_t) (part >> 6)].load() >> (part & 63)) & 1u) != 0;
}

void SequencePlayer::setTranspose(int semitones) noexcept
{
    transpose = std::clamp(semitones, -kMaxTranspose, kMaxTranspose);
}

void SequencePlayer::emit(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int offset) noexcept
{
    if (outCount < outCapacity)
        out[outCount++] = { (std::uint32_t) offset, status, data1, data2 };
}

void SequencePlayer::note(int channel, int type, int key, int value, int part) noexcept
{
    auto& count = notesOn[(std::size_t) (channel * 128 + key)];
    auto& mine = partNotes[(std::size_t) (part * 128 + key)];
    if (type == 0x90 && value > 0) {
        count = (std::uint8_t) std::min(255, count + 1);
        mine = (std::uint8_t) std::min(255, mine + 1);
    } else if (type == 0x80 || type == 0x90) {
        count = count > 0 ? (std::uint8_t) (count - 1) : (std::uint8_t) 0;
        mine = mine > 0 ? (std::uint8_t) (mine - 1) : (std::uint8_t) 0;
    }
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
    partNotes.fill(0);
}

void SequencePlayer::releasePart(int part, int at) noexcept
{
    const auto& p = current->parts[(std::size_t) part];
    const int channel = p.channel;
    for (int key = 0; key < 128; ++key) {
        auto& mine = partNotes[(std::size_t) (part * 128 + key)];
        auto& count = notesOn[(std::size_t) (channel * 128 + key)];
        for (; mine > 0; --mine) {
            emit((std::uint8_t) (0x80 | channel), (std::uint8_t) key, 0, at);
            count = count > 0 ? (std::uint8_t) (count - 1) : (std::uint8_t) 0;
        }
    }
    // Its channel's pedals, when no other part on that channel plays.
    for (std::size_t i = 0; i < current->parts.size(); ++i)
        if ((int) i != part && current->parts[i].channel == channel && enabledNow((int) i))
            return;
    if (pedal[(std::size_t) channel] != 0) {
        emit((std::uint8_t) (0xB0 | channel), 64, 0, at);
        pedal[(std::size_t) channel] = 0;
    }
    if (sostenuto[(std::size_t) channel] != 0) {
        emit((std::uint8_t) (0xB0 | channel), 66, 0, at);
        sostenuto[(std::size_t) channel] = 0;
    }
}

void SequencePlayer::restorePedals(int at) noexcept
{
    // The sustain pedal of each channel as the sequence has it at the position (events of the parts playing).
    std::array<int, 16> state;
    state.fill(-1);
    for (std::size_t i = 0; i < next && i < current->events.size(); ++i) {
        const auto& e = current->events[i];
        if ((e.status & 0xF0) == 0xB0 && e.data1 == 64 && enabledNow(e.part))
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
        appliedMask = { enabled[0].load(), enabled[1].load() };
        appliedTranspose = transpose.load();
    }
    if (current == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;
    // A new transposition: everything released, as a seek (the pedals come back below if it plays on).
    if (const int t = transpose.load(); t != appliedTranspose) {
        releaseAll(0);
        appliedTranspose = t;
        playing = false;
    }
    // Parts switched off: their notes (and their channel's pedals) released; parts switched on start with their
    // next notes.
    const std::array<std::uint64_t, 2> mask = { enabled[0].load(), enabled[1].load() };
    if (mask != appliedMask) {
        const auto before = appliedMask;
        appliedMask = mask;
        for (int part = 0; part < (int) current->parts.size(); ++part)
            if (((before[(std::size_t) (part >> 6)] >> (part & 63)) & 1u) && !enabledNow(part))
                releasePart(part, 0);
    }
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
        ++next;
        if (!enabledNow(e.part))
            continue;
        const int type = e.status & 0xF0, channel = e.status & 0x0F;
        const int at = std::clamp((int) std::lround(offset), 0, numSamples - 1);
        if (type == 0x80 || type == 0x90) {
            const int key = e.data1 + appliedTranspose;
            if (key < 0 || key > 127)
                continue; // transposed out of the MIDI range
            emit(e.status, (std::uint8_t) key, e.data2, at);
            note(channel, type, key, e.data2, e.part);
        } else {
            emit(e.status, e.data1, e.data2, at);
            if (type == 0xB0 && e.data1 == 64)
                pedal[(std::size_t) channel] = e.data2;
            else if (type == 0xB0 && e.data1 == 66)
                sostenuto[(std::size_t) channel] = e.data2;
        }
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
