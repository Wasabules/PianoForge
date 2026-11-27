// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pf::app {

/// Every setting in one window: the preset bar on top (the factory presets and the user's, saved, reverted,
/// deleted; marked when the sound no longer is the preset's), the sections on the left, the chosen section on the
/// right (scrolled when taller than the window). The sections are the application's panels, owned elsewhere.
class SettingsPanel final : public juce::Component, private juce::Timer {
public:
    enum class Section { engine, tone, room, audio, recording, gestures, streamLook, desktop, count };
    static juce::String title(Section s);

    /// What the preset bar asks of the application.
    struct Presets {
        std::function<juce::StringArray()> factoryNames, userNames;
        std::function<juce::String(int factoryIndex)> factoryDescription;
        std::function<juce::String()> current; // the preset last applied or saved ("": none)
        std::function<bool()> currentIsFactory;
        std::function<bool()> modified; // the settings no longer are the preset's
        std::function<void(int index)> applyFactory, applyUser;
        std::function<void()> save, saveAs, revert, remove;
    };

    /// `panels` in Section order; `fills` those that scroll on their own (given the whole area, not their height).
    SettingsPanel(std::array<juce::Component*, (size_t) Section::count> panels, std::array<bool, (size_t) Section::count> fills,
                  Presets presets);
    ~SettingsPanel() override;

    void show(Section s);
    Section section() const noexcept { return current; }
    /// The preset list and its state again (a preset saved, deleted or applied from elsewhere).
    void refreshPresets();

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void layoutSection();

    std::array<juce::Component*, (size_t) Section::count> panels;
    std::array<bool, (size_t) Section::count> fills;
    std::array<int, (size_t) Section::count> naturalHeight {};
    Presets presets;
    Section current = Section::audio;

    juce::Label presetLabel { {}, "Preset" }, state, description;
    juce::ComboBox presetList;
    juce::TextButton revert { "Revert" }, save { "Save" }, saveAs { "Save as..." }, remove { "Delete" };
    std::array<juce::TextButton, (size_t) Section::count> sectionButtons;
    juce::Viewport viewport;
    juce::Rectangle<int> barArea, sideArea;
    bool lastModified = false;
};

} // namespace pf::app
