// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <pianoforge/midi_sequence.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>

namespace pf::app {

/// The lab's MIDI file player: the engine's SequencePlayer (shared with the browser version, web/), with the file
/// read from disk and the block's events written into the audio host's MidiBuffer, to the sample, before the
/// on-screen keyboard (which shows them). See pf::SequencePlayer for the transport and what a seek or a pause does.
class MidiFilePlayer final : public pf::SequencePlayer {
public:
    using Sequence = pf::MidiSequence;

    /// Message thread: reads a MIDI file (nullptr and `error` filled when it cannot).
    static std::shared_ptr<const Sequence> read(const juce::File& file, juce::String& error);

    /// Audio thread: adds the events of `numSamples` frames at `sampleRate` to `midi`, frame f at position
    /// offset + f x stride (the engine below the device's rate: positions in the device's samples). No allocation
    /// (the buffer has room), no lock.
    void render(juce::MidiBuffer& midi, int numSamples, double sampleRate, int stride = 1, int offset = 0) noexcept;

private:
    std::array<pf::Event, 4096> scratch {}; // a block's events: a release of everything fits (16 x 128 notes, pedals)
};

} // namespace pf::app
