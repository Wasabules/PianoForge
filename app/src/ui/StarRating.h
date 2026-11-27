// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pf::app {

/// A rating of 0 to 5 stars: clicked to set (the same star again: none). Drawn, not a font's glyph.
class StarRating final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void(int)> onChange;

    void setRating(int r, bool isMixed = false)
    {
        rating = juce::jlimit(0, 5, r);
        mixed = isMixed;
        repaint();
    }
    int getRating() const noexcept { return rating; }

    /// Five stars in `area`, `rating` of them filled.
    static void draw(juce::Graphics& g, juce::Rectangle<float> area, int rating, bool dim = false)
    {
        const float size = std::min(area.getHeight(), area.getWidth() / 5.0f);
        for (int i = 0; i < 5; ++i) {
            const auto c = juce::Rectangle<float>(size, size).withPosition(area.getX() + size * (float) i, area.getCentreY() - size / 2.0f).getCentre();
            juce::Path star;
            star.addStar(c, 5, size * 0.2f, size * 0.46f, 0.0f);
            if (i < rating) {
                g.setColour(dim ? theme::dim : theme::busy);
                g.fillPath(star);
            } else {
                g.setColour(theme::dim.withAlpha(0.6f));
                g.strokePath(star, juce::PathStrokeType(1.0f));
            }
        }
    }

    void paint(juce::Graphics& g) override { draw(g, getLocalBounds().toFloat().reduced(1.0f), rating, mixed); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const float size = std::min((float) getHeight(), getWidth() / 5.0f);
        const int star = juce::jlimit(1, 5, (int) (e.position.x / size) + 1);
        setRating(star == rating && !mixed ? 0 : star);
        if (onChange)
            onChange(rating);
    }

private:
    int rating = 0;
    bool mixed = false; // several items, several ratings
};

} // namespace pf::app
