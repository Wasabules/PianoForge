#include "SelfTest.h"

#include "AudioHost.h"
#include "KeyboardInput.h"
#include "OfflineRender.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"
#include "TakeRecorder.h"

#include <pianoforge/modal_engine.hpp>

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
    r.line("PianoForge self-test - " + juce::Time::getCurrentTime().toString(true, true));
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
    bool startsAtNoteOn = true;
    for (const auto* s : takes) {
        flush(player, fs);
        const int start = (int) s->noteOnFrame; // playback skips the take's pre-roll
        startsAtNoteOn = startsAtNoteOn && start > 0;
        const auto rendered = renderNote(player, s->note, s->velocity, (int) (s->length - start), 512);
        const auto reference = decodeFile(s->file);
        float diff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < juce::jmin(rendered.getNumSamples(), reference.getNumSamples() - start); ++k)
                diff = juce::jmax(diff, std::abs(rendered.getSample(c, k) - reference.getSample(c, k + start)));
        worst = juce::jmax(worst, (double) juce::Decibels::gainToDecibels(diff / s->peak, -300.0f));
    }
    r.check(worst < -80.0 && startsAtNoteOn,
            "fidelity over " + juce::String((int) takes.size()) + " takes, from the note-on (pre-roll skipped): largest "
                "difference " + juce::String(worst, 1) + " dB below each take's peak (threshold -80 dB)");

    // 3. Resampling: output at 44.1 kHz, the level must be preserved.
    const Sample* c4 = takes.front();
    for (const auto* s : takes)
        if (s->note == 60 && std::abs(s->velocity - 64) < std::abs(c4->velocity - 64))
            c4 = s;
    {
        const double device = std::abs(fs - 48000.0) < 1.0 ? 44100.0 : 48000.0; // any rate other than the corpus'
        player.prepare(device, 512);
        flush(player, device);
        const auto rendered = renderNote(player, c4->note, c4->velocity, (int) (2 * device), 512);
        const auto reference = decodeFile(c4->file);
        const double levelDiff = rmsDb(rendered, 0, (int) (2 * device)) - rmsDb(reference, (int) c4->noteOnFrame, (int) (2 * fs));
        bool finite = true;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < rendered.getNumSamples(); ++k)
                finite = finite && std::isfinite(rendered.getSample(c, k));
        r.check(finite && std::abs(levelDiff) < 0.2,
                "resampling " + juce::String(fs / 1000.0, 1) + " -> " + juce::String(device / 1000.0, 1)
                    + " kHz: level difference " + juce::String(levelDiff, 3) + " dB (threshold +-0.2 dB)");
    }

    // 4. Key release. With the measured damper curves, the release of A0 (velocity 80, held 0.5 s) must follow
    //    the library's own release take; without them, the exponential fallback must reach -40 dB in time.
    player.prepare(fs, 512);
    player.playWholeTake = false;
    const auto releaseTake = folder.getParentDirectory().getChildFile("releases").getChildFile("021-A0_v080_h500ms.flac");
    if (!b->releaseCurves.empty() && releaseTake.existsAsFile() && !b->byNote[21].empty()) {
        flush(player, fs);
        const int hold = (int) std::lround(0.5 * fs), preroll = (int) std::lround(0.05 * fs), win = (int) (0.02 * fs);
        const auto rendered = renderNote(player, 21, 80, hold + (int) (2.0 * fs), 512, hold);
        const auto reference = decodeFile(releaseTake); // note-on after a 50 ms pre-roll
        const double atRelease = rmsDb(reference, preroll + hold - win, win);
        double sum = 0.0;
        int count = 0;
        for (int k = win; k < (int) (1.5 * fs); k += win) {
            const double ref = rmsDb(reference, preroll + hold + k, win);
            if (ref < atRelease - 60.0)
                continue; // below the noise floor of the comparison
            sum += std::abs(rmsDb(rendered, hold + k, win) - ref);
            ++count;
        }
        const double error = count > 0 ? sum / count : 99.0;
        r.check(error < 4.0, "key release of A0 (velocity 80, held 0.5 s) against the library's release take: mean "
                                 "envelope error " + juce::String(error, 1) + " dB over 1.5 s (threshold 4 dB)");
    } else if (c4->releaseDecay40 > 0.0f) {
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
                                   + juce::String(seconds * 1000.0, 1) + " ms, i.e. " + juce::String(juce::roundToInt(factor))
                                   + " times real time (threshold: 20)");
    }

    // 6. A chord through the application's MIDI path (keyboard -> velocity curve -> collector -> audio host ->
    //    player), 480-sample blocks, timestamps 31 ms in the past as delivered by Windows MIDI Services.
    {
        AudioHost host { player };
        pf::VelocityCurve curve;
        KeyboardInput keyboard { host.collector, curve };
        host.collector.reset(fs);
        player.prepare(fs, 480);
        player.playWholeTake = false;
        flush(player, fs);
        std::vector<float> l(480), rr(480);
        float* outputs[2] = { l.data(), rr.data() };
        host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 480, {}); // first block: sets the time base
        const int notes[] = { 48, 52, 55, 60 };
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (int k = 0; k < 4; ++k)
            keyboard.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(1, notes[k], (juce::uint8) 80)
                                                            .withTimeStamp(now - 0.031 + 0.001 * k));
        const auto before = player.noteOnsReceived.load();
        int voices = 0;
        for (int k = 0; k < 4; ++k) {
            juce::Thread::sleep(10);
            host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 480, {});
            voices = juce::jmax(voices, player.activeVoices.load());
        }
        r.check(voices == 4 && player.noteOnsReceived.load() - before == 4,
                "chord through the MIDI path: " + juce::String((int) (player.noteOnsReceived.load() - before))
                    + " note-ons reached the engine, " + juce::String(voices) + " voices sounding (expected 4 and 4)");
    }

    // 7. Re-strike: the previous voice keeps sounding, then fades (-40 dB in 0.8 s, freed at -80 dB, ~1.6 s).
    {
        player.prepare(fs, 512);
        player.playWholeTake = true;
        player.restrikeFadeSeconds = 0.8f;
        flush(player, fs);
        std::vector<float> l(512), rr(512);
        float* outputs[2] = { l.data(), rr.data() };
        const pf::Event strike { 0, 0x90, 60, 64 };
        player.process(outputs, 2, 512, &strike, 1);
        for (int k = 0; k < (int) (0.5 * fs) / 512; ++k)
            player.process(outputs, 2, 512, nullptr, 0);
        player.process(outputs, 2, 512, &strike, 1);
        const int right_after = player.activeVoices.load();
        for (int k = 0; k < (int) (2.0 * fs) / 512; ++k)
            player.process(outputs, 2, 512, nullptr, 0);
        const int two_seconds_later = player.activeVoices.load();
        r.check(right_after == 2 && two_seconds_later == 1,
                "re-strike of C4: " + juce::String(right_after) + " voices right after (expected 2), "
                    + juce::String(two_seconds_later) + " two seconds later (expected 1)");
    }

    // 8. Takes: a session played through the audio host (notes, pedal, re-strike), saved as a take, written to a
    //    MIDI file (one tick per sample), read back and rendered offline, must give exactly the live audio.
    {
        player.playWholeTake = false;
        player.restrikeFadeSeconds = 0.8f;
        AudioHost host { player };
        host.start(fs, 512);
        const int blocks = (int) (4.0 * fs) / 512;
        juce::AudioBuffer<float> live(2, blocks * 512);
        std::vector<float> l(512), rr(512);
        float* outputs[2] = { l.data(), rr.data() };
        const std::vector<std::pair<int, juce::MidiMessage>> script = {
            { 10, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90) }, { 12, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 70) },
            { 40, juce::MidiMessage::controllerEvent(1, 64, 127) }, { 60, juce::MidiMessage::noteOff(1, 60) },
            { 62, juce::MidiMessage::noteOff(1, 64) }, { 150, juce::MidiMessage::controllerEvent(1, 64, 0) },
            { 200, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 50) }, { 215, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100) },
            { 260, juce::MidiMessage::noteOff(1, 60) } };
        size_t next = 0;
        for (int k = 0; k < blocks; ++k) {
            while (next < script.size() && script[next].first == k)
                host.send(script[next++].second);
            host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
            live.copyFrom(0, k * 512, l.data(), 512);
            live.copyFrom(1, k * 512, rr.data(), 512);
        }
        std::vector<RecordedEvent> recorded;
        host.drainRecorded(recorded);
        TakeRecorder recorder;
        recorder.add(recorded, host.currentSegment(), host.currentRate());
        const auto take = recorder.lastTake(10.0, 0.2);
        double worstDb = 999.0;
        int notesInFile = 0;
        if (take.has_value() && !recorded.empty()) {
            const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest-take.mid");
            file.deleteFile();
            {
                juce::FileOutputStream out(file);
                TakeRecorder::toMidiFile(*take).writeTo(out);
            }
            juce::MidiFile reread;
            juce::FileInputStream in(file);
            reread.readFrom(in, false); // false: the reader must not add note-offs either
            reread.convertTimestampTicksToSeconds();
            juce::MidiMessageSequence sequence;
            for (int t = 0; t < reread.getNumTracks(); ++t)
                sequence.addSequence(*reread.getTrack(t), 0.0);
            for (const auto* e : sequence)
                notesInFile += e->message.isNoteOn() ? 1 : 0;
            const auto offline = renderSequence(sequence, player.sharedBank(), fs, 0.8f, false, 1.0);
            const auto start = std::max<std::int64_t>(0, recorded.front().sample - (std::int64_t) std::llround(0.2 * fs));
            float diff = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < juce::jmin(offline.getNumSamples(), live.getNumSamples() - (int) start); ++k)
                    diff = juce::jmax(diff, std::abs(offline.getSample(c, k) - live.getSample(c, k + (int) start)));
            worstDb = juce::Decibels::gainToDecibels(diff, -300.0f);
            if (worstDb > -120.0) { // diagnostic: events as recorded and as read back from the file
                juce::String events = "    recorded (sample, bytes):";
                for (const auto& e : take->events)
                    events << " " << (int) e.sample << ":" << juce::String::toHexString(e.status) << "/" << (int) e.data1 << "/" << (int) e.data2;
                events << "\n    from the file:";
                for (const auto* e : sequence)
                    if (e->message.isNoteOnOrOff() || e->message.isController())
                        events << " " << (int) std::llround(e->message.getTimeStamp() * fs) << ":"
                               << juce::String::toHexString(e->message.getRawData()[0]) << "/" << e->message.getRawData()[1] << "/"
                               << (e->message.getRawDataSize() > 2 ? e->message.getRawData()[2] : 0);
                int worstAt = 0;
                float worstValue = 0.0f;
                for (int k = 0; k < juce::jmin(offline.getNumSamples(), live.getNumSamples() - (int) start); ++k)
                    if (const float d = std::abs(offline.getSample(0, k) - live.getSample(0, k + (int) start)); d > worstValue) {
                        worstValue = d;
                        worstAt = k;
                    }
                r.line(events + "\n    largest difference at sample " + juce::String(worstAt) + " of the take");
            }
            file.deleteFile();
        }
        r.check(take.has_value() && take->notes == 4 && notesInFile == 4 && worstDb < -120.0,
                "take recorded live, saved as MIDI, re-rendered offline: " + juce::String(take.has_value() ? take->notes : 0)
                    + " notes captured, " + juce::String(notesInFile) + " in the file, largest difference with the live audio "
                    + (worstDb <= -300.0 ? juce::String("none (bit-identical)") : juce::String(worstDb, 1) + " dBFS"));
    }

    // 9. Modal engine, when the corpus state has a modal profile (forge export).
    if (const auto path = folder.getParentDirectory().getChildFile("modal-profile.bin"); path.existsAsFile()) {
        std::string error;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto profile = pf::loadModalProfile(path.getFullPathName().toStdString(), error);
        const double loadS = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        std::size_t layers = 0, modes = 0;
        if (profile != nullptr)
            for (const auto& n : profile->notes) {
                layers = std::max(layers, n.layers.size());
                for (const auto& l : n.layers)
                    modes += l.modes.size();
            }
        r.check(profile != nullptr && !profile->notes.empty(),
                profile != nullptr ? "modal profile: " + juce::String((int) profile->notes.size()) + " notes x "
                                         + juce::String((int) layers) + " layers, " + juce::String((int) modes)
                                         + " modes, loaded in " + juce::String(loadS, 2) + " s"
                                   : "modal profile: " + juce::String(error));
        if (profile != nullptr) {
            // C4 at velocity 64 against the reference take: level per time window.
            const Sample* c4Take = nullptr;
            for (const auto& s : b->samples)
                if (s.note == 60 && s.velocity == 64)
                    c4Take = &s;
            pf::ModalEngine modalEngine;
            modalEngine.install(profile);
            modalEngine.prepare(fs, 512);
            const int frames = (int) (5.0 * fs);
            juce::AudioBuffer<float> out(2, frames);
            out.clear();
            const pf::Event on { 0, 0x90, 60, 64 };
            for (int pos = 0; pos < frames; pos += 512) {
                float* o[2] = { out.getWritePointer(0, pos), out.getWritePointer(1, pos) };
                modalEngine.process(o, 2, juce::jmin(512, frames - pos), pos == 0 ? &on : nullptr, pos == 0 ? 1 : 0);
            }
            if (c4Take != nullptr && c4Take->length - c4Take->noteOnFrame >= frames) {
                const float scale = c4Take->peak / 32767.0f;
                juce::String detail;
                double worstWindow = 0.0;
                bool finite = true;
                for (const auto [a, z] : { std::pair { 0.0, 0.1 }, std::pair { 0.1, 1.0 }, std::pair { 1.0, 5.0 } }) {
                    double em = 0.0, er = 0.0;
                    for (int k = (int) (a * fs); k < (int) (z * fs); ++k) {
                        const auto i0 = (size_t) (c4Take->noteOnFrame + k);
                        for (int c = 0; c < 2; ++c) {
                            const double m = out.getSample(c, k), ref = c4Take->pcm[2 * i0 + (size_t) c] * scale;
                            finite = finite && std::isfinite(m);
                            em += m * m;
                            er += ref * ref;
                        }
                    }
                    const double diff = 10.0 * std::log10((em + 1e-30) / (er + 1e-30));
                    worstWindow = juce::jmax(worstWindow, std::abs(diff));
                    detail << (detail.isEmpty() ? "" : ", ") << juce::String(a, 1) << "-" << juce::String(z, 1) << " s "
                           << (diff >= 0 ? "+" : "") << juce::String(diff, 1) << " dB";
                }
                r.check(finite && worstWindow < 2.0, "modal C4 (velocity 64) against the reference take, level per window: " + detail
                                                   + " (threshold +-2 dB)");
            }

            // Load: sustain pedal down, 64 notes struck at once, 10 s in 128-sample blocks.
            modalEngine.prepare(fs, 128);
            std::vector<float> l(128), rr(128);
            float* outputs[2] = { l.data(), rr.data() };
            std::vector<pf::Event> events { { 0, 0xB0, 64, 127 } };
            for (int note = 21; note <= 108 && events.size() < 65; ++note)
                if (profile->find(note) != nullptr)
                    events.push_back({ 0, 0x90, (std::uint8_t) note, 80 });
            const int blocks = (int) (10.0 * fs) / 128;
            const auto start = juce::Time::getMillisecondCounterHiRes();
            int maxModes = 0;
            for (int k = 0; k < blocks; ++k) {
                modalEngine.process(outputs, 2, 128, k == 0 ? events.data() : nullptr, k == 0 ? (int) events.size() : 0);
                maxModes = juce::jmax(maxModes, modalEngine.activeModes.load());
            }
            const double seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
            r.check(10.0 / seconds > 3.0, "modal load: " + juce::String((int) events.size() - 1) + " notes, sustain down, up to "
                                              + juce::String(maxModes) + " resonators, 10 s rendered in " + juce::String(seconds, 2)
                                              + " s, i.e. " + juce::String(10.0 / seconds, 1) + " times real time (threshold: 3)");
        }
    } else {
        r.line("[----]  modal engine: no modal-profile.bin next to the corpus passes (forge export)");
    }

    r.line(r.passed ? "RESULT: every check passes" : "RESULT: at least one check fails");
    reportFile.replaceWithText(r.text, false, false, "\n");
    return r.passed ? 0 : 1;
}

} // namespace pf::app
