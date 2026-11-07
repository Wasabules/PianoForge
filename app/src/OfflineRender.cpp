#include "OfflineRender.h"

#include <pianoforge/limiter.hpp>

#include "ReferencePlayer.h"

#include <pianoforge/modal_engine.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>

namespace pf::app {

juce::AudioBuffer<float> renderSequence(const juce::MidiMessageSequence& sequence, std::shared_ptr<const SampleBank> bank,
                                        double sampleRate, float restrikeFadeSeconds, bool playWholeTake, double tailSeconds)
{
    ReferencePlayer player;
    player.installBank(std::move(bank));
    player.outputGain = 1.0f; // unity: the render is compared with other renders, the lab volume is not applied
    player.restrikeFadeSeconds = restrikeFadeSeconds;
    player.playWholeTake = playWholeTake;
    return renderWith(player, sequence, sampleRate, tailSeconds);
}

juce::AudioBuffer<float> renderSequenceModal(const juce::MidiMessageSequence& sequence,
                                             std::shared_ptr<const pf::ModalProfile> profile, double sampleRate,
                                             const pf::ModalEngine::Settings& settings, double tailSeconds)
{
    pf::ModalEngine engine;
    engine.applySettings(settings); // before the profile: the free strings are tuned when it is installed
    engine.install(std::move(profile));
    engine.outputGain = 1.0f;
    return renderWith(engine, sequence, sampleRate, tailSeconds);
}

void applyLimiter(juce::AudioBuffer<float>& audio, bool enabled, double sampleRate)
{
    if (!enabled || audio.getNumChannels() < 2)
        return;
    pf::Limiter limiter;
    limiter.prepare(sampleRate);
    limiter.process(audio.getWritePointer(0), audio.getWritePointer(1), audio.getNumSamples());
}

void applyRoom(juce::AudioBuffer<float>& audio, const pf::Room::Settings& settings, double sampleRate)
{
    if (!settings.enabled || audio.getNumChannels() < 2)
        return;
    pf::Room room;
    room.applySettings(settings);
    room.prepare(sampleRate);
    for (int start = 0; start < audio.getNumSamples(); start += 512) {
        const int n = juce::jmin(512, audio.getNumSamples() - start);
        room.process(audio.getWritePointer(0, start), audio.getWritePointer(1, start), n);
    }
}

juce::AudioBuffer<float> renderWith(pf::Engine& player, const juce::MidiMessageSequence& sequence, double sampleRate,
                                    double tailSeconds)
{
    constexpr int block = 512;
    player.prepare(sampleRate, block);

    const auto total = (int) std::ceil((sequence.getEndTime() + tailSeconds) * sampleRate);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    std::vector<pf::Event> events;
    int next = 0;
    for (int pos = 0; pos < total; pos += block) {
        const int n = juce::jmin(block, total - pos);
        events.clear();
        while (next < sequence.getNumEvents()) {
            const auto& m = sequence.getEventPointer(next)->message;
            const auto sample = (std::int64_t) std::llround(m.getTimeStamp() * sampleRate);
            if (sample >= pos + n)
                break;
            ++next;
            if (m.isNoteOnOrOff() || m.isController()) {
                const auto* raw = m.getRawData();
                events.push_back({ (std::uint32_t) juce::jlimit<std::int64_t>(0, n - 1, sample - pos),
                                   (std::uint8_t) (raw[0] & 0xF0), raw[1], m.getRawDataSize() > 2 ? raw[2] : (std::uint8_t) 0 });
            }
        }
        float* outputs[2] = { out.getWritePointer(0, pos), out.getWritePointer(1, pos) };
        player.process(outputs, 2, n, events.data(), (int) events.size());
    }
    return out;
}

juce::Result writeWav(const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file)
{
    if (!file.getParentDirectory().createDirectory())
        return juce::Result::fail("cannot create " + file.getParentDirectory().getFullPathName());
    file.deleteFile();
    auto fileStream = std::make_unique<juce::FileOutputStream>(file);
    if (fileStream->failedToOpen())
        return juce::Result::fail("cannot write " + file.getFullPathName());
    std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sampleRate)
                             .withNumChannels(audio.getNumChannels())
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr)
        return juce::Result::fail("cannot create a WAV writer for " + file.getFullPathName());
    writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    return juce::Result::ok();
}

int runOfflineRender(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--render");
    const juce::File midiFile(arguments[i + 1]), outFile(arguments[i + 2]), corpus(arguments[i + 3]);
    const juce::File report(outFile.withFileExtension("txt"));
    if (!outFile.getParentDirectory().createDirectory()) {
        std::fprintf(stderr, "Cannot create %s\n", outFile.getParentDirectory().getFullPathName().toRawUTF8());
        return 1;
    }

    juce::MidiFile midi;
    {
        juce::FileInputStream in(midiFile);
        if (!in.openedOk() || !midi.readFrom(in, false)) { // false: no added note-offs (see TakeRecorder)
            report.replaceWithText("Cannot read the MIDI file " + midiFile.getFullPathName());
            return 1;
        }
    }
    midi.convertTimestampTicksToSeconds();
    juce::MidiMessageSequence sequence;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        sequence.addSequence(*midi.getTrack(t), 0.0);

    BankLoader loader;
    loader.load(corpus);
    while (loader.isLoading())
        juce::Thread::sleep(20);
    std::shared_ptr<const SampleBank> bank = loader.takeResult();
    if (bank == nullptr || bank->samples.empty()) {
        report.replaceWithText("No take found in " + corpus.getFullPathName());
        return 1;
    }
    const double fs = bank->sampleRate;
    const float restrike = arguments[i + 4].isNotEmpty() && !arguments[i + 4].startsWith("--")
                               ? (float) arguments[i + 4].getDoubleValue() : 0.8f;
    const bool modal = arguments.indexOf("--engine") >= 0 && arguments[arguments.indexOf("--engine") + 1] == "modal";
    std::shared_ptr<const pf::ModalProfile> profile;
    if (modal) {
        const auto path = corpus.getParentDirectory().getChildFile("modal-profile.bin");
        std::string error;
        profile = pf::loadModalProfile(path.getFullPathName().toStdString(), error);
        if (profile == nullptr) {
            report.replaceWithText("Cannot load the modal profile: " + juce::String(error));
            return 1;
        }
    }
    const auto start = juce::Time::getMillisecondCounterHiRes();
    pf::ModalEngine::Settings settings; // the engine's defaults
    const auto out = modal ? renderSequenceModal(sequence, profile, fs, settings) : renderSequence(sequence, bank, fs, restrike, false);
    const double seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
    if (const auto written = writeWav(out, fs, outFile); written.failed()) {
        report.replaceWithText(written.getErrorMessage());
        return 1;
    }
    int notes = 0;
    for (const auto* e : sequence)
        notes += e->message.isNoteOn() ? 1 : 0;
    juce::String log;
    log << "Corpus: " << loader.status() << "\nEngine: " << (modal ? "modal" : "reference") << "\nRendered " << notes << " notes, " << juce::String(out.getNumSamples() / fs, 1)
        << " s of audio in " << juce::String(seconds, 2) << " s; peak "
        << juce::String(juce::Decibels::gainToDecibels(out.getMagnitude(0, out.getNumSamples())), 1) << " dBFS; re-strike fade "
        << juce::String(restrike, 2) << " s\nOutput: " << outFile.getFullPathName() << "\n";
    report.replaceWithText(log, false, false, "\n");
    return 0;
}

} // namespace pf::app
