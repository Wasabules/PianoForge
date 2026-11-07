#include "UpdateChecker.h"
#include "SelfTest.h"

#include "AudioExport.h"
#include "AudioHost.h"

#include <memory>
#include "KeyboardInput.h"
#include "MidiFilePlayer.h"
#include "OfflineRender.h"
#include "ui/EnginePanel.h"
#include "ui/RoomPanel.h"
#include "ReferencePlayer.h"
#include "SampleBank.h"
#include "TakeRecorder.h"

#include <pianoforge/modal_engine.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <thread>

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
    // The audio hosts below feed an OBS feed of their own: a PianoForge running beside keeps its own.
#if JUCE_WINDOWS
    _putenv_s("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedSelfTest");
#else
    setenv("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedSelfTest", 1);
#endif
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
        auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
        AudioHost& host = *hostOwner;
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

    // 7b. Sostenuto in the reference player: C4 held when it is pressed rings on after its release (E4, struck
    //     after it, is damped as usual), and its damper lands when the sostenuto is released. Levels against the
    //     same playing without the sostenuto.
    {
        auto play = [&](bool sostenuto) {
            player.prepare(fs, 512);
            player.playWholeTake = false;
            flush(player, fs);
            std::vector<float> out;
            std::vector<float> l(512), rr(512);
            float* outputs[2] = { l.data(), rr.data() };
            const int at[] = { 0, (int) (0.2 * fs) / 512, (int) (0.25 * fs) / 512, (int) (0.45 * fs) / 512, (int) (2.0 * fs) / 512 };
            for (int k = 0; k < (int) (3.5 * fs) / 512; ++k) {
                std::vector<pf::Event> events;
                if (k == at[0])
                    events.push_back({ 0, 0x90, 60, 80 });
                if (k == at[1] && sostenuto)
                    events.push_back({ 0, 0xB0, 66, 127 });
                if (k == at[2]) {
                    events.push_back({ 0, 0x80, 60, 64 });
                    events.push_back({ 10, 0x90, 64, 80 });
                }
                if (k == at[3])
                    events.push_back({ 0, 0x80, 64, 64 });
                if (k == at[4] && sostenuto)
                    events.push_back({ 0, 0xB0, 66, 0 });
                player.process(outputs, 2, 512, events.data(), (int) events.size());
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        };
        auto level = [&](const std::vector<float>& x, double a, double b) {
            double e = 0.0;
            const int i0 = (int) (a * fs), i1 = (int) (b * fs);
            for (int i = i0; i < i1; ++i)
                e += (double) x[(size_t) i] * x[(size_t) i];
            return 10.0 * std::log10(e / (i1 - i0) + 1e-30);
        };
        const auto with = play(true), without = play(false);
        const double kept = level(with, 1.4, 1.9) - level(without, 1.4, 1.9);
        const double landed = level(with, 1.4, 1.9) - level(with, 3.0, 3.4);
        r.check(kept > 20.0 && landed > 20.0,
                "sostenuto (reference player): C4 rings on, " + juce::String(kept, 1) + " dB over the same playing without it "
                    + "(expected > 20), and its damper lands once released (" + juce::String(landed, 1) + " dB down, expected > 20)");
    }

    // 8. Takes: a session played through the audio host (notes, pedal, re-strike), saved as a take, written to a
    //    MIDI file (one tick per sample), read back and rendered offline, must give exactly the live audio.
    {
        player.playWholeTake = false;
        player.restrikeFadeSeconds = 0.8f;
        auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
        AudioHost& host = *hostOwner;
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

    // 8b. Live recording: started and stopped from another thread while the audio callbacks run, the WAV must be
    //     the audio sent to the device over the span, sample for sample, and the MIDI must line up with it (tick 0
    //     = WAV sample 0, the pedal already down re-stated there).
    {
        constexpr int block = 256;
        auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
        AudioHost& host = *hostOwner;
        host.start(fs, block);
        const int blocks = (int) (3.0 * fs) / block;
        juce::AudioBuffer<float> live(2, blocks * block);
        std::vector<float> l(block), rr(block);
        float* outputs[2] = { l.data(), rr.data() };
        const std::vector<std::pair<int, juce::MidiMessage>> script = {
            { 10, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90) }, { 20, juce::MidiMessage::controllerEvent(1, 64, 127) },
            { 50, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 70) }, { 80, juce::MidiMessage::noteOff(1, 60) },
            { 120, juce::MidiMessage::controllerEvent(1, 64, 0) }, { 150, juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100) },
            { 210, juce::MidiMessage::noteOff(1, 67) } };
        const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest-recording.wav");
        TakeRecorder recorder;
        std::vector<RecordedEvent> recorded;
        LiveRecorder::Span span;
        std::thread stopper;
        juce::String startError;
        size_t next = 0;
        for (int k = 0; k < blocks; ++k) {
            if (k == 40)
                if (const auto started = host.live.start(wav, fs, host.currentSegment(), "self-test"); started.failed())
                    startError = started.getErrorMessage();
            if (k == 200) { // stop() waits for the audio thread: from another thread, as the user interface does
                stopper = std::thread([&] { span = host.live.stop(); });
                while (!host.live.stopPending()) // the request is made before this block, as it would be live
                    std::this_thread::yield();
            }
            while (next < script.size() && script[next].first == k)
                host.send(script[next++].second);
            host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, block, {});
            live.copyFrom(0, k * block, l.data(), block);
            live.copyFrom(1, k * block, rr.data(), block);
            if (k == 200)
                stopper.join(); // acknowledged by the callback just run
        }
        host.drainRecorded(recorded);
        recorder.add(recorded, host.currentSegment(), host.currentRate());
        float wavError = -1.0f;
        int frames = 0;
        if (span.valid) {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(wav)); reader != nullptr) {
                frames = (int) reader->lengthInSamples;
                juce::AudioBuffer<float> file(2, frames);
                reader->read(&file, 0, frames, 0, true, true);
                wavError = 0.0f;
                for (int c = 0; c < 2; ++c)
                    for (int k = 0; k < frames && span.start + k < live.getNumSamples(); ++k)
                        wavError = juce::jmax(wavError, std::abs(file.getSample(c, k) - live.getSample(c, (int) span.start + k)));
            }
        }
        const auto take = span.valid ? recorder.span(span.segment, span.start, span.end) : std::nullopt;
        bool aligned = take.has_value() && !take->events.empty() && take->events.front().sample == 0
                       && take->events.front().status == 0xB0 && take->events.front().data2 == 127;
        int inside = 0;
        for (const auto& e : recorded)
            inside += e.sample >= span.start && e.sample < span.end ? 1 : 0;
        aligned = aligned && take.has_value() && (int) take->events.size() == inside + 1 && take->notes == 2;
        if (aligned)
            for (const auto& e : take->events) // every event at its own position, relative to the WAV's first sample
                aligned = aligned && e.sample >= 0 && e.sample < span.end - span.start;
        wav.deleteFile();
        r.check(startError.isEmpty() && span.valid && span.start == 40 * block && span.end == 200 * block
                    && frames == (int) (span.end - span.start) && wavError == 0.0f && span.dropped == 0 && aligned,
                "live recording: started at block 40, stopped at block 200 from another thread: span "
                    + juce::String(span.start) + " to " + juce::String(span.end) + ", WAV of " + juce::String(frames)
                    + " frames, " + (wavError == 0.0f ? juce::String("identical to the audio sent to the device")
                                                   : "largest difference " + juce::String(wavError))
                    + ", MIDI " + (aligned ? "aligned (pedal re-stated at tick 0, " + juce::String(take->notes) + " notes)"
                                           : juce::String("NOT aligned")) + startError);
    }

    // 8c. Stream output: two devices on their own clocks (and rates). Over two minutes, a 1 kHz sine must come out
    //     at 1 kHz in the consumer's real time, clean, without a dropout once settled, the FIFO at its target.
    {
        struct Case {
            double inRate, outRate, drift;
            int inBlock, outBlock;
        };
        for (const auto& c : { Case { 44100.0, 48000.0, 150e-6, 128, 480 }, Case { 44100.0, 44100.0, -80e-6, 64, 441 } }) {
            DriftFifo d;
            d.prepare(c.inRate, c.outRate, c.inBlock, c.outBlock);
            const double outTrue = c.outRate * (1.0 + c.drift), seconds = 120.0;
            std::vector<float> inL((size_t) c.inBlock), inR((size_t) c.inBlock), outL, outR((size_t) c.outBlock);
            outL.reserve((size_t) (seconds * outTrue) + (size_t) c.outBlock);
            const float* in[2] = { inL.data(), inR.data() };
            double tp = 0.0, tc = 0.0;
            long long produced = 0;
            std::uint32_t settledUnderruns = 0;
            bool settled = false;
            double lowPpm = 1e9, highPpm = -1e9; // the ratio's correction over the last 30 s
            while (tc < seconds) {
                if (tp <= tc) {
                    for (int k = 0; k < c.inBlock; ++k)
                        inL[(size_t) k] = inR[(size_t) k] = 0.5f * (float) std::sin(6.283185307179586 * 1000.0 * (double) (produced + k) / c.inRate);
                    d.push(in, 2, c.inBlock);
                    produced += c.inBlock;
                    tp += c.inBlock / c.inRate;
                } else {
                    const size_t at = outL.size();
                    outL.resize(at + (size_t) c.outBlock);
                    float* out[2] = { outL.data() + at, outR.data() };
                    d.pull(out, 2, c.outBlock);
                    tc += c.outBlock / outTrue;
                    if (tc > seconds - 30.0) {
                        lowPpm = juce::jmin(lowPpm, d.correctionPpm());
                        highPpm = juce::jmax(highPpm, d.correctionPpm());
                    }
                    if (!settled && tc > 10.0) {
                        settled = true;
                        settledUnderruns = d.underruns() + d.overruns();
                    }
                }
            }
            // The sine against a fit over 20 ms windows of the last 30 s, at 1 kHz in the consumer's real time.
            const double w = 6.283185307179586 * 1000.0 / outTrue;
            const int window = (int) (0.02 * outTrue);
            double residualDb = -300.0;
            for (size_t s = outL.size() - (size_t) (30.0 * outTrue); s + (size_t) window < outL.size(); s += (size_t) window) {
                // Least squares on cos and sin (over a few tens of cycles they are not quite orthogonal).
                double cc = 0.0, cs = 0.0, ss = 0.0, xc = 0.0, xs = 0.0, fitted = 0.0, rest = 0.0;
                for (int k = 0; k < window; ++k) {
                    const double co = std::cos(w * (double) (s + (size_t) k)), si = std::sin(w * (double) (s + (size_t) k));
                    cc += co * co;
                    cs += co * si;
                    ss += si * si;
                    xc += outL[s + (size_t) k] * co;
                    xs += outL[s + (size_t) k] * si;
                }
                const double det = cc * ss - cs * cs;
                const double cosPart = (xc * ss - xs * cs) / det, sinPart = (xs * cc - xc * cs) / det;
                for (int k = 0; k < window; ++k) {
                    const double fit = cosPart * std::cos(w * (double) (s + (size_t) k)) + sinPart * std::sin(w * (double) (s + (size_t) k));
                    fitted += fit * fit;
                    rest += std::pow(outL[s + (size_t) k] - fit, 2.0);
                }
                residualDb = juce::jmax(residualDb, 10.0 * std::log10(rest / fitted + 1e-30));
            }
            const double fillError = std::abs(d.fillSeconds() - d.targetSeconds()) / d.targetSeconds();
            const double truePpm = -1e6 * c.drift / (1.0 + c.drift);
            const bool clean = d.underruns() + d.overruns() == settledUnderruns && fillError < 0.3 && residualDb < -60.0
                               && lowPpm > truePpm - 40.0 && highPpm < truePpm + 40.0;
            r.check(clean, "stream output, " + juce::String(c.inRate / 1000.0, 1) + " kHz into " + juce::String(c.outRate / 1000.0, 1)
                               + " kHz with the clocks " + juce::String(juce::roundToInt(c.drift * 1e6)) + " ppm apart, 2 min: ratio corrected by "
                               + juce::String(juce::roundToInt(lowPpm)) + " to " + juce::String(juce::roundToInt(highPpm))
                               + " ppm (needed " + juce::String(juce::roundToInt(truePpm)) + "), FIFO at " + juce::String(1000.0 * d.fillSeconds(), 1)
                               + " ms (target " + juce::String(1000.0 * d.targetSeconds(), 1) + "), "
                               + juce::String((int) (d.underruns() + d.overruns() - settledUnderruns)) + " dropouts once settled, "
                               + "1 kHz sine residual " + juce::String(residualDb, 1) + " dB");
        }
    }

    // 8d. MIDI file player: a file written to disk (a tempo change, a drum note on channel 10), read back and
    //     played through the audio host, must reach the engine to the sample at 100 % and at 150 %; a seek and a
    //     pause must release what sounds and put the pedal back as the file has it.
    {
        juce::MidiMessageSequence track;
        track.addEvent(juce::MidiMessage::tempoMetaEvent(600000), 0.0); // 100 beats per minute: 0.6 s per quarter
        const std::vector<std::pair<int, juce::MidiMessage>> script = {
            { 960, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90) }, { 960, juce::MidiMessage::noteOn(10, 36, (juce::uint8) 100) },
            { 1440, juce::MidiMessage::controllerEvent(1, 64, 127) },    { 1920, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 70) },
            { 2400, juce::MidiMessage::noteOff(1, 60) },                 { 2880, juce::MidiMessage::noteOff(1, 64) },
            { 3840, juce::MidiMessage::controllerEvent(1, 64, 0) } };
        for (const auto& [tick, m] : script)
            track.addEvent(m, tick);
        juce::MidiFile file;
        file.setTicksPerQuarterNote(960);
        file.addTrack(track);
        const auto path = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest-player.mid");
        path.deleteFile();
        {
            juce::FileOutputStream out(path);
            file.writeTo(out);
        }
        juce::String error;
        const auto sequence = MidiFilePlayer::read(path, error);
        path.deleteFile();
        const double expected[] = { 0.6, 0.9, 1.2, 1.5, 1.8, 2.4 }; // the six events of channel 1, in seconds
        r.check(sequence != nullptr && sequence->notes == 3 && sequence->parts.size() == 2 && sequence->parts[1].drums
                    && sequence->events.size() == 7
                    && std::abs(sequence->length - 2.4) < 1e-9,
                "MIDI file read back: " + (sequence == nullptr ? error
                                                               : juce::String(sequence->notes) + " notes, " + juce::String((int) sequence->events.size())
                                                                     + " events, " + juce::String(sequence->length, 3) + " s, "
                                                                     + juce::String((int) sequence->parts.size()) + " parts")
                    + " (expected 3 notes, 7 events, 2.400 s, 2 parts: the drums kept, off by default)");

        player.playWholeTake = false;
        std::vector<float> l(512), rr(512);
        float* outputs[2] = { l.data(), rr.data() };
        for (const double speed : { 1.0, 1.5 }) {
            auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
            AudioHost& host = *hostOwner;
            host.start(fs, 512);
            host.filePlayer.load(sequence);
            host.filePlayer.setSpeed(speed);
            host.filePlayer.play();
            std::vector<RecordedEvent> got;
            for (int k = 0; k < (int) (3.0 * fs) / 512; ++k)
                host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
            host.drainRecorded(got);
            double offBy = got.size() == 6 ? 0.0 : 1e9;
            for (size_t e = 0; e < got.size() && e < 6; ++e)
                offBy = juce::jmax(offBy, std::abs((double) got[e].sample - expected[e] / speed * fs));
            r.check(offBy <= 0.501 && !host.filePlayer.wantsToPlay(),
                    "MIDI file at " + juce::String(juce::roundToInt(speed * 100.0)) + " %: " + juce::String((int) got.size())
                        + " events reached the engine, at most " + juce::String(offBy, 2) + " samples from their time (expected 6, <= 0.5), "
                        + (host.filePlayer.wantsToPlay() ? "still playing" : "stopped at the end"));
        }
        {
            auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
            AudioHost& host = *hostOwner;
            host.start(fs, 512);
            host.filePlayer.load(sequence);
            host.filePlayer.play();
            auto run = [&](double seconds) {
                for (int k = 0; k < juce::roundToInt(seconds * fs / 512.0); ++k)
                    host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
                std::vector<RecordedEvent> got;
                host.drainRecorded(got);
                juce::StringArray text;
                for (const auto& e : got)
                    text.add(juce::String::toHexString((int) e.status) + " " + juce::String((int) e.data1) + " " + juce::String((int) e.data2));
                return text.joinIntoString(", ");
            };
            run(1.3);                         // C4 and E4 sounding, pedal down
            host.filePlayer.seek(2.0);        // both released, pedal up, then down again as the file has it at 2 s
            const auto seek = run(0.1);
            host.filePlayer.pause();          // the pedal up
            const auto pause = run(0.1);
            host.filePlayer.play();           // the pedal down again, then up at 2.4 s
            const auto resume = run(0.6);
            r.check(seek == "80 60 0, 80 64 0, b0 64 0, b0 64 127" && pause == "b0 64 0" && resume == "b0 64 127, b0 64 0",
                    "MIDI file seek and pause: seek [" + seek + "], pause [" + pause + "], resume [" + resume
                        + "] (expected [80 60 0, 80 64 0, b0 64 0, b0 64 127], [b0 64 0], [b0 64 127, b0 64 0])");
        }
    }

    // 8e. Room: a chord through the audio host, in a concert hall (what the physics gives), the room off, heard from
    //     the keyboard, then from the front row with keep loudness. From the keyboard, the room is only added (the
    //     direct sound is the engine's) and rings on once the chord is damped; from the front row, the chord is as loud
    //     over its 3 s (keep loudness keeps the total energy: the room has not built up in the first moments) but the
    //     room dominates its tail.
    {
        auto play = [&](bool roomOn, float distance) {
            player.playWholeTake = false;
            auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
            AudioHost& host = *hostOwner;
            pf::Room::Settings s;
            s.enabled = roomOn;
            s.levelDb = 0.0f;
            s.size = 0.85f;
            s.decaySeconds = 2.2f;
            s.distance = distance;
            s.keepLoudness = true;
            host.room.applySettings(s);
            host.start(fs, 512);
            std::vector<float> l(512), rr(512), out;
            float* outputs[2] = { l.data(), rr.data() };
            const int blocks = (int) (3.0 * fs) / 512;
            for (int k = 0; k < blocks; ++k) {
                for (const int note : { 48, 64, 67, 72 }) { // a chord: many partials (one note's few would leave the
                    if (k == 2)                               // room's response at them to chance, +-1 to 2 dB)
                        host.send(juce::MidiMessage::noteOn(1, note, (juce::uint8) 90));
                    if (k == (int) (0.5 * fs) / 512)
                        host.send(juce::MidiMessage::noteOff(1, note));
                }
                host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
                for (int i = 0; i < 512; ++i) // both channels' energy (a narrower image moves it between them)
                    out.push_back(std::sqrt(l[(size_t) i] * l[(size_t) i] + rr[(size_t) i] * rr[(size_t) i]));
            }
            return out;
        };
        auto level = [&](const std::vector<float>& x, double a, double b) {
            double e = 0.0;
            for (int i = (int) (a * fs); i < (int) (b * fs); ++i)
                e += (double) x[(size_t) i] * x[(size_t) i];
            return 10.0 * std::log10(e / ((b - a) * fs) + 1e-30);
        };
        const auto dry = play(false, 1.0f), keyboard = play(true, 1.0f), front = play(true, 6.0f);
        const double during = level(keyboard, 0.05, 0.45) - level(dry, 0.05, 0.45);
        const double tail = level(keyboard, 1.5, 2.5) - level(dry, 1.5, 2.5);
        const double frontDuring = level(front, 0.05, 2.95) - level(dry, 0.05, 2.95);
        const double frontTail = level(front, 1.5, 2.5) - level(dry, 1.5, 2.5);
        r.check(during < 1.0 && tail > 0.0 && std::abs(frontDuring) < 1.5 && frontTail > 6.0,
                "room: from the keyboard, " + juce::String(during, 1) + " dB over the dry sound during the chord (expected < 1), "
                    + juce::String(tail, 1) + " dB 1 to 2 s after the release (the room 16.5 dB under the direct sound there: > 0); from the front row, "
                    + juce::String(frontDuring, 1) + " dB over the 3 s (keep loudness: within 1.5), " + juce::String(frontTail, 1)
                    + " dB after the release (expected > 6)");
    }

    // 8f. The engine below the device's rate: a device at 4 x 44.1 kHz (what the Windows mixer can impose) runs
    //     the engine at 44.1 kHz and brings its sound up. Blocks of 1001 device samples, so that a frame straddles
    //     two blocks (the carry). The same chord as at 44.1 kHz, at the same level; nothing above 24 kHz but the
    //     upsampler's residue (a seam between two blocks would be a click, broadband); the device's rate mode keeps
    //     the engine at the device's rate.
    {
        player.playWholeTake = false;
        const double device = 4.0 * fs;
        auto play = [&](double rate, int block, std::vector<float>& out, int& factor, double& engineRate) {
            auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
            AudioHost& host = *hostOwner;
            host.start(rate, block);
            factor = host.rateFactor();
            engineRate = host.currentRate();
            std::vector<float> l((size_t) block), rr((size_t) block);
            float* outputs[2] = { l.data(), rr.data() };
            const int blocks = (int) (1.2 * rate) / block;
            for (int k = 0; k < blocks; ++k) {
                if (k == 2)
                    for (const int note : { 48, 60, 67, 76 })
                        host.send(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100));
                host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, block, {});
                out.insert(out.end(), l.begin(), l.end());
            }
        };
        std::vector<float> base, high;
        int baseFactor = 0, highFactor = 0;
        double baseRate = 0.0, highRate = 0.0;
        play(fs, 512, base, baseFactor, baseRate);
        play(device, 1001, high, highFactor, highRate);
        // From the onset (the chord does not land at the same time in blocks of different lengths).
        auto level = [](const std::vector<float>& x, double rate, double a, double b) {
            std::size_t onset = 0;
            while (onset < x.size() && std::abs(x[onset]) < 1e-4f)
                ++onset;
            double e = 0.0;
            for (std::size_t i = onset + (std::size_t) (a * rate); i < onset + (std::size_t) (b * rate) && i < x.size(); ++i)
                e += (double) x[(size_t) i] * x[(size_t) i];
            return 10.0 * std::log10(e / ((b - a) * rate) + 1e-30);
        };
        double levelGap = 0.0;
        for (double t = 0.0; t < 0.95; t += 0.1)
            levelGap = std::max(levelGap, std::abs(level(high, device, t, t + 0.1) - level(base, fs, t, t + 0.1)));
        // Above 24.1 kHz (the first image of 20 kHz), against the whole: Hann-windowed DFT of 8192 samples at 0.3 s.
        const int n = 8192, from = (int) (0.3 * device);
        double total = 0.0, above = 0.0;
        std::vector<double> w((size_t) n);
        for (int j = 0; j < n; ++j) {
            w[(size_t) j] = (0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * j / n)) * high[(size_t) (from + j)];
            total += w[(size_t) j] * w[(size_t) j];
        }
        for (int bin = (int) std::ceil(24100.0 / device * n); bin <= n / 2; ++bin) {
            double re = 0.0, im = 0.0;
            const double step = 2.0 * juce::MathConstants<double>::pi * bin / n;
            for (int j = 0; j < n; ++j) {
                re += w[(size_t) j] * std::cos(step * j);
                im -= w[(size_t) j] * std::sin(step * j);
            }
            above += (bin == n / 2 ? 1.0 : 2.0) * (re * re + im * im) / n; // Parseval: one-sided, both halves
        }
        const double imagesDb = 10.0 * std::log10(above / std::max(total, 1e-30) + 1e-30);
        auto atDeviceOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
        AudioHost& atDevice = *atDeviceOwner;
        atDevice.rateMode = 1;
        atDevice.start(device, 1001);
        r.check(baseFactor == 1 && highFactor == 4 && baseRate == fs && highRate == fs && levelGap < 0.1 && imagesDb < -80.0
                    && atDevice.currentRate() == device && atDevice.rateFactor() == 1,
                "engine below the device's rate: at " + juce::String(device / 1000.0, 1) + " kHz the engine runs at "
                    + juce::String(highRate / 1000.0, 1) + " kHz (x" + juce::String(highFactor) + "), level within "
                    + juce::String(levelGap, 3) + " dB of 44.1 kHz (expected < 0.1), " + juce::String(imagesDb, 1)
                    + " dB above 24 kHz (expected < -80); the device's rate mode: engine at "
                    + juce::String(atDevice.currentRate() / 1000.0, 1) + " kHz");
    }

    // 8g. Recording formats: a 32-bit float WAV (a 1 kHz sine at -6 dBFS on the left, 440 Hz at -12 dBFS on the
    //     right, 3 s) converted into each format, read back. WAV 24 and FLAC: the same samples, to the 24-bit
    //     rounding; MP3 and Opus: the duration (plus the encoder's delay and padding) and each channel's level.
    //     Then a 96 kHz master to MP3 (brought down to 48 kHz), and a whole background job.
    {
        const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest-formats");
        dir.deleteRecursively();
        dir.createDirectory();
        auto writeMaster = [&](const juce::File& file, double rate) {
            const int n = (int) (3.0 * rate);
            juce::AudioBuffer<float> x(2, n);
            for (int i = 0; i < n; ++i) {
                x.setSample(0, i, (float) (0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate)));
                x.setSample(1, i, (float) (0.25 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / rate)));
            }
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor(stream, juce::AudioFormatWriterOptions {}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(32)
                                                     .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)))
                w->writeFromAudioSampleBuffer(x, 0, n);
            return x;
        };
        struct Read {
            juce::AudioBuffer<float> x;
            double rate = 0.0;
        };
        auto readBack = [](const juce::File& file) {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            Read got;
            if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(file) }) {
                got.rate = reader->sampleRate;
                got.x.setSize((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read(&got.x, 0, (int) reader->lengthInSamples, 0, true, true);
            }
            return got;
        };
        auto levelDb = [](const Read& g, int c) { // RMS from 1 to 2 s
            double e = 0.0;
            const int a = (int) (1.0 * g.rate), b = std::min(g.x.getNumSamples(), (int) (2.0 * g.rate));
            for (int i = a; i < b && c < g.x.getNumChannels(); ++i)
                e += (double) g.x.getSample(c, i) * g.x.getSample(c, i);
            return 10.0 * std::log10(e / std::max(1, b - a) + 1e-30);
        };
        const double rate = 48000.0;
        const auto master = dir.getChildFile("master.wav");
        const auto source = writeMaster(master, rate);
        const double refL = 10.0 * std::log10(0.125), refR = 10.0 * std::log10(0.03125); // a sine's mean square
        juce::StringArray lines;
        bool ok = true;
        std::vector<float> wav24Samples;
        for (const auto& f : soundFormats()) {
            if (!canWrite(f.format)) {
                lines.add(juce::String(f.shortName) + " not on this system");
                continue;
            }
            const auto target = dir.getChildFile(juce::String("out-") + f.key + f.extension);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto result = convertSound(master, target, f.format);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            const auto g = readBack(target);
            if (result.failed() || g.x.getNumSamples() == 0) {
                ok = false;
                lines.add(juce::String(f.shortName) + ": " + (result.failed() ? result.getErrorMessage() : juce::String("cannot read it back")));
                continue;
            }
            const double seconds = g.x.getNumSamples() / g.rate;
            juce::String line = juce::String(f.shortName) + " " + juce::String(seconds, 3) + " s at " + juce::String(g.rate / 1000.0, 1) + " kHz, "
                              + juce::String(target.getSize() / 1024) + " KB in " + juce::String(juce::roundToInt(ms)) + " ms";
            if (f.format == SoundFormat::wav24 || f.format == SoundFormat::flac || f.format == SoundFormat::wavFloat) {
                float largest = 0.0f;
                bool sameAsWav24 = f.format != SoundFormat::flac || wav24Samples.size() == (size_t) g.x.getNumSamples();
                for (int j = 0; j < g.x.getNumSamples() && j < source.getNumSamples(); ++j) {
                    largest = std::max(largest, std::abs(g.x.getSample(0, j) - source.getSample(0, j)));
                    if (f.format == SoundFormat::flac && sameAsWav24 && g.x.getSample(0, j) != wav24Samples[(size_t) j])
                        sameAsWav24 = false;
                }
                if (f.format == SoundFormat::wav24)
                    wav24Samples.assign(g.x.getReadPointer(0), g.x.getReadPointer(0) + g.x.getNumSamples());
                const float bound = f.format == SoundFormat::wavFloat ? 0.0f : 1.0f / 8388608.0f; // half a 24-bit step
                const bool good = g.x.getNumSamples() == source.getNumSamples() && largest <= bound && sameAsWav24;
                ok = ok && good;
                line << ", largest difference " << (largest > 0.0f ? juce::String(20.0 * std::log10(largest), 1) + " dBFS" : juce::String("none"))
                     << (f.format == SoundFormat::flac ? (sameAsWav24 ? ", the WAV 24's samples" : ", NOT the WAV 24's samples") : "");
            } else {
                const double dl = levelDb(g, 0) - refL, dr = levelDb(g, 1) - refR;
                const double extra = seconds - 3.0;
                const double kbps = target.getSize() * 8.0 / 1000.0 / 3.0;
                const bool good = extra > -0.001 && extra < 0.15 && std::abs(dl) < 0.3 && std::abs(dr) < 0.3
                               && (f.format != SoundFormat::mp3 || std::abs(kbps - 320.0) < 20.0);
                ok = ok && good;
                line << ", levels " << juce::String(dl, 2) << " / " << juce::String(dr, 2) << " dB, " << juce::String(juce::roundToInt(kbps)) << " kbit/s";
            }
            lines.add(line);
        }
        // 96 kHz to MP3: brought down to 48 kHz first.
        if (canWrite(SoundFormat::mp3)) {
            const auto hiMaster = dir.getChildFile("master-96k.wav");
            writeMaster(hiMaster, 96000.0);
            const auto target = dir.getChildFile("out-96k.mp3");
            const auto result = convertSound(hiMaster, target, SoundFormat::mp3);
            const auto g = readBack(target);
            const double dl = levelDb(g, 0) - refL;
            const bool good = result.wasOk() && g.rate == 48000.0 && std::abs(dl) < 0.3;
            ok = ok && good;
            lines.add("96 kHz to MP3: " + (result.failed() ? result.getErrorMessage() : "at " + juce::String(g.rate / 1000.0, 1) + " kHz, level " + juce::String(dl, 2) + " dB"));
        }
        // A background job: the master removed once every format is made.
        {
            const auto jobMaster = dir.getChildFile("take.master.wav");
            writeMaster(jobMaster, rate);
            RecordingExporter exporter;
            RecordingExporter::Job job;
            job.master = jobMaster;
            job.removeMaster = true;
            job.label = "take";
            job.targets.emplace_back(SoundFormat::flac, dir.getChildFile("take.flac"));
            job.targets.emplace_back(SoundFormat::opus, dir.getChildFile("take.opus"));
            exporter.submit(std::move(job));
            const auto until = juce::Time::getMillisecondCounterHiRes() + 20000.0;
            while (exporter.busy() && juce::Time::getMillisecondCounterHiRes() < until)
                juce::Thread::sleep(10);
            const auto reports = exporter.takeReports();
            const bool good = reports.size() == 1 && reports[0].ok && !jobMaster.exists() && dir.getChildFile("take.flac").existsAsFile()
                           && dir.getChildFile("take.opus").existsAsFile();
            ok = ok && good;
            lines.add("background job: " + (reports.empty() ? juce::String("no report") : reports[0].text) + (jobMaster.exists() ? ", master left" : ", master removed"));
        }
        dir.deleteRecursively();
        r.check(ok, "recording formats: " + lines.joinIntoString("; "));
    }

    // 8h. Settings read back (an instrument preset, an import): the panels, rebuilt from a settings file changed
    //     under them, set the engine and the room as the file says, a room preset of one's own included.
    {
        const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest.settings");
        file.deleteFile();
        juce::PropertiesFile::Options options;
        juce::PropertiesFile settings(file, options);
        pf::ModalEngine engine;
        ReferencePlayer reference;
        pf::Room room;
        EnginePanel enginePanel(engine, reference, settings);
        RoomPanel roomPanel(room, settings);
        const bool before = engine.unisonSpread.load() == 1.0f && room.distance.load() == 1.0f;
        settings.setValue("unisonSpread", 2.5);
        settings.setValue("modalBody", false);
        settings.setValue("referenceA4", 442.0);
        settings.setValue("roomModel", 2);
        settings.setValue("roomUserPresets", "[{\"name\":\"Hall, 12 m\",\"roomLevel\":-2.0,\"roomSize\":0.93,\"roomDecay\":3.0,"
                                             "\"roomBrightness\":0.45,\"roomWidth\":1.0,\"roomDistance\":12.0,\"keepLoudness\":false}]");
        settings.setValue("roomPreset", 200);
        settings.setValue("roomPresetName", "Hall, 12 m");
        settings.setValue("roomHeadphones", true);
        enginePanel.reload();
        roomPanel.reload();
        const bool engineOk = std::abs(engine.unisonSpread.load() - 2.5f) < 1e-6f && !engine.bodyEnabled.load()
                           && std::abs(engine.referenceA4.load() - 442.0f) < 1e-3f;
        const bool roomOk = std::abs(room.distance.load() - 12.0f) < 1e-3f && std::abs(room.decaySeconds.load() - 3.0f) < 1e-3f
                         && !room.keepLoudness.load() && roomPanel.presetName().startsWith("Hall, 12 m") && room.headphones.load()
                         && roomPanel.onHeadphones();
        r.check(before && engineOk && roomOk,
                "settings read back: unison " + juce::String(engine.unisonSpread.load(), 2) + ", body " + (engine.bodyEnabled.load() ? "on" : "off")
                    + ", A4 " + juce::String(engine.referenceA4.load(), 1) + " Hz; room \"" + roomPanel.presetName() + "\", decay "
                    + juce::String(room.decaySeconds.load(), 1) + " s, keep loudness " + (room.keepLoudness.load() ? "on" : "off")
                    + ", " + (room.headphones.load() ? "headphones" : "speakers") + " (expected 2.50, off, 442.0; Hall, 12 m, 3.0 s, off, headphones)");
        file.deleteFile();
    }

    // 8i. Takes listened to: a WAV (440 Hz on the left, 660 Hz on the right) played through the audio host, heard as
    //     it is; then two in A/B (A silent, B the tone), switched to B mid-way with a 10 ms crossfade; the end stops
    //     the playback. Not recorded: the live recording tap does not see it.
    {
        const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest-takes");
        dir.deleteRecursively();
        dir.createDirectory();
        auto write = [&](const juce::File& f, double amp) {
            const int n = (int) (1.0 * fs);
            juce::AudioBuffer<float> x(2, n);
            for (int i = 0; i < n; ++i) {
                x.setSample(0, i, (float) (amp * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / fs)));
                x.setSample(1, i, (float) (amp * std::sin(2.0 * juce::MathConstants<double>::pi * 660.0 * i / fs)));
            }
            writeWav(x, fs, f);
            return x;
        };
        const auto tone = write(dir.getChildFile("tone.wav"), 0.25);
        write(dir.getChildFile("silence.wav"), 0.0);
        player.playWholeTake = false;
        auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
        AudioHost& host = *hostOwner;
        host.start(fs, 512);
        std::vector<float> l(512), rr(512);
        float* outputs[2] = { l.data(), rr.data() };
        auto run = [&](double seconds, std::vector<float>& outL) {
            for (int k = 0; k < (int) (seconds * fs) / 512; ++k) {
                host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
                outL.insert(outL.end(), l.begin(), l.end());
            }
        };
        const auto loaded = host.playback.load(dir.getChildFile("tone.wav"));
        host.playback.play();
        std::vector<float> heard;
        run(0.5, heard);
        float heardWorst = 0.0f; // against the file, sample for sample (the engine is silent)
        for (std::size_t k = 0; k < heard.size(); ++k)
            heardWorst = std::max(heardWorst, std::abs(heard[k] - tone.getSample(0, (int) k)));
        // A/B: A silent, B the tone; B from 0.25 s.
        host.playback.load(dir.getChildFile("silence.wav"), dir.getChildFile("tone.wav"));
        host.playback.play();
        std::vector<float> ab;
        run(0.25, ab);
        host.playback.setSide(1);
        run(0.5, ab);
        auto rms = [&](double a, double b) {
            double e = 0.0;
            for (int i = (int) (a * fs); i < (int) (b * fs); ++i)
                e += (double) ab[(size_t) i] * ab[(size_t) i];
            return std::sqrt(e / ((b - a) * fs));
        };
        const double before = rms(0.05, 0.24), after = rms(0.3, 0.7);
        std::vector<float> rest;
        run(0.5, rest); // past the end (1 s): stopped
        const bool stopped = !host.playback.isPlaying();
        r.check(loaded.isEmpty() && heardWorst < 1e-6f && before < 1e-6 && std::abs(after - 0.25 / std::sqrt(2.0)) < 0.01 && stopped,
                "takes listened to: the file heard as it is (largest difference " + juce::String(heardWorst, 7) + "); A/B: A "
                    + juce::String(before, 4) + ", B " + juce::String(after, 4) + " RMS (expected 0, 0.177); stopped at the end: "
                    + (stopped ? "yes" : "no"));
        host.playback.release();
        dir.deleteRecursively();
    }

    // 8k. The OBS plugin's feed: a chord through the audio host, the room heard on headphones, the volume at -6 dB.
    //     The device plays the feed's heard mix at the volume (it comes after the room, the feed before it); the
    //     speakers mix is what the room gives on speakers (a host whose room is heard on speakers: the same, sample
    //     for sample); the dry mix is the engine's sound; the events ride along at their frames, the volume too.
    {
        auto play = [&](bool headphones, std::vector<float> (&mixes)[3], std::vector<float>& device, std::vector<pf::FeedEvent>& events,
                        float& volume) {
            player.playWholeTake = false;
            auto hostOwner = std::make_unique<AudioHost>(player); // on the heap: hosts are large
            AudioHost& host = *hostOwner;
            pf::Room::Settings s;
            s.enabled = true;
            s.size = 0.5f;
            s.distance = 3.0f;
            s.headphones = headphones;
            host.room.applySettings(s);
            host.volume = 0.5f;
            host.speakersWanted = true;
            host.start(fs, 512);
            pf::AudioFeedReader reader;
            const bool open = reader.open();
            const auto from = reader.written();
            const auto eventsFrom = reader.eventsWritten();
            std::vector<float> l(512), rr(512), frames(2 * 1024);
            float* outputs[2] = { l.data(), rr.data() };
            auto at = from;
            for (int k = 0; k < (int) fs / 512; ++k) {
                for (const int note : { 48, 64, 67 })
                    if (k == 2)
                        host.send(juce::MidiMessage::noteOn(1, note, (juce::uint8) 50));
                host.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, 2, 512, {});
                device.insert(device.end(), l.begin(), l.end());
                // The block's frames of each mix, read as they come (as OBS does).
                const auto written = reader.written();
                for (int m = 0; m < 3 && open && written > at; ++m) {
                    const int n = reader.read((pf::FeedMix) m, at, frames.data(), (int) (written - at));
                    for (int i = 0; i < n; ++i)
                        mixes[m].push_back(frames[(std::size_t) (2 * i)]);
                }
                at = written;
            }
            pf::FeedEvent got[16];
            const int n = open ? reader.readEvents(eventsFrom, got, 16) : 0;
            for (int i = 0; i < n; ++i)
                events.push_back(got[i]);
            volume = reader.volume();
            return open && host.feedOn();
        };
        std::vector<float> phones[3], speakers[3], phonesDevice, speakersDevice;
        std::vector<pf::FeedEvent> events, ignored;
        float volume = 0.0f, ignoredVolume = 0.0f;
        const bool fed = play(true, phones, phonesDevice, events, volume) && play(false, speakers, speakersDevice, ignored, ignoredVolume);
        float deviceWorst = 0.0f, speakersWorst = 0.0f, phonesApart = 0.0f, dryApart = 0.0f, peak = 0.0f;
        for (const float v : phones[0])
            peak = std::max(peak, std::abs(v));
        const std::size_t n = std::min({ phones[0].size(), phones[1].size(), phones[2].size(), speakers[0].size(), phonesDevice.size() });
        for (std::size_t k = 0; k < n; ++k) {
            deviceWorst = std::max(deviceWorst, std::abs(phonesDevice[k] - 0.5f * phones[0][k]));
            speakersWorst = std::max(speakersWorst, std::abs(phones[1][k] - speakers[0][k]));
            phonesApart = std::max(phonesApart, std::abs(phones[0][k] - phones[1][k]));
            dryApart = std::max(dryApart, std::abs(phones[2][k] - phones[1][k]));
        }
        const bool chord = events.size() == 3 && (events[0].status & 0xF0) == 0x90 && events[0].data1 == 48 && events[2].data1 == 67;
        r.check(fed && n > 40000 && deviceWorst < 1e-6f && speakersWorst < 1e-6f && phonesApart > 1e-3f && dryApart > 1e-3f && chord
                    && volume == 0.5f,
                "OBS feed: the device plays the heard mix at the volume (" + juce::String(deviceWorst, 7) + " apart), the speakers mix is the room on speakers ("
                    + juce::String(speakersWorst, 7) + " apart, the headphones' " + juce::String(phonesApart, 3) + " away), the dry one the engine's ("
                    + juce::String(dryApart, 3) + " away), " + juce::String((int) events.size()) + " events, volume " + juce::String(volume, 2)
                    + "; peak " + juce::String(peak, 3) + ", " + juce::String((int) n) + " frames");
    }

    // 8j. Updates: versions compared; a manifest of our own (a relative installer, next to it) read from a file
    //     address, the installer downloaded and checked; a wrong SHA-256 or size refused, nothing left behind;
    //     GitHub's release format; plain http refused. The installer is never run here.
    {
        const bool versions = UpdateChecker::compareVersions("0.10.0", "0.9.3") > 0 && UpdateChecker::compareVersions("v1.2", "1.2.0") == 0
                           && UpdateChecker::compareVersions("2.0.0-beta", "2.0.0") < 0 && UpdateChecker::compareVersions("0.1.0", PIANOFORGE_VERSION) <= 0;
        const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pianoforge-selftest-update");
        dir.deleteRecursively();
        dir.getChildFile("site").createDirectory();
        const auto setup = dir.getChildFile("site").getChildFile("PianoForge-9.9.9-setup.exe");
        {
            juce::MemoryBlock bytes(300000);
            juce::Random random(7);
            random.fillBitsRandomly(bytes.getData(), bytes.getSize());
            setup.replaceWithData(bytes.getData(), bytes.getSize());
        }
        const auto sum = UpdateChecker::sha256Of(setup);
        auto manifest = [&](const juce::String& sha, juce::int64 size) {
            const auto file = dir.getChildFile("site").getChildFile("latest.json");
            file.replaceWithText("{ \"version\": \"9.9.9\", \"url\": \"PianoForge-9.9.9-setup.exe\", \"sha256\": \"" + sha
                                 + "\", \"size\": " + juce::String(size) + ", \"notes\": \"Test\" }");
            return juce::URL(file).toString(false);
        };
        const auto found = UpdateChecker::fetch(manifest(sum, setup.getSize()), PIANOFORGE_VERSION);
        const auto saved = dir.getChildFile("download").getChildFile(found.release.fileName());
        const auto downloaded = found.ok ? UpdateChecker::download(found.release, saved) : found.error;
        const bool good = found.ok && found.newer && downloaded.isEmpty() && saved.existsAsFile() && UpdateChecker::sha256Of(saved) == sum;
        saved.deleteFile();
        auto wrongSum = sum.replaceSection(0, 1, sum[0] == 'a' ? "b" : "a");
        const auto badSum = UpdateChecker::download(UpdateChecker::fetch(manifest(wrongSum, setup.getSize()), "0.1.0").release, saved);
        const bool refusedSum = badSum.contains("SHA-256") && !saved.exists() && !saved.getSiblingFile(saved.getFileName() + ".part").exists();
        const auto badSize = UpdateChecker::download(UpdateChecker::fetch(manifest(sum, setup.getSize() - 1), "0.1.0").release, saved);
        const bool refusedSize = badSize.isNotEmpty() && !saved.exists();
        const auto github = UpdateChecker::parse(
            "{\"tag_name\":\"v1.2.3\",\"body\":\"Notes\",\"assets\":[{\"name\":\"PianoForge-1.2.3-setup.exe\",\"size\":42,"
            "\"browser_download_url\":\"https://github.com/o/r/releases/download/v1.2.3/PianoForge-1.2.3-setup.exe\","
            "\"digest\":\"sha256:" + sum + "\"}]}", "1.2.2", "https://api.github.com/repos/o/r/releases/latest");
        const bool githubOk = github.ok && github.newer && github.release.version == "1.2.3" && github.release.sha256 == sum
                           && github.release.size == 42 && github.release.fileName() == "PianoForge-1.2.3-setup.exe";
        const bool httpRefused = !UpdateChecker::addressAllowed("http://example.com/latest.json")
                              && !UpdateChecker::fetch("http://example.com/latest.json", "0.1.0").ok;
        r.check(versions && good && refusedSum && refusedSize && githubOk && httpRefused,
                "updates: versions " + juce::String(versions ? "ordered" : "MISORDERED") + "; manifest " + (found.ok ? "read" : found.error)
                    + (found.newer ? ", 9.9.9 newer" : "") + "; download " + (downloaded.isEmpty() ? "checked" : downloaded)
                    + "; wrong SHA-256 " + (refusedSum ? "refused" : "ACCEPTED") + ", wrong size " + (refusedSize ? "refused" : "ACCEPTED")
                    + "; GitHub release " + (githubOk ? "read" : github.error) + "; plain http " + (httpRefused ? "refused" : "ACCEPTED"));
        dir.deleteRecursively();
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
