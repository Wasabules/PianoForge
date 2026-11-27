// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "LinuxAudio.h"
#include "LiveRecorder.h"
#include "MidiFilePlayer.h"
#include "MirrorOutput.h"
#include "TakePlayback.h"
#include "TakeRecorder.h"
#include "Tone.h"

#include <pianoforge/audio_feed.hpp>
#include <pianoforge/compressor.hpp>
#include <pianoforge/engine.hpp>
#include <pianoforge/limiter.hpp>
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
                                                // (Linux: more than the device's buffer, see gapAllowance)
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
///
/// After the room: the convolution reverb (the engine's sound before the room through an impulse response, added to
/// the room's output, in parallel) and the output's tone (an EQ); then the volume and the limiter: what is heard and
/// recorded. The OBS plugin's feed takes the sound before the volume, as three mixes (pf::FeedMix): as heard, as on
/// speakers (a second room, the head left out, run while an OBS source takes that mix; the reverb and the tone as
/// heard), and dry; the MIDI events with their frames, the state for overlays and the marks (chapters) ride along.
/// The stream's compressor works on what leaves for a stream only (the feed's mixes, the mirror).
class AudioHost final : public juce::AudioIODeviceCallback {
public:
    explicit AudioHost(pf::Engine& e) : engine(e) { feedOpen = feed.create(); }
    ~AudioHost()
    {
        delete irPending.exchange(nullptr);
        delete irSpent.exchange(nullptr);
    }

