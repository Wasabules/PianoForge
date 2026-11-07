#pragma once

#include "PluginProcessor.h"

#include "../../app/src/ui/Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <memory>

namespace pf::plugin {

namespace theme = pf::app::theme;

class CurveGraph;

/// The plugin's window, in the application's look: the profile (where it comes from, choose another), the sound
/// (volume, what the engine models), the room and the listening position (presets, speakers or headphones), the
/// tuning, the velocity curve (drawn, or the application's), the limiter; voices, engine rate and latency below.
class Editor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit Editor(Processor&);
    ~Editor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Row {
        juce::Label label;
        juce::Slider slider;
        std::unique_ptr<SliderAttachment> attachment;
    };
    struct Toggle {
        juce::ToggleButton button;
        std::unique_ptr<ButtonAttachment> attachment;
    };
    struct Choice {
        juce::Label label;
        juce::ComboBox box;
        std::unique_ptr<ComboAttachment> attachment;
    };

    void timerCallback() override;
    void setupRow(Row&, const char* id, const juce::String& name, const juce::String& tooltip);
    void setupToggle(Toggle&, const char* id, const juce::String& tooltip);
    void setupChoice(Choice&, const char* id, const juce::String& name, const juce::String& tooltip);
    void setParameter(const char* id, float value);
    float parameter(const char* id) const;
    pf::Room::Settings currentRoom() const;
    void chooseRoomPreset();
    void choosePosition();
    void refreshPresets();
    void refreshCurveControls();
    void layoutRow(Row&, juce::Rectangle<int>& area);

    Processor& processor;
    theme::LookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::Drawable> logo;

    juce::Label title, profileLine, roomSummary, footer;
    juce::TextButton chooseProfile { "Choose profile..." }, applicationProfile { "Application's profile" };

    Row volume, sympatheticLevel, unison, absorption;
    std::array<Toggle, 6> options; // body, residual, sympathetic, held keys, pedal catch, release noise
    Toggle roomOn, keepLoudness, limiterOn;
    juce::ComboBox roomPreset, position;
    juce::Label roomPresetLabel, positionLabel;
    Row roomSize, roomDecay, roomBrightness, roomWidth, roomLevel, distance;
    Choice heardOn, temperament, root;
    Row a4;

    std::unique_ptr<CurveGraph> curveGraph;
    juce::Slider softest, hardest;
    juce::Label softestLabel, hardestLabel;
    juce::TextButton linearCurve { "Linear" }, appCurve { "Application's curve" };

    float meter = 0.0f; // the output's peak, dBFS, falling slowly
    juce::Rectangle<int> header, soundCard, outputCard, roomCard, positionCard, tuningCard, curveCard, meterArea;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Editor)
};

} // namespace pf::plugin
