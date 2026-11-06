#include "MidiFilePlayer.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace pf::app {

std::shared_ptr<const MidiFilePlayer::Sequence> MidiFilePlayer::read(const juce::File& file, juce::String& error)
{
    juce::MidiFile midi;
    {
        juce::FileInputStream in(file);
        // false: no note-off added before a re-strike of a note still held (the file is played as written).
        if (!in.openedOk() || !midi.readFrom(in, false)) {
            error = "cannot read " + file.getFileName() + " as a MIDI file";
            return nullptr;
        }
    }
    if (midi.getTimeFormat() <= 0) {
        error = file.getFileName() + ": SMPTE time is not supported";
        return nullptr;
    }
    midi.convertTimestampTicksToSeconds();
    auto s = std::make_shared<Sequence>();
    s->name = file.getFileNameWithoutExtension();
    s->path = file.getFullPathName();
    s->tracks = midi.getNumTracks();
    // General MIDI percussion (channel 10) would sound as piano notes: left out, unless it is all the file has.
    bool otherNotes = false, drumNotes = false;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        for (const auto* holder : *midi.getTrack(t))
            if (holder->message.isNoteOn())
                (holder->message.getChannel() == 10 ? drumNotes : otherNotes) = true;
    s->drumsLeftOut = drumNotes && otherNotes;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        for (const auto* holder : *midi.getTrack(t)) {
            const auto& m = holder->message;
            if (!(m.isNoteOnOrOff() || m.isController()) || (s->drumsLeftOut && m.getChannel() == 10))
                continue;
            const auto* raw = m.getRawData();
            s->events.push_back({ m.getTimeStamp(), raw[0], raw[1], m.getRawDataSize() > 2 ? raw[2] : (std::uint8_t) 0 });
            s->notes += m.isNoteOn() ? 1 : 0;
        }
    std::stable_sort(s->events.begin(), s->events.end(), [](const Event& a, const Event& b) { return a.seconds < b.seconds; });
    s->length = s->events.empty() ? 0.0 : s->events.back().seconds;
    if (s->notes == 0) {
        error = file.getFileName() + " holds no note";
        return nullptr;
    }
    return s;
}

void MidiFilePlayer::load(std::shared_ptr<const Sequence> sequence)
{
    wantPlaying = false;
    // Once the pointer is withdrawn, a render() that starts sees it gone (sequentially consistent: the store comes
    // before the flag is read here, the flag is raised before the pointer is read there), and the one that may be
    // running is waited for. render() takes microseconds and comes back every block: the wait is short.
    published.store(nullptr);
    while (rendering.load())
        std::this_thread::yield();
    owned = std::move(sequence);
    positionSeconds = 0.0;
    seekTarget = 0.0;
    published.store(owned.get(), std::memory_order_release);
}

void MidiFilePlayer::note(const Event& e) noexcept
{
    const int channel = e.status & 0x0F, type = e.status & 0xF0;
    auto& count = sounding[(std::size_t) (channel * 128 + (e.data1 & 0x7F))];
    if (type == 0x90 && e.data2 > 0)
        count = (std::uint8_t) std::min(255, count + 1);
    else if (type == 0x80 || type == 0x90)
        count = count > 0 ? (std::uint8_t) (count - 1) : (std::uint8_t) 0;
    else if (type == 0xB0 && e.data1 == 64)
        pedal[(std::size_t) channel] = e.data2;
}

void MidiFilePlayer::releaseAll(juce::MidiBuffer& midi, int at) noexcept
{
    for (int channel = 0; channel < 16; ++channel) {
        for (int key = 0; key < 128; ++key) {
            auto& count = sounding[(std::size_t) (channel * 128 + key)];
            for (; count > 0; --count)
                midi.addEvent(juce::MidiMessage::noteOff(channel + 1, key), at);
        }
        if (pedal[(std::size_t) channel] != 0) {
            midi.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 64, 0), at);
            pedal[(std::size_t) channel] = 0;
        }
    }
}

void MidiFilePlayer::restorePedals(juce::MidiBuffer& midi, int at) noexcept
{
    // The sustain pedal of each channel as the file has it at the position (events before it).
    std::array<int, 16> state;
    state.fill(-1);
    for (std::size_t i = 0; i < next && i < current->events.size(); ++i) {
        const auto& e = current->events[i];
        if ((e.status & 0xF0) == 0xB0 && e.data1 == 64)
            state[(std::size_t) (e.status & 0x0F)] = e.data2;
    }
    for (int channel = 0; channel < 16; ++channel)
        if (state[(std::size_t) channel] > 0) {
            midi.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 64, state[(std::size_t) channel]), at);
            pedal[(std::size_t) channel] = (std::uint8_t) state[(std::size_t) channel];
        }
}

void MidiFilePlayer::render(juce::MidiBuffer& midi, int numSamples, double sampleRate) noexcept
{
    rendering.store(true);
    renderBlock(midi, numSamples, sampleRate);
    rendering.store(false);
}

void MidiFilePlayer::renderBlock(juce::MidiBuffer& midi, int numSamples, double sampleRate) noexcept
{
    const auto* s = published.load();
    if (s != current) { // a new file (or none): what the old one had sounding is released (the old one is not read)
        releaseAll(midi, 0);
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
        releaseAll(midi, 0);
        playing = false; // if it plays on, the pedals are put back below, as at a start
        pos = juce::jlimit(0.0, current->length, seekTarget.load());
        next = (std::size_t) (std::lower_bound(current->events.begin(), current->events.end(), pos,
                                               [](const Event& e, double t) { return e.seconds < t; })
                              - current->events.begin());
    }
    const bool want = wantPlaying.load();
    if (!want) {
        if (playing.load()) { // paused: the notes and the pedal are released
            releaseAll(midi, 0);
            playing = false;
        }
        positionSeconds = pos;
        return;
    }
    if (!playing.load()) { // starting (or resuming): the pedals as the file has them here
        restorePedals(midi, 0);
        playing = true;
    }
    // The block covers [pos, pos + n / fs x speed) of the file's time.
    const double factor = speed.load(), span = numSamples / sampleRate * factor;
    const auto& events = current->events;
    while (next < events.size()) { // each event at its nearest sample; one that rounds past the block waits for the next
        const auto& e = events[next];
        const double offset = (e.seconds - pos) / factor * sampleRate;
        if (offset >= numSamples - 0.5)
            break;
        midi.addEvent(juce::MidiMessage(e.status, e.data1, e.data2), juce::jlimit(0, numSamples - 1, (int) std::lround(offset)));
        note(e);
        ++next;
    }
    pos += span;
    if (next >= events.size() && pos >= current->length) {
        if (loop.load()) { // back to the start: what is sounding is released, the pedals are put back
            releaseAll(midi, numSamples - 1);
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

} // namespace pf::app
