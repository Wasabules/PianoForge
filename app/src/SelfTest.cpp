#include "SelfTest.h"

#include "ReferencePlayer.h"
#include "SampleBank.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace pf::app {

namespace {

struct Report {
    juce::String text;
    bool passed = true;
    void line(const juce::String& s) { text << s << "\n"; }
    void check(bool condition, const juce::String& s)
    {
        line((condition ? "[PASS]  " : "[FAIL]  ") + s);
        passed = passed && condition;
    }
};

/// Renders `frames` stereo frames: note-on at the start, note-off at `releaseAt` (-1: never).
juce::AudioBuffer<float> renderNote(ReferencePlayer& p, int note, int velocity, int frames, int block, int releaseAt = -1)
{
    juce::AudioBuffer<float> out(2, frames);
    out.clear();
    std::vector<float> l((size_t) block), r((size_t) block);
    for (int start = 0; start < frames; start += block) {
        const int n = juce::jmin(block, frames - start);
        pf::Event events[2];
        int count = 0;
        if (start == 0)
            events[count++] = { 0, 0x90, (std::uint8_t) note, (std::uint8_t) velocity };
        if (releaseAt >= start && releaseAt < start + n)
            events[count++] = { (std::uint32_t) (releaseAt - start), 0x80, (std::uint8_t) note, 64 };
        float* outputs[2] = { l.data(), r.data() };
        p.process(outputs, 2, n, events, count);
        out.copyFrom(0, start, l.data(), n);
        out.copyFrom(1, start, r.data(), n);
    }
    return out;
}

/// Fades everything out and flushes the tails before the next check.
void flush(ReferencePlayer& p, double fs)
{
    p.allSoundOff();
    std::vector<float> l(512), r(512);
    float* outputs[2] = { l.data(), r.data() };
    for (int i = 0; i < (int) (0.3 * fs) / 512 + 1; ++i)
        p.process(outputs, 2, 512, nullptr, 0);
}

juce::AudioBuffer<float> decodeFile(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(file) };
    juce::AudioBuffer<float> b(2, reader ? (int) reader->lengthInSamples : 0);
    if (reader)
        reader->read(&b, 0, b.getNumSamples(), 0, true, true);
    return b;
}

double rmsDb(const juce::AudioBuffer<float>& b, int start, int n)
{
    double sum = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int k = start; k < juce::jmin(start + n, b.getNumSamples()); ++k)
            sum += (double) b.getSample(c, k) * b.getSample(c, k);
    return 10.0 * std::log10(juce::jmax(1.0e-30, sum / (2.0 * n)));
}

} // namespace

