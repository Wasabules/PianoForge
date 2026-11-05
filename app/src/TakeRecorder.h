#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
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
        std::int64_t lengthSamples = 0;    // up to the last event
        int notes = 0;
    };

    void add(const std::vector<RecordedEvent>& newEvents, std::uint32_t currentSegment, double currentRate);
    std::size_t size() const noexcept { return events.size(); }

    /// Events after the last silence of at least `gapSeconds`, starting `leadSeconds` before the first one, with
    /// the pedal state at that moment re-stated at the start. Empty when nothing was played.
    std::optional<Take> lastTake(double gapSeconds = 10.0, double leadSeconds = 0.2) const;

    /// The take as a MIDI file: 120 BPM with sampleRate / 2 ticks per quarter note, i.e. one tick per sample.
    static juce::MidiFile toMidiFile(const Take& take);
    /// The take as a sequence timed in seconds, as an offline render expects it.
    static juce::MidiMessageSequence toSequence(const Take& take);

private:
    std::vector<RecordedEvent> events;
    std::map<std::uint32_t, double> rates; // sample rate of each device session
};

} // namespace pf::app
