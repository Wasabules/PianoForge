#include "RecordingPanel.h"

#include "Theme.h"

namespace pf::app {

namespace {

constexpr double kDefaultMinimum = 30.0;

} // namespace

RecordingPanel::RecordingPanel(juce::PropertiesFile& s) : settings(s)
{
    minimumLabel.setText("Shortest kept", juce::dontSendNotification);
    minimum.setRange(0.0, 300.0, 1.0);
    minimum.setSkewFactorFromMidPoint(45.0);
    minimum.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    minimum.textFromValueFunction = [](double v) { return v < 0.5 ? juce::String("Keep all") : galleryClock(v); };
    minimum.valueFromTextFunction = [](const juce::String& t) {
        if (t.containsIgnoreCase("all"))
            return 0.0;
        const auto parts = juce::StringArray::fromTokens(t, ":", {});
        return parts.size() == 2 ? parts[0].getDoubleValue() * 60.0 + parts[1].getDoubleValue() : t.getDoubleValue();
    };
    minimum.setDoubleClickReturnValue(true, kDefaultMinimum);
    minimum.setTooltip("A recording stopped before this length is a draft: not kept, its files go to the recycle bin. "
                       "Keep all: every recording is kept.");
    minimum.onValueChange = [this] { save(); };
    minimumNote.setText("A recording shorter than this is a draft: it is not kept (its files go to the recycle bin). Takes "
                        "saved with Ctrl+S are always kept.",
                        juce::dontSendNotification);
    ask.setTooltip("As a recording stops, a window asks for the piece, its composer, tags and a rating; the files are "
                   "named from them (the conversions to MP3 and the others wait for the name)");
    ask.onClick = [this] { save(); };

    patternLabel.setText("Name pattern", juce::dontSendNotification);
    pattern.onTextChange = [this] {
        updatePreview();
        save();
    };
    patternNote.setText("{piece}, {composer}, {date}, {time}, {n} (its number among the piece's recordings), {tags}. An empty "
                        "one takes the separator before it away. Without a piece: the automatic name (the date and time).",
                        juce::dontSendNotification);
    folderPerPiece.setTooltip("A named recording goes in a folder named after its piece (Recordings\\Nocturne Op. 9 No. 2\\...)");
    folderPerPiece.onClick = [this] {
        updatePreview();
        save();
    };
    resetPattern.onClick = [this] { pattern.setText(GalleryNaming {}.pattern, juce::sendNotification); };
    for (auto* l : { &minimumNote, &patternNote, &preview }) {
        l->setColour(juce::Label::textColourId, theme::dim);
        l->setFont(theme::font(12.5f));
        l->setJustificationType(juce::Justification::topLeft);
    }
    preview.setColour(juce::Label::textColourId, theme::text);
    for (auto* c : std::initializer_list<juce::Component*> { &minimumLabel, &minimumNote, &patternLabel, &patternNote, &preview, &minimum,
                                                             &ask, &folderPerPiece, &pattern, &resetPattern })
        addAndMakeVisible(*c);
    reload();
    setSize(640, 400);
}

void RecordingPanel::reload()
{
    minimum.setValue(settings.getDoubleValue("recordMinSeconds", kDefaultMinimum), juce::dontSendNotification);
    minimum.updateText();
    ask.setToggleState(settings.getBoolValue("recordAskName", true), juce::dontSendNotification);
    pattern.setText(settings.getValue("namingPattern", GalleryNaming {}.pattern), juce::dontSendNotification);
    folderPerPiece.setToggleState(settings.getBoolValue("namingFolderPerPiece", true), juce::dontSendNotification);
    updatePreview();
}

GalleryNaming RecordingPanel::naming() const
{
    GalleryNaming n;
    n.pattern = pattern.getText().trim().isEmpty() ? GalleryNaming {}.pattern : pattern.getText().trim();
    n.folderPerPiece = folderPerPiece.getToggleState();
    return n;
}

void RecordingPanel::save()
{
    settings.setValue("recordMinSeconds", minimum.getValue());
    settings.setValue("recordAskName", ask.getToggleState());
    settings.setValue("namingPattern", pattern.getText().trim());
    settings.setValue("namingFolderPerPiece", folderPerPiece.getToggleState());
    settings.saveIfNeeded();
}

void RecordingPanel::updatePreview()
{
    GalleryMeta m;
    m.piece = "Nocturne in C# minor, Op. posth.";
    m.composer = "Chopin";
    m.tags = "practice";
    const auto n = naming();
    const auto now = juce::Time::getCurrentTime();
    const auto folder = galleryFolder(n, m, juce::File("C:\\Recordings"));
    preview.setText("For example: " + (n.folderPerPiece ? folder.getFileName() + juce::File::getSeparatorString() : juce::String())
                        + galleryStem(n, m, now, 3) + ".wav",
                    juce::dontSendNotification);
}

void RecordingPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    theme::drawCard(g, keepArea, "What is kept");
    theme::drawCard(g, nameArea, "How recordings are named");
}

void RecordingPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    keepArea = area.removeFromTop(16 + 22 + 30 + 40 + 30);
    area.removeFromTop(10);
    nameArea = area.removeFromTop(16 + 22 + 30 + 44 + 30 + 28);
    auto in = keepArea.reduced(12, 8);
    in.removeFromTop(22);
    auto row = in.removeFromTop(30);
    minimumLabel.setBounds(row.removeFromLeft(140).withTrimmedLeft(4));
    minimum.setBounds(row.removeFromLeft(380).reduced(0, 3));
    minimumNote.setBounds(in.removeFromTop(40).withTrimmedLeft(4));
    ask.setBounds(in.removeFromTop(28));

    in = nameArea.reduced(12, 8);
    in.removeFromTop(22);
    row = in.removeFromTop(30);
    patternLabel.setBounds(row.removeFromLeft(140).withTrimmedLeft(4));
    resetPattern.setBounds(row.removeFromRight(80).reduced(0, 2));
    row.removeFromRight(8);
    pattern.setBounds(row.reduced(0, 2));
    patternNote.setBounds(in.removeFromTop(44).withTrimmedLeft(4));
    folderPerPiece.setBounds(in.removeFromTop(28));
    preview.setBounds(in.removeFromTop(28).withTrimmedLeft(4));
}

} // namespace pf::app
