// SPDX-License-Identifier: AGPL-3.0-or-later

// The VST3 plugin as a host loads it (juce::VST3PluginFormat): what it declares, the same sound as the engine,
// any block size, automation, headphones, the state of a project, a host at 192 kHz, a second instance, the cost.
// The sound tests need a modal profile (the application's corpus); without one they are skipped. Exits non-zero
// on a failure.
//   PianoForgePluginTest <PianoForge.vst3> [<modal-profile.bin>]   (compared against when the application's corpus
//                                                                    has no profile; else the corpus's is)
//   PianoForgePluginTest <PianoForge.vst3> --show-editor <seconds> [<file.png>]   (the editor in a window, saved)
#include <pianoforge/limiter.hpp>
#include <pianoforge/modal_engine.hpp>
#include <pianoforge/modal_profile.hpp>
#include <pianoforge/room.hpp>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../../app/src/SettingsLocation.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const juce::String& what)
{
    std::printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what.toRawUTF8());
    failures += ok ? 0 : 1;
}

struct Stereo {
    std::vector<float> l, r;
};

struct Note {
    int at;
    juce::MidiMessage message;
};

/// Renders `total` samples through the plugin, in blocks of the sizes given in turn (cycling).
Stereo render(juce::AudioPluginInstance& p, int total, const std::vector<Note>& notes, const std::vector<int>& blocks)
{
    Stereo out;
    out.l.reserve((size_t) total);
    out.r.reserve((size_t) total);
    juce::AudioBuffer<float> buffer(2, *std::max_element(blocks.begin(), blocks.end()));
    juce::MidiBuffer midi;
    size_t b = 0;
    for (int pos = 0; pos < total;) {
        const int n = std::min(blocks[b++ % blocks.size()], total - pos);
        buffer.setSize(2, n, false, false, true);
        buffer.clear();
        midi.clear();
        for (const auto& note : notes)
            if (note.at >= pos && note.at < pos + n)
                midi.addEvent(note.message, note.at - pos);
        p.processBlock(buffer, midi);
        out.l.insert(out.l.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + n);
        out.r.insert(out.r.end(), buffer.getReadPointer(1), buffer.getReadPointer(1) + n);
        pos += n;
    }
    return out;
}

double energy(const Stereo& s, size_t from = 0, size_t to = SIZE_MAX)
{
    double e = 0.0;
    for (size_t i = from; i < std::min(to, s.l.size()); ++i)
        e += (double) s.l[i] * s.l[i] + (double) s.r[i] * s.r[i];
    return e;
}

double db(double ratio)
{
    return 10.0 * std::log10(ratio + 1e-300);
}

float largestDifference(const Stereo& a, const Stereo& b)
{
    float worst = 0.0f;
    for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i)
        worst = std::max({ worst, std::abs(a.l[i] - b.l[i]), std::abs(a.r[i] - b.r[i]) });
    return worst;
}

float peakOf(const Stereo& s)
{
    float p = 0.0f;
    for (size_t i = 0; i < s.l.size(); ++i)
        p = std::max({ p, std::abs(s.l[i]), std::abs(s.r[i]) });
    return p;
}

juce::AudioProcessorParameter* find(juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* parameter : p.getParameters())
        if (parameter->getName(64) == name)
            return parameter;
    return nullptr;
}

/// A parameter set as a host automates it: in its own units, through its normalized value.
void set(juce::AudioPluginInstance& p, const juce::String& name, float value)
{
    auto* parameter = find(p, name);
    if (parameter == nullptr) {
        check(false, "parameter " + name + " exists");
        return;
    }
    // A hosted parameter knows only its normalized value and its text: a switch or a choice by its index, the others
    // by their value as text (the plugin parses it).
    float normalized = 0.0f;
    if (const int steps = parameter->getNumSteps(); parameter->isDiscrete() && steps > 1 && steps <= 64)
        normalized = juce::jlimit(0.0f, 1.0f, value / (float) (steps - 1));
    else
        normalized = parameter->getValueForText(juce::String(value, 3));
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(normalized);
    parameter->endChangeGesture();
}

/// One block of silence: a parameter or a program set from the host reaches an LV2 plugin with the audio (an atom
/// message in its next run()), and its new values come back the same way, which JUCE's LV2 host hands to its
/// parameters on the message thread; VST3 takes them at once.
void settle(juce::AudioPluginInstance& p)
{
    render(p, 512, {}, { 512 });
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}

