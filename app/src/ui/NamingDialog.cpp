#include "NamingDialog.h"

#include "Theme.h"

namespace pf::app {

NamingDialog::NamingDialog(Info i, GalleryNaming n, std::vector<GalleryItem> k, Done d)
    : info(std::move(i)), naming(std::move(n)), known(std::move(k)), done(std::move(d))
{
    heading.setText("What did you just play?", juce::dontSendNotification);
    heading.setFont(theme::font(17.0f, true));
    summary.setText(galleryClock(info.seconds) + "  -  " + juce::String(info.notes) + " notes  -  " + info.formats + "  -  "
                        + info.time.formatted("%Y-%m-%d %H:%M"),
                    juce::dontSendNotification);
    summary.setColour(juce::Label::textColourId, theme::dim);
    pieceLabel.setText("Piece", juce::dontSendNotification);
    composerLabel.setText("Composer", juce::dontSendNotification);
    tagsLabel.setText("Tags", juce::dontSendNotification);
    ratingLabel.setText("Rating", juce::dontSendNotification);
    preview.setColour(juce::Label::textColourId, theme::dim);
    preview.setFont(theme::font(12.5f));
    preview.setJustificationType(juce::Justification::topLeft);

    piece.setTextToShowWhenEmpty("e.g. Nocturne in C# minor, Op. posth.", theme::dim);
    composer.setTextToShowWhenEmpty("e.g. Chopin", theme::dim);
    tags.setTextToShowWhenEmpty("e.g. practice, slow, stream (commas between)", theme::dim);
    piece.onTextChange = [this] {
        // The pieces played before that contain what is typed (the most recent first); the composer of the one
        // typed exactly, unless one was typed.
        suggestions.clear();
        const auto text = piece.getText().trim();
        for (const auto& p : galleryPieces(known))
            if (text.isEmpty() || p.containsIgnoreCase(text))
                suggestions.add(p);
        list.updateContent();
        list.repaint();
        if (composerAuto)
            composer.setText(galleryComposerOf(known, text), juce::dontSendNotification);
        update();
    };
    composer.onTextChange = [this] {
        composerAuto = composer.isEmpty();
        update();
    };
    tags.onTextChange = [this] { update(); };
    for (auto* e : { &piece, &composer, &tags }) {
        e->onReturnKey = [this] { finish(Action::save); };
        e->onEscapeKey = [this] { finish(Action::automatic); };
    }
    rating.setTooltip("How it went: click a star (the same one again: none)");
    list.setRowHeight(22);
    list.setColour(juce::ListBox::backgroundColourId, theme::panel);
    list.setColour(juce::ListBox::outlineColourId, theme::panelEdge);
    list.setOutlineThickness(1);
    for (const auto& p : galleryPieces(known))
        suggestions.add(p);
    list.setVisible(!suggestions.isEmpty()); // none named yet: no list

    save.setTooltip("The files named after the piece (Enter)");
    save.onClick = [this] { finish(Action::save); };
    automatic.setTooltip("The files keep their date and time for a name (Escape); name them later in the gallery");
    automatic.onClick = [this] { finish(Action::automatic); };
    discard.setTooltip("Not kept: its files go to the recycle bin");
    discard.setColour(juce::TextButton::buttonColourId, theme::bad.withAlpha(0.5f));
    discard.onClick = [this] { finish(Action::discard); };

    for (auto* c : std::initializer_list<juce::Component*> { &heading, &summary, &pieceLabel, &composerLabel, &tagsLabel, &ratingLabel,
                                                             &preview, &piece, &composer, &tags, &rating, &list, &save, &automatic, &discard })
        addAndMakeVisible(*c);
    list.setVisible(!suggestions.isEmpty());
    update();
    setSize(560, suggestions.isEmpty() ? 300 : 400);
}

NamingDialog::~NamingDialog()
{
    if (!finished && done) // closed by its window's button: the automatic name
        done(Action::automatic, {});
}

void NamingDialog::launch(juce::Component* parent, Info info, GalleryNaming naming, std::vector<GalleryItem> known, Done done)
{
    juce::DialogWindow::LaunchOptions o;
    auto* dialog = new NamingDialog(std::move(info), std::move(naming), std::move(known), std::move(done));
    o.content.setOwned(dialog);
    o.dialogTitle = "Name the recording";
    o.dialogBackgroundColour = theme::background;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = parent;
    o.launchAsync();
    dialog->piece.grabKeyboardFocus();
}

GalleryMeta NamingDialog::meta() const
{
    GalleryMeta m;
    m.piece = piece.getText().trim();
    m.composer = composer.getText().trim();
    m.tags = tags.getText().trim();
    m.rating = rating.getRating();
    return m;
}

void NamingDialog::update()
{
    const auto m = meta();
    if (m.piece.isEmpty()) {
        preview.setText("Without a piece, the automatic name: " + GalleryNaming::automaticStem(info.time), juce::dontSendNotification);
        save.setEnabled(false);
        return;
    }
    save.setEnabled(true);
    const auto folder = galleryFolder(naming, m, info.root);
    const auto stem = galleryStem(naming, m, info.time, galleryCount(known, m.piece) + 1);
    preview.setText("Saved as " + folder.getFileName() + juce::File::getSeparatorString() + stem + "  (" + info.formats + ")",
                    juce::dontSendNotification);
}

void NamingDialog::finish(Action a)
{
    if (finished)
        return;
    if (a == Action::save && meta().piece.isEmpty())
        a = Action::automatic;
    finished = true;
    if (done)
        done(a, meta());
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState(0);
}

void NamingDialog::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= suggestions.size())
        return;
    if (selected)
        g.fillAll(theme::accent.withAlpha(0.28f));
    const auto p = suggestions[row];
    const auto who = galleryComposerOf(known, p);
    const int times = galleryCount(known, p);
    g.setColour(theme::text);
    g.setFont(theme::font(13.0f));
    g.drawText(p, 8, 0, width - 160, height, juce::Justification::centredLeft, true);
    g.setColour(theme::dim);
    g.drawText((who.isNotEmpty() ? who + ", " : juce::String()) + juce::String(times) + (times == 1 ? " time" : " times"), width - 160, 0, 152,
               height, juce::Justification::centredRight, true);
}

void NamingDialog::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= suggestions.size())
        return;
    composerAuto = true;
    piece.setText(suggestions[row], juce::sendNotification);
    piece.grabKeyboardFocus();
}

void NamingDialog::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
}

void NamingDialog::resized()
{
    auto area = getLocalBounds().reduced(16, 12);
    heading.setBounds(area.removeFromTop(26));
    summary.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);
    auto row = [&](juce::Label& label, juce::Component& field, int height = 28) {
        auto r = area.removeFromTop(height);
        label.setBounds(r.removeFromLeft(80));
        field.setBounds(r.reduced(0, 2));
        area.removeFromTop(4);
    };
    row(pieceLabel, piece);
    if (list.isVisible()) {
        list.setBounds(area.removeFromTop(4 * 22 + 2).withTrimmedLeft(80));
        area.removeFromTop(6);
    }
    row(composerLabel, composer);
    row(tagsLabel, tags);
    auto r = area.removeFromTop(26);
    ratingLabel.setBounds(r.removeFromLeft(80));
    rating.setBounds(r.removeFromLeft(130));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(32);
    discard.setBounds(buttons.removeFromLeft(100));
    save.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    automatic.setBounds(buttons.removeFromRight(200));
    area.removeFromBottom(6);
    preview.setBounds(area);
}

} // namespace pf::app
