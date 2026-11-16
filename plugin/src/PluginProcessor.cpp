#include "PluginProcessor.h"

#include "PluginEditor.h"

#include "../../app/src/CurveText.h"
#include "../../app/src/Presets.h"
#include "../../app/src/RoomPresets.h"
#include "../../app/src/Tuning.h"

#include <chrono>
#include <filesystem>
#include <map>

namespace pf::plugin {

namespace {

juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> list;
    const auto db = [](float v, int) { return String(v, 1) + " dB"; };
    const auto percent = [](float v, int) { return String(roundToInt(v)) + " %"; };
    const auto fraction = [](float v, int) { return String(roundToInt(100.0f * v)) + " %"; };
    const auto seconds = [](float v, int) { return String(v, v < 1.0f ? 3 : 2) + " s"; };
    const auto addFloat = [&](const char* id, const char* name, NormalisableRange<float> range, float def, const char* label,
                              std::function<String(float, int)> text) {
        list.push_back(std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name, range, def,
                                                             AudioParameterFloatAttributes().withLabel(label).withStringFromValueFunction(text)));
    };
    const auto addBool = [&](const char* id, const char* name, bool def) {
        list.push_back(std::make_unique<AudioParameterBool>(ParameterID { id, 1 }, name, def));
    };
    const auto skewed = [](float lo, float hi, float step, float centre) {
        NormalisableRange<float> r(lo, hi, step);
        r.setSkewForCentre(centre);
        return r;
    };

