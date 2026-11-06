#pragma once

#include "LiveRecorder.h"
#include "MirrorOutput.h"
#include "TakeRecorder.h"

#include <pianoforge/engine.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <vector>

namespace pf::app {

/// Connects the audio device to the engine: gathers MIDI (connected keyboards, on-screen keyboard, UI
/// commands), times it to the sample, hands it to the engine on every block, and records every event that
/// reached the engine (lock-free) so that takes can be saved afterwards.
class AudioHost final : public juce::AudioIODeviceCallback {
public:
    explicit AudioHost(pf::Engine& e) : engine(e) {}

    juce::MidiMessageCollector collector; // MIDI from keyboards (MIDI thread) and from the UI (message thread)
    juce::MidiKeyboardState keyboardState; // on-screen keyboard; also displays the notes received
    LiveRecorder live;                     // records what the device plays (start / stop from the interface)
    MirrorOutput mirror;                   // a copy for streaming software, on another device (OBS)

    /// Message thread: sends a message as if it came from a keyboard.
    void send(const juce::MidiMessage& m)
    {
        if (ready)
            collector.addMessageToQueue(juce::MidiMessage(m).withTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001));
    }

    /// Starts a device session (also used by the self-test without a device).
    void start(double sampleRate, int blockSize)
    {
        collector.reset(sampleRate);
        midi.ensureSize(16384);
        engine.prepare(sampleRate, blockSize);
        mirror.mainFormat(sampleRate, blockSize);
        samplesElapsed = 0;
        recordingRate = sampleRate;
        segment.fetch_add(1);
        ready = true;
    }

    /// Message thread: moves the events recorded since the last call into `out`.
    void drainRecorded(std::vector<RecordedEvent>& out)
    {
        const auto scope = recordFifo.read(recordFifo.getNumReady());
        scope.forEach([&](int index) { out.push_back(recordBuffer[(size_t) index]); });
    }

    std::uint32_t currentSegment() const noexcept { return segment.load(); }
    double currentRate() const noexcept { return recordingRate.load(); }
    std::uint32_t recordOverflows() const noexcept { return overflows.load(); }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        start(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    }

    void audioDeviceStopped() override
    {
        live.deviceStopped();
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
        if (count > 0) { // record what reaches the engine, at its exact position
            const auto seg = segment.load(std::memory_order_relaxed);
            const auto scope = recordFifo.write(count);
            int k = 0;
            scope.forEach([&](int index) {
                const auto& e = events[(size_t) k++];
                recordBuffer[(size_t) index] = { samplesElapsed + e.offset, seg, e.status, e.data1, e.data2 };
            });
            if (k < count)
                overflows.fetch_add(1, std::memory_order_relaxed);
        }
        engine.process(outputs, numOutputs, n, events.data(), count);
        live.push(outputs, numOutputs, n, samplesElapsed); // a copy into a lock-free FIFO: nothing waits on it
        mirror.push(outputs, numOutputs, n);               // likewise
        samplesElapsed += n;
    }

private:
    static constexpr int kMaxEvents = 1024;
    static constexpr int kRecordCapacity = 1 << 16;
    pf::Engine& engine;
    juce::MidiBuffer midi;
    std::array<pf::Event, kMaxEvents> events {};
    std::atomic<bool> ready { false };

    std::int64_t samplesElapsed = 0; // audio thread
    std::atomic<double> recordingRate { 0.0 };
    std::atomic<std::uint32_t> segment { 0 };
    std::atomic<std::uint32_t> overflows { 0 };
    juce::AbstractFifo recordFifo { kRecordCapacity };
    std::vector<RecordedEvent> recordBuffer = std::vector<RecordedEvent>(kRecordCapacity); // 1 MB: on the heap, not the stack
};

} // namespace pf::app
