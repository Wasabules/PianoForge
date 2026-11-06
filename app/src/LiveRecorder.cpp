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
        startSample.store(blockStart, std::memory_order_relaxed);
        pushed.store(0, std::memory_order_relaxed);
        if (!state.compare_exchange_strong(s, kRecording, std::memory_order_acq_rel))
            return; // cancelled meanwhile
        s = kRecording;
    } else if (s == kStopRequested) {
        // The recording ends where this block starts: [start, start + pushed).
        endSample.store(startSample.load(std::memory_order_relaxed) + pushed.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
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

void LiveRecorder::deviceStopped() noexcept
{
    for (int s = state.load(); s == kRecording || s == kStopRequested; s = state.load()) {
        endSample.store(startSample.load() + pushed.load());
        if (state.compare_exchange_strong(s, kStopped))
            break;
    }
}

juce::Result LiveRecorder::start(const juce::File& wav, double sampleRate, std::uint32_t sessionSegment,
                                 const juce::String& description)
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
        writing = true;
        writerThread = std::thread([this] { writerLoop(); });
    }
    armed = true;
    state.store(kRequested, std::memory_order_release); // everything above happens before the audio thread sees it
    return juce::Result::ok();
}

void LiveRecorder::drain()
{
    const auto scope = fifo.read(fifo.getNumReady());
    if (scope.blockSize1 > 0 && !writer->writeFromAudioSampleBuffer(buffer, scope.startIndex1, scope.blockSize1))
        writeFailed = true;
    if (scope.blockSize2 > 0 && !writer->writeFromAudioSampleBuffer(buffer, scope.startIndex2, scope.blockSize2))
        writeFailed = true;
}

void LiveRecorder::writerLoop()
{
    while (writing.load()) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drain();
}

LiveRecorder::Span LiveRecorder::stop()
{
    Span span;
    if (!armed)
        return span;
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
    span.sampleRate = rate;
    span.segment = segment;
    span.start = startSample.load();
    span.end = started ? endSample.load() : span.start;
    span.dropped = dropped.load();
    span.wav = withAudio ? wavFile : juce::File();
    span.valid = started && span.end > span.start;
    if (!started)
        span.error = "the audio device did not run";
    else if (writeFailed.load())
        span.error = "writing " + wavFile.getFullPathName() + " failed (disk full?)";
    state = kIdle;
    armed = false;
    withAudio = false;
    return span;
}

} // namespace pf::app
