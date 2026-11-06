#pragma once

#include "LiveRecorder.h"
#include "MidiFilePlayer.h"
#include "MirrorOutput.h"
#include "TakeRecorder.h"

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/engine.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <vector>

namespace pf::app {

/// How hard the audio thread works, measured on every callback (written by the audio thread, read by the
/// interface): the time the callback takes against the time the block lasts. Above 100 % a block is late and the
/// device plays a gap; long before that, the variations of the system can make one late.
struct AudioLoad {
    std::atomic<float> last { 0.0f };           // the last callback
    std::atomic<float> peak { 0.0f };           // the highest since the interface last took it (exchange)
    std::atomic<std::uint32_t> lateBlocks { 0 }; // callbacks longer than their block: the device missed audio
    std::atomic<std::uint32_t> gaps { 0 };      // callbacks arriving more than 1.5 blocks after the previous one
    std::atomic<std::uint64_t> callbacks { 0 };
    std::atomic<double> loadSum { 0.0 };        // the loads added up (the interface averages its differences)
    std::atomic<double> blockSeconds { 0.0 };   // the time one block lasts
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f }; // output peaks since the interface last took them
};

/// Connects the audio device to the engine: gathers MIDI (connected keyboards, on-screen keyboard, UI
/// commands, the MIDI file player), times it to the sample, hands it to the engine on every block, and records
/// every event that reached the engine (lock-free) so that takes can be saved afterwards.
class AudioHost final : public juce::AudioIODeviceCallback {
public:
    explicit AudioHost(pf::Engine& e) : engine(e) { feedOpen = feed.create(); }

    juce::MidiMessageCollector collector; // MIDI from keyboards (MIDI thread) and from the UI (message thread)
    juce::MidiKeyboardState keyboardState; // on-screen keyboard; also displays the notes received
    LiveRecorder live;                     // records what the device plays (start / stop from the interface)
    MirrorOutput mirror;                   // a copy for streaming software, on another device
    MidiFilePlayer filePlayer;             // a MIDI file, played to the sample with what the keyboards play
    bool feedOn() const noexcept { return feedOpen; } // the audio feed of the OBS plugin (obs/)

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
        midi.ensureSize(65536); // room for a busy block plus the player's releases: no allocation
        engine.prepare(sampleRate, blockSize);
        mirror.mainFormat(sampleRate, blockSize);
        feed.setFormat((std::uint32_t) std::lround(sampleRate), (std::uint32_t) std::max(blockSize, 1));
        samplesElapsed = 0;
        lastCallbackTicks = 0;
        load.blockSeconds = blockSize / std::max(sampleRate, 1.0);
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
    AudioLoad load;
    /// Message thread: the dropout counters back to zero.
    void resetDropouts() noexcept
    {
        load.lateBlocks = 0;
        load.gaps = 0;
    }

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
        const auto startTicks = juce::Time::getHighResolutionTicks(); // QueryPerformanceCounter: no system call
        midi.clear();
        collector.removeNextBlockOfMessages(midi, n);
        filePlayer.render(midi, n, recordingRate.load(std::memory_order_relaxed)); // before the keyboard: it shows the notes
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
        feed.write(outputs, numOutputs, n);                // the OBS plugin's feed: a copy into shared memory
        notePeak(load.peakLeft, numOutputs > 0 ? outputs[0] : nullptr, n);
        notePeak(load.peakRight, numOutputs > 1 ? outputs[1] : nullptr, n);
        samplesElapsed += n;
        measure(startTicks, n);
    }

private:
    void measure(std::int64_t startTicks, int n) noexcept
    {
        const auto endTicks = juce::Time::getHighResolutionTicks();
        const double tick = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
        const double rate = recordingRate.load(std::memory_order_relaxed);
        const double block = n / std::max(rate, 1.0);
        const auto used = (float) ((double) (endTicks - startTicks) * tick / block);
        load.last.store(used, std::memory_order_relaxed);
        if (used > load.peak.load(std::memory_order_relaxed))
            load.peak.store(used, std::memory_order_relaxed);
        // A late block delays the next callback too: that gap is the same dropout, counted once.
        if (lastCallbackTicks != 0 && !lastWasLate && (double) (startTicks - lastCallbackTicks) * tick > 1.5 * block + 0.002)
            load.gaps.fetch_add(1, std::memory_order_relaxed);
        lastWasLate = used > 1.0f;
        if (lastWasLate)
            load.lateBlocks.fetch_add(1, std::memory_order_relaxed);
        lastCallbackTicks = startTicks;
        load.loadSum.store(load.loadSum.load(std::memory_order_relaxed) + used, std::memory_order_relaxed); // one writer
        load.callbacks.fetch_add(1, std::memory_order_release);
    }

    static void notePeak(std::atomic<float>& peak, const float* x, int n) noexcept
    {
        if (x == nullptr)
            return;
        float p = 0.0f;
        for (int k = 0; k < n; ++k)
            p = juce::jmax(p, std::abs(x[k]));
        if (p > peak.load(std::memory_order_relaxed))
            peak.store(p, std::memory_order_relaxed);
    }

    static constexpr int kMaxEvents = 1024;
    static constexpr int kRecordCapacity = 1 << 16;
    pf::Engine& engine;
    pf::AudioFeedWriter feed; // before the callbacks start; written by the audio thread only
    bool feedOpen = false;
    juce::MidiBuffer midi;
    std::array<pf::Event, kMaxEvents> events {};
    std::atomic<bool> ready { false };

    std::int64_t samplesElapsed = 0; // audio thread
    std::int64_t lastCallbackTicks = 0; // audio thread
    bool lastWasLate = false;           // audio thread
    std::atomic<double> recordingRate { 0.0 };
    std::atomic<std::uint32_t> segment { 0 };
    std::atomic<std::uint32_t> overflows { 0 };
    juce::AbstractFifo recordFifo { kRecordCapacity };
    std::vector<RecordedEvent> recordBuffer = std::vector<RecordedEvent>(kRecordCapacity); // 1 MB: on the heap, not the stack
};

} // namespace pf::app
