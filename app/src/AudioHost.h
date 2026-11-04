#pragma once

#include <pianoforge/engine.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>

namespace pf::app {

/// Connects the audio device to the engine: gathers MIDI (connected keyboards, on-screen keyboard, UI
/// commands), times it to the sample and hands it to the engine on every block.
class AudioHost final : public juce::AudioIODeviceCallback {
public:
    explicit AudioHost(pf::Engine& e) : engine(e) {}

    juce::MidiMessageCollector collector; // MIDI from keyboards (MIDI thread) and from the UI (message thread)
    juce::MidiKeyboardState keyboardState; // on-screen keyboard; also displays the notes received

    /// Message thread: sends a message as if it came from a keyboard.
    void send(const juce::MidiMessage& m)
    {
        if (ready)
            collector.addMessageToQueue(juce::MidiMessage(m).withTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001));
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        collector.reset(device->getCurrentSampleRate());
        midi.ensureSize(16384);
        engine.prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
        ready = true;
    }

    void audioDeviceStopped() override
    {
        engine.release();
    }

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs, int n,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        midi.clear();
        collector.removeNextBlockOfMessages(midi, n);
        keyboardState.processNextMidiBuffer(midi, 0, n, true);
        int count = 0;
        for (const auto meta : midi) {
            if (count >= kMaxEvents)
                break;
            if (meta.numBytes < 1 || meta.numBytes > 3)
                continue; // system exclusive: irrelevant to the engine
            events[(size_t) count++] = { (std::uint32_t) meta.samplePosition, meta.data[0],
                                         (std::uint8_t) (meta.numBytes > 1 ? meta.data[1] : 0),
                                         (std::uint8_t) (meta.numBytes > 2 ? meta.data[2] : 0) };
        }
        engine.process(outputs, numOutputs, n, events.data(), count);
    }

private:
    static constexpr int kMaxEvents = 1024;
    pf::Engine& engine;
    juce::MidiBuffer midi;
    std::array<pf::Event, kMaxEvents> events {};
    std::atomic<bool> ready { false };
};

} // namespace pf::app
