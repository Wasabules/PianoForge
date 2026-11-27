// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace pf::app {

/// One event as it reached the engine: absolute position in output samples within a device session (a
/// "segment" starts each time the audio device starts, with its own sample rate).
struct RecordedEvent {
    std::int64_t sample = 0;
    std::uint32_t segment = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
};

/// Never lose a take: every event that reached the engine is kept (message thread), and the last take - what was
/// played after the last long silence - can be saved at any time, as a standard MIDI file with one tick per
/// sample, so that re-rendering it gives exactly the audio heard live.
class TakeRecorder {
public:
    struct Take {
        double sampleRate = 0.0;
        std::vector<RecordedEvent> events; // positions relative to the take start
        std::int64_t lengthSamples = 0;    // up to the last event (a span: its exact length)
        int notes = 0;
        bool exactLength = false;          // a span: the MIDI file ends exactly where the span does
        juce::String text;                 // a text event at the start of the MIDI file, when not empty
    };

    void add(const std::vector<RecordedEvent>& newEvents, std::uint32_t currentSegment, double currentRate);
    std::size_t size() const noexcept { return events.size(); }

    /// Events after the last silence of at least `gapSeconds`, starting `leadSeconds` before the first one, with
    /// the pedal state at that moment re-stated at the start. Empty when nothing was played. `before` (a position
    /// in the last device session): only what came before it (a gesture that asked for the save left out), the
    /// notes and pedals still down there released there.
    std::optional<Take> lastTake(double gapSeconds = 10.0, double leadSeconds = 0.2,
                                 std::int64_t before = std::numeric_limits<std::int64_t>::max()) const;

    /// The events of device session `segment` in [from, to) (sample positions), relative to `from`, with the
    /// pedal state at `from` re-stated at the start: the MIDI of a live recording, aligned with its WAV.
    std::optional<Take> span(std::uint32_t segment, std::int64_t from, std::int64_t to) const;

    /// The take as a MIDI file with one tick per sample: 120 BPM and sampleRate / 2 ticks per quarter note (240 BPM
    /// and sampleRate / 4 above 65.5 kHz, a MIDI file holding at most 32767 ticks per quarter note).
    static juce::MidiFile toMidiFile(const Take& take);
    /// The take as a sequence timed in seconds, as an offline render expects it.
    static juce::MidiMessageSequence toSequence(const Take& take);

private:
    std::vector<RecordedEvent> events;
    std::map<std::uint32_t, double> rates; // sample rate of each device session
};

} // namespace pf::app
