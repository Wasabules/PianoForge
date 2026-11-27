// SPDX-License-Identifier: AGPL-3.0-or-later

#include "GalleryPanel.h"

#include "Theme.h"

#include <algorithm>
#include <set>

namespace pf::app {

namespace {

enum Column { stars = 1, pieceColumn, composerColumn, name, kind, date, length, size, notesColumn, files };
constexpr double kLengthMax = 3600.0; // s: at the top, no upper bound
constexpr double kSizeMax = 2000.0;   // MB: likewise
constexpr const char* kTypeNames[] = { "MIDI", "WAV", "MP3", "FLAC", "Opus" };
constexpr int kTypeBits[] = { GalleryFilter::midi, GalleryFilter::wav, GalleryFilter::mp3, GalleryFilter::flac, GalleryFilter::opus };

juce::String keyOf(const GalleryItem& item)
{
    return item.folder.getFullPathName() + "|" + item.stem;
}

} // namespace

// ---- The collections, on the left ----

class GalleryPanel::Sidebar final : public juce::Component, private juce::ListBoxModel {
public:
    explicit Sidebar(GalleryPanel& p) : panel(p)
    {
        list.setRowHeight(26);
        list.setColour(juce::ListBox::backgroundColourId, theme::panel);
        list.setColour(juce::ListBox::outlineColourId, theme::panelEdge);
        list.setOutlineThickness(1);
        addAndMakeVisible(list);
    }

    /// The collections again from the items, the one chosen kept.
    void rebuild()
    {
        const auto& all = panel.items;
        int recordings = 0, takes = 0, unnamed = 0;
        for (const auto& i : all) {
            recordings += i.recording ? 1 : 0;
            takes += i.recording ? 0 : 1;
            unnamed += i.meta.piece.isEmpty() ? 1 : 0;
        }
        rows.clear();
        rows.push_back({ Row::all, "Everything", {}, (int) all.size() });
        rows.push_back({ Row::recordings, "Recordings", {}, recordings });
        rows.push_back({ Row::takes, "Takes", {}, takes });
        rows.push_back({ Row::unnamed, "Not named yet", {}, unnamed });
        auto pieces = galleryPieces(all);
        pieces.sortNatural();
        if (!pieces.isEmpty())
            rows.push_back({ Row::heading, "Pieces", {}, pieces.size() });
        for (const auto& p : pieces)
            rows.push_back({ Row::pieceRow, p, p, galleryCount(all, p) });
        int selectedRow = 0;
        for (size_t r = 0; r < rows.size(); ++r)
            if (rows[r].type == chosen.type && rows[r].piece == chosen.piece)
                selectedRow = (int) r;
        list.updateContent();
        list.selectRow(selectedRow, true, true);
        list.repaint();
    }

    void resized() override { list.setBounds(getLocalBounds()); }

private:
    struct Row {
        enum Type { all, recordings, takes, unnamed, heading, pieceRow } type;
        juce::String label, piece;
        int count = 0;
    };

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;
        const auto& r = rows[(size_t) row];
        if (r.type == Row::heading) {
            g.setColour(theme::dim);
            g.setFont(theme::font(11.5f, true));
            g.drawText(r.label.toUpperCase(), 10, 0, width - 20, height, juce::Justification::bottomLeft);
            return;
        }
        if (selected)
            g.fillAll(theme::accent.withAlpha(0.3f));
        g.setColour(theme::text);
        g.setFont(theme::font(13.0f));
        const int indent = r.type == Row::pieceRow ? 18 : 10;
        g.drawText(r.label, indent, 0, width - indent - 40, height, juce::Justification::centredLeft, true);
        g.setColour(theme::dim);
        g.drawText(juce::String(r.count), width - 44, 0, 36, height, juce::Justification::centredRight);
    }

    void selectedRowsChanged(int row) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;
        if (rows[(size_t) row].type == Row::heading) { // not a collection: the one below
            list.selectRow(std::min(row + 1, (int) rows.size() - 1));
            return;
        }
        chosen = rows[(size_t) row];
        auto& f = panel.filter;
        f.kind = chosen.type == Row::recordings ? GalleryFilter::recordings : chosen.type == Row::takes ? GalleryFilter::takes : GalleryFilter::all;
        f.unnamedOnly = chosen.type == Row::unnamed;
        f.piece = chosen.type == Row::pieceRow ? chosen.piece : juce::String();
        panel.applyFilters();
    }

    GalleryPanel& panel;
    juce::ListBox list { "Collections", this };
    std::vector<Row> rows;
    Row chosen { Row::all, {}, {}, 0 };
};

// ---- The panel ----

