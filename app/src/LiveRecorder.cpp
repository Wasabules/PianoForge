// SPDX-License-Identifier: AGPL-3.0-or-later

#include "LiveRecorder.h"

#include <chrono>

namespace pf::app {

LiveRecorder::~LiveRecorder()
{
    if (armed)
        stop();
}

void LiveRecorder::push(const float* const* outputs, int numChannels, int numSamples, std::int64_t blockStart) noexcept
{
    int s = state.load(std::memory_order_acquire);
    if (s == kRequested) {
        firstSample.store(blockStart, std::memory_order_relaxed);
        pushed.store(0, std::memory_order_relaxed);
        if (!atFirst) { // at once: from this block
            std::int64_t none = -1;
            trigger.compare_exchange_strong(none, blockStart, std::memory_order_acq_rel);
        }
        if (!state.compare_exchange_strong(s, kRecording, std::memory_order_acq_rel))
            return; // cancelled meanwhile
        s = kRecording;
    } else if (s == kStopRequested) {
        // The blocks pushed end where this one starts: [first, first + pushed).
        endSample.store(firstSample.load(std::memory_order_relaxed) + pushed.load(std::memory_order_relaxed), std::memory_order_relaxed);
        state.compare_exchange_strong(s, kStopped, std::memory_order_acq_rel);
        return;
    }
    if (s != kRecording || numSamples <= 0)
        return;
    if (withAudio) {
        int start1, size1, start2, size2;
        fifo.prepareToWrite(numSamples, start1, size1, start2, size2);
        if (size1 + size2 < numSamples) {
            dropped.fetch_add(numSamples, std::memory_order_relaxed); // the writer fell behind (disk stalled)
        } else {
            for (int c = 0; c < channels; ++c) {
                const float* source = c < numChannels ? outputs[c] : nullptr;
                if (source != nullptr) {
                    buffer.copyFrom(c, start1, source, size1);
                    if (size2 > 0)
                        buffer.copyFrom(c, start2, source + size1, size2);
                } else {
                    buffer.clear(c, start1, size1);
                    if (size2 > 0)
                        buffer.clear(c, start2, size2);
                }
            }
            fifo.finishedWrite(size1 + size2);
        }
    }
    pushed.fetch_add(numSamples, std::memory_order_relaxed);
}

void LiveRecorder::noteOn(std::int64_t position) noexcept
{
    const int s = state.load(std::memory_order_acquire);
    if (!atFirst || (s != kRequested && s != kRecording) || position < notesFrom.load(std::memory_order_acquire))
        return;
    std::int64_t none = -1;
    trigger.compare_exchange_strong(none, position, std::memory_order_acq_rel);
}

void LiveRecorder::deviceStopped() noexcept
{
    for (int s = state.load(); s == kRecording || s == kStopRequested; s = state.load()) {
        endSample.store(firstSample.load() + pushed.load());
        if (state.compare_exchange_strong(s, kStopped))
            break;
    }
}

std::int64_t LiveRecorder::beginning() const noexcept
{
    return std::max(trigger.load(std::memory_order_acquire) - preroll, firstSample.load(std::memory_order_acquire));
}

double LiveRecorder::seconds() const noexcept
{
    if (rate <= 0.0 || trigger.load() < 0)
        return 0.0;
    return (double) std::max<std::int64_t>(0, firstSample.load() + pushed.load() - beginning()) / rate;
}

juce::Result LiveRecorder::start(const juce::File& wav, double sampleRate, std::uint32_t sessionSegment,
                                 const juce::String& description, bool atFirstNote, std::int64_t notesFromPosition)
{
    if (armed)
        return juce::Result::fail("already recording");
    if (sampleRate <= 0.0)
        return juce::Result::fail("no audio device is running");
    rate = sampleRate;
    segment = sessionSegment;
    wavFile = wav;
    channels = 2;
    pushed = 0;
    dropped = 0;
    trigger = -1;
    endAt = -1;
    notesFrom = notesFromPosition;
    atFirst = atFirstNote;
    preroll = atFirstNote ? (std::int64_t) std::llround(kPrerollSeconds * rate) : 0;
    hold = (std::int64_t) std::llround(kHoldSeconds * rate);
    writeFailed = false;
    withAudio = wav != juce::File();
    if (withAudio) {
        if (!wav.getParentDirectory().createDirectory())
            return juce::Result::fail("cannot create " + wav.getParentDirectory().getFullPathName());
        wav.deleteFile();
        auto fileStream = std::make_unique<juce::FileOutputStream>(wav);
        if (fileStream->failedToOpen())
            return juce::Result::fail("cannot write " + wav.getFullPathName());
        // Broadcast WAV: the date and time of the start, and its position in the device session.
        std::unordered_map<juce::String, juce::String> metadata;
        const auto bwav = juce::WavAudioFormat::createBWAVMetadata(
            description, "PianoForge", wav.getFileNameWithoutExtension(), juce::Time::getCurrentTime(), 0,
            "A=PCM,F=" + juce::String(juce::roundToInt(rate)) + ",W=32,M=stereo,T=PianoForge live recording");
        for (int i = 0; i < bwav.size(); ++i)
            metadata[bwav.getAllKeys()[i]] = bwav.getAllValues()[i];
        std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate(rate)
                                 .withNumChannels(channels)
                                 .withBitsPerSample(32)
                                 .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)
                                 .withMetadataValues(metadata);
        juce::WavAudioFormat format;
        writer = format.createWriterFor(stream, options);
        if (writer == nullptr)
            return juce::Result::fail("cannot create a WAV writer for " + wav.getFullPathName());
        const int capacity = (int) std::ceil(kFifoSeconds * rate);
        buffer.setSize(channels, capacity);
        buffer.clear();
        fifo.setTotalSize(capacity);
        fifo.reset();
        front = 0;
        written = 0;
        writing = true;
        writerThread = std::thread([this] { writerLoop(); });
    }
    armed = true;
    state.store(kRequested, std::memory_order_release); // everything above happens before the audio thread sees it
    return juce::Result::ok();
}

