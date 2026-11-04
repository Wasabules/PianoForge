#include "OfflineRender.h"

#include "ReferencePlayer.h"
#include "SampleBank.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>

namespace pf::app {

int runOfflineRender(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--render");
    const juce::File midiFile(arguments[i + 1]), outFile(arguments[i + 2]), corpus(arguments[i + 3]);
    const juce::File report(outFile.withFileExtension("txt"));
    juce::String log;
    if (!outFile.getParentDirectory().createDirectory()) {
        std::fprintf(stderr, "Cannot create %s\n", outFile.getParentDirectory().getFullPathName().toRawUTF8());
        return 1;
    }

    juce::MidiFile midi;
    {
        juce::FileInputStream in(midiFile);
        if (!in.openedOk() || !midi.readFrom(in)) {
            report.replaceWithText("Cannot read the MIDI file " + midiFile.getFullPathName());
            return 1;
        }
    }
    midi.convertTimestampTicksToSeconds();
    juce::MidiMessageSequence sequence;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        sequence.addSequence(*midi.getTrack(t), 0.0);
    sequence.updateMatchedPairs();

    BankLoader loader;
    loader.load(corpus);
    while (loader.isLoading())
        juce::Thread::sleep(20);
    auto bank = loader.takeResult();
    if (bank == nullptr || bank->samples.empty()) {
        report.replaceWithText("No take found in " + corpus.getFullPathName());
        return 1;
    }
    log << "Corpus: " << loader.status() << "\n";
    const double fs = bank->sampleRate;

    ReferencePlayer player;
    player.installBank(std::move(bank));
    player.outputGain = 1.0f; // unity gain: the render is compared with the library's own render
    if (arguments[i + 4].isNotEmpty())
        player.restrikeFadeSeconds = (float) arguments[i + 4].getDoubleValue();
    constexpr int block = 512;
    player.prepare(fs, block);

    const double lastEvent = sequence.getEndTime();
    const auto total = (int) std::ceil((lastEvent + 4.0) * fs);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    std::vector<pf::Event> events;
    int next = 0, notes = 0;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (int pos = 0; pos < total; pos += block) {
        const int n = juce::jmin(block, total - pos);
        events.clear();
        while (next < sequence.getNumEvents()) {
            const auto& m = sequence.getEventPointer(next)->message;
            const auto sample = (std::int64_t) std::llround(m.getTimeStamp() * fs);
            if (sample >= pos + n)
                break;
            ++next;
            if (m.isNoteOnOrOff() || m.isController()) {
                const auto* raw = m.getRawData();
                events.push_back({ (std::uint32_t) juce::jlimit<std::int64_t>(0, n - 1, sample - pos),
                                   (std::uint8_t) (raw[0] & 0xF0), raw[1], m.getRawDataSize() > 2 ? raw[2] : (std::uint8_t) 0 });
                notes += m.isNoteOn() ? 1 : 0;
            }
        }
        float* outputs[2] = { out.getWritePointer(0, pos), out.getWritePointer(1, pos) };
        player.process(outputs, 2, n, events.data(), (int) events.size());
    }
    const double seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;

    outFile.deleteFile();
    juce::WavAudioFormat wav;
    auto fileStream = std::make_unique<juce::FileOutputStream>(outFile);
    if (fileStream->failedToOpen()) {
        report.replaceWithText("Cannot write " + outFile.getFullPathName());
        return 1;
    }
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(fs)
                             .withNumChannels(2)
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr) {
        report.replaceWithText("Cannot write " + outFile.getFullPathName());
        return 1;
    }
    writer->writeFromAudioSampleBuffer(out, 0, total);
    writer.reset();

    log << "Rendered " << notes << " notes, " << juce::String(total / fs, 1) << " s of audio in "
        << juce::String(seconds, 2) << " s; peak " << juce::String(juce::Decibels::gainToDecibels(out.getMagnitude(0, total)), 1)
        << " dBFS; re-strike fade " << juce::String(player.restrikeFadeSeconds.load(), 2) << " s\nOutput: "
        << outFile.getFullPathName() << "\n";
    report.replaceWithText(log, false, false, "\n");
    return 0;
}

} // namespace pf::app