GalleryPanel::GalleryPanel(TakePlayback& p, std::vector<std::pair<juce::File, bool>> r, Hooks h)
    : playback(p), roots(std::move(r)), hooks(std::move(h))
{
    sidebar = std::make_unique<Sidebar>(*this);
    addAndMakeVisible(*sidebar);

    // The filters.
    search.setTextToShowWhenEmpty("Search: piece, composer, tags, notes, name", theme::dim);
    search.onTextChange = [this] { applyFilters(); };
    for (size_t i = 0; i < types.size(); ++i) {
        types[i].setButtonText(kTypeNames[i]);
        types[i].setToggleState(true, juce::dontSendNotification);
        types[i].setTooltip("Show what has a " + juce::String(kTypeNames[i]) + " file (untick to leave those out)");
        types[i].onClick = [this] { applyFilters(); };
        addAndMakeVisible(types[i]);
    }
    dateFilter.addItemList({ "Any time", "Today", "Last 7 days", "Last 30 days", "Last 12 months" }, 1);
    dateFilter.setSelectedId(1, juce::dontSendNotification);
    dateFilter.onChange = [this] { applyFilters(); };
    ratingFilter.addItem("Any rating", 1);
    for (int k = 1; k <= 5; ++k)
        ratingFilter.addItem(juce::String(k) + (k == 1 ? " star" : " stars") + (k < 5 ? " or more" : ""), k + 1);
    ratingFilter.setSelectedId(1, juce::dontSendNotification);
    ratingFilter.onChange = [this] { applyFilters(); };
    for (auto* s : { &lengthFilter, &sizeFilter }) {
        s->setSliderStyle(juce::Slider::TwoValueHorizontal);
        s->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s->onValueChange = [this] { applyFilters(); };
    }
    lengthFilter.setRange(0.0, kLengthMax, 1.0);
    lengthFilter.setSkewFactorFromMidPoint(240.0);
    lengthFilter.setMinAndMaxValues(0.0, kLengthMax, juce::dontSendNotification);
    lengthFilter.setTooltip("Only what lasts between these lengths (at the right end: no upper bound)");
    sizeFilter.setRange(0.0, kSizeMax, 0.1);
    sizeFilter.setSkewFactorFromMidPoint(40.0);
    sizeFilter.setMinAndMaxValues(0.0, kSizeMax, juce::dontSendNotification);
    sizeFilter.setTooltip("Only what takes between these sizes on disk, every file together (at the right end: no upper bound)");
    for (auto* l : { &lengthLabel, &sizeLabel, &countLabel })
        l->setFont(theme::font(12.5f));
    countLabel.setColour(juce::Label::textColourId, theme::dim);
    countLabel.setJustificationType(juce::Justification::centredRight);
    resetFilters.setTooltip("Every filter back to showing everything (the collection on the left stays)");
    resetFilters.onClick = [this] {
        search.clear();
        for (auto& t : types)
            t.setToggleState(true, juce::dontSendNotification);
        dateFilter.setSelectedId(1, juce::dontSendNotification);
        ratingFilter.setSelectedId(1, juce::dontSendNotification);
        lengthFilter.setMinAndMaxValues(0.0, kLengthMax, juce::dontSendNotification);
        sizeFilter.setMinAndMaxValues(0.0, kSizeMax, juce::dontSendNotification);
        applyFilters();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &search, &dateFilter, &ratingFilter, &lengthFilter, &sizeFilter, &lengthLabel,
                                                             &sizeLabel, &countLabel, &resetFilters })
        addAndMakeVisible(*c);

    // The list.
    auto& header = table.getHeader();
    header.addColumn("Rating", stars, 78, 60);
    header.addColumn("Piece", pieceColumn, 190, 80);
    header.addColumn("Composer", composerColumn, 110, 60);
    header.addColumn("Name", name, 210, 80);
    header.addColumn("Kind", kind, 76, 50);
    header.addColumn("Date", date, 118, 90);
    header.addColumn("Length", length, 58, 44);
    header.addColumn("Size", size, 66, 44);
    header.addColumn("Notes", notesColumn, 54, 40);
    header.addColumn("Files", files, 120, 60);
    header.setColumnVisible(composerColumn, false); // (right-click the header: every column)
    header.setColumnVisible(kind, false);
    header.setColumnVisible(notesColumn, false);
    header.setSortColumnId(date, false);
    table.setMultipleSelectionEnabled(true);
    table.setRowHeight(26);
    table.setColour(juce::ListBox::backgroundColourId, theme::panel);
    table.setColour(juce::ListBox::outlineColourId, theme::panelEdge);
    table.setOutlineThickness(1);
    addAndMakeVisible(table);

    // The details of the selection.
    detailsHeading.setFont(theme::font(15.0f, true));
    pieceLabel.setText("Piece", juce::dontSendNotification);
    composerLabel.setText("Composer", juce::dontSendNotification);
    tagsLabel.setText("Tags", juce::dontSendNotification);
    ratingLabel.setText("Rating", juce::dontSendNotification);
    notesLabel.setText("Notes", juce::dontSendNotification);
    for (auto* l : { &pieceLabel, &composerLabel, &tagsLabel, &ratingLabel, &notesLabel }) {
        l->setColour(juce::Label::textColourId, theme::dim);
        l->setFont(theme::font(12.5f));
    }
    filesInfo.setColour(juce::Label::textColourId, theme::dim);
    filesInfo.setFont(theme::font(12.0f));
    filesInfo.setJustificationType(juce::Justification::topLeft);
    notes.setMultiLine(true, true);
    notes.setReturnKeyStartsNewLine(true);
    tags.setTextToShowWhenEmpty("commas between", theme::dim);
    piece.onTextChange = [this] { pieceEdited = !loadingDetails || pieceEdited; };
    composer.onTextChange = [this] { composerEdited = !loadingDetails || composerEdited; };
    tags.onTextChange = [this] { tagsEdited = !loadingDetails || tagsEdited; };
    notes.onTextChange = [this] { notesEdited = !loadingDetails || notesEdited; };
    for (auto* e : { &piece, &composer, &tags })
        e->onReturnKey = [this] { applyDetails(); };
    rating.onChange = [this](int) { ratingEdited = true; };
    pieceMenu.setTooltip("A piece already in the gallery");
    pieceMenu.onClick = [this] {
        chooseFrom(piece, galleryPieces(items), pieceEdited, [this] { // its composer, when one was given before
            if (!composerEdited && composer.isEmpty())
                if (const auto who = galleryComposerOf(items, piece.getText()); who.isNotEmpty()) {
                    composer.setText(who, juce::dontSendNotification);
                    composerEdited = true;
                }
        });
    };
    composerMenu.setTooltip("A composer already in the gallery");
    composerMenu.onClick = [this] { chooseFrom(composer, galleryComposers(items), composerEdited, {}); };
    renameByPattern.setToggleState(true, juce::dontSendNotification);
    renameByPattern.setTooltip("With a piece: the files renamed by the name pattern, and moved to the piece's folder if the "
                               "pattern files by piece (Settings > Recording)");
    applyButton.setTooltip("What is written here, for every item selected (Enter); a field left as it was for several items stays theirs");
    applyButton.onClick = [this] { applyDetails(); };
    revertButton.onClick = [this] { showDetails(); };
    patternButton.setTooltip("The name pattern and the shortest recording kept (Settings > Recording)");
    patternButton.onClick = [this] {
        if (hooks.openNamingSettings)
            hooks.openNamingSettings();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &detailsHeading, &pieceLabel, &composerLabel, &tagsLabel, &ratingLabel, &notesLabel,
                                                             &filesInfo, &piece, &composer, &tags, &notes, &pieceMenu, &composerMenu, &rating,
                                                             &renameByPattern, &applyButton, &revertButton, &patternButton })
        addAndMakeVisible(*c);

    // Listening and actions.
    playButton.setTooltip("Listen to the item selected (double-click a row, or Space)");
    playButton.onClick = [this] { listen(); };
    stopButton.onClick = [this] { playback.stop(); };
    abButton.setTooltip("Select two: both play from the same instant, and the switch beside changes the one heard (A and B keys)");
    abButton.onClick = [this] { listenAB(); };
    sideButton.onClick = [this] { playback.setSide(playback.currentSide() == 0 ? 1 : 0); };
    midiButton.setTooltip("Plays its MIDI through the engine, with the settings heard now (the MIDI file player)");
    midiButton.onClick = [this] {
        if (const auto s = selected(); !s.empty() && hooks.playMidi) {
            std::vector<juce::File> list; // what is shown, in its order: previous / next in the player go through it
            for (const int i : shown)
                if (const auto m = items[(size_t) i].midi(); m.existsAsFile())
                    list.push_back(m);
            hooks.playMidi(s.front()->midi(), std::move(list));
        }
    };
    renderButton.setTooltip("A new WAV from its MIDI, with the engine, the room and the limiter as they are now");
    renderButton.onClick = [this] {
        if (const auto s = selected(); !s.empty() && hooks.renderAgain)
            hooks.renderAgain(s.front()->midi());
    };
    renameButton.setTooltip("A name of your own for its files (the pattern aside)");
    renameButton.onClick = [this] { renameSelected(); };
    deleteButton.setTooltip("Every file of the items selected to the recycle bin (Delete)");
    deleteButton.onClick = [this] { deleteSelected(); };
    showButton.onClick = [this] {
        if (const auto s = selected(); !s.empty() && !s.front()->files.empty())
            s.front()->files.front().revealToUser();
        else if (!roots.empty())
            roots.front().first.startAsProcess();
    };
    refreshButton.onClick = [this] { refresh(); };
    for (auto* b : { &playButton, &stopButton, &abButton, &sideButton, &midiButton, &renderButton, &renameButton, &deleteButton,
                     &showButton, &refreshButton })
        addAndMakeVisible(*b);
    position.setSliderStyle(juce::Slider::LinearHorizontal);
    position.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    position.setRange(0.0, 1.0, 0.0);
    position.onDragStart = [this] { dragging = true; };
    position.onDragEnd = [this] {
        dragging = false;
        playback.seek(position.getValue());
    };
    addAndMakeVisible(position);
    timeLabel.setJustificationType(juce::Justification::centredRight);
    for (auto* l : { &timeLabel, &nowLabel }) {
        l->setFont(theme::font(13.0f));
        addAndMakeVisible(*l);
    }

    refresh();
    setSize(1320, 780);
    timerCallback();
    startTimerHz(15);
}

