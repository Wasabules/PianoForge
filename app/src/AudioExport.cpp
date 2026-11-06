#include "AudioExport.h"

#include "Mp3Encoder.h"

#include <pianoforge/upsampler.hpp>

#include <cmath>
#include <unordered_map>

namespace pf::app {

const std::vector<SoundFormatInfo>& soundFormats()
{
    static const std::vector<SoundFormatInfo> formats {
        { SoundFormat::wavFloat, "wav32", "WAV, 32-bit float (lossless, as played)", ".wav", "WAV 32-bit" },
        { SoundFormat::wav24, "wav24", "WAV, 24-bit (lossless, 3/4 of the size)", ".wav", "WAV 24-bit" },
        { SoundFormat::flac, "flac", "FLAC, 24-bit (lossless, about 1/3 of the size)", ".flac", "FLAC" },
        { SoundFormat::mp3, "mp3", "MP3, 320 kbit/s", ".mp3", "MP3" },
        { SoundFormat::opus, "opus", "Opus, 192 kbit/s", ".opus", "Opus" },
    };
    return formats;
}

const SoundFormatInfo& infoFor(SoundFormat format)
{
    for (const auto& f : soundFormats())
        if (f.format == format)
            return f;
    return soundFormats().front();
}

bool canWrite(SoundFormat format)
{
    return format != SoundFormat::mp3 || Mp3Encoder::available();
}

namespace {

/// The factor (1, 2, 4 or 8) that brings `rate` to one the MP3 encoder takes, or 0.
int mp3Factor(double rate)
{
    for (int k = 1; k <= 8; k *= 2)
        if (Mp3Encoder::takesRate(rate / k))
            return k;
    return 0;
}

} // namespace

juce::Result convertSound(const juce::File& source, const juce::File& target, SoundFormat format, std::atomic<float>* progress,
                          const std::atomic<bool>* cancel)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(source.createInputStream().release(), true));
    if (reader == nullptr)
        return juce::Result::fail("cannot read " + source.getFileName());
    const int channels = (int) reader->numChannels;
    const double rate = reader->sampleRate;
    const auto length = reader->lengthInSamples;
    if (!target.getParentDirectory().createDirectory())
        return juce::Result::fail("cannot create " + target.getParentDirectory().getFullPathName());
    target.deleteFile();

    // The writer: JUCE's for WAV, FLAC and Opus; Media Foundation's for MP3.
    std::unique_ptr<juce::AudioFormatWriter> writer;
    Mp3Encoder mp3;
    int factor = 1;
    std::vector<pf::Downsampler> down;
    std::unordered_map<juce::String, juce::String> metadata; // the WAV's (Broadcast WAV: date, description)
    for (const auto& key : reader->metadataValues.getAllKeys()) // (the coding history: the new word length)
        metadata[key] = format == SoundFormat::wav24 ? reader->metadataValues[key].replace("W=32", "W=24") : reader->metadataValues[key];
    auto create = [&](juce::AudioFormat& f, juce::AudioFormatWriterOptions options) -> juce::Result {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(target);
        if (static_cast<juce::FileOutputStream&>(*stream).failedToOpen())
            return juce::Result::fail("cannot write " + target.getFullPathName());
        writer = f.createWriterFor(stream, options.withSampleRate(rate).withNumChannels(channels));
        return writer != nullptr ? juce::Result::ok() : juce::Result::fail("cannot create a writer for " + target.getFileName());
    };
    juce::FlacAudioFormat flacFormat;
    juce::OpusAudioFormat opusFormat;
    juce::Result made = juce::Result::ok();
    switch (format) {
    case SoundFormat::wavFloat:
        made = create(wav, juce::AudioFormatWriterOptions {}
                               .withBitsPerSample(32)
                               .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)
                               .withMetadataValues(metadata));
        break;
    case SoundFormat::wav24:
        made = create(wav, juce::AudioFormatWriterOptions {}.withBitsPerSample(24).withMetadataValues(metadata));
        break;
    case SoundFormat::flac:
        made = create(flacFormat, juce::AudioFormatWriterOptions {}.withBitsPerSample(24).withQualityOptionIndex(5));
        break;
    case SoundFormat::opus: {
        const int index = opusFormat.getQualityOptions().indexOf("192 kbps");
        made = create(opusFormat, juce::AudioFormatWriterOptions {}.withBitsPerSample(32).withQualityOptionIndex(std::max(0, index)));
        break;
    }
    case SoundFormat::mp3: {
        factor = mp3Factor(rate);
        if (factor == 0)
            return juce::Result::fail("MP3 cannot be made at " + juce::String(rate / 1000.0, 1) + " kHz");
        std::string error;
        if (!mp3.open(target.getFullPathName().toStdString(), (int) std::lround(rate / factor), channels, 320, error)) {
            target.deleteFile();
            return juce::Result::fail("MP3: " + juce::String::fromUTF8(error.c_str()));
        }
        down.resize((std::size_t) channels);
        for (auto& d : down)
            d.prepare(factor);
        break;
    }
    }
    if (made.failed()) {
        target.deleteFile();
        return made;
    }

    // Through, in blocks.
    constexpr int kBlock = 32768;
    juce::AudioBuffer<float> block(channels, kBlock);
    std::vector<std::vector<float>> low((std::size_t) channels, std::vector<float>((std::size_t) kBlock + 8));
    std::vector<const float*> lowPointers((std::size_t) channels);
    juce::Result result = juce::Result::ok();
    for (juce::int64 pos = 0; pos < length && result.wasOk(); pos += kBlock) {
        if (cancel != nullptr && cancel->load()) {
            result = juce::Result::fail("cancelled");
            break;
        }
        const int n = (int) std::min<juce::int64>(kBlock, length - pos);
        if (!reader->read(&block, 0, n, pos, true, true)) {
            result = juce::Result::fail("cannot read " + source.getFileName());
            break;
        }
        if (writer != nullptr) {
            if (!writer->writeFromAudioSampleBuffer(block, 0, n))
                result = juce::Result::fail("cannot write " + target.getFileName() + " (disk full?)");
        } else {
            int produced = n;
            for (int c = 0; c < channels; ++c) {
                if (factor > 1)
                    produced = down[(std::size_t) c].process(block.getReadPointer(c), n, low[(std::size_t) c].data());
                lowPointers[(std::size_t) c] = factor > 1 ? low[(std::size_t) c].data() : block.getReadPointer(c);
            }
            std::string error;
            if (!mp3.write(lowPointers.data(), produced, error))
                result = juce::Result::fail("MP3: " + juce::String::fromUTF8(error.c_str()));
        }
        if (progress != nullptr)
            progress->store((float) ((double) (pos + n) / (double) std::max<juce::int64>(1, length)));
    }
    if (writer != nullptr) {
        writer.reset(); // finishes the file
    } else if (result.wasOk()) {
        std::string error;
        if (!mp3.close(error))
            result = juce::Result::fail("MP3: " + juce::String::fromUTF8(error.c_str()));
    }
    if (result.failed()) {
        mp3.abandon(); // releases the file before it goes
        target.deleteFile();
    }
    return result;
}

