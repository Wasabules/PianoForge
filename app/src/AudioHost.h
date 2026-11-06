#pragma once

#include "LiveRecorder.h"
#include "MidiFilePlayer.h"
#include "MirrorOutput.h"
#include "TakeRecorder.h"

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/engine.hpp>
#include <pianoforge/room.hpp>
#include <pianoforge/upsampler.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <cmath>
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
///
/// The engine's rate: under a device at 88.2 to 384 kHz (the Windows mixer imposes its own), the engine and the
/// room run at the device's rate divided by 2, 4 or 8, down to 44.1 or 48 kHz, and their sound is brought up to
/// the device's by a polyphase interpolator (pf::Upsampler: flat to 20 kHz, images 90 dB down, 0.7 ms): four times
/// less work at 192 kHz, for nothing audible lost. What is recorded, captured as takes, streamed (OBS, mirror) is
/// the engine's sound, at its rate; the MIDI events keep their timing to within one engine sample.
class AudioHost final : public juce::AudioIODeviceCallback {
public:
    explicit AudioHost(pf::Engine& e) : engine(e) { feedOpen = feed.create(); }

    juce::MidiMessageCollector collector; // MIDI from keyboards (MIDI thread) and from the UI (message thread)
    juce::MidiKeyboardState keyboardState; // on-screen keyboard; also displays the notes received
    LiveRecorder live;                     // records what the device plays (start / stop from the interface)
    MirrorOutput mirror;                   // a copy for streaming software, on another device
    MidiFilePlayer filePlayer;             // a MIDI file, played to the sample with what the keyboards play
    pf::Room room;                         // the room around the piano, at the output (off until the interface sets it)
    /// The engine's rate: 0 the device's divided down to 48 kHz or less (applies at the next device start), 1 the
    /// device's own.
    std::atomic<int> rateMode { 0 };
    bool feedOn() const noexcept { return feedOpen; } // the audio feed of the OBS plugin (obs/)