GalleryPanel::~GalleryPanel()
{
    stopTimer();
}

// ---- Reading, filtering, sorting ----

void GalleryPanel::refresh()
{
    std::set<juce::String> keep;
    for (const auto* i : selected())
        keep.insert(keyOf(*i));
    items = scanGallery(roots);
    sidebar->rebuild(); // (applies the filters)
    applyFilters();
    juce::SparseSet<int> rows;
    for (size_t r = 0; r < shown.size(); ++r)
        if (keep.count(keyOf(items[(size_t) shown[r]])) != 0)
            rows.addRange({ (int) r, (int) r + 1 });
    table.setSelectedRows(rows, juce::dontSendNotification);
    showDetails();
    updateButtons();
}

void GalleryPanel::readFilters()
{
    filter.search = search.getText();
    int bits = 0;
    for (size_t i = 0; i < types.size(); ++i)
        bits |= types[i].getToggleState() ? kTypeBits[i] : 0;
    filter.types = bits == GalleryFilter::anyType ? GalleryFilter::anyType : bits;
    const auto now = juce::Time::getCurrentTime();
    const auto midnight = juce::Time(now.getYear(), now.getMonth(), now.getDayOfMonth(), 0, 0);
    switch (dateFilter.getSelectedId()) {
    case 2: filter.since = midnight; break;
    case 3: filter.since = now - juce::RelativeTime::days(7); break;
    case 4: filter.since = now - juce::RelativeTime::days(30); break;
    case 5: filter.since = now - juce::RelativeTime::days(365); break;
    default: filter.since = juce::Time(); break;
    }
    filter.minRating = std::max(0, ratingFilter.getSelectedId() - 1);
    filter.minSeconds = lengthFilter.getMinValue();
    filter.maxSeconds = lengthFilter.getMaxValue() >= kLengthMax ? -1.0 : lengthFilter.getMaxValue();
    filter.minBytes = (juce::int64) (sizeFilter.getMinValue() * 1e6);
    filter.maxBytes = sizeFilter.getMaxValue() >= kSizeMax ? -1 : (juce::int64) (sizeFilter.getMaxValue() * 1e6);
    lengthLabel.setText("Length " + galleryClock(filter.minSeconds) + " to " + (filter.maxSeconds < 0.0 ? juce::String("any") : galleryClock(filter.maxSeconds)),
                        juce::dontSendNotification);
    sizeLabel.setText("Size " + (filter.minBytes > 0 ? gallerySize(filter.minBytes) : juce::String("0")) + " to "
                          + (filter.maxBytes < 0 ? juce::String("any") : gallerySize(filter.maxBytes)),
                      juce::dontSendNotification);
}