/// The profile the plugin plays: the one beside the application's corpus, as the plugin finds it
/// (Processor::applicationProfile); else the one given. (ctest gives Noire's, the Windows machine's corpus: another
/// machine's corpus may be another bank.)
juce::File playedProfile(const juce::String& given)
{
    const auto options = pf::app::settingsOptions();
    if (const auto file = options.getDefaultFile(); file.existsAsFile()) {
        const juce::PropertiesFile settings(file, options); // read only
        const auto corpus = settings.getValue("corpus");
        if (juce::File::isAbsolutePath(corpus))
            if (const auto profile = juce::File(corpus).getParentDirectory().getChildFile("modal-profile.bin"); profile.existsAsFile())
                return profile;
    }
    return juce::File(given);
}

/// Back to silence, as a host does between two takes: reset() (VST3 maps it to the plugin's own); LV2 has none, its
/// hosts deactivate and activate the plugin, and JUCE's LV2 host leaves reset() empty: the same, done here.
void restart(juce::AudioPluginInstance& p)
{
    p.reset();
    if (p.getPluginDescription().pluginFormatName == "LV2") {
        const double rate = p.getSampleRate();
        const int block = p.getBlockSize();
        p.releaseResources();
        p.prepareToPlay(rate, block);
    }
}

/// Waits for the plugin's profile: a note until it sounds (it loads in the background).
bool waitForSound(juce::AudioPluginInstance& p, double rate, double seconds)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1000.0 * seconds;
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        const auto s = render(p, (int) (0.3 * rate), { { 0, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100) },
                                                       { (int) (0.1 * rate), juce::MidiMessage::noteOff(1, 60) } }, { 512 });
        if (energy(s) > 1e-6) {
            restart(p);
            render(p, (int) (2.0 * rate), {}, { 512 }); // let it fade
            return true;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    }
    return false;
}

std::unique_ptr<juce::AudioPluginInstance> create(juce::AudioPluginFormat& format, const juce::PluginDescription& d, double rate, int block)
{
    juce::String error;
    auto instance = format.createInstanceFromDescription(d, rate, block, error);
    if (instance == nullptr) {
        check(false, "instance created: " + error);
        return nullptr;
    }
    instance->enableAllBuses();
    instance->setPlayConfigDetails(0, 2, rate, block);
    instance->prepareToPlay(rate, block);
    return instance;
}

} // namespace

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // every line as it comes
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) {
        std::printf("usage: PianoForgePluginTest <PianoForge.vst3> [<modal-profile.bin> | --show-editor <seconds>]\n");
        return 2;
    }
    // Resolved (a path ending in "/../.." is not found by the VST3 scanner).
    const auto bundle = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[1])).getFullPathName();
    // The VST3 bundle, or (Linux) the LV2 one: the same checks through either format.
    const bool lv2 = bundle.endsWithIgnoreCase(".lv2");
    std::unique_ptr<juce::AudioPluginFormat> formatOwned;
#if JUCE_PLUGINHOST_LV2
    if (lv2) { // found by its URI, in the folders of LV2_PATH: the bundle's own
        const char* specs = std::getenv("LV2_PATH"); // (ctest: JUCE's copy of LV2's specifications, the class hierarchy)
        const auto path = juce::File(bundle).getParentDirectory().getFullPathName() + (specs != nullptr ? ":" + juce::String(specs) : juce::String());
        setenv("LV2_PATH", path.toRawUTF8(), 1);
        formatOwned = std::make_unique<juce::LV2PluginFormat>();
    }
