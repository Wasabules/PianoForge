// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TakeRecorder.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pf::app {

void TakeRecorder::add(const std::vector<RecordedEvent>& newEvents, std::uint32_t currentSegment, double currentRate)
{
    if (currentRate > 0.0)
        rates[currentSegment] = currentRate;
    events.insert(events.end(), newEvents.begin(), newEvents.end());
}

std::optional<TakeRecorder::Take> TakeRecorder::lastTake(double gapSeconds, double leadSeconds, std::int64_t before) const
{
    if (events.empty())
        return std::nullopt;
    const auto segment = events.back().segment;
    const auto rate = rates.count(segment) != 0 ? rates.at(segment) : 0.0;
    if (rate <= 0.0)
        return std::nullopt;
    // The events up to `before` (in the last session): [0, end).
    std::size_t end = events.size();
    while (end > 0 && events[end - 1].segment == segment && events[end - 1].sample >= before)
        --end;
    if (end == 0 || events[end - 1].segment != segment)
        return std::nullopt;

    // Walk back to the last long silence (or the start of the device session).
    const auto gap = (std::int64_t) std::llround(gapSeconds * rate);
    std::size_t first = end - 1;
    while (first > 0 && events[first - 1].segment == segment && events[first].sample - events[first - 1].sample < gap)
        --first;

    // Pedals already down when the take starts: re-stated at its start.
    std::array<int, 128> controllers {};
    controllers.fill(-1);
    for (std::size_t i = 0; i < first; ++i)
        if (events[i].segment == segment && (events[i].status & 0xF0) == 0xB0)
            controllers[events[i].data1] = events[i].data2;

    Take take;
    take.sampleRate = rate;
    const auto start = std::max<std::int64_t>(0, events[first].sample - (std::int64_t) std::llround(leadSeconds * rate));
    for (const int cc : { 64, 66, 67 })
        if (controllers[(size_t) cc] > 0)
            take.events.push_back({ 0, segment, 0xB0, (std::uint8_t) cc, (std::uint8_t) controllers[(size_t) cc] });
    std::array<bool, 128> held {};
    for (std::size_t i = first; i < end; ++i) {
        auto e = events[i];
        e.sample -= start;
        take.events.push_back(e);
        const int type = e.status & 0xF0;
        take.notes += (type == 0x90 && e.data2 > 0) ? 1 : 0;
        if (type == 0x90 || type == 0x80)
            held[e.data1 & 0x7F] = type == 0x90 && e.data2 > 0;
        if (type == 0xB0)
            controllers[e.data1 & 0x7F] = e.data2;
    }
    if (end < events.size()) { // cut before `before`: what is still down there is released there
        const auto at = std::max(take.events.back().sample, before - start);
        for (int k = 0; k < 128; ++k)
            if (held[(std::size_t) k])
                take.events.push_back({ at, segment, 0x80, (std::uint8_t) k, 0 });
        for (const int cc : { 64, 66, 67 })
            if (controllers[(std::size_t) cc] > 0)
                take.events.push_back({ at, segment, 0xB0, (std::uint8_t) cc, 0 });
    }
    take.lengthSamples = take.events.back().sample;
    return take;
}

std::optional<TakeRecorder::Take> TakeRecorder::span(std::uint32_t segment, std::int64_t from, std::int64_t to) const
{
    const auto rate = rates.count(segment) != 0 ? rates.at(segment) : 0.0;
    if (rate <= 0.0 || to <= from)
        return std::nullopt;
    std::array<int, 128> controllers {};
    controllers.fill(-1);
    Take take;
    take.sampleRate = rate;
    take.exactLength = true;
    take.lengthSamples = to - from;
    for (const auto& e : events) // pedals already down at the start: re-stated there
        if (e.segment == segment && e.sample < from && (e.status & 0xF0) == 0xB0)
            controllers[e.data1] = e.data2;
    for (const int cc : { 64, 66, 67 })
        if (controllers[(size_t) cc] > 0)
            take.events.push_back({ 0, segment, 0xB0, (std::uint8_t) cc, (std::uint8_t) controllers[(size_t) cc] });
    for (auto e : events)
        if (e.segment == segment && e.sample >= from && e.sample < to) {
            e.sample -= from;
            take.events.push_back(e);
            take.notes += ((e.status & 0xF0) == 0x90 && e.data2 > 0) ? 1 : 0;
        }
    return take;
}

juce::MidiFile TakeRecorder::toMidiFile(const Take& take)
{
    juce::MidiMessageSequence sequence; // timestamps in ticks: one tick per sample
    const bool fast = take.sampleRate > 65534.0;
    sequence.addEvent(juce::MidiMessage::tempoMetaEvent(fast ? 250000 : 500000), 0.0); // 240 or 120 BPM
    sequence.addEvent(juce::MidiMessage::textMetaEvent(1, take.text.isNotEmpty() ? take.text
                                                              : "PianoForge take, sample rate " + juce::String(take.sampleRate, 0)
                                                                    + " Hz, velocities after the velocity curve"), 0.0);
    for (const auto& e : take.events)
        sequence.addEvent(juce::MidiMessage(e.status, e.data1, e.data2), (double) e.sample);
    sequence.addEvent(juce::MidiMessage::endOfTrack(), (double) take.lengthSamples + (take.exactLength ? 0.0 : take.sampleRate));
    // No updateMatchedPairs(): it inserts a note-off before a re-strike of a note still held, which changes what
    // the engine receives (a damper release instead of a re-strike). The file holds exactly what was played.
    juce::MidiFile file;
    file.setTicksPerQuarterNote((int) std::lround(take.sampleRate / (fast ? 4.0 : 2.0)));
    file.addTrack(sequence);
    return file;
}

juce::MidiMessageSequence TakeRecorder::toSequence(const Take& take)
{
    juce::MidiMessageSequence sequence;
    for (const auto& e : take.events)
        sequence.addEvent(juce::MidiMessage(e.status, e.data1, e.data2), (double) e.sample / take.sampleRate);
    return sequence; // events exactly as played (see toMidiFile)
}

} // namespace pf::app