void GalleryPanel::applyFilters()
{
    std::set<juce::String> keep;
    for (const auto* i : selected())
        keep.insert(keyOf(*i));
    readFilters();
    shown.clear();
    juce::int64 bytes = 0;
    double seconds = 0.0;
    for (size_t i = 0; i < items.size(); ++i)
        if (filter.matches(items[i])) {
            shown.push_back((int) i);
            bytes += items[i].bytes;
            seconds += items[i].seconds;
        }
    sortShown();
    countLabel.setText(juce::String((int) shown.size()) + " of " + juce::String((int) items.size()) + "  -  " + gallerySize(bytes) + "  -  "
                           + galleryClock(seconds),
                       juce::dontSendNotification);
    juce::SparseSet<int> rows;
    for (size_t r = 0; r < shown.size(); ++r)
        if (keep.count(keyOf(items[(size_t) shown[r]])) != 0)
            rows.addRange({ (int) r, (int) r + 1 });
    table.setSelectedRows(rows, juce::dontSendNotification);
    showDetails();
    updateButtons();
}

void GalleryPanel::sortOrderChanged(int column, bool forwards)
{
    sortColumn = column;
    sortForwards = forwards;
    sortShown();
}

void GalleryPanel::sortShown()
{
    const int column = sortColumn;
    auto less = [&](int ia, int ib) {
        const auto& a = items[(size_t) ia];
        const auto& b = items[(size_t) ib];
        switch (column) {
        case stars: return a.meta.rating < b.meta.rating;
        case pieceColumn: return a.meta.piece.compareNatural(b.meta.piece) < 0;
        case composerColumn: return a.meta.composer.compareNatural(b.meta.composer) < 0;
        case name: return a.stem.compareNatural(b.stem) < 0;
        case kind: return a.recording < b.recording;
        case length: return a.seconds < b.seconds;
        case size: return a.bytes < b.bytes;
        case notesColumn: return a.notes < b.notes;
        case files: return a.extensions() < b.extensions();
        default: return a.time < b.time;
        }
    };
    std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) { return sortForwards ? less(a, b) : less(b, a); });
    table.updateContent();
    table.repaint();
}

