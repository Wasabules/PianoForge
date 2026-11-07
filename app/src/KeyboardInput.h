#pragma once

#include <pianoforge/piano_gestures.hpp>
#include <pianoforge/velocity_curve.hpp>

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

namespace pf::app {

/// MIDI from hardware keyboards: applies the velocity curve to note-ons and the pedal curve to the sustain pedal
/// (CC64), then hands the message to the collector. The mouse, the computer keyboard and MIDI files do not go
/// through here, so the curves only affect real keyboards. The piano gestures (keys struck together that no piece
/// asks for, a key struck twice: pf::PianoGestures) are recognized here too, from the keyboard's own notes; an action
/// recognized is handed to the message thread (onGesture). The notes of a gesture sound as any others.
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

    // Piano gestures (message thread: the settings, learning; MIDI thread: the notes).
    std::atomic<bool> gesturesOn { true };
    /// Message thread, called with the action recognized and the time of the gesture's first note (seconds).
    std::function<void(int action, double start)> onGesture;
    void setGesture(int action, const pf::PianoGesture& g)
    {
        const std::lock_guard<std::mutex> guard(gestureLock);
        gestures.set(action, g);
    }
    pf::PianoGesture gesture(int action) const
    {
        const std::lock_guard<std::mutex> guard(gestureLock);
        return gestures.get(action);
    }
    int gestureOwner(const pf::PianoGesture& g, int except) const
    {
        const std::lock_guard<std::mutex> guard(gestureLock);
        return gestures.owner(g, except);
    }
    void learnGesture()
    {
        const std::lock_guard<std::mutex> guard(gestureLock);
        gestures.learn();
    }
    void stopLearningGesture()
    {
        const std::lock_guard<std::mutex> guard(gestureLock);
        gestures.stopLearning();
    }
    /// Message thread, while learning: the gesture played, once the keys are still.
    std::optional<pf::PianoGesture> learnedGesture()
    {
        const std::lock_guard<std::mutex> guard(gestureLock);
        return gestures.takeLearned(juce::Time::getMillisecondCounterHiRes() * 0.001);
    }

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& raw) override
    {
        // Windows MIDI Services timestamps lag the arrival time by a constant ~31 ms (measured with JUCE 9.0.5).
        // The collector would then place every event at the start of the audio block, adding up to one block
        // of jitter. A timestamp that disagrees with the arrival time by more than 2 ms is replaced by it.
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        lastTimestampOffsetMs.store((float) ((raw.getTimeStamp() - now) * 1000.0), std::memory_order_relaxed);
        const auto message = std::abs(raw.getTimeStamp() - now) > 0.002 ? juce::MidiMessage(raw).withTimeStamp(now) : raw;
        if (message.isNoteOnOrOff())
            gestureNote(message);

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
    void gestureNote(const juce::MidiMessage& m)
    {
        int action = -1;
        double start = 0.0;
        {
            const std::lock_guard<std::mutex> guard(gestureLock);
            if (m.isNoteOn()) {
                const int a = gestures.noteOn(m.getNoteNumber(), m.getTimeStamp(), start);
                action = gesturesOn.load(std::memory_order_relaxed) ? a : -1;
            } else {
                gestures.noteOff(m.getNoteNumber(), m.getTimeStamp());
            }
        }
        if (action >= 0 && onGesture) // (a copy: the handler, set once before the MIDI starts, guards its owner itself)
            juce::MessageManager::callAsync([handler = onGesture, action, start] { handler(action, start); });
    }

    mutable std::mutex gestureLock;
    pf::PianoGestures gestures;
    juce::MidiMessageCollector& collector;
    const pf::VelocityCurve& curve;
    const pf::PedalCurve* pedalCurve;
};

} // namespace pf::app
