#pragma once

#include "../Gallery.h"
#include "StarRating.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace pf::app {

/// Asked as a recording stops: what was played. The piece (the pieces played before suggested as one types), its
/// composer (filled in from an earlier recording of it), tags and a rating; the name the files will have, shown as it
/// is typed. Save, keep the automatic name (Escape, or the window closed), or discard (to the recycle bin).
class NamingDialog final : public juce::Component, private juce::ListBoxModel {
public:
    enum class Action { save, automatic, discard };
    struct Info {
        juce::Time time;
        double seconds = 0.0;
        int notes = 0;
        juce::String formats; // "wav mp3 mid"
        juce::File root;      // the Recordings folder
    };
    using Done = std::function<void(Action, const GalleryMeta&)>;

    NamingDialog(Info info, GalleryNaming naming, std::vector<GalleryItem> known, Done done);
    ~NamingDialog() override; // closed without a choice: the automatic name

    /// Opens it, modal, centred on `parent`.
    static void launch(juce::Component* parent, Info info, GalleryNaming naming, std::vector<GalleryItem> known, Done done);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return suggestions.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    void finish(Action a);
    void update();
    GalleryMeta meta() const;

    Info info;
    GalleryNaming naming;
    std::vector<GalleryItem> known;
    Done done;
    bool finished = false;
    bool composerAuto = true; // the composer field as filled in, not typed

    juce::Label heading, summary, pieceLabel, composerLabel, tagsLabel, ratingLabel, preview;
    juce::TextEditor piece, composer, tags;
    StarRating rating;
    juce::ListBox list { "Pieces", this };
    juce::StringArray suggestions;
    juce::TextButton save { "Save" }, automatic { "Keep the automatic name" }, discard { "Discard" };
};

} // namespace pf::app