int runSelfTest(const juce::StringArray& arguments)
{
    const int i = arguments.indexOf("--selftest");
    const juce::File folder(arguments[i + 1]);
    const juce::File reportFile(arguments[i + 2].isNotEmpty() ? arguments[i + 2] : "selftest.txt");
    Report r;
    r.line("PianoForge reference player self-test - " + juce::Time::getCurrentTime().toString(true, true));
    r.line("Corpus: " + folder.getFullPathName());

    // 1. Load a subset: A0, C4 and C8 at every velocity, plus every note at velocity 64.
    BankLoader loader;
    loader.load(folder, [](int note, int vel) { return note == 21 || note == 60 || note == 108 || vel == 64; });
    while (loader.isLoading())
        juce::Thread::sleep(20);
    auto bank = loader.takeResult();
    r.line("Loading: " + loader.status());
    r.check(bank != nullptr && !bank->samples.empty(), "the corpus contains takes");
    if (bank == nullptr || bank->samples.empty()) {
        reportFile.replaceWithText(r.text);
        return 1;
    }
    const double fs = bank->sampleRate;
    std::vector<const Sample*> takes;
    for (const auto& s : bank->samples)
        if (s.length > 0)
            takes.push_back(&s);

    ReferencePlayer player;
    const auto* b = bank.get();
    player.installBank(std::move(bank));
    player.outputGain = 1.0f;

    // 2. Fidelity: at the corpus rate, playback reproduces the file (up to the dithered 16 bits).
    player.prepare(fs, 512);
    player.playWholeTake = true;
    double worst = -300.0;
    for (const auto* s : takes) {
        flush(player, fs);
        const auto rendered = renderNote(player, s->note, s->velocity, (int) s->length, 512);
        const auto reference = decodeFile(s->file);
        float diff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < juce::jmin(rendered.getNumSamples(), reference.getNumSamples()); ++k)
                diff = juce::jmax(diff, std::abs(rendered.getSample(c, k) - reference.getSample(c, k)));
        worst = juce::jmax(worst, (double) juce::Decibels::gainToDecibels(diff / s->peak, -300.0f));
    }
    r.check(worst < -80.0, "fidelity over " + juce::String((int) takes.size()) + " takes: largest difference "
                               + juce::String(worst, 1) + " dB below each take's peak (threshold -80 dB)");

    // 3. Resampling: output at 44.1 kHz, the level must be preserved.
    const Sample* c4 = takes.front();
    for (const auto* s : takes)
        if (s->note == 60 && std::abs(s->velocity - 64) < std::abs(c4->velocity - 64))
            c4 = s;
    {
        player.prepare(44100.0, 512);
        flush(player, 44100.0);
        const auto rendered = renderNote(player, c4->note, c4->velocity, 2 * 44100, 512);
        const auto reference = decodeFile(c4->file);
        const double levelDiff = rmsDb(rendered, 0, 2 * 44100) - rmsDb(reference, 0, (int) (2 * fs));
        bool finite = true;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < rendered.getNumSamples(); ++k)
                finite = finite && std::isfinite(rendered.getSample(c, k));
        r.check(finite && std::abs(levelDiff) < 0.2,
                "resampling 48 -> 44.1 kHz: level difference " + juce::String(levelDiff, 3) + " dB (threshold +-0.2 dB)");
    }

    // 4. Key release: -40 dB reached at the time measured by the bench (plus the note's own decay).
    player.prepare(fs, 512);
    player.playWholeTake = false;
    if (c4->releaseDecay40 > 0.0f) {
        flush(player, fs);
        const int releaseAt = (int) fs;
        const int t40 = (int) (c4->releaseDecay40 * fs);
        const auto rendered = renderNote(player, c4->note, c4->velocity, releaseAt + t40 + (int) fs, 512, releaseAt);
        const int win = (int) (0.01 * fs);
        const double attenuation = rmsDb(rendered, releaseAt - win, win) - rmsDb(rendered, releaseAt + t40 - win / 2, win);
        r.check(attenuation >= 39.0 && attenuation <= 65.0,
                "key release of " + juce::MidiMessage::getMidiNoteName(c4->note, true, true, 4) + ": -"
                    + juce::String(attenuation, 1) + " dB after " + juce::String(c4->releaseDecay40, 2)
                    + " s (expected: 40 dB and a little more)");
    } else {
        r.line("[----]  key release: no decay measurement for the C4 take");
    }

    // 5. Load: 64 simultaneous voices, 128-sample blocks.
    {
        player.prepare(fs, 128);
        player.playWholeTake = true;
        flush(player, fs);
        std::vector<float> l(128), rr(128);
        float* outputs[2] = { l.data(), rr.data() };
        // 64 different notes: striking the same note twice would cut the previous voice (re-strike).
        std::vector<pf::Event> events;
        for (int note = 21; note <= 108 && events.size() < 64; ++note)
            if (!b->byNote[(size_t) note].empty())
                events.push_back({ 0, 0x90, (std::uint8_t) note, 64 });
        const int blocks = (int) (10.0 * fs) / 128;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        int maxVoices = 0;
        for (int k = 0; k < blocks; ++k) {
            player.process(outputs, 2, 128, k == 0 ? events.data() : nullptr, k == 0 ? (int) events.size() : 0);
            maxVoices = juce::jmax(maxVoices, player.activeVoices.load());
        }
        const double seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        const double factor = 10.0 / seconds;
        r.check(factor > 20.0, "load: " + juce::String(maxVoices) + " simultaneous voices, 10 s rendered in "
                                   + juce::String(seconds * 1000.0, 1) + " ms, i.e. " + juce::String(factor, 0)
                                   + " times real time (threshold: 20)");
    }

    r.line(r.passed ? "RESULT: every check passes" : "RESULT: at least one check fails");
    reportFile.replaceWithText(r.text, false, false, "\n");
    return r.passed ? 0 : 1;
}

} // namespace pf::app