// ---- The list ----

void GalleryPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool isSelected)
{
    g.fillAll(isSelected ? theme::accent.withAlpha(0.28f) : (row % 2 ? theme::panel : theme::panel.brighter(0.04f)));
}

void GalleryPanel::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row < 0 || row >= (int) shown.size())
        return;
    const auto& e = items[(size_t) shown[(size_t) row]];
    if (column == stars) {
        StarRating::draw(g, juce::Rectangle<float>(6.0f, 6.0f, (float) w - 10.0f, (float) h - 12.0f), e.meta.rating);
        return;
    }
    juce::String text;
    switch (column) {
    case pieceColumn: text = e.meta.piece.isNotEmpty() ? e.meta.piece : juce::String("-"); break;
    case composerColumn: text = e.meta.composer; break;
    case name: text = e.stem; break;
    case kind: text = e.recording ? "Recording" : "Take"; break;
    case date: text = e.time.formatted("%Y-%m-%d %H:%M"); break;
    case length: text = galleryClock(e.seconds); break;
    case size: text = gallerySize(e.bytes); break;
    case notesColumn: text = e.notes >= 0 ? juce::String(e.notes) : juce::String("-"); break;
    case files: text = e.extensions(); break;
    default: break;
    }
    g.setColour(column == pieceColumn && e.meta.piece.isNotEmpty() ? theme::text : theme::dim);
    g.setFont(theme::font(13.0f, column == pieceColumn && e.meta.piece.isNotEmpty()));
    const bool right = column == length || column == size || column == notesColumn;
    g.drawText(text, 6, 0, w - 10, h, right ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

juce::String GalleryPanel::getCellTooltip(int row, int)
{
    if (row < 0 || row >= (int) shown.size())
        return {};
    const auto& e = items[(size_t) shown[(size_t) row]];
    juce::String t = e.folder.getChildFile(e.stem).getFullPathName();
    if (e.meta.tags.isNotEmpty())
        t << "\nTags: " << e.meta.tags;
    if (e.meta.notes.isNotEmpty())
        t << "\n" << e.meta.notes;
    return t;
}

void GalleryPanel::cellDoubleClicked(int, int, const juce::MouseEvent&)
{
    listen();
}

void GalleryPanel::selectedRowsChanged(int)
{
    showDetails();
    updateButtons();
}

std::vector<GalleryItem*> GalleryPanel::selected()
{
    std::vector<GalleryItem*> out;
    const auto set = table.getSelectedRows();
    for (int i = 0; i < set.size(); ++i)
        if (set[i] >= 0 && set[i] < (int) shown.size())
            out.push_back(&items[(size_t) shown[(size_t) set[i]]]);
    return out;
}

void GalleryPanel::updateButtons()
{
    const auto s = selected();
    const bool one = s.size() == 1;
    const bool audio = !s.empty() && s.front()->audio().existsAsFile();
    const bool midi = !s.empty() && s.front()->midi().existsAsFile();
    playButton.setEnabled(audio);
    abButton.setEnabled(s.size() == 2 && s[0]->audio().existsAsFile() && s[1]->audio().existsAsFile());
    midiButton.setEnabled(one && midi);
    renderButton.setEnabled(one && midi);
    renameButton.setEnabled(one);
    deleteButton.setEnabled(!s.empty());
}

// ---- The details: what is known of the selection ----

void GalleryPanel::showDetails()
{
    const auto s = selected();
    loadingDetails = true;
    pieceEdited = composerEdited = tagsEdited = ratingEdited = notesEdited = false;
    const bool any = !s.empty();
    for (auto* c : std::initializer_list<juce::Component*> { &piece, &composer, &tags, &notes, &pieceMenu, &composerMenu, &rating, &applyButton,
                                                             &revertButton, &renameByPattern })
        c->setEnabled(any);
    if (!any) {
        detailsHeading.setText("Nothing selected", juce::dontSendNotification);
        for (auto* e : { &piece, &composer, &tags, &notes })
            e->clear();
        rating.setRating(0);
        filesInfo.setText("Select an item to name it, rate it or write about it; several at once to give them the same piece "
                          "or tags.",
                          juce::dontSendNotification);
        loadingDetails = false;
        return;
    }
    // A field the selection shares: shown; else empty, "(several)" in grey, left as it is unless typed in.
    auto common = [&](auto get) {
        const juce::String first = get(*s.front());
        for (const auto* i : s)
            if (get(*i) != first)
                return std::make_pair(juce::String(), true);
        return std::make_pair(first, false);
    };
    auto fill = [&](juce::TextEditor& e, std::pair<juce::String, bool> v) {
        e.setText(v.first, juce::dontSendNotification);
        e.setTextToShowWhenEmpty(v.second ? "(several)" : juce::String(), theme::dim);
    };
    fill(piece, common([](const GalleryItem& i) { return i.meta.piece; }));
    fill(composer, common([](const GalleryItem& i) { return i.meta.composer; }));
    fill(tags, common([](const GalleryItem& i) { return i.meta.tags; }));
    fill(notes, common([](const GalleryItem& i) { return i.meta.notes; }));
    if (tags.isEmpty() && !common([](const GalleryItem& i) { return i.meta.tags; }).second)
        tags.setTextToShowWhenEmpty("commas between", theme::dim);
    const auto r = common([](const GalleryItem& i) { return juce::String(i.meta.rating); });
    rating.setRating(r.second ? 0 : r.first.getIntValue(), r.second);
    if (s.size() == 1) {
        const auto& i = *s.front();
        detailsHeading.setText(i.meta.piece.isNotEmpty() ? i.meta.piece : i.stem, juce::dontSendNotification);
        juce::String info;
        info << (i.recording ? "Recording" : "Take") << ", " << i.time.formatted("%Y-%m-%d %H:%M") << "\n"
             << galleryClock(i.seconds) << (i.notes >= 0 ? ", " + juce::String(i.notes) + " notes" : juce::String()) << ", "
             << gallerySize(i.bytes) << "\n\n";
        for (const auto& f : i.files)
            info << f.getFileName() << "  (" << gallerySize(f.getSize()) << ")\n";
        info << "\nIn " << i.folder.getFullPathName();
        filesInfo.setText(info, juce::dontSendNotification);
    } else {
        juce::int64 bytes = 0;
        double seconds = 0.0;
        for (const auto* i : s) {
            bytes += i->bytes;
            seconds += i->seconds;
        }
        detailsHeading.setText(juce::String((int) s.size()) + " selected", juce::dontSendNotification);
        filesInfo.setText(galleryClock(seconds) + ", " + gallerySize(bytes) + ".\n\nWhat is typed here applies to all of them; a "
                              "field left as it was keeps each one's own.",
                          juce::dontSendNotification);
    }
    loadingDetails = false;
}

int GalleryPanel::numberOf(const GalleryItem& item, const juce::String& p) const
{
    int n = 1;
    for (const auto& other : items)
        if (&other != &item && other.meta.piece == p && other.time < item.time)
            ++n;
    return n;
}

juce::File GalleryPanel::rootOf(const GalleryItem& item) const
{
    for (const auto& [root, recordings] : roots)
        if (item.folder == root || item.folder.isAChildOf(root))
            return root;
    return item.folder;
}

void GalleryPanel::applyDetails()
{
    const auto s = selected();
    if (s.empty())
        return;
    const bool single = s.size() == 1;
    const auto naming = hooks.naming ? hooks.naming() : GalleryNaming {};
    playback.release(); // (it may hold one of them in memory; no file is open)
    juce::StringArray failures;
    std::set<juce::String> renamed;
    for (auto* item : s) {
        auto m = item->meta;
        if (single || pieceEdited)
            m.piece = piece.getText().trim();
        if (single || composerEdited)
            m.composer = composer.getText().trim();
        if (single || tagsEdited)
            m.tags = tags.getText().trim();
        if (single || notesEdited)
            m.notes = notes.getText().trim();
        if (single || ratingEdited)
            m.rating = rating.getRating();
        item->meta = m;
        const auto metaFile = item->metaFile();
        if (!writeGalleryMeta(metaFile, m)) {
            failures.add(item->stem);
            continue;
        }
        // The metadata file is one of its files now (or no longer).
        item->files.erase(std::remove(item->files.begin(), item->files.end(), metaFile), item->files.end());
        if (metaFile.existsAsFile())
            item->files.push_back(metaFile);
        if (renameByPattern.getToggleState() && m.piece.isNotEmpty()) {
            const auto folder = galleryFolder(naming, m, rootOf(*item));
            const auto stem = galleryStem(naming, m, item->time, numberOf(*item, m.piece));
            if (const auto moved = moveGalleryItem(*item, folder, stem); moved.failed())
                failures.add(moved.getErrorMessage());
        }
        renamed.insert(keyOf(*item));
    }
    refresh();
    // The same items selected, under their new names.
    juce::SparseSet<int> rows;
    for (size_t r = 0; r < shown.size(); ++r)
        if (renamed.count(keyOf(items[(size_t) shown[r]])) != 0)
            rows.addRange({ (int) r, (int) r + 1 });
    table.setSelectedRows(rows, juce::sendNotification);
    nowLabel.setText(failures.isEmpty() ? juce::String((int) s.size()) + (s.size() == 1 ? " item" : " items") + " updated"
                                        : "Not all: " + failures.joinIntoString("; "),
                     juce::dontSendNotification);
}

void GalleryPanel::chooseFrom(juce::TextEditor& field, const juce::StringArray& values, bool& edited, std::function<void()> then)
{
    if (values.isEmpty())
        return;
    juce::PopupMenu menu;
    for (int i = 0; i < values.size(); ++i)
        menu.addItem(i + 1, values[i]);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&field),
                       [safe = juce::Component::SafePointer<GalleryPanel>(this), &field, &edited, values, then](int chosen) {
                           if (safe == nullptr || chosen <= 0)
                               return;
                           field.setText(values[chosen - 1], juce::dontSendNotification);
                           edited = true;
                           if (then)
                               then();
                       });
}