    addFloat(param::volume, "Volume", { -24.0f, 24.0f, 0.1f }, 6.0f, "dB", db);
    addBool(param::body, "Soundboard body", true);
    addBool(param::residual, "Attack residual", true);
    addBool(param::sympathetic, "Sympathetic resonance", true);
    addBool(param::heldKeys, "Held keys resonate", true);
    addBool(param::pedalCatch, "Pedal catches releases", true);
    addBool(param::releaseNoise, "Release noise", true);
    addFloat(param::sympatheticLevel, "Sympathetic level", { -24.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::unison, "Unison tuning", { 0.0f, 300.0f, 1.0f }, 100.0f, "%", percent);
    addFloat(param::absorption, "Re-strike absorption", { 0.0f, 95.0f, 1.0f }, 100.0f * pf::kDefaultRestrikeAbsorption, "%", percent);
    addBool(param::room, "Room", true);
    const auto& hall = app::kRoomPresets[(size_t) app::kDefaultRoomPreset];
    addFloat(param::roomSize, "Room volume", { 0.0f, 1.0f, 0.005f }, hall.size, "m3", [](float v, int) {
        return String(roundToInt(60.0 * std::pow(40000.0 / 60.0, (double) v))) + String::fromUTF8(" m\xc2\xb3");
    });
    addFloat(param::roomDecay, "Room decay", skewed(0.3f, 8.0f, 0.05f, 2.0f), hall.decay, "s", seconds);
    addFloat(param::roomBrightness, "Room brightness", { 0.0f, 1.0f, 0.01f }, hall.brightness, "%", fraction);
    addFloat(param::roomWidth, "Room width", { 0.0f, 1.0f, 0.01f }, hall.width, "%", fraction);
    addFloat(param::roomLevel, "Room level", { -20.0f, 20.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::distance, "Distance", skewed(1.0f, 30.0f, 0.1f, 6.0f),
             app::kListeningPositions[(size_t) app::kDefaultPosition].distance, "m", [](float v, int) { return String(v, 1) + " m"; });
    addBool(param::keepLoudness, "Keep loudness", true);
    list.push_back(std::make_unique<AudioParameterChoice>(ParameterID { param::heardOn, 1 }, "Heard on",
                                                          StringArray { "Speakers", "Headphones (binaural)" }, 0));
    addFloat(param::a4, "A4", { 415.0f, 466.0f, 0.1f }, 440.0f, "Hz", [](float v, int) { return String(v, 1) + " Hz"; });
    StringArray temperaments;
    for (const auto& t : app::kTemperaments)
        temperaments.add(t.name);
    list.push_back(std::make_unique<AudioParameterChoice>(ParameterID { param::temperament, 1 }, "Temperament", temperaments, 0));
    list.push_back(std::make_unique<AudioParameterChoice>(ParameterID { param::root, 1 }, "Temperament root",
                                                          StringArray { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" }, 0));
    addBool(param::limiter, "Output limiter", true);

    // The hammers' voicing, the touch and the lid, the output's tone (the application's Engine and Tone sections).
    const auto partial = [](float v, int) { return "partial " + String(v, 1); };
    const auto times = [](float v, int) { return String(v, 2) + " x"; };
    const auto ms = [](float v, int) { return String(roundToInt(v)) + " ms"; };
    addFloat(param::voicingBass, "Voicing bass", { -18.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::voicingMiddle, "Voicing middle", { -18.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::voicingTreble, "Voicing treble", { -18.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::voicingFollow, "Voicing follows velocity", { 0.0f, 100.0f, 1.0f }, 100.0f * pf::ModalEngine::kDefaultVoicingVelocity,
             "%", percent);
    addFloat(param::voicingCorner, "Voicing from partial", { 2.0f, 10.0f, 0.5f }, pf::ModalEngine::kDefaultVoicingCorner, "", partial);
    addFloat(param::dynamicRange, "Dynamic range", { 25.0f, 150.0f, 1.0f }, 100.0f, "%", percent);
    addFloat(param::sustainLength, "Sustain length", skewed(0.25f, 4.0f, 0.01f, 1.0f), 1.0f, "x", times);
    addFloat(param::damperSpeed, "Damper speed", skewed(0.2f, 5.0f, 0.01f, 1.0f), 1.0f, "x", times);
    addFloat(param::attackSoft, "Attack", { -24.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::attackLength, "Attack length", skewed(5.0f, 300.0f, 1.0f, 40.0f), pf::ModalEngine::kDefaultAttackSoftMs, "ms", ms);
    list.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID { param::lid, 1 }, "Lid", NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f,
        AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float v, int) {
                return v >= 99.5f ? String("Open") : std::abs(v - 50.0f) < 0.5f ? String("Half stick") : v <= 0.5f ? String("Closed")
                                                                                                           : String(roundToInt(v)) + " % open";
            })
            .withValueFromStringFunction([](const String& t) {
                if (t.containsIgnoreCase("half"))
                    return 50.0f;
                if (t.containsIgnoreCase("closed"))
                    return 0.0f;
                if (t.containsIgnoreCase("open") && !t.containsAnyOf("0123456789"))
                    return 100.0f;
                return t.getFloatValue();
            })));
    addFloat(param::toneBass, "Bass", { -12.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::tonePresence, "Presence", { -12.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    addFloat(param::toneAir, "Air", { -12.0f, 12.0f, 0.5f }, 0.0f, "dB", db);
    return { list.begin(), list.end() };
}

/// One profile in memory per file (and per version of it) for every instance in the process.
std::shared_ptr<const pf::ModalProfile> sharedProfile(const juce::File& file, std::string& error)
{
    static std::mutex lock;
    static std::map<juce::String, std::weak_ptr<const pf::ModalProfile>> cache;
    const auto key = file.getFullPathName() + "|" + juce::String(file.getLastModificationTime().toMilliseconds()) + "|"
                   + juce::String(file.getSize());
    std::lock_guard<std::mutex> guard(lock); // a second instance asking for it meanwhile waits, then shares it
    if (const auto it = cache.find(key); it != cache.end())
        if (auto profile = it->second.lock())
            return profile;
#if JUCE_WINDOWS
    const std::filesystem::path path(file.getFullPathName().toWideCharPointer());
#else
    const std::filesystem::path path(file.getFullPathName().toStdString());
#endif
    auto profile = pf::loadModalProfile(path, error);
    if (profile != nullptr)
        cache[key] = profile;
    return profile;
}

juce::PropertiesFile::Options applicationSettings()
{
    juce::PropertiesFile::Options o; // as the application stores them
    o.applicationName = "PianoForge";
    o.folderName = "PianoForge";
    o.filenameSuffix = ".settings";
    return o;
}

} // namespace

Processor::Processor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PianoForge", parameterLayout())
{
    const auto raw = [this](const char* id) { return Raw { parameters.getRawParameterValue(id) }; };
    volume = raw(param::volume);
    body = raw(param::body);
    residual = raw(param::residual);
    sympathetic = raw(param::sympathetic);
    heldKeys = raw(param::heldKeys);
    pedalCatch = raw(param::pedalCatch);
    releaseNoise = raw(param::releaseNoise);
    sympatheticLevel = raw(param::sympatheticLevel);
    unison = raw(param::unison);
    absorption = raw(param::absorption);
    room = raw(param::room);
    roomSize = raw(param::roomSize);
    roomDecay = raw(param::roomDecay);
    roomBrightness = raw(param::roomBrightness);
    roomWidth = raw(param::roomWidth);
    roomLevel = raw(param::roomLevel);
    distance = raw(param::distance);
    keepLoudness = raw(param::keepLoudness);
    heardOn = raw(param::heardOn);
    a4 = raw(param::a4);
    temperament = raw(param::temperament);
    root = raw(param::root);
    limiter = raw(param::limiter);
    voicingBass = raw(param::voicingBass);
    voicingMiddle = raw(param::voicingMiddle);
    voicingTreble = raw(param::voicingTreble);
    voicingFollow = raw(param::voicingFollow);
    voicingCorner = raw(param::voicingCorner);
    dynamicRange = raw(param::dynamicRange);
    sustainLength = raw(param::sustainLength);
    damperSpeed = raw(param::damperSpeed);
    attackSoft = raw(param::attackSoft);
    attackLength = raw(param::attackLength);
    lid = raw(param::lid);
    toneBass = raw(param::toneBass);
    tonePresence = raw(param::tonePresence);
    toneAir = raw(param::toneAir);
    roomPresetName = app::kRoomPresets[(size_t) app::kDefaultRoomPreset].name;
    positionName = app::kListeningPositions[(size_t) app::kDefaultPosition].name;
    events.resize(8192);
    chunk.resize(EngineRunner::kMaxEvents);
    curveTable.set(curve);
    loadProfile(applicationProfile());
}

Processor::~Processor()
{
    std::lock_guard<std::mutex> guard(loaderLock);
    if (loader.joinable())
        loader.join();
}

// ---- The profile ----

juce::File Processor::applicationProfile()
{
    const auto options = applicationSettings();
    const auto file = options.getDefaultFile();
    if (!file.existsAsFile())
        return {};
    juce::PropertiesFile settings(file, options); // read only: nothing changes, nothing is saved
    const auto corpus = settings.getValue("corpus");
    if (!juce::File::isAbsolutePath(corpus))
        return {};
    // forge export writes the profile in the corpus state folder, next to the passes (as the application reads it).
    return juce::File(corpus).getParentDirectory().getChildFile("modal-profile.bin");
}

bool Processor::applicationCurve(pf::VelocityCurve::Settings& out)
{
    const auto options = applicationSettings();
    const auto file = options.getDefaultFile();
    if (!file.existsAsFile())
        return false;
    juce::PropertiesFile settings(file, options);
    const auto text = settings.getValue("velocityCurve");
    if (text.isEmpty())
        return false;
    out = app::parseCurveText(text);
    return true;
}

Processor::ProfileStatus Processor::profileStatus() const
{
    std::lock_guard<std::mutex> guard(statusLock);
    return status;
}

void Processor::setStatus(const juce::String& text, bool loaded)
{
    std::lock_guard<std::mutex> guard(statusLock);
    status.text = text;
    status.loaded = loaded;
}

void Processor::chooseProfile(const juce::File& file)
{
    chosenProfile = file;
    loadProfile(file.existsAsFile() ? file : applicationProfile());
}

void Processor::loadProfile(const juce::File& file)
{
    std::lock_guard<std::mutex> guard(loaderLock);
    if (loader.joinable())
        loader.join();
    {
        std::lock_guard<std::mutex> statusGuard(statusLock);
        status.chosen = chosenProfile != juce::File();
        if (status.loaded && file == loadingProfile && file.existsAsFile())
            return; // already playing that one
        status.path = file.getFullPathName();
    }
    loadingProfile = file;
    if (!file.existsAsFile()) {
        setStatus(chosenProfile != juce::File()
                      ? "The profile chosen for this project is missing (" + chosenProfile.getFullPathName() + "), and the application has none"
                      : juce::String("No modal profile: set up the PianoForge application with its corpus, or choose a "
                                     "modal-profile.bin (written by forge export)"),
                  false);
        return;
    }
    setStatus("Loading " + file.getFullPathName() + "...", false);
    loader = std::thread([this, file] {
        std::string error;
        auto profile = sharedProfile(file, error);
        if (profile == nullptr) {
            setStatus("The profile did not load: " + juce::String(error) + " (" + file.getFullPathName() + ")", false);
            return;
        }
        std::size_t layers = 0;
        for (const auto& n : profile->notes)
            layers = std::max(layers, n.layers.size());
        const auto summary = juce::String((int) profile->notes.size()) + " notes x " + juce::String((int) layers) + " layers, "
                           + juce::String(juce::roundToInt((double) profile->bytes / 1.0e6)) + " MB";
        installProfile(std::move(profile));
        setStatus(summary, true);
    });
}

void Processor::installProfile(std::shared_ptr<const pf::ModalProfile> profile)
{
    std::lock_guard<std::mutex> guard(lifecycle);
    // Keep the audio thread out (Dekker: it raises inProcess, then reads suspended; this the other way round).
    suspended.store(true);
    while (inProcess.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    runner.install(std::move(profile));
    suspended.store(false);
}

// ---- The velocity curve ----

void Processor::setCurve(const pf::VelocityCurve::Settings& s)
{
    curve = s;
    curveTable.set(s);
}

// ---- Audio ----

void Processor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    std::lock_guard<std::mutex> guard(lifecycle);
    runner.prepare(sampleRate, samplesPerBlock);
    setLatencySamples(runner.latencySamples());
    appliedA4 = -1.0f; // the tuning applied again at the first block
    prepared = true;
}

void Processor::releaseResources()
{
    std::lock_guard<std::mutex> guard(lifecycle);
    prepared = false;
    runner.engine.release();
}

bool Processor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet().isDisabled() && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

double Processor::getTailLengthSeconds() const
{
    return runner.room.enabled.load() ? runner.room.tailSeconds() : 0.0;
}

void Processor::applyParameters() noexcept
{
    auto& e = runner.engine;
    e.outputGain = juce::Decibels::decibelsToGain(volume.get());
    e.bodyEnabled = body.on();
    e.residualEnabled = residual.on();
    e.sympatheticEnabled = sympathetic.on();
    e.heldKeysResonate = heldKeys.on();
    e.pedalCatch = pedalCatch.on();
    e.releaseNoiseEnabled = releaseNoise.on();
    e.sympatheticGain = juce::Decibels::decibelsToGain(sympatheticLevel.get());
    e.unisonSpread = unison.get() / 100.0f;
    e.restrikeAbsorption = absorption.get() / 100.0f;
    e.voicingBassDb = voicingBass.get();
    e.voicingMiddleDb = voicingMiddle.get();
    e.voicingTrebleDb = voicingTreble.get();
    e.voicingVelocity = voicingFollow.get() / 100.0f;
    e.voicingCorner = voicingCorner.get();
    e.dynamicRange = dynamicRange.get() / 100.0f;
    e.sustainLength = sustainLength.get();
    e.damperRate = damperSpeed.get();
    e.attackSoftDb = attackSoft.get();
    e.attackSoftMs = attackLength.get();
    e.lidOpen = lid.get() / 100.0f;
    runner.toneBassDb = toneBass.get();
    runner.tonePresenceDb = tonePresence.get();
    runner.toneAirDb = toneAir.get();
    pf::Room::Settings r;
    r.enabled = room.on();
    r.size = roomSize.get();
    r.decaySeconds = roomDecay.get();
    r.brightness = roomBrightness.get();
    r.width = roomWidth.get();
    r.levelDb = roomLevel.get();
    r.distance = distance.get();
    r.keepLoudness = keepLoudness.on();
    r.headphones = juce::roundToInt(heardOn.get()) == 1;
    runner.room.applySettings(r); // the room only reconfigures what changed
    runner.limiter.enabled = limiter.on();
    const int t = juce::jlimit(0, (int) app::kTemperaments.size() - 1, juce::roundToInt(temperament.get()));
    const int pc = juce::jlimit(0, 11, juce::roundToInt(root.get()));
    if (a4.get() != appliedA4 || t != appliedTemperament || pc != appliedRoot) {
        appliedA4 = a4.get();
        appliedTemperament = t;
        appliedRoot = pc;
        e.referenceA4 = appliedA4;
        const auto offsets = app::pitchClassOffsets(app::kTemperaments[(size_t) t], pc);
        for (int i = 0; i < 12; ++i)
            e.pitchClassCents[(size_t) i] = (float) offsets[(size_t) i];
        e.tuningVersion.fetch_add(1); // the free strings follow
    }
}

void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    for (int c = 2; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    inProcess.store(true);
    if (suspended.load() || !prepared || buffer.getNumChannels() < 2 || n <= 0) {
        buffer.clear();
        inProcess.store(false);
        return;
    }
    applyParameters();
    if (resetRequested.exchange(false))
        runner.reset();

    // The block's MIDI, at its samples; note-on velocities through the curve.
    int count = 0;
    for (const auto meta : midi) {
        if (count == (int) events.size())
            break;
        if (meta.numBytes < 1 || meta.numBytes > 3)
            continue; // system exclusive: nothing for the engine
        const std::uint8_t head = meta.data[0], d1 = meta.numBytes > 1 ? meta.data[1] : 0;
        std::uint8_t d2 = meta.numBytes > 2 ? meta.data[2] : 0;
        if ((head & 0xF0) == 0x90 && d2 > 0) {
            lastVelocityIn.store(d2, std::memory_order_relaxed);
            d2 = curveTable.map(d2);
            lastVelocityOut.store(d2, std::memory_order_relaxed);
        }
        events[(size_t) count++] = { (std::uint32_t) juce::jlimit(0, n - 1, meta.samplePosition), head, d1, d2 };
    }

    // In pieces no longer than the runner was prepared for, each with at most its number of events.
    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);
    int next = 0;
    for (int pos = 0; pos < n;) {
        int end = std::min(n, pos + runner.maxBlock());
        int used = 0;
        while (next < count && (int) events[(size_t) next].offset < end) {
            if (used == EngineRunner::kMaxEvents) {
                if ((int) events[(size_t) next].offset > pos)
                    end = (int) events[(size_t) next].offset; // the rest from there, in the next piece
                else
                    ++next; // more than kMaxEvents on one sample: the extra ones are dropped
                continue;
            }
            auto e = events[(size_t) next++];
            e.offset -= (std::uint32_t) pos;
            chunk[(size_t) used++] = e;
        }
        runner.process(left + pos, right + pos, end - pos, chunk.data(), used);
        pos = end;
    }
    inProcess.store(false);
}

// ---- State ----

void Processor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty("stateVersion", 1, nullptr);
    state.setProperty("profile", chosenProfile.getFullPathName(), nullptr);
    state.setProperty("velocityCurve", app::formatCurveText(curve), nullptr);
    state.setProperty("roomPreset", roomPresetName, nullptr);
    state.setProperty("position", positionName, nullptr);
    state.setProperty("program", currentProgram, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void Processor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(parameters.state.getType()))
        return;
    const auto tree = juce::ValueTree::fromXml(*xml);
    parameters.replaceState(tree);
    setCurve(app::parseCurveText(tree.getProperty("velocityCurve").toString()));
    roomPresetName = tree.getProperty("roomPreset", roomPresetName).toString();
    positionName = tree.getProperty("position", positionName).toString();
    currentProgram = juce::jlimit(0, getNumPrograms() - 1, (int) tree.getProperty("program", 0));
    const auto path = tree.getProperty("profile").toString();
    chooseProfile(juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File());
}

// ---- Programs: the application's factory presets ----

int Processor::getNumPrograms()
{
    return (int) app::factoryPresets().size();
}

const juce::String Processor::getProgramName(int index)
{
    const auto& list = app::factoryPresets();
    return index >= 0 && index < (int) list.size() ? juce::String(list[(size_t) index].name) : juce::String();
}

void Processor::setCurrentProgram(int index)
{
    if (index != currentProgram)
        applyProgram(index);
}

void Processor::applyProgram(int index)
{
    const auto& list = app::factoryPresets();
    if (index < 0 || index >= (int) list.size())
        return;
    const auto& preset = list[(size_t) index];
    auto set = [this](const char* id, float value) {
        if (auto* p = parameters.getParameter(id)) {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(value));
            p->endChangeGesture();
        }
    };
    // The application's settings as the plugin's parameters (their key, the parameter, the scale between them).
    struct Link {
        const char* key;
        const char* id;
        float scale;
    };
    static constexpr Link kLinks[] = {
        { "modalBody", param::body, 1.0f }, { "modalResidual", param::residual, 1.0f }, { "modalSympathetic", param::sympathetic, 1.0f },
        { "modalHeldKeys", param::heldKeys, 1.0f }, { "modalPedalCatch", param::pedalCatch, 1.0f },
        { "modalReleaseNoise", param::releaseNoise, 1.0f }, { "sympatheticDb", param::sympatheticLevel, 1.0f },
        { "unisonSpread", param::unison, 100.0f }, { "restrikeAbsorption", param::absorption, 100.0f },
        { "voicingBassDb", param::voicingBass, 1.0f }, { "voicingMiddleDb", param::voicingMiddle, 1.0f },
        { "voicingTrebleDb", param::voicingTreble, 1.0f }, { "voicingVelocity", param::voicingFollow, 100.0f },
        { "voicingCorner", param::voicingCorner, 1.0f }, { "dynamicRange", param::dynamicRange, 100.0f },
        { "sustainLength", param::sustainLength, 1.0f }, { "damperRate", param::damperSpeed, 1.0f },
        { "attackSoftDb", param::attackSoft, 1.0f }, { "attackSoftMs", param::attackLength, 1.0f }, { "lidOpen", param::lid, 100.0f },
        { "toneBassDb", param::toneBass, 1.0f }, { "tonePresenceDb", param::tonePresence, 1.0f }, { "toneAirDb", param::toneAir, 1.0f },
        { "roomOn", param::room, 1.0f }, { "roomKeepLoudness", param::keepLoudness, 1.0f }, { "roomLevel", param::roomLevel, 1.0f },
    };
    // The sound back to its defaults first: what the preset does not name is as measured.
    for (const auto& link : kLinks)
        if (auto* p = parameters.getParameter(link.id))
            set(link.id, p->convertFrom0to1(p->getDefaultValue()));
    int roomIndex = app::kDefaultRoomPreset;
    float metres = app::kListeningPositions[(size_t) app::kDefaultPosition].distance;
    for (const auto& [key, value] : preset.values) {
        const juce::String k(key);
        if (k == "roomPreset")
            roomIndex = juce::jlimit(0, (int) app::kRoomPresets.size() - 1, (int) value - 1);
        else if (k == "roomDistance")
            metres = (float) (double) value;
        else
            for (const auto& link : kLinks)
                if (k == link.key)
                    set(link.id, (float) (double) value * link.scale);
    }
    const auto& hall = app::kRoomPresets[(size_t) roomIndex];
    set(param::roomSize, hall.size);
    set(param::roomDecay, hall.decay);
    set(param::roomBrightness, hall.brightness);
    set(param::roomWidth, hall.width);
    set(param::distance, metres);
    roomPresetName = hall.name;
    positionName = "Custom";
    for (const auto& p : app::kListeningPositions)
        if (std::abs(p.distance - metres) < 0.05f)
            positionName = p.name;
    currentProgram = index;
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

juce::AudioProcessorEditor* Processor::createEditor()
{
    return new Editor(*this);
}

} // namespace pf::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new pf::plugin::Processor();
}
