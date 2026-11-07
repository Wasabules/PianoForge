#include "TakesPanel.h"

#include "Theme.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <map>

namespace pf::app {

namespace {

enum Column { name = 1, kind, date, length, notes, files };
constexpr const char* kAudioOrder[] = { ".wav", ".flac", ".opus", ".mp3" }; // what to listen to, lossless first

/// The file's stem without ".master" (the master of a recording made in other formats only).
juce::String stemOf(const juce::File& f)
{
    auto stem = f.getFileNameWithoutExtension();
    return stem.endsWith(".master") ? stem.dropLastCharacters(7) : stem;
}

} // namespace

juce::File TakesPanel::Entry::audio() const
{
    for (const auto* ext : kAudioOrder)
        for (const auto& f : files)
            if (f.hasFileExtension(ext) && !f.getFileNameWithoutExtension().endsWith(".master"))
                return f;
    for (const auto& f : files)
        if (f.hasFileExtension(".wav"))
            return f; // only the master
    return {};
}

juce::File TakesPanel::Entry::midi() const
{
    for (const auto& f : files)
        if (f.hasFileExtension(".mid"))
            return f;
    return {};
}

juce::String TakesPanel::Entry::extensions() const
{
    juce::StringArray e;
    for (const auto& f : files)
        e.addIfNotAlreadyThere(f.getFileNameWithoutExtension().endsWith(".master") ? "master" : f.getFileExtension().substring(1));
    return e.joinIntoString(" ");
}

TakesPanel::TakesPanel(TakePlayback& p, std::vector<juce::File> f, Hooks h) : playback(p), folders(std::move(f)), hooks(std::move(h))
{
    auto& header = table.getHeader();
    header.addColumn("Name", name, 300, 120);
    header.addColumn("Kind", kind, 90, 60);
    header.addColumn("Date", date, 150, 100);
    header.addColumn("Length", length, 70, 50);
    header.addColumn("Notes", notes, 60, 40);
    header.addColumn("Files", files, 190, 80);
    header.setSortColumnId(date, false);
    table.setMultipleSelectionEnabled(true);
    table.setRowHeight(26);
    table.setColour(juce::ListBox::backgroundColourId, theme::panel);
    table.setColour(juce::ListBox::outlineColourId, theme::panelEdge);
    table.setOutlineThickness(1);
    addAndMakeVisible(table);

    playButton.setTooltip("Listen to the take selected (double-click a row, or Space)");
    playButton.onClick = [this] { listen(); };
    stopButton.onClick = [this] { playback.stop(); };
    abButton.setTooltip("Select two takes: both play from the same instant, and the switch below changes the one heard");
    abButton.onClick = [this] { listenAB(); };
    sideButton.setTooltip("The take heard in an A/B (or the A and B keys)");
    sideButton.onClick = [this] { playback.setSide(playback.currentSide() == 0 ? 1 : 0); };
    midiButton.setTooltip("Plays the take's MIDI through the engine, with the settings heard now (the MIDI file player)");
    midiButton.onClick = [this] {
        if (const auto rows = selection(); !rows.empty() && hooks.playMidi)
            hooks.playMidi(entries[(size_t) rows.front()].midi());
    };
    renderButton.setTooltip("A new WAV from the take's MIDI, with the engine, the room and the limiter as they are now");
    renderButton.onClick = [this] {
        if (const auto rows = selection(); !rows.empty() && hooks.renderAgain)
            hooks.renderAgain(entries[(size_t) rows.front()].midi());
    };
    renameButton.onClick = [this] { renameSelected(); };
    deleteButton.setTooltip("Moves every file of the takes selected to the recycle bin");
    deleteButton.onClick = [this] { deleteSelected(); };
    showButton.onClick = [this] {
        if (const auto rows = selection(); !rows.empty() && !entries[(size_t) rows.front()].files.empty())
            entries[(size_t) rows.front()].files.front().revealToUser();
        else if (!folders.empty())
            folders.front().startAsProcess();
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
    timeLabel.setFont(theme::font(13.0f));
    nowLabel.setFont(theme::font(13.0f));
    note.setText("Takes: Ctrl+S saves the last thing played (Documents/PianoForge/Takes). Recordings: Ctrl+R "
                 "(Documents/PianoForge/Recordings). What is listened to here is heard only: not recorded again nor "
                 "streamed.",
                 juce::dontSendNotification);
    note.setColour(juce::Label::textColourId, theme::dim);
    note.setFont(theme::font(12.0f));
    for (auto* l : { &timeLabel, &nowLabel, &note })
        addAndMakeVisible(*l);

    refresh();
    setSize(920, 560);
    timerCallback(); // the playback state from the start, not a tick later
    startTimerHz(15);
}

TakesPanel::~TakesPanel()
{
    stopTimer();
}

void TakesPanel::refresh()
{
    // Every file of the folders, grouped by stem (a recording in several formats is one take).
    std::map<juce::String, Entry> byKey;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (const auto& folder : folders) {
        const bool takes = folder.getFileName() == "Takes";
        for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.wav;*.flac;*.mp3;*.opus;*.mid")) {
            const auto key = folder.getFullPathName() + "|" + stemOf(f);
            auto& e = byKey[key];
            if (e.files.empty()) {
                e.stem = stemOf(f);
                e.kind = takes ? "Take" : "Recording";
                e.folder = folder;
                e.time = f.getCreationTime();
            }
            e.files.push_back(f);
            e.time = std::min(e.time, f.getCreationTime());
        }
    }
    entries.clear();
    for (auto& [key, e] : byKey) {
        if (const auto a = e.audio(); a.existsAsFile())
            if (std::unique_ptr<juce::AudioFormatReader> r { formats.createReaderFor(a) }; r != nullptr && r->sampleRate > 0)
                e.seconds = (double) r->lengthInSamples / r->sampleRate;
        if (const auto m = e.midi(); m.existsAsFile()) {
            juce::MidiFile midi;
            if (juce::FileInputStream in(m); in.openedOk() && midi.readFrom(in, false)) {
                midi.convertTimestampTicksToSeconds();
                e.notes = 0;
                double last = 0.0;
                for (int t = 0; t < midi.getNumTracks(); ++t)
                    for (const auto* ev : *midi.getTrack(t)) {
                        e.notes += ev->message.isNoteOn() ? 1 : 0;
                        last = std::max(last, ev->message.getTimeStamp());
                    }
                if (e.seconds <= 0.0)
                    e.seconds = last;
            }
        }
        entries.push_back(std::move(e));
    }
    sortOrderChanged(sortColumn, sortForwards);
}

void TakesPanel::sortOrderChanged(int column, bool forwards)
{
    sortColumn = column;
    sortForwards = forwards;
    auto less = [column](const Entry& a, const Entry& b) {
        switch (column) {
        case name: return a.stem.compareNatural(b.stem) < 0;
        case kind: return a.kind < b.kind;
        case length: return a.seconds < b.seconds;
        case notes: return a.notes < b.notes;
        case files: return a.extensions() < b.extensions();
        default: return a.time < b.time;
        }
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) { return forwards ? less(a, b) : less(b, a); });
    table.updateContent();
    table.repaint();
    updateButtons();
}

void TakesPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? theme::accent.withAlpha(0.28f) : (row % 2 ? theme::panel : theme::panel.brighter(0.04f)));
}

void TakesPanel::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row < 0 || row >= (int) entries.size())
        return;
    const auto& e = entries[(size_t) row];
    juce::String text;
    switch (column) {
    case name: text = e.stem; break;
    case kind: text = e.kind; break;
    case date: text = e.time.formatted("%Y-%m-%d %H:%M"); break;
    case length: text = clock(e.seconds); break;
    case notes: text = e.notes >= 0 ? juce::String(e.notes) : juce::String("-"); break;
    case files: text = e.extensions(); break;
    default: break;
    }
    g.setColour(column == name ? theme::text : theme::dim);
    g.setFont(theme::font(13.0f));
    g.drawText(text, 6, 0, w - 8, h, column == length || column == notes ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

void TakesPanel::cellDoubleClicked(int, int, const juce::MouseEvent&)
{
    listen();
}

void TakesPanel::selectedRowsChanged(int)
{
    updateButtons();
}

std::vector<int> TakesPanel::selection() const
{
    std::vector<int> rows;
    const auto set = table.getSelectedRows();
    for (int i = 0; i < set.size(); ++i)
        if (set[i] >= 0 && set[i] < (int) entries.size())
            rows.push_back(set[i]);
    return rows;
}

void TakesPanel::updateButtons()
{
    const auto rows = selection();
    const bool one = rows.size() == 1;
    const bool audio = !rows.empty() && entries[(size_t) rows.front()].audio().existsAsFile();
    const bool midi = !rows.empty() && entries[(size_t) rows.front()].midi().existsAsFile();
    playButton.setEnabled(audio);
    abButton.setEnabled(rows.size() == 2 && entries[(size_t) rows[0]].audio().existsAsFile() && entries[(size_t) rows[1]].audio().existsAsFile());
    midiButton.setEnabled(one && midi);
    renderButton.setEnabled(one && midi);
    renameButton.setEnabled(one);
    deleteButton.setEnabled(!rows.empty());
}

void TakesPanel::listen()
{
    const auto rows = selection();
    if (rows.empty())
        return;
    const auto file = entries[(size_t) rows.front()].audio();
    if (const auto error = playback.load(file); error.isNotEmpty()) {
        nowLabel.setText(error, juce::dontSendNotification);
        return;
    }
    playback.play();
}

void TakesPanel::listenAB()
{
    const auto rows = selection();
    if (rows.size() != 2)
        return;
    if (const auto error = playback.load(entries[(size_t) rows[0]].audio(), entries[(size_t) rows[1]].audio()); error.isNotEmpty()) {
        nowLabel.setText(error, juce::dontSendNotification);
        return;
    }
    playback.play();
}

void TakesPanel::renameSelected()
{
    const auto rows = selection();
    if (rows.size() != 1)
        return;
    const auto entry = entries[(size_t) rows.front()];
    auto* window = new juce::AlertWindow("Rename the take", "A new name for every file of \"" + entry.stem + "\":",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", entry.stem, "Name");
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([this, entry, window](int result) {
        const auto wanted = juce::File::createLegalFileName(window->getTextEditorContents("name").trim());
        if (result != 1 || wanted.isEmpty() || wanted == entry.stem)
            return;
        for (const auto& f : entry.files) // none may be replaced
            if (entry.folder.getChildFile(f.getFileName().replaceFirstOccurrenceOf(entry.stem, wanted)).exists()) {
                nowLabel.setText("A take named \"" + wanted + "\" exists already", juce::dontSendNotification);
                return;
            }
        playback.release(); // (it may hold this take in memory; no file is open)
        for (const auto& f : entry.files)
            f.moveFileTo(entry.folder.getChildFile(f.getFileName().replaceFirstOccurrenceOf(entry.stem, wanted)));
        refresh();
    }), true);
}

void TakesPanel::deleteSelected()
{
    const auto rows = selection();
    if (rows.empty())
        return;
    std::vector<Entry> doomed;
    juce::StringArray names;
    for (int r : rows) {
        doomed.push_back(entries[(size_t) r]);
        names.add(entries[(size_t) r].stem);
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions {}
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle(rows.size() == 1 ? "Delete a take" : "Delete " + juce::String((int) rows.size()) + " takes")
                                     .withMessage("Move every file of " + names.joinIntoString(", ") + " to the recycle bin?")
                                     .withButton("Delete", 1)
                                     .withButton("Cancel", 0)
                                     .withAssociatedComponent(this),
                                 [safe = juce::Component::SafePointer<TakesPanel>(this), doomed](int result) {
                                     if (result == 0 || safe == nullptr)
                                         return;
                                     safe->playback.release();
                                     int failed = 0;
                                     for (const auto& e : doomed)
                                         for (const auto& f : e.files)
                                             failed += f.moveToTrash() ? 0 : 1;
                                     safe->refresh();
                                     safe->nowLabel.setText(failed == 0 ? juce::String("Moved to the recycle bin")
                                                                        : juce::String(failed) + " files could not be moved",
                                                            juce::dontSendNotification);
                                 });
}

juce::String TakesPanel::clock(double seconds) const
{
    const int s = juce::roundToInt(seconds);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

void TakesPanel::timerCallback()
{
    const double length = playback.lengthSeconds();
    if (length > 0.0 && std::abs(position.getMaximum() - length) > 1e-3)
        position.setRange(0.0, length, 0.0);
    if (!dragging)
        position.setValue(playback.positionSeconds(), juce::dontSendNotification);
    position.setEnabled(playback.isLoaded());
    timeLabel.setText(playback.isLoaded() ? clock(playback.positionSeconds()) + " / " + clock(length) : juce::String(),
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

bool TakesPanel::keyPressed(const juce::KeyPress& key)
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

void TakesPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
}

void TakesPanel::resized()
{
    auto area = getLocalBounds().reduced(12);
    note.setBounds(area.removeFromBottom(34));
    area.removeFromBottom(4);
    auto row = area.removeFromBottom(30);
    for (auto* b : { &midiButton, &renderButton, &renameButton, &deleteButton })
        b->setBounds(row.removeFromLeft(b == &midiButton ? 160 : 112).reduced(2, 0));
    refreshButton.setBounds(row.removeFromRight(90).reduced(2, 0));
    showButton.setBounds(row.removeFromRight(120).reduced(2, 0));
    area.removeFromBottom(6);
    row = area.removeFromBottom(30);
    for (auto* b : { &playButton, &stopButton, &abButton, &sideButton })
        b->setBounds(row.removeFromLeft(b == &sideButton ? 110 : 80).reduced(2, 0));
    row.removeFromLeft(8);
    timeLabel.setBounds(row.removeFromRight(100));
    position.setBounds(row);
    area.removeFromBottom(2);
    nowLabel.setBounds(area.removeFromBottom(22));
    area.removeFromBottom(6);
    table.setBounds(area);
}

} // namespace pf::app
