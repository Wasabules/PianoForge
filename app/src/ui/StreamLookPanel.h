// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../StreamLook.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace pf::app {

/// The "Stream look" window: the look of the OBS plugin's keyboard, set here as in OBS (each side follows the other:
/// StreamLook), with its preview as OBS renders it. On top, the styles and the way back to the defaults; then the
/// preview; then the settings in tabs (pianoforge/stream_look.hpp's pages), each with its help as a tooltip, shown
/// only when it applies (the hues for the colour mode chosen...).
class StreamLookPanel final : public juce::Component, private juce::Timer {
public:
    explicit StreamLookPanel(StreamLook& look);
    ~StreamLookPanel() override;

    /// The controls brought to the look (it changed from OBS's side).
    void refresh();

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Preview;
    class Page;

    void timerCallback() override;
    void edited(); // a control moved: the other controls' visibility, the style shown as one's own

    StreamLook& look;
    juce::ComboBox style;
    juce::TextButton reset { "Back to the defaults" };
    juce::Label status;
    std::unique_ptr<Preview> preview;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::vector<std::unique_ptr<Page>> pages;
    int ticks = 0;
    std::uint64_t frameSerial = 0;
};

} // namespace pf::app