// ---- Listening and the actions ----

void GalleryPanel::listen()
{
    const auto s = selected();
    if (s.empty())
        return;
    if (const auto error = playback.load(s.front()->audio()); error.isNotEmpty()) {
        nowLabel.setText(error, juce::dontSendNotification);
        return;
    }
    playback.play();
}

void GalleryPanel::listenAB()
{
    const auto s = selected();
    if (s.size() != 2)
        return;
    if (const auto error = playback.load(s[0]->audio(), s[1]->audio()); error.isNotEmpty()) {
        nowLabel.setText(error, juce::dontSendNotification);
        return;
    }
    playback.play();
}

void GalleryPanel::renameSelected()
{
    const auto s = selected();
    if (s.size() != 1)
        return;
    const auto key = keyOf(*s.front());
    const auto stem = s.front()->stem;
    auto* window = new juce::AlertWindow("Rename", "A new name for every file of \"" + stem + "\":", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", stem, "Name");
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<GalleryPanel>(this), key, stem,
                                                                        window](int result) {
        const auto wanted = window->getTextEditorContents("name").trim().removeCharacters("<>:\"/\\|?*").trimCharactersAtEnd(". ");
        if (safe == nullptr || result != 1 || wanted.isEmpty() || wanted == stem)
            return;
        for (auto& item : safe->items)
            if (keyOf(item) == key) {
                safe->playback.release();
                const auto moved = moveGalleryItem(item, item.folder, wanted);
                safe->nowLabel.setText(moved.wasOk() ? "Renamed " + item.stem : moved.getErrorMessage(), juce::dontSendNotification);
                break;
            }
        safe->refresh();
    }), true);
}

