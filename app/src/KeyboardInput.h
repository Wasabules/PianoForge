#pragma once

#include <pianoforge/velocity_curve.hpp>

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>

namespace pf::app {

/// MIDI from hardware keyboards: applies the velocity curve to note-ons and the pedal curve to the sustain pedal
/// (CC64), then hands the message to the collector. The mouse, the computer keyboard and MIDI files do not go
/// through here, so the curves only affect real keyboards.
class KeyboardInput final : public juce::MidiInputCallback {
public:
    KeyboardInput(juce::MidiMessageCollector& c, const pf::VelocityCurve& v, const pf::PedalCurve* p = nullptr)
        : collector(c), curve(v), pedalCurve(p)
    {
    }

    // Last note-on received, for the UI (input = what the keyboard sent, output = after the curve).
    std::atomic<int> lastInput { -1 }, lastOutput { -1 };
    std::atomic<std::uint32_t> noteCount { 0 };
    // Last sustain pedal value, likewise.
    std::atomic<int> lastPedalInput { -1 }, lastPedalOutput { -1 };
    std::atomic<std::uint32_t> pedalCount { 0 };

    // Range learning: while active, the softest and loudest velocities received are recorded; likewise the lowest
    // and highest pedal values.
    std::atomic<bool> learning { false }, pedalLearning { false };
    std::atomic<int> learnedMin { 127 }, learnedMax { 0 }, pedalMin { 127 }, pedalMax { 0 };

    void startLearning() noexcept
    {
        learnedMin = 127;
        learnedMax = 0;
        learning = true;
    }

    void startPedalLearning() noexcept
    {
        pedalMin = 127;
        pedalMax = 0;
        pedalLearning = true;
    }

    std::atomic<float> lastTimestampOffsetMs { 0.0f }; // driver timestamp minus arrival time, for diagnostics

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& raw) override
    {
        // Windows MIDI Services timestamps lag the arrival time by a constant ~31 ms (measured with JUCE 9.0.5).
        // The collector would then place every event at the start of the audio block, adding up to one block
        // of jitter. A timestamp that disagrees with the arrival time by more than 2 ms is replaced by it.
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        lastTimestampOffsetMs.store((float) ((raw.getTimeStamp() - now) * 1000.0), std::memory_order_relaxed);
        const auto message = std::abs(raw.getTimeStamp() - now) > 0.002 ? juce::MidiMessage(raw).withTimeStamp(now) : raw;

        if (message.isController() && message.getControllerNumber() == 64) { // the sustain pedal, through its curve
            const int in = message.getControllerValue();
            const int out = pedalCurve != nullptr ? pedalCurve->map(in) : in;
            if (pedalLearning.load(std::memory_order_relaxed)) {
                pedalMin = juce::jmin(pedalMin.load(), in);
                pedalMax = juce::jmax(pedalMax.load(), in);
            }
            lastPedalInput.store(in, std::memory_order_relaxed);
            lastPedalOutput.store(out, std::memory_order_relaxed);
            pedalCount.fetch_add(1, std::memory_order_relaxed);
            collector.handleIncomingMidiMessage(source, juce::MidiMessage::controllerEvent(message.getChannel(), 64, out)
                                                            .withTimeStamp(message.getTimeStamp()));
            return;
        }
        if (!message.isNoteOn()) { // note-offs (including note-on with velocity 0), other controllers: unchanged
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
    const pf::PedalCurve* pedalCurve;
};

} // namespace pf::app