RecordingExporter::~RecordingExporter()
{
    {
        const std::lock_guard<std::mutex> guard(lock);
        stopping = true;
    }
    cancel = true;
    wake.notify_all();
    if (worker.joinable())
        worker.join();
    // The jobs not started: their masters under the recordings' names.
    for (const auto& job : queue)
        if (job.removeMaster)
            if (const auto asWav = job.master.getSiblingFile(job.label + ".wav"); !asWav.exists())
                job.master.moveFileTo(asWav);
}

void RecordingExporter::submit(Job job)
{
    {
        const std::lock_guard<std::mutex> guard(lock);
        queue.push_back(std::move(job));
        if (!worker.joinable())
            worker = std::thread([this] { run(); });
    }
    wake.notify_all();
}

bool RecordingExporter::busy() const
{
    const std::lock_guard<std::mutex> guard(lock);
    return working || !queue.empty();
}

juce::String RecordingExporter::progressText() const
{
    const std::lock_guard<std::mutex> guard(lock);
    if (!working)
        return {};
    return current + " " + juce::String(juce::roundToInt(progress.load() * 100.0f)) + " %";
}

std::vector<RecordingExporter::Report> RecordingExporter::takeReports()
{
    const std::lock_guard<std::mutex> guard(lock);
    return std::exchange(reports, {});
}

void RecordingExporter::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> guard(lock);
            wake.wait(guard, [this] { return stopping || !queue.empty(); });
            if (stopping)
                return; // the jobs not started: their masters stay as they are
            job = std::move(queue.front());
            queue.pop_front();
            working = true;
        }
        convert(job);
        const std::lock_guard<std::mutex> guard(lock);
        working = false;
        current.clear();
    }
}

void RecordingExporter::convert(Job& job)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    juce::StringArray made, failed;
    for (const auto& [format, file] : job.targets) {
        {
            const std::lock_guard<std::mutex> guard(lock);
            current = infoFor(format).shortName;
        }
        progress = 0.0f;
        const auto result = convertSound(job.master, file, format, &progress, &cancel);
        if (result.wasOk())
            made.add(file.getFileExtension() == ".wav" ? juce::String(infoFor(format).shortName) : file.getFileExtension());
        else
            failed.add(juce::String(infoFor(format).shortName) + ": " + result.getErrorMessage());
    }
    Report report;
    if (failed.isEmpty()) {
        if (job.removeMaster)
            job.master.deleteFile();
        report.text = "Saved " + job.label + " " + made.joinIntoString(" + ") + " (converted in "
                    + juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) + " s)";
    } else {
        report.ok = false;
        // Nothing is lost: the master stays, under the recording's name when it was to go.
        juce::String kept = job.master.getFileName();
        if (job.removeMaster) {
            const auto asWav = job.master.getSiblingFile(job.label + ".wav");
            if (!asWav.exists() && job.master.moveFileTo(asWav))
                kept = asWav.getFileName();
        }
        report.text = (made.isEmpty() ? juce::String() : "Saved " + made.joinIntoString(" + ") + "; ") + "failed: "
                    + failed.joinIntoString("; ") + " (kept " + kept + ")";
    }
    const std::lock_guard<std::mutex> guard(lock);
    reports.push_back(report);
}

} // namespace pf::app