#endif
    if (formatOwned == nullptr)
        formatOwned = std::make_unique<juce::VST3PluginFormat>();
    auto& format = *formatOwned;
    const juce::String formatName = lv2 ? "LV2" : "VST3";
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, lv2 ? juce::String("urn:pianoforge:piano") : bundle);
    check(types.size() == 1, "found in " + bundle + ": " + juce::String(types.size()) + " plugin");
    if (types.isEmpty())
        return 1;
    const auto& d = *types[0];

    if (argc >= 4 && juce::String(argv[2]) == "--show-editor") { // a window for a screenshot
        auto p = create(format, d, 48000.0, 512);
        if (p == nullptr)
            return 1;
        waitForSound(*p, 48000.0, 10.0);
        juce::DocumentWindow window("PianoForge plugin", juce::Colours::black, 0);
        window.setUsingNativeTitleBar(true);
        window.setContentNonOwned(p->createEditorAndMakeActive(), true);
        window.setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil((int) (1000.0 * juce::String(argv[3]).getDoubleValue()));
        if (argc >= 5) { // the editor as a picture (the README's screenshots)
            auto* editor = window.getContentComponent();
            const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[4]));
            file.deleteFile();
            juce::FileOutputStream out(file);
            juce::PNGImageFormat().writeImageToStream(image, out);
        }
        delete window.getContentComponent();
        return 0;
    }

    // 1. What it declares.
    check(d.isInstrument && d.name == "PianoForge" && d.pluginFormatName == formatName && d.manufacturerName == "PianoForge",
          "an instrument: " + d.name + " by " + d.manufacturerName + " (" + d.pluginFormatName + ", " + d.category + ")");
    auto p = create(format, d, 48000.0, 512);
    if (p == nullptr)
        return 1;
    check(p->getTotalNumInputChannels() == 0 && p->getTotalNumOutputChannels() == 2 && p->acceptsMidi() && p->hasEditor(),
          "no input, stereo output, MIDI in, an editor (" + juce::String(p->getTotalNumOutputChannels()) + " outputs)");
    juce::StringArray names; // ours (the VST3 wrapper adds Bypass, Program, and hidden ones that carry MIDI controllers)
    int controllers = 0;
    for (auto* parameter : p->getParameters()) {
        const auto name = parameter->getName(64);
        if (name.startsWith("MIDI CC"))
            ++controllers;
        else if (name != "Bypass" && name != "Program") // (the programs, as VST3 carries them)
            names.add(name);
    }
    bool all = true;
    for (const auto* name : { "Volume", "Sympathetic resonance", "Unison tuning", "Room", "Room decay", "Distance", "Heard on",
                              "A4", "Temperament", "Output limiter", "Voicing treble", "Dynamic range", "Lid", "Presence" })
        all = all && names.contains(name);
    check(all && names.size() == 37 && (lv2 || controllers >= 128), // (LV2 carries MIDI controllers as they are)
          juce::String(names.size()) + " automatable parameters (" + names.joinIntoString(", ") + "); MIDI controllers (CC64, CC66, CC67...) "
              "reach it through " + juce::String(controllers) + " hidden ones, as VST3 carries them");

    // 2. The profile, loaded in the background (the application's corpus).
    const auto t0 = std::chrono::steady_clock::now();
    if (!waitForSound(*p, 48000.0, 20.0)) {
        std::printf("no modal profile reachable (the application's corpus): the sound tests are skipped\n");
        return failures ? 1 : 0;
    }
    const double firstLoad = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(true, "the profile loaded in the background, sound after " + juce::String(firstLoad, 2) + " s");

    const double rate = 48000.0;
    const int total = (int) (3.0 * rate);
    const std::vector<Note> phrase { { 0, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100) },
                                     { 4800, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 80) },
                                     { 9600, juce::MidiMessage::noteOn(1, 67, (juce::uint8) 90) },
                                     { 12000, juce::MidiMessage::controllerEvent(1, 64, 127) },
                                     { 48000, juce::MidiMessage::noteOff(1, 60) },
                                     { 48000, juce::MidiMessage::noteOff(1, 64) },
                                     { 48000, juce::MidiMessage::noteOff(1, 67) },
                                     { 96000, juce::MidiMessage::controllerEvent(1, 64, 0) } };

    const int lastStep = juce::SystemStats::getEnvironmentVariable("PF_TEST_STEPS", "99").getIntValue();
    if (lastStep <= 2)
        return failures ? 1 : 0;
    // 3. The same sound as the engine itself (room and limiter off; the rest as the application's defaults).
    if (argc >= 3) {
        std::string error;
        const auto reference = playedProfile(juce::String::fromUTF8(argv[2]));
        const auto profile = pf::loadModalProfile(std::filesystem::path(reference.getFullPathName().toWideCharPointer()), error);
        if (profile != nullptr) {
            set(*p, "Room", 0.0f);
            set(*p, "Output limiter", 0.0f);
            restart(*p);
            render(*p, 512, {}, { 512 }); // the parameters applied
            const auto plugin = render(*p, total, phrase, { 512 });
            pf::ModalEngine engine;
            engine.install(profile);
            engine.prepare(rate, 512);
            engine.outputGain = juce::Decibels::decibelsToGain(6.0f);
            Stereo direct;
            std::vector<float> l(512), r(512);
            float* outs[2] = { l.data(), r.data() };
            for (int pos = 0; pos < total; pos += 512) {
                std::vector<pf::Event> events;
                for (const auto& note : phrase)
                    if (note.at >= pos && note.at < pos + 512) {
                        const auto* raw = note.message.getRawData();
                        events.push_back({ (std::uint32_t) (note.at - pos), raw[0], raw[1], raw[2] });
                    }
                engine.process(outs, 2, 512, events.data(), (int) events.size());
                direct.l.insert(direct.l.end(), l.begin(), l.end());
                direct.r.insert(direct.r.end(), r.begin(), r.end());
            }
            const float diff = largestDifference(plugin, direct), peak = peakOf(direct);
            check(peak > 1e-3f && diff <= 1e-5f * peak, "the plugin plays the engine's sound (" + reference.getParentDirectory().getFileName()
                                                            + "'s profile): largest difference "
                                                            + juce::String(juce::roundToInt(20.0 * std::log10(diff / peak + 1e-12))) + " dB under the peak");
            set(*p, "Room", 1.0f);
            set(*p, "Output limiter", 1.0f);
        }
    }

    if (lastStep <= 3)
        return failures ? 1 : 0;
    // 4. Any block size (room and limiter on): blocks of 1 to 512 samples (the most the host announced: VST3 allows
    //    no more) give what blocks of 512 give.
    std::mt19937 rng(5);
    std::vector<int> odd;
    for (int i = 0; i < 400; ++i)
        odd.push_back(i % 7 == 0 ? 1 + (int) (rng() % 7) : 1 + (int) (rng() % 512));
    restart(*p);
    render(*p, (int) rate, {}, { 512 });
    const auto fixed = render(*p, total, phrase, { 512 });
    restart(*p);
    render(*p, (int) rate, {}, { 512 });
    const auto varied = render(*p, total, phrase, odd);
    // The engine takes a few decisions at its block boundaries (a partial's fade starting, the soundboard going to
    // sleep): other blocks move them by a few samples, far under hearing.
    const auto underPeak = [](const Stereo& a, const Stereo& b) {
        return 20.0 * std::log10(largestDifference(a, b) / std::max(peakOf(a), 1e-9f) + 1e-12);
    };
    check(underPeak(fixed, varied) < -40.0, "blocks of 1 to 512 samples: the same sound within "
                                                + juce::String(juce::roundToInt(underPeak(fixed, varied))) + " dB under the peak");
    check(peakOf(fixed) <= 0.8913f && peakOf(fixed) > 0.01f, "under the limiter's ceiling: peak " + juce::String(20.0 * std::log10(peakOf(fixed)), 1) + " dBFS");

    // 5. Automation: the volume 6 dB down.
    set(*p, "Volume", 0.0f);
    restart(*p);
    render(*p, (int) rate, {}, { 512 });
    const auto softer = render(*p, total, phrase, { 512 });
    const double change = db(energy(softer) / energy(fixed));
    check(std::abs(change + 6.0) < 0.2, "Volume automated from +6 to 0 dB: " + juce::String(change, 2) + " dB");
    set(*p, "Volume", 6.0f);

    // 6. Heard on headphones: another rendering, the same energy within 1.5 dB.
    set(*p, "Heard on", 1.0f);
    restart(*p);
    render(*p, (int) rate, {}, { 512 });
    const auto ears = render(*p, total, phrase, { 512 });
    const double earsChange = db(energy(ears) / energy(fixed));
    check(largestDifference(ears, fixed) > 1e-4f && std::abs(earsChange) < 1.5,
          "heard on headphones: binaural, " + juce::String(earsChange, 2) + " dB against speakers");

    if (lastStep <= 6)
        return failures ? 1 : 0;
    // 7. A project saved and opened again: the state gives the same parameters and the same sound.
    set(*p, "Volume", 0.0f);
    settle(*p);
    juce::MemoryBlock state;
    p->getStateInformation(state);
    restart(*p);
    render(*p, (int) rate, {}, { 512 });
    const auto before = render(*p, total, phrase, { 512 });
    auto second = create(format, d, rate, 512);
    const auto t1 = std::chrono::steady_clock::now();
    second->setStateInformation(state.getData(), (int) state.getSize());
    const bool secondReady = waitForSound(*second, rate, 10.0);
    const double secondLoad = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    restart(*second);
    render(*second, (int) rate, {}, { 512 });
    const auto after = render(*second, total, phrase, { 512 });
    const auto* heard = find(*second, "Heard on");
    check(secondReady && largestDifference(before, after) == 0.0f && heard != nullptr && heard->getValue() > 0.5f,
          "a project opened again: headphones and volume restored, the same sound (largest difference "
              + juce::String(largestDifference(before, after), 9) + "); the second instance shares the profile (sound after "
              + juce::String(secondLoad, 2) + " s, the first after " + juce::String(firstLoad, 2) + " s)");
    second.reset();

    // 8. A host at 192 kHz: the engine at 48 kHz brought up to it, the latency reported; any block size.
    {
        auto fast = create(format, d, 192000.0, 1024);
        waitForSound(*fast, 192000.0, 10.0);
        std::vector<Note> phrase4;
        for (const auto& note : phrase) // the same instants (multiples of 4 samples: on the engine's frames)
            phrase4.push_back({ note.at * 4, note.message });
        const int total4 = total * 4;
        restart(*fast);
        render(*fast, 192000, {}, { 1024 });
        const auto big = render(*fast, total4, phrase4, { 1024 });
        restart(*fast);
        render(*fast, 192000, {}, { 1024 });
        std::vector<int> tiny;
        for (int i = 0; i < 300; ++i)
            tiny.push_back(1 + (int) (rng() % 9));
        tiny.push_back(1024);
        tiny.push_back(333);
        const auto small = render(*fast, total4, phrase4, tiny);
        const double level = db(energy(big) / 4.0 / energy(fixed)); // the same settings as `fixed`: the defaults
        check(fast->getLatencySamples() == 127 && underPeak(big, small) < -40.0 && std::abs(level) < 0.5,
              "a host at 192 kHz: latency " + juce::String(fast->getLatencySamples()) + " samples (0.66 ms), blocks of 1 to 1024 the "
                  "same sound within " + juce::String(juce::roundToInt(underPeak(big, small))) + " dB under the peak, level " + juce::String(level, 2)
                  + " dB against 48 kHz");
    }

    // 9. Cost: 64 notes with the pedal down, 10 s at 48 kHz, blocks of 128, room on.
    {
        std::vector<Note> busy { { 0, juce::MidiMessage::controllerEvent(1, 64, 127) } };
        for (int i = 0; i < 64; ++i)
            busy.push_back({ i * 480, juce::MidiMessage::noteOn(1, 21 + (i * 7) % 88, (juce::uint8) 80) });
        restart(*p);
        const auto t2 = std::chrono::steady_clock::now();
        render(*p, (int) (10.0 * rate), busy, { 128 });
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
        check(seconds < 5.0, "64 notes with the pedal down through the plugin: " + juce::String(100.0 * seconds / 10.0, 1) + " % of real time");
    }

    // 10. The factory presets as the plugin's programs: each by its name; one chosen sets the sound as the
    //     application's would (Night practice: the lid closed, the dynamics at 60 %), another puts it back (As
    //     measured: open, 100 %); the lid closed is heard (the same phrase quieter).
    {
        juce::StringArray programs;
        for (int i = 0; i < p->getNumPrograms(); ++i)
            programs.add(p->getProgramName(i));
        const int night = programs.indexOf("Night practice"), measured = programs.indexOf("As measured");
        auto text = [&](const char* name) {
            const auto* parameter = find(*p, name);
            return parameter != nullptr ? parameter->getText(parameter->getValue(), 64) : juce::String("?");
        };
        bool nightOk = false, backOk = false;
        juce::String nightSeen, backSeen; // what the host shows, when it is not so
        double quieter = 0.0;
        if (night >= 0 && measured >= 0) {
            p->setCurrentProgram(night);
            settle(*p);
            // (LV2 has no text for a value: its hosts show the number, 0 for the lid closed, 100 open.)
            nightOk = (text("Lid") == "Closed" || text("Lid") == "0") && text("Dynamic range").startsWith("60");
            nightSeen = "lid " + text("Lid") + ", dynamics " + text("Dynamic range");
            restart(*p);
            render(*p, 512, {}, { 512 });
            const auto closed = render(*p, total, phrase, { 512 });
            p->setCurrentProgram(measured);
            settle(*p);
            backOk = (text("Lid") == "Open" || text("Lid") == "100") && text("Dynamic range").startsWith("100");
            backSeen = "lid " + text("Lid") + ", dynamics " + text("Dynamic range");
            set(*p, "Lid", 0.0f);
            restart(*p);
            render(*p, 512, {}, { 512 });
            const auto lidOnly = render(*p, total, phrase, { 512 });
            set(*p, "Lid", 100.0f);
            restart(*p);
            render(*p, 512, {}, { 512 });
            const auto open = render(*p, total, phrase, { 512 });
            quieter = db(energy(open) / std::max(energy(lidOnly), 1e-30));
            (void) closed;
        }
        check(programs.size() >= 10 && nightOk && backOk && quieter > 1.0,
              juce::String(programs.size()) + " programs (the application's factory presets: " + programs.joinIntoString(", ")
                  + "); Night practice sets the lid closed and the dynamics at 60 %" + (nightOk ? "" : " (NOT so: " + nightSeen + ")")
                  + ", As measured puts them back" + (backOk ? "" : " (NOT so: " + backSeen + ")") + "; the lid closed: the phrase " + juce::String(quieter, 1) + " dB quieter (expected > 1)");
    }

    p->releaseResources();
    return failures ? 1 : 0;
}