void LiveRecorder::drain(bool final)
{
    int ready = fifo.getNumReady();
    if (ready <= 0)
        return;
    if (front == 0)
        front = firstSample.load(std::memory_order_acquire); // the first block's position (pushed before its frames)
    auto discard = [&](int n) {
        const auto scope = fifo.read(n);
        front += scope.blockSize1 + scope.blockSize2;
        ready -= scope.blockSize1 + scope.blockSize2;
    };
    if (trigger.load(std::memory_order_acquire) < 0) { // waiting for the first note: only the pre-roll kept
        if (ready > preroll)
            discard(ready - (int) preroll);
        return;
    }
    if (const auto begin = beginning(); front < begin)
        discard((int) std::min<std::int64_t>(ready, begin - front));
    // What may be written now: all but the last kHoldSeconds (an end may still come before), up to an end asked for.
    std::int64_t limit = final ? ready : std::max<std::int64_t>(0, ready - hold);
    if (const auto at = endAt.load(std::memory_order_acquire); at >= 0)
        limit = std::min(limit, std::max<std::int64_t>(0, at - front));
    if (final)
        limit = std::min(limit, std::max<std::int64_t>(0, endSample.load(std::memory_order_acquire) - front));
    if (limit <= 0)
        return;
    const auto scope = fifo.read((int) limit);
    if (scope.blockSize1 > 0 && !writer->writeFromAudioSampleBuffer(buffer, scope.startIndex1, scope.blockSize1))
        writeFailed = true;
    if (scope.blockSize2 > 0 && !writer->writeFromAudioSampleBuffer(buffer, scope.startIndex2, scope.blockSize2))
        writeFailed = true;
    front += scope.blockSize1 + scope.blockSize2;
    written += scope.blockSize1 + scope.blockSize2;
}

void LiveRecorder::writerLoop()
{
    while (writing.load()) {
        drain(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drain(true);
}

LiveRecorder::Span LiveRecorder::stop(std::int64_t at)
{
    Span span;
    if (!armed)
        return span;
    if (at >= 0)
        endAt.store(at, std::memory_order_release); // the writer goes no further from now on
    bool started = true;
    for (int s = state.load();; s = state.load()) {
        if (s == kRecording && state.compare_exchange_strong(s, kStopRequested))
            break;
        if (s == kRequested && state.compare_exchange_strong(s, kIdle)) {
            started = false; // the device never ran a block
            break;
        }
        if (s == kStopped || s == kIdle || s == kStopRequested)
            break;
    }
    if (started) { // the audio thread acknowledges at its next block; without one, the recording ends where it is
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (state.load() != kStopped && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        deviceStopped();
    }
    if (writerThread.joinable()) {
        writing = false;
        writerThread.join();
    }
    writer.reset(); // flushes and closes the file
    const bool played = trigger.load() >= 0;
    span.sampleRate = rate;
    span.segment = segment;
    span.start = played ? beginning() : 0;
    span.end = endSample.load();
    if (at >= 0 && at < span.end) // ended earlier: where asked, or as near as the frames already written allow
        span.end = std::max(at, withAudio ? span.start + written : span.start);
    span.dropped = dropped.load();
    span.wav = withAudio ? wavFile : juce::File();
    span.valid = started && played && span.end > span.start;
    if (!started)
        span.error = "the audio device did not run";
    else if (!played)
        span.error = "no note was played";
    else if (writeFailed.load())
        span.error = "writing " + wavFile.getFullPathName() + " failed (disk full?)";
    state = kIdle;
    armed = false;
    withAudio = false;
    return span;
}

} // namespace pf::app
