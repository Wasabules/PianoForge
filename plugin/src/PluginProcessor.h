#pragma once

#include "EngineRunner.h"

#include <pianoforge/modal_profile.hpp>
#include <pianoforge/velocity_curve.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pf::plugin {

/// Parameter identifiers (automatable; their names and defaults are the application's).
namespace param {
inline constexpr const char* volume = "volume";
inline constexpr const char* body = "body";
inline constexpr const char* residual = "residual";
inline constexpr const char* sympathetic = "sympathetic";
inline constexpr const char* heldKeys = "heldKeys";
inline constexpr const char* pedalCatch = "pedalCatch";
inline constexpr const char* releaseNoise = "releaseNoise";
inline constexpr const char* sympatheticLevel = "sympatheticLevel";
inline constexpr const char* unison = "unison";
inline constexpr const char* restrike = "restrike";
inline constexpr const char* room = "room";
inline constexpr const char* roomSize = "roomSize";
inline constexpr const char* roomDecay = "roomDecay";
inline constexpr const char* roomBrightness = "roomBrightness";
inline constexpr const char* roomWidth = "roomWidth";
inline constexpr const char* roomLevel = "roomLevel";
inline constexpr const char* distance = "distance";
inline constexpr const char* keepLoudness = "keepLoudness";
inline constexpr const char* heardOn = "heardOn";
inline constexpr const char* a4 = "a4";
inline constexpr const char* temperament = "temperament";
inline constexpr const char* root = "root";
inline constexpr const char* limiter = "limiter";
} // namespace param

/// PianoForge as an instrument plugin (VST3): the application's modal engine, room, headphones and limiter, played
/// from the host's MIDI (notes, CC64 continuous sustain, CC66 sostenuto, CC67 soft pedal), with the same settings as
/// automatable parameters. The modal profile is never inside the plugin: it is the one of the application's corpus
/// (its settings say where), or a modal-profile.bin chosen in the plugin and kept in the project. It loads in the
/// background, once per process for every instance (they share it).
class Processor final : public juce::AudioProcessor {
public:
    Processor();
    ~Processor() override;

    // AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override { resetRequested.store(true); } // done by the audio thread at its next block
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "PianoForge"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState parameters;

    // The modal profile (message thread).
    struct ProfileStatus {
        juce::String text;        // what to show
        bool loaded = false;
        bool chosen = false;      // a file chosen in the plugin, not the application's
        juce::String path;
    };
    ProfileStatus profileStatus() const;
    /// A modal-profile.bin chosen in the plugin, kept in the project; an empty file: the application's again.
    void chooseProfile(const juce::File& file);
    /// The profile of the application's corpus (its settings), or a non-existent file.
    static juce::File applicationProfile();

    // The velocity curve (message thread): applied to every note-on received.
    pf::VelocityCurve::Settings curveSettings() const { return curve; }
    void setCurve(const pf::VelocityCurve::Settings& s);
    /// The application's curve (Keyboard > Velocity curve), if it has one.
    static bool applicationCurve(pf::VelocityCurve::Settings& out);

    // For the editor, from any thread.
    std::atomic<int> lastVelocityIn { 0 }, lastVelocityOut { 0 };
    int activeVoices() const noexcept { return runner.engine.activeVoices.load(); }
    float takePeak() noexcept { return runner.engine.outputPeak.exchange(0.0f); }
    int rateFactor() const noexcept { return runner.rateFactor(); }
    double engineRate() const noexcept { return runner.engineRate(); }
    pf::Room::Settings roomSettings() const noexcept { return runner.room.settings(); }

    // The room presets and the listening positions' names shown in the editor (kept in the project).
    juce::String roomPresetName, positionName;

private:
    void loadProfile(const juce::File& file);
    void installProfile(std::shared_ptr<const pf::ModalProfile> profile);
    void setStatus(const juce::String& text, bool loaded);
    void applyParameters() noexcept;

    EngineRunner runner;
    pf::VelocityCurve curveTable;
    pf::VelocityCurve::Settings curve;

    // Lifecycle: prepare / release and a profile's installation take this lock; the audio thread never does. While
    // a profile installs, the audio thread is kept out (suspended: silence) and the installer waits for it to leave.
    std::mutex lifecycle;
    std::atomic<bool> suspended { false }, inProcess { false }, resetRequested { false };
    bool prepared = false;

    std::mutex loaderLock;
    std::thread loader;
    juce::File chosenProfile;   // explicit choice; empty: the application's
    juce::File loadingProfile;  // what was asked for last

    mutable std::mutex statusLock;
    ProfileStatus status;

    std::vector<pf::Event> events, chunk; // a block's events (allocated in the constructor)

    // The parameters, read on the audio thread every block.
    struct Raw {
        std::atomic<float>* p = nullptr;
        float get() const noexcept { return p->load(std::memory_order_relaxed); }
        bool on() const noexcept { return get() >= 0.5f; }
    };
    Raw volume, body, residual, sympathetic, heldKeys, pedalCatch, releaseNoise, sympatheticLevel, unison, restrike, room,
        roomSize, roomDecay, roomBrightness, roomWidth, roomLevel, distance, keepLoudness, heardOn, a4, temperament, root, limiter;
    float appliedA4 = -1.0f;
    int appliedTemperament = -1, appliedRoot = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};

} // namespace pf::plugin