void GalleryPanel::deleteSelected()
{
    const auto s = selected();
    if (s.empty())
        return;
    std::vector<GalleryItem> doomed;
    juce::StringArray names;
    for (const auto* i : s) {
        doomed.push_back(*i);
        names.add(i->meta.piece.isNotEmpty() ? i->meta.piece + " (" + i->stem + ")" : i->stem);
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions {}
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle(s.size() == 1 ? juce::String("Delete") : "Delete " + juce::String((int) s.size()) + " items")
                                     .withMessage("Move every file of " + names.joinIntoString(", ") + " to the recycle bin?")
                                     .withButton("Delete", 1)
                                     .withButton("Cancel", 0)
                                     .withAssociatedComponent(this),
                                 [safe = juce::Component::SafePointer<GalleryPanel>(this), doomed](int result) {
                                     if (result == 0 || safe == nullptr)
                                         return;
                                     safe->playback.release();
                                     int failed = 0;
                                     for (const auto& e : doomed) {
                                         for (const auto& f : e.files)
                                             failed += f.moveToTrash() ? 0 : 1;
                                         // A piece's folder left empty: away too.
                                         if (e.folder.isDirectory() && e.folder.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0
                                             && safe->rootOf(e) != e.folder)
                                             e.folder.deleteFile();
                                     }
                                     safe->refresh();
                                     safe->nowLabel.setText(failed == 0 ? juce::String("Moved to the recycle bin")
                                                                        : juce::String(failed) + " files could not be moved",
                                                            juce::dontSendNotification);
                                 });
}

void GalleryPanel::timerCallback()
{
    const double total = playback.lengthSeconds();
    if (total > 0.0 && std::abs(position.getMaximum() - total) > 1e-3)
        position.setRange(0.0, total, 0.0);
    if (!dragging)
        position.setValue(playback.positionSeconds(), juce::dontSendNotification);
    position.setEnabled(playback.isLoaded());
    timeLabel.setText(playback.isLoaded() ? galleryClock(playback.positionSeconds()) + " / " + galleryClock(total) : juce::String(),
                      juce::dontSendNotification);
    const bool ab = playback.hasB();
    sideButton.setVisible(ab);
    sideButton.setButtonText(playback.currentSide() == 0 ? "Hearing A" : "Hearing B");
    sideButton.setToggleState(playback.currentSide() == 1, juce::dontSendNotification);
    if (playback.isLoaded())
        nowLabel.setText((playback.isPlaying() ? juce::String("Playing ") : juce::String("Stopped: "))
                             + (ab ? "A " + playback.name(0) + "  |  B " + playback.name(1) : playback.name(0)),
                         juce::dontSendNotification);
    stopButton.setEnabled(playback.isPlaying());
}

