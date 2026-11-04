#pragma once

#include <pianoforge/velocity_curve.hpp>

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>

namespace pf::app {

/// MIDI from hardware keyboards: applies the velocity curve, then hands the message to the collector.
/// The mouse and the computer keyboard do not go through here, so the curve only affects real keyboards.
class KeyboardInput final : public juce::MidiInputCallback {
public:
    KeyboardInput(juce::MidiMessageCollector& c, const pf::VelocityCurve& v) : collector(c), curve(v) {}

    // Last note-on received, for the UI (input = what the keyboard sent, output = after the curve).
    std::atomic<int> lastInput { -1 }, lastOutput { -1 };
    std::atomic<std::uint32_t> noteCount { 0 };

    // Range learning: while active, the softest and loudest velocities received are recorded.
    std::atomic<bool> learning { false };
    std::atomic<int> learnedMin { 127 }, learnedMax { 0 };

    void startLearning() noexcept
    {
        learnedMin = 127;
        learnedMax = 0;
        learning = true;
    }

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override
    {
        if (!message.isNoteOn()) { // note-offs (including note-on with velocity 0), pedals: unchanged
            collector.handleIncomingMidiMessage(source, message);
            return;
        }
        const int in = message.getVelocity();
        const int out = curve.map(in);
        if (learning.load(std::memory_order_relaxed)) {
            learnedMin = juce::jmin(learnedMin.load(), in);
            learnedMax = juce::jmax(learnedMax.load(), in);
        }
        lastInput.store(in, std::memory_order_relaxed);
        lastOutput.store(out, std::memory_order_relaxed);
        noteCount.fetch_add(1, std::memory_order_relaxed);
        collector.handleIncomingMidiMessage(
            source, juce::MidiMessage::noteOn(message.getChannel(), message.getNoteNumber(), (juce::uint8) out)
                        .withTimeStamp(message.getTimeStamp()));
    }

private:
    juce::MidiMessageCollector& collector;
    const pf::VelocityCurve& curve;
};

} // namespace pf::app