    juce::MidiMessageCollector collector; // MIDI from keyboards (MIDI thread) and from the UI (message thread)
    juce::MidiKeyboardState keyboardState; // on-screen keyboard; also displays the notes received
    LiveRecorder live;                     // records what the device plays (start / stop from the interface)
    MirrorOutput mirror;                   // a copy for streaming software, on another device
    MidiFilePlayer filePlayer;             // a MIDI file, played to the sample with what the keyboards play
    pf::Room room;                         // the room around the piano, at the output (off until the interface sets it)
    pf::Limiter limiter;                   // after it: nothing over -1 dBFS reaches the device, a recording or a stream
    /// The lab's volume (linear), after the room (what is heard and recorded; the feed comes before it).
    std::atomic<float> volume { 1.0f };
    /// An OBS source takes the speakers mix (the interface sets it from the feed's readers): the second room runs.
    std::atomic<bool> speakersWanted { false };
    TakePlayback playback;                 // takes listened to: heard only (after the recording and streaming taps)
    /// The output's tone (dB): bass (a low shelf), presence (a bell), air (a high shelf); see OutputEq.
    std::atomic<float> toneBassDb { 0.0f }, tonePresenceDb { 0.0f }, toneAirDb { 0.0f };
    /// The convolution reverb: on, its level (dB) and the room's output's (dB; kDryOffDb or below: none).
    std::atomic<bool> convolutionOn { false };
    std::atomic<float> convolutionWetDb { -12.0f }, convolutionDryDb { 0.0f };
    /// Message thread: the impulse response the convolution uses from now on (nullptr: none), crossfaded in.
    void setImpulse(std::shared_ptr<const ImpulseResponse> ir)
    {
        collectGarbage();
        auto* handed = new ImpulseLoad;
        if (ir != nullptr) {
            handed->buffer.makeCopyOf(ir->samples);
            handed->rate = ir->sampleRate;
            handed->stereo = ir->stereo();
        }
        impulseShared = std::move(ir);
        delete irPending.exchange(handed); // one not taken yet is replaced
    }
    std::shared_ptr<const ImpulseResponse> impulse() const { return impulseShared; } // message thread
    /// Message thread: frees what the audio thread let go of (an impulse response handed over).
    void collectGarbage() { delete irSpent.exchange(nullptr); }
    /// What the offline renders need to sound as heard after the room. Message thread.
    OutputTone outputTone() const
    {
        OutputTone t;
        t.bassDb = toneBassDb.load();
        t.presenceDb = tonePresenceDb.load();
        t.airDb = toneAirDb.load();
        t.convolution = convolutionOn.load() && impulseShared != nullptr;
        t.wetDb = convolutionWetDb.load();
        t.dryDb = convolutionDryDb.load();
        t.impulse = impulseShared;
        return t;
    }
    /// The stream's compressor (the feed's three mixes and the mirror alike).
    void setStreamCompressor(const pf::Compressor::Settings& s) noexcept
    {
        for (auto& c : streamCompressors)
            c.applySettings(s);
    }
    float streamReduction() const noexcept { return streamCompressors[0].reduction(); }
    /// The engine's rate: 0 the device's divided down to 48 kHz or less (applies at the next device start), 1 the
    /// device's own.
    std::atomic<int> rateMode { 0 };
    bool feedOn() const noexcept { return feedOpen; } // the audio feed of the OBS plugin (obs/)
    /// Message thread: the state for overlays, the marks (chapters in OBS's recordings), the readers.
    void setFeedInfo(const pf::FeedInfo& info) noexcept { feed.setInfo(info); }
    void addFeedMark(const juce::String& text) noexcept { feed.addMark(text.toRawUTF8()); }
    void addFeedCommand(pf::FeedCommand command) noexcept { feed.addCommand(command); }
    /// Message thread: the device session position of something that happened at `seconds`
    /// (juce::Time::getMillisecondCounterHiRes() / 1000, the clock of the MIDI's timestamps), at the engine's rate:
    /// within a block of where its MIDI reached the engine (a little early: a cut there leaves it out).
    std::int64_t positionAt(double seconds) const noexcept
    {
        const double rate = recordingRate.load();
        return callbackElapsed.load() + (std::int64_t) std::floor((seconds - callbackSeconds.load()) * rate);
    }
    int feedReaders(pf::AudioFeedWriter::Reader* out, int max) const noexcept { return feed.readers(out, max); }

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
        { // the speakers mix's room: the heard one's settings, the head left out (prepare starts on them, no glide)
            auto s = room.settings();
            s.headphones = false;
            roomSpeakers.applySettings(s);
        }
        roomSpeakers.prepare(rate);
        speakersRunning = false;
        limiter.prepare(rate);
        takeImpulse(); // (no block runs: the impulse response handed over is ready with prepare)
        convolution.prepare({ rate, (juce::uint32) engineBlock, 2 });
        convolutionRunning = false;
        wetNow = 0.0f;
        dryNow = 1.0f;
        for (auto& c : streamCompressors)
            c.prepare(rate);
        mirror.mainFormat(rate, engineBlock);
        feed.setFormat((std::uint32_t) std::lround(rate), (std::uint32_t) engineBlock);
        for (auto& b : { &engineL, &engineR, &dryL, &dryR, &speakersL, &speakersR, &convL, &convR, &streamL, &streamR, &mirrorL, &mirrorR })
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
    /// The last sustain pedal value the engine received (CC64, 0 to 127), whoever sent it.
    std::atomic<int> pedalValue { 0 };
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
    /// Linux: the kernel id of the thread running the callbacks (JUCE's ALSA thread, or JACK's), 0 before the first.
    std::atomic<std::int64_t> audioThread { 0 };
    /// Message thread: the dropout counters back to zero.
    void resetDropouts() noexcept
    {
        load.lateBlocks = 0;
        load.gaps = 0;
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override
    {
#if JUCE_LINUX
        // ALSA keeps a ring buffer (JUCE's: four blocks) that PipeWire's graph empties a quantum at a time: when the
        // quantum is longer than the block, the callbacks come in bursts (two, then a wait of two blocks). Only a gap
        // longer than what the device holds is a dropout.
        gapAllowance.store((d->getOutputLatencyInSamples() + d->getCurrentBufferSizeSamples()) / std::max(1.0, d->getCurrentSampleRate()),
                           std::memory_order_relaxed);
#endif
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
#if JUCE_LINUX
        audioThread.store(linuxaudio::threadId(), std::memory_order_relaxed); // the interface makes it realtime
#endif
        callbackSeconds.store(juce::Time::getMillisecondCounterHiRes() * 0.001, std::memory_order_relaxed);
        callbackElapsed.store(samplesElapsed, std::memory_order_relaxed);
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
            if (meta.numBytes == 3 && (meta.data[0] & 0xF0) == 0xB0 && meta.data[1] == 64) // the sustain pedal, for the UI
                pedalValue.store(meta.data[2], std::memory_order_relaxed);
            if (meta.numBytes == 3 && (meta.data[0] & 0xF0) == 0x90 && meta.data[2] > 0) // a recording armed starts with it
                live.noteOn(samplesElapsed + at);
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
            shape(numOutputs > 0 ? outputs[0] : nullptr, numOutputs > 1 ? outputs[1] : nullptr, n, count);
            finish(outputs, numOutputs, n);
            if (numOutputs >= 2)
                playback.render(outputs[0], outputs[1], n, recordingRate.load(std::memory_order_relaxed));
        } else {
            float* engineOut[2] = { engineL.data(), engineR.data() };
            engine.process(engineOut, 2, frames, events.data(), count);
            shape(engineOut[0], engineOut[1], frames, count);
            finish(engineOut, 2, frames);
            playback.render(engineOut[0], engineOut[1], frames, recordingRate.load(std::memory_order_relaxed));
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
    /// The engine's sound (frames at its rate) through the room, whichever engine; the OBS feed's mixes taken from it
    /// (before the volume and the limiter); then the volume (a ramp over the block) and the limiter: what is heard.
    void shape(float* left, float* right, int frames, int numEvents) noexcept
    {
        if (left == nullptr || frames <= 0)
            return;
        const bool stereo = right != nullptr;
        const bool mixes = feedOpen && stereo && frames <= (int) dryL.size(); // (a block longer than announced: heard only)
        const bool speakers = mixes && speakersWanted.load(std::memory_order_relaxed);
        if (mixes) {
            std::copy(left, left + frames, dryL.begin());
            std::copy(right, right + frames, dryR.begin());
        }
        // The convolution reverb: the sound before the room through the impulse response (once it was on, one more
        // block, its level fading to none).
        const bool irReady = takeImpulse();
        const bool wantReverb = stereo && irReady && convolutionOn.load(std::memory_order_relaxed) && frames <= (int) convL.size();
        const bool reverb = wantReverb || (convolutionRunning && stereo && frames <= (int) convL.size());
        if (reverb) {
            if (!convolutionRunning)
                convolution.reset(); // from silence
            std::copy(left, left + frames, convL.begin());
            std::copy(right, right + frames, convR.begin());
            float* channels[2] = { convL.data(), convR.data() };
            juce::dsp::AudioBlock<float> block(channels, 2, (size_t) frames);
            convolution.process(juce::dsp::ProcessContextReplacing<float>(block));
        }
        const float wetTarget = wantReverb ? std::pow(10.0f, convolutionWetDb.load(std::memory_order_relaxed) / 20.0f) : 0.0f;
        const float dryTarget = wantReverb ? dryGain(convolutionDryDb.load(std::memory_order_relaxed)) : 1.0f;
        const double rate = recordingRate.load(std::memory_order_relaxed);
        if (speakers) { // the heard room's settings, the head left out (from silence when it starts)
            if (!speakersRunning)
                roomSpeakers.reset();
            speakersRunning = true;
            auto s = room.settings();
            s.headphones = false;
            roomSpeakers.applySettings(s);
            std::copy(left, left + frames, speakersL.begin());
            std::copy(right, right + frames, speakersR.begin());
            roomSpeakers.process(speakersL.data(), speakersR.data(), frames);
            if (reverb)
                mixReverb(speakersL.data(), speakersR.data(), frames, wetTarget, dryTarget);
            eqSpeakers.update(rate, toneBassDb.load(std::memory_order_relaxed), tonePresenceDb.load(std::memory_order_relaxed),
                              toneAirDb.load(std::memory_order_relaxed));
            eqSpeakers.process(speakersL.data(), speakersR.data(), frames);
        } else {
            speakersRunning = false;
        }
        if (stereo)
            room.process(left, right, frames);
        if (reverb)
            mixReverb(left, right, frames, wetTarget, dryTarget);
        wetNow = reverb ? wetTarget : 0.0f;
        dryNow = reverb ? dryTarget : 1.0f;
        convolutionRunning = wantReverb;
        eqHeard.update(rate, toneBassDb.load(std::memory_order_relaxed), tonePresenceDb.load(std::memory_order_relaxed),
                       toneAirDb.load(std::memory_order_relaxed));
        eqHeard.process(left, right, frames);
        if (feedOpen) {
            pf::AudioFeedWriter::Block b;
            b.left[0] = left;
            b.right[0] = stereo ? right : left;
            const bool squeeze = streamCompressors[0].isOn() && frames <= (int) streamL.size();
            if (squeeze) { // the stream's compressor: on copies (what is heard stays as it is)
                std::copy(left, left + frames, streamL.begin());
                std::copy(stereo ? right : left, (stereo ? right : left) + frames, streamR.begin());
                streamCompressors[0].process(streamL.data(), streamR.data(), frames);
                b.left[0] = streamL.data();
                b.right[0] = streamR.data();
            }
            if (speakers) {
                if (squeeze)
                    streamCompressors[1].process(speakersL.data(), speakersR.data(), frames);
                b.left[1] = speakersL.data();
                b.right[1] = speakersR.data();
            }
            if (mixes) {
                if (squeeze)
                    streamCompressors[2].process(dryL.data(), dryR.data(), frames);
                b.left[2] = dryL.data();
                b.right[2] = dryR.data();
            }
            b.frames = frames;
            for (int i = 0; i < numEvents; ++i)
                feedEvents[(std::size_t) i] = { events[(std::size_t) i].offset, events[(std::size_t) i].status,
                                                events[(std::size_t) i].data1, events[(std::size_t) i].data2, 0, 0 };
            b.events = feedEvents.data();
            b.numEvents = numEvents;
            b.volume = volume.load(std::memory_order_relaxed);
            feed.write(b);
        }
        const float target = volume.load(std::memory_order_relaxed);
        if (target != volumeNow || target != 1.0f) {
            const float step = (target - volumeNow) / (float) frames;
            for (int k = 0; k < frames; ++k) {
                const float g = volumeNow + step * (float) (k + 1);
                left[k] *= g;
                if (stereo)
                    right[k] *= g;
            }
            volumeNow = target;
        }
        if (stereo)
            limiter.process(left, right, frames);
    }

    /// The engine's sound, at its rate: recorded, mirrored, measured; then the engine's clock advances.
    void finish(float* const* out, int channels, int frames) noexcept
    {
        live.push(out, channels, frames, samplesElapsed); // a copy into a lock-free FIFO: nothing waits on it
        if (streamCompressors[3].isOn() && channels >= 2 && frames <= (int) mirrorL.size()) { // what leaves for a stream
            std::copy(out[0], out[0] + frames, mirrorL.begin());
            std::copy(out[1], out[1] + frames, mirrorR.begin());
            streamCompressors[3].process(mirrorL.data(), mirrorR.data(), frames);
            const float* squeezed[2] = { mirrorL.data(), mirrorR.data() };
            mirror.push(squeezed, 2, frames);
        } else {
            mirror.push(out, channels, frames); // likewise
        }
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
        const double allowed = std::max(1.5 * block, gapAllowance.load(std::memory_order_relaxed)) + 0.002;
        if (lastCallbackTicks != 0 && !lastWasLate && (double) (startTicks - lastCallbackTicks) * tick > allowed)
            load.gaps.fetch_add(1, std::memory_order_relaxed);
        lastWasLate = used > 1.0f;
        if (lastWasLate)
            load.lateBlocks.fetch_add(1, std::memory_order_relaxed);
        lastCallbackTicks = startTicks;
        load.loadSum.store(load.loadSum.load(std::memory_order_relaxed) + used, std::memory_order_relaxed); // one writer
        load.callbacks.fetch_add(1, std::memory_order_release);
    }

    /// The room's output at the dry level, the reverb's added at the wet level (both gliding over the block).
    void mixReverb(float* left, float* right, int frames, float wetTarget, float dryTarget) noexcept
    {
        const float wetStep = (wetTarget - wetNow) / (float) frames, dryStep = (dryTarget - dryNow) / (float) frames;
        for (int k = 0; k < frames; ++k) {
            const float w = wetNow + wetStep * (float) (k + 1), d = dryNow + dryStep * (float) (k + 1);
            left[k] = d * left[k] + w * convL[(std::size_t) k];
            right[k] = d * right[k] + w * convR[(std::size_t) k];
        }
    }

    /// Audio thread (or a device starting): an impulse response handed over goes to the convolution (wait-free; the
    /// emptied hand-over back to the message thread, once it took the last one).
    bool takeImpulse() noexcept
    {
        if (irSpent.load(std::memory_order_acquire) == nullptr)
            if (auto* handed = irPending.exchange(nullptr, std::memory_order_acq_rel)) {
                const bool some = handed->buffer.getNumSamples() > 0;
                if (some)
                    convolution.loadImpulseResponse(std::move(handed->buffer), handed->rate,
                                                    handed->stereo ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no,
                                                    juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::yes);
                convolutionReady.store(some, std::memory_order_relaxed);
                irSpent.store(handed, std::memory_order_release);
            }
        return convolutionReady.load(std::memory_order_relaxed);
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
    pf::AudioFeedWriter feed; // before the callbacks start; written by the audio thread (blocks), the message thread (info)
    bool feedOpen = false;
    pf::Room roomSpeakers;    // the speakers mix's room (audio thread)
    bool speakersRunning = false;
    float volumeNow = 1.0f;   // the volume applied at the end of the last block (audio thread)
    std::vector<float> dryL, dryR, speakersL, speakersR;
    // The convolution reverb: the impulse response handed over (message thread -> audio thread, and back emptied).
    struct ImpulseLoad {
        juce::AudioBuffer<float> buffer;
        double rate = 48000.0;
        bool stereo = false;
    };
    std::atomic<ImpulseLoad*> irPending { nullptr }, irSpent { nullptr };
    std::shared_ptr<const ImpulseResponse> impulseShared; // message thread: the one in use, for the offline renders
    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 2048 } }; // no latency
    std::atomic<bool> convolutionReady { false };
    bool convolutionRunning = false; // audio thread
    float wetNow = 0.0f, dryNow = 1.0f;
    std::vector<float> convL, convR;
    OutputEq eqHeard, eqSpeakers; // audio thread
    std::array<pf::Compressor, 4> streamCompressors; // the feed's three mixes, the mirror
    std::vector<float> streamL, streamR, mirrorL, mirrorR;
    std::vector<pf::FeedEvent> feedEvents = std::vector<pf::FeedEvent>(kMaxEvents); // on the heap: hosts live on stacks (the self-test)
    juce::MidiBuffer midi;
    std::array<pf::Event, kMaxEvents> events {};
    std::atomic<bool> ready { false };

    std::int64_t samplesElapsed = 0; // audio thread
    std::atomic<double> callbackSeconds { 0.0 };     // the last callback's start (positionAt)
    std::atomic<std::int64_t> callbackElapsed { 0 }; // and the session position then
    std::int64_t lastCallbackTicks = 0; // audio thread
    std::atomic<double> gapAllowance { 0.0 }; // Linux: the longest gap between callbacks the device's buffer covers, s
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