bool GalleryPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey) {
        if (playback.isPlaying())
            playback.stop();
        else if (playback.isLoaded())
            playback.play();
        else
            listen();
        return true;
    }
    if (key.getTextCharacter() == 'a' || key.getTextCharacter() == 'b') {
        playback.setSide(key.getTextCharacter() == 'b' ? 1 : 0);
        return true;
    }
    if (key == juce::KeyPress::deleteKey) {
        deleteSelected();
        return true;
    }
    return false;
}

void GalleryPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    g.setColour(theme::panel);
    g.fillRoundedRectangle(filterArea.toFloat(), 4.0f);
    theme::drawCard(g, detailsArea, "About the selection");
}

void GalleryPanel::resized()
{
    auto area = getLocalBounds().reduced(12);

    // The filters, on top.
    filterArea = area.removeFromTop(2 * 30 + 3 * 6);
    {
        auto in = filterArea.reduced(8, 6);
        auto row = in.removeFromTop(30);
        search.setBounds(row.removeFromLeft(300).reduced(0, 2));
        row.removeFromLeft(12);
        for (auto& t : types)
            t.setBounds(row.removeFromLeft(66));
        row.removeFromLeft(8);
        dateFilter.setBounds(row.removeFromLeft(150).reduced(0, 2));
        row.removeFromLeft(8);
        ratingFilter.setBounds(row.removeFromLeft(160).reduced(0, 2));
        row.removeFromLeft(8);
        resetFilters.setBounds(row.removeFromLeft(70).reduced(0, 2));
        in.removeFromTop(6);
        row = in.removeFromTop(30);
        lengthLabel.setBounds(row.removeFromLeft(150));
        lengthFilter.setBounds(row.removeFromLeft(240));
        row.removeFromLeft(24);
        sizeLabel.setBounds(row.removeFromLeft(160));
        sizeFilter.setBounds(row.removeFromLeft(240));
        countLabel.setBounds(row);
    }
    area.removeFromTop(10);

    // Listening and the actions, below.
    auto actions = area.removeFromBottom(30);
    for (auto* b : { &midiButton, &renderButton, &renameButton, &deleteButton })
        b->setBounds(actions.removeFromLeft(b == &midiButton ? 160 : 112).reduced(2, 0));
    refreshButton.setBounds(actions.removeFromRight(90).reduced(2, 0));
    showButton.setBounds(actions.removeFromRight(120).reduced(2, 0));
    area.removeFromBottom(6);
    auto transport = area.removeFromBottom(30);
    for (auto* b : { &playButton, &stopButton, &abButton, &sideButton })
        b->setBounds(transport.removeFromLeft(b == &sideButton ? 110 : 80).reduced(2, 0));
    transport.removeFromLeft(8);
    timeLabel.setBounds(transport.removeFromRight(110));
    position.setBounds(transport);
    area.removeFromBottom(2);
    nowLabel.setBounds(area.removeFromBottom(22));
    area.removeFromBottom(6);

    // The collections, the list, the details.
    sidebar->setBounds(area.removeFromLeft(210));
    area.removeFromLeft(10);
    detailsArea = area.removeFromRight(310);
    area.removeFromRight(10);
    table.setBounds(area);
    {
        auto in = detailsArea.reduced(12, 8);
        in.removeFromTop(22);
        detailsHeading.setBounds(in.removeFromTop(26));
        in.removeFromTop(4);
        auto field = [&](juce::Label& label, juce::Component& editor, juce::Component* menu) {
            label.setBounds(in.removeFromTop(18));
            auto r = in.removeFromTop(28);
            if (menu != nullptr) {
                menu->setBounds(r.removeFromRight(30).reduced(0, 2));
                r.removeFromRight(4);
            }
            editor.setBounds(r.reduced(0, 2));
            in.removeFromTop(4);
        };
        field(pieceLabel, piece, &pieceMenu);
        field(composerLabel, composer, &composerMenu);
        field(tagsLabel, tags, nullptr);
        ratingLabel.setBounds(in.removeFromTop(18));
        rating.setBounds(in.removeFromTop(26).removeFromLeft(130));
        in.removeFromTop(4);
        notesLabel.setBounds(in.removeFromTop(18));
        notes.setBounds(in.removeFromTop(84));
        in.removeFromTop(6);
        auto r = in.removeFromTop(26);
        patternButton.setBounds(r.removeFromRight(80).reduced(0, 1));
        renameByPattern.setBounds(r);
        in.removeFromTop(4);
        r = in.removeFromTop(30);
        applyButton.setBounds(r.removeFromRight(90));
        r.removeFromRight(8);
        revertButton.setBounds(r.removeFromRight(80));
        in.removeFromTop(10);
        filesInfo.setBounds(in);
    }
}

} // namespace pf::app
