#pragma once

#include <pianoforge/piano_gestures.hpp>

#include <juce_audio_utils/juce_audio_utils.h>

#include <vector>

namespace pf::app {

/// The on-screen keyboard, showing the piano gestures (Keyboard > Piano gestures) on its keys: a dot under each key
/// of a gesture of keys struck together, two under a key struck twice. The mouse over such a key lights the other
/// keys of its gestures, and a tooltip says what they do.
class GestureKeyboard final : public juce::MidiKeyboardComponent, public juce::TooltipClient {
public:
    struct Entry {
        pf::PianoGesture gesture;
        juce::String action;
    };

    explicit GestureKeyboard(juce::MidiKeyboardState& state)
        : juce::MidiKeyboardComponent(state, juce::KeyboardComponentBase::horizontalKeyboard)
    {
    }

    /// The gestures to show (the actions' names with them).
    void setGestures(std::vector<Entry> list)
    {
        entries = std::move(list);
        repaint();
    }
    bool hasGestures() const noexcept { return !entries.empty(); }

    juce::String getTooltip() override
    {
        if (hovered < 0)
            return {};
        juce::StringArray lines;
        for (const auto& e : entries)
            if (std::binary_search(e.gesture.keys.begin(), e.gesture.keys.end(), hovered))
                lines.add(e.action + ": " + juce::String(e.gesture.describe()));
        if (lines.isEmpty())
            return {};
        return juce::String(pf::PianoGesture::keyName(hovered)) + ", piano gesture" + (lines.size() > 1 ? "s" : "") + "\n"
             + lines.joinIntoString("\n") + "\n(Keyboard > Piano gestures)";
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        juce::MidiKeyboardComponent::mouseMove(e);
        setHovered(getNoteAndVelocityAtPosition(e.position).note);
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        juce::MidiKeyboardComponent::mouseDrag(e);
        setHovered(getNoteAndVelocityAtPosition(e.position).note);
    }
    void mouseExit(const juce::MouseEvent& e) override
    {
        juce::MidiKeyboardComponent::mouseExit(e);
        setHovered(-1);
    }

private:
    void drawWhiteNote(int note, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
                       juce::Colour lineColour, juce::Colour textColour) override
    {
        juce::MidiKeyboardComponent::drawWhiteNote(note, g, area, isDown, isOver, lineColour, textColour);
        decorate(note, g, area, false);
    }
    void drawBlackNote(int note, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
                       juce::Colour fill) override
    {
        juce::MidiKeyboardComponent::drawBlackNote(note, g, area, isDown, isOver, fill);
        decorate(note, g, area, true);
    }

    /// The key's marks: lit with the hovered key's gestures, and a dot (two for a double tap) near its front.
    void decorate(int note, juce::Graphics& g, juce::Rectangle<float> area, bool black) const
    {
        bool member = false, twice = false, lit = false;
        for (const auto& e : entries) {
            if (!std::binary_search(e.gesture.keys.begin(), e.gesture.keys.end(), note))
                continue;
            member = true;
            twice = twice || e.gesture.kind == pf::PianoGesture::Kind::doubleTap;
            lit = lit || (hovered >= 0 && std::binary_search(e.gesture.keys.begin(), e.gesture.keys.end(), hovered));
        }
        if (!member)
            return;
        const juce::Colour accent { 0xff4fa3d9 };
        if (lit) {
            g.setColour(accent.withAlpha(black ? 0.7f : 0.35f));
            g.fillRect(area.reduced(1.0f, 0.0f));
        }
        const float r = juce::jlimit(2.0f, 4.0f, area.getWidth() * 0.16f);
        const float y = area.getBottom() - (black ? 3.0f : 5.0f) * r;
        g.setColour(black ? accent.brighter(0.4f) : accent.darker(0.1f));
        if (twice) {
            g.fillEllipse(area.getCentreX() - 2.4f * r, y - r, 2.0f * r, 2.0f * r);
            g.fillEllipse(area.getCentreX() + 0.4f * r, y - r, 2.0f * r, 2.0f * r);
        } else {
            g.fillEllipse(area.getCentreX() - r, y - r, 2.0f * r, 2.0f * r);
        }
    }

    void setHovered(int note)
    {
        if (note == hovered)
            return;
        auto involved = [&](int key) {
            for (const auto& e : entries)
                if (std::binary_search(e.gesture.keys.begin(), e.gesture.keys.end(), key))
                    return true;
            return false;
        };
        const bool repaintNeeded = (hovered >= 0 && involved(hovered)) || (note >= 0 && involved(note));
        hovered = note;
        if (repaintNeeded)
            repaint();
    }

    std::vector<Entry> entries;
    int hovered = -1;
};

} // namespace pf::app