    /// Message thread: sends a message as if it came from a keyboard.
    void send(const juce::MidiMessage& m)
    {
        if (ready)
            collector.addMessageToQueue(juce::MidiMessage(m).withTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001));
    }

    /// Starts a device session (also used by the self-test without a device). Allocates.
    void start(double sampleRate, int blockSize)
    {
        collector.reset(sampleRate); // the MIDI's positions are the device's samples
        midi.ensureSize(65536); // room for a busy block plus the player's releases: no allocation
        int k = 1;
        if (rateMode.load() == 0)
            while (k < 8 && sampleRate / k > 50000.0 && std::fmod(sampleRate, 2.0 * k) == 0.0)
                k *= 2;
        factor = k;
        const double rate = sampleRate / k;
        const int engineBlock = std::max(1, blockSize) / k + 2;
        engine.prepare(rate, engineBlock);
        room.prepare(rate);
        mirror.mainFormat(rate, engineBlock);
        feed.setFormat((std::uint32_t) std::lround(rate), (std::uint32_t) engineBlock);
        for (auto& b : { &engineL, &engineR })
            b->assign((std::size_t) engineBlock, 0.0f);
        for (auto& b : { &upL, &upR })
            b->assign((std::size_t) (engineBlock * k), 0.0f);
        upsampleL.prepare(k);
        upsampleR.prepare(k);
        carry = 0;
        samplesElapsed = 0;
        lastCallbackTicks = 0;
        load.blockSeconds = blockSize / std::max(sampleRate, 1.0);
        device = sampleRate;
        recordingRate = rate;
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
    /// The engine's rate (what is recorded and captured).
    double currentRate() const noexcept { return recordingRate.load(); }
    double deviceRate() const noexcept { return device.load(); }
    int rateFactor() const noexcept
    {
        const double r = recordingRate.load();
        return r > 0.0 ? (int) std::lround(device.load() / r) : 1;
    }
    std::uint32_t recordOverflows() const noexcept { return overflows.load(); }
    AudioLoad load;
    /// Message thread: the dropout counters back to zero.
    void resetDropouts() noexcept
    {
        load.lateBlocks = 0;
        load.gaps = 0;
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override
    {
        start(d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
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
        const int k = factor;
        // The device's first `carry` samples come from the engine frame rendered last time; the engine renders the
        // frames that cover the rest (blocks are longer than 8 samples, so longer than the carry; a block longer
        // than the device announced is cut to what the buffers hold rather than overrun them).
        const int frames = k == 1 ? n : std::min((int) engineL.size(), (n - carry + k - 1) / k);
        midi.clear();
        collector.removeNextBlockOfMessages(midi, n);
        // Before the keyboard (it shows the notes); at the device's positions of the engine's frames.
        filePlayer.render(midi, frames, recordingRate.load(std::memory_order_relaxed), k, k == 1 ? 0 : carry);
        keyboardState.processNextMidiBuffer(midi, 0, n, true);
        int count = 0;
        for (const auto meta : midi) {
            if (count >= kMaxEvents)
                break;
            if (meta.numBytes < 1 || meta.numBytes > 3)
                continue; // system exclusive: irrelevant to the engine
            const int at = k == 1 ? meta.samplePosition : std::min(frames - 1, std::max(0, meta.samplePosition - carry) / k);
            events[(size_t) count++] = { (std::uint32_t) at, meta.data[0],
                                         (std::uint8_t) (meta.numBytes > 1 ? meta.data[1] : 0),
                                         (std::uint8_t) (meta.numBytes > 2 ? meta.data[2] : 0) };
        }
        if (count > 0) { // record what reaches the engine, at its exact position
            const auto seg = segment.load(std::memory_order_relaxed);
            const auto scope = recordFifo.write(count);
            int written = 0;
            scope.forEach([&](int index) {
                const auto& e = events[(size_t) written++];
                recordBuffer[(size_t) index] = { samplesElapsed + e.offset, seg, e.status, e.data1, e.data2 };
            });
            if (written < count)
                overflows.fetch_add(1, std::memory_order_relaxed);
        }
        if (k == 1) {
            engine.process(outputs, numOutputs, n, events.data(), count);
            if (numOutputs >= 2 && outputs[0] != nullptr && outputs[1] != nullptr)
                room.process(outputs[0], outputs[1], n); // whichever engine: what is heard, recorded and streamed
            finish(outputs, numOutputs, n);
        } else {
            float* engineOut[2] = { engineL.data(), engineR.data() };
            engine.process(engineOut, 2, frames, events.data(), count);
            room.process(engineOut[0], engineOut[1], frames);
            finish(engineOut, 2, frames);
            // Up to the device's rate: the carry from last time, then the new frames; what is left over waits.
            upsampleL.process(engineL.data(), frames, upL.data());
            upsampleR.process(engineR.data(), frames, upR.data());
            const float* up[2] = { upL.data(), upR.data() };
            const int fresh = std::min(n - carry, frames * k);
            for (int c = 0; c < numOutputs; ++c) {
                if (outputs[c] == nullptr)
                    continue;
                if (c >= 2) {
                    std::fill(outputs[c], outputs[c] + n, 0.0f);
                    continue;
                }
                std::copy(carried[c].begin(), carried[c].begin() + carry, outputs[c]);
                std::copy(up[c], up[c] + fresh, outputs[c] + carry);
                std::fill(outputs[c] + carry + fresh, outputs[c] + n, 0.0f); // (only past an oversized block)
            }
            const int left = frames * k - fresh;
            for (int c = 0; c < 2; ++c)
                std::copy(up[c] + fresh, up[c] + fresh + left, carried[c].begin());
            carry = left;
        }
        measure(startTicks, n);
    }

private:
    /// The engine's sound, at its rate: recorded, streamed, measured; then the engine's clock advances.
    void finish(float* const* out, int channels, int frames) noexcept
    {
        live.push(out, channels, frames, samplesElapsed); // a copy into a lock-free FIFO: nothing waits on it
        mirror.push(out, channels, frames);               // likewise
        feed.write(out, channels, frames);                // the OBS plugin's feed: a copy into shared memory
        notePeak(load.peakLeft, channels > 0 ? out[0] : nullptr, frames);
        notePeak(load.peakRight, channels > 1 ? out[1] : nullptr, frames);
        samplesElapsed += frames;
    }

    void measure(std::int64_t startTicks, int n) noexcept
    {
        const auto endTicks = juce::Time::getHighResolutionTicks();
        const double tick = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
        const double rate = device.load(std::memory_order_relaxed);
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
    std::atomic<double> recordingRate { 0.0 }, device { 0.0 };
    // The engine below the device's rate (audio thread, set by start()).
    int factor = 1, carry = 0;
    std::vector<float> engineL, engineR, upL, upR;
    std::array<std::array<float, 8>, 2> carried {};
    pf::Upsampler upsampleL, upsampleR;
    std::atomic<std::uint32_t> segment { 0 };
    std::atomic<std::uint32_t> overflows { 0 };
    juce::AbstractFifo recordFifo { kRecordCapacity };
    std::vector<RecordedEvent> recordBuffer = std::vector<RecordedEvent>(kRecordCapacity); // 1 MB: on the heap, not the stack
};

} // namespace pf::app
