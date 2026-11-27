// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace pf::app {

/// A secondary window around a panel the main component owns: closing it hides it, the panel lives on.
class ToolWindow final : public juce::DocumentWindow {
public:
    ToolWindow(const juce::String& title, juce::Component& content, juce::Component& owner)
        : juce::DocumentWindow(title, theme::background, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content, true);
        setResizable(true, false);
        centreAroundComponent(&owner, getWidth(), getHeight());
    }

    void show()
    {
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override { setVisible(false); }
};

} // namespace pf::app
