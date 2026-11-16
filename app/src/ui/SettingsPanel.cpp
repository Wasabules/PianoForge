#include "SettingsPanel.h"

#include "Theme.h"

namespace pf::app {

namespace {

constexpr int kUserId = 1000;      // the combo box's ids: factory presets from 1, the user's from kUserId
constexpr int kBarHeight = 64, kSideWidth = 210;

} // namespace

juce::String SettingsPanel::title(Section s)
{
    switch (s) {
    case Section::engine: return "Engine";
    case Section::tone: return "Tone and dynamics";
    case Section::room: return "Room";
    case Section::audio: return "Audio, MIDI and streaming";
    case Section::gestures: return "Piano gestures";
    case Section::streamLook: return "Stream look (OBS)";
    case Section::count: break;
    }
    return {};
}

SettingsPanel::SettingsPanel(std::array<juce::Component*, (size_t) Section::count> p, std::array<bool, (size_t) Section::count> f,
                             Presets pr)
    : panels(p), fills(f), presets(std::move(pr))
{
    for (size_t i = 0; i < panels.size(); ++i)
        naturalHeight[i] = panels[i] != nullptr ? panels[i]->getHeight() : 0;

    presetLabel.setColour(juce::Label::textColourId, theme::dim);
    presetList.setTooltip("A factory preset sets the sound (the engine's models, the voicing, the touch, the tone, the room): "
                          "the tuning, the volume and the curves stay. A preset of yours holds the whole instrument.");
    presetList.onChange = [this] {
        const int id = presetList.getSelectedId();
        if (id >= kUserId)
            presets.applyUser(id - kUserId);
        else if (id >= 1)
            presets.applyFactory(id - 1);
        refreshPresets();
    };
    state.setColour(juce::Label::textColourId, theme::busy);
    description.setColour(juce::Label::textColourId, theme::dim);
    description.setFont(juce::FontOptions(13.0f));
    revert.setTooltip("The preset again, as it was saved (what was changed since is lost)");
    revert.onClick = [this] {
        presets.revert();
        refreshPresets();
    };
    save.setTooltip("Your preset replaced by the instrument as it sounds now");
    save.onClick = [this] {
        presets.save();
        refreshPresets();
    };
    saveAs.setTooltip("The whole instrument as it sounds now, under a name: one of your presets");
    saveAs.onClick = [this] { presets.saveAs(); };
    remove.setTooltip("Delete this preset of yours (the instrument as it sounds now stays)");
    remove.onClick = [this] { presets.remove(); };
    for (auto* c : std::initializer_list<juce::Component*> { &presetLabel, &presetList, &state, &description, &revert, &save, &saveAs, &remove })
        addAndMakeVisible(*c);

    for (size_t i = 0; i < sectionButtons.size(); ++i) {
        auto& b = sectionButtons[i];
        b.setButtonText(title((Section) i));
        b.setClickingTogglesState(true);
        b.setRadioGroupId(1);
        b.setConnectedEdges(juce::Button::ConnectedOnTop | juce::Button::ConnectedOnBottom);
        b.onClick = [this, i] {
            if (sectionButtons[i].getToggleState())
                show((Section) i);
        };
        addAndMakeVisible(b);
    }
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    refreshPresets();
    show(Section::audio);
    const auto screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const int height = screen != nullptr ? juce::jmin(900, (int) screen->userBounds.getHeight() - 60) : 860;
    setSize(1030, height);
    startTimerHz(2);
}

SettingsPanel::~SettingsPanel()
{
    stopTimer();
    viewport.setViewedComponent(nullptr, false); // the panels are owned elsewhere
}

void SettingsPanel::show(Section s)
{
    current = s;
    sectionButtons[(size_t) s].setToggleState(true, juce::dontSendNotification);
    viewport.setViewedComponent(panels[(size_t) s], false);
    viewport.setViewPosition(0, 0);
    layoutSection();
}

void SettingsPanel::layoutSection()
{
    auto* panel = panels[(size_t) current];
    if (panel == nullptr || viewport.getWidth() <= 0)
        return;
    if (fills[(size_t) current]) {
        viewport.setScrollBarsShown(false, false);
        panel->setSize(viewport.getWidth(), viewport.getHeight());
    } else {
        viewport.setScrollBarsShown(true, false);
        panel->setSize(viewport.getWidth() - viewport.getScrollBarThickness(), naturalHeight[(size_t) current]);
    }
}

void SettingsPanel::refreshPresets()
{
    presetList.clear(juce::dontSendNotification);
    const auto factory = presets.factoryNames(), user = presets.userNames();
    presetList.addSectionHeading("Factory presets");
    for (int i = 0; i < factory.size(); ++i)
        presetList.addItem(factory[i], 1 + i);
    if (!user.isEmpty()) {
        presetList.addSeparator();
        presetList.addSectionHeading("My presets");
        for (int i = 0; i < user.size(); ++i)
            presetList.addItem(user[i], kUserId + i);
    }
    const auto name = presets.current();
    const bool factoryCurrent = presets.currentIsFactory();
    int selected = 0;
    if (factoryCurrent) {
        selected = factory.indexOf(name) >= 0 ? 1 + factory.indexOf(name) : 0;
    } else if (user.indexOf(name) >= 0) {
        selected = kUserId + user.indexOf(name);
    }
    presetList.setSelectedId(selected, juce::dontSendNotification);
    if (selected == 0)
        presetList.setText(name.isEmpty() ? "No preset" : name, juce::dontSendNotification);
    description.setText(factoryCurrent && selected > 0 ? presets.factoryDescription(selected - 1)
                        : selected >= kUserId        ? juce::String("Your preset: the whole instrument (engine, tuning, curves, volume and sound).")
                                                     : juce::String("The settings as they are, not saved as a preset."),
                        juce::dontSendNotification);
    save.setEnabled(selected >= kUserId);
    remove.setEnabled(selected >= kUserId);
    revert.setEnabled(selected > 0);
    lastModified = !presets.modified(); // shown again by the timer
    timerCallback();
}

void SettingsPanel::timerCallback()
{
    const bool modified = presetList.getSelectedId() > 0 && presets.modified();
    if (modified == lastModified)
        return;
    lastModified = modified;
    state.setText(modified ? "modified" : juce::String(), juce::dontSendNotification);
}

void SettingsPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    g.setColour(theme::panel);
    g.fillRect(barArea);
    g.fillRect(sideArea);
    g.setColour(theme::panelEdge);
    g.drawHorizontalLine(barArea.getBottom() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine(sideArea.getRight() - 1, (float) sideArea.getY(), (float) sideArea.getBottom());
}

void SettingsPanel::resized()
{
    auto area = getLocalBounds();
    barArea = area.removeFromTop(kBarHeight);
    auto bar = barArea.reduced(12, 8);
    auto top = bar.removeFromTop(26);
    presetLabel.setBounds(top.removeFromLeft(56));
    presetList.setBounds(top.removeFromLeft(320));
    top.removeFromLeft(8);
    state.setBounds(top.removeFromLeft(80));
    for (auto* b : { &remove, &saveAs, &save, &revert }) {
        b->setBounds(top.removeFromRight(b == &saveAs ? 96 : 76));
        top.removeFromRight(6);
    }
    bar.removeFromTop(4);
    description.setBounds(bar.withTrimmedLeft(56));

    sideArea = area.removeFromLeft(kSideWidth);
    auto side = sideArea.reduced(8, 12);
    for (auto& b : sectionButtons) {
        b.setBounds(side.removeFromTop(34));
        side.removeFromTop(4);
    }
    viewport.setBounds(area);
    layoutSection();
}

} // namespace pf::app
