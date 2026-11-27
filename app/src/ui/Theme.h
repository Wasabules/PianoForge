// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace pf::app {

/// The application's colours and the few drawing helpers its panels share. A dark theme, one accent colour, and
/// the three load colours (comfortable, busy, overloaded) used wherever the audio load shows.
namespace theme {

inline const juce::Colour background { 0xff1b1f24 };
inline const juce::Colour panel { 0xff252b32 };
inline const juce::Colour panelEdge { 0xff323a43 };
inline const juce::Colour text { 0xffe6e9ed };
inline const juce::Colour dim { 0xff8d97a3 };
inline const juce::Colour accent { 0xff4fa3d9 };
inline const juce::Colour record { 0xffe0484b };
inline const juce::Colour good { 0xff5cbf6a };
inline const juce::Colour busy { 0xffe5a83b };
inline const juce::Colour bad { 0xffe0484b };

/// The load zones: below 50 % comfortable, 50 to 80 % busy (a spike of the system can make a block late), above
/// 80 % overloaded (dropouts likely).
inline constexpr float kBusyLoad = 0.5f, kOverloaded = 0.8f;
inline juce::Colour forLoad(float load)
{
    return load < kBusyLoad ? good : load < kOverloaded ? busy : bad;
}

inline juce::FontOptions font(float height, bool bold = false)
{
    return juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain);
}

/// A panel with a small uppercase title; returns the area inside.
inline juce::Rectangle<int> drawCard(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title)
{
    g.setColour(panel);
    g.fillRoundedRectangle(bounds.toFloat(), 6.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 6.0f, 1.0f);
    auto inner = bounds.reduced(12, 8);
    if (title.isNotEmpty()) {
        g.setColour(dim);
        g.setFont(font(11.0f, true));
        g.drawText(title.toUpperCase(), inner.removeFromTop(16), juce::Justification::centredLeft);
        inner.removeFromTop(2);
    }
    return inner;
}

/// The look of the whole application: JUCE's dark scheme with these colours.
class LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    LookAndFeel()
        : juce::LookAndFeel_V4(juce::LookAndFeel_V4::ColourScheme(background, panel, panel, panelEdge, text, accent,
                                                                    juce::Colours::white, panelEdge, text))
    {
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::DocumentWindow::backgroundColourId, background);
        setColour(juce::TextButton::buttonColourId, panel.brighter(0.08f));
        setColour(juce::TextButton::buttonOnColourId, accent);
        setColour(juce::ComboBox::backgroundColourId, panel.brighter(0.05f));
        setColour(juce::ComboBox::outlineColourId, panelEdge);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.35f));
        setColour(juce::Slider::thumbColourId, accent);
        setColour(juce::Slider::trackColourId, accent.withAlpha(0.6f));
        setColour(juce::Slider::backgroundColourId, panelEdge);
        setColour(juce::Slider::textBoxOutlineColourId, panelEdge);
        setColour(juce::Label::textColourId, text);
        setColour(juce::ToggleButton::tickColourId, accent);
        setColour(juce::GroupComponent::outlineColourId, panelEdge);
        setColour(juce::GroupComponent::textColourId, dim);
        setColour(juce::TooltipWindow::backgroundColourId, panel.brighter(0.1f));
    }

    int getMenuBarItemWidth(juce::MenuBarComponent& bar, int index, const juce::String& title) override
    {
        return juce::LookAndFeel_V4::getMenuBarItemWidth(bar, index, title) + 8;
    }
};

} // namespace theme

} // namespace pf::app
