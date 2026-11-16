#pragma once

#include "../Gallery.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace pf::app {

/// How recordings are kept and named (Settings > Recording): the shortest one kept (shorter: a draft, not kept), the
/// question as one stops, the naming pattern (with a preview) and a folder per piece. Read by the application when a
/// recording stops and by the gallery when it names.
class RecordingPanel final : public juce::Component {
public:
    explicit RecordingPanel(juce::PropertiesFile& settings);

    double minimumSeconds() const { return minimum.getValue(); }
    bool askName() const { return ask.getToggleState(); }
    GalleryNaming naming() const;
    void reload();
    void saveNow() { save(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void save();
    void updatePreview();

    juce::PropertiesFile& settings;
    juce::Label minimumLabel, minimumNote, patternLabel, patternNote, preview;
    juce::Slider minimum;
    juce::ToggleButton ask { "Ask what was played when a recording stops" }, folderPerPiece { "A folder per piece" };
    juce::TextEditor pattern;
    juce::TextButton resetPattern { "Default" };
    juce::Rectangle<int> keepArea, nameArea;
};

} // namespace pf::app
