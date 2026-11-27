// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace pf::app {

/// The output level: two bars (left, right) in dBFS with a peak hold, red when a sample reached full scale.
class LevelMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    /// Message thread, about 20 times a second: the peaks since the last call (linear).
    void push(float left, float right);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { clipped = false; } // click: clear the clip light

private:
    std::array<float, 2> level {}, hold {};
    std::array<int, 2> holdAge {};
    bool clipped = false;
};

/// The audio thread's load at a glance: a bar from 0 to 100 % over the three zones (comfortable, busy,
/// overloaded), the average filled, the recent peak as a mark, and the dropouts since the counters were reset.
/// A click opens the details (onClick).
class LoadMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    void set(float average, float peak, int dropouts);
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();
    }
    std::function<void()> onClick;

private:
    float average = 0.0f, peak = 0.0f;
    int dropouts = 0;
};

/// The load over the last minute: one column per 100 ms (its peak), over the zones.
class LoadGraph final : public juce::Component {
public:
    static constexpr int kColumns = 600;
    void push(float peak, bool dropout);
    void paint(juce::Graphics&) override;

private:
    std::vector<float> peaks = std::vector<float>(kColumns, 0.0f);
    std::vector<bool> dropouts = std::vector<bool>(kColumns, false);
    int next = 0, filled = 0;
};

/// A dashboard card: a title and a few lines, the first one larger.
class InfoCard final : public juce::Component {
public:
    explicit InfoCard(juce::String cardTitle) : title(std::move(cardTitle)) {}
    void setLines(const juce::StringArray& newLines, juce::Colour accentLine = theme::text);
    void paint(juce::Graphics&) override;

private:
    juce::String title;
    juce::StringArray lines;
    juce::Colour firstColour = theme::text;
};

} // namespace pf::app
