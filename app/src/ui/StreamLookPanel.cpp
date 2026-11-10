#include "StreamLookPanel.h"

#include "Theme.h"

#include <pianoforge/audio_feed.hpp>

#include <cmath>
#include <cstring>

namespace pf::app {

namespace {
constexpr std::uint64_t kAskAheadNs = 1'500'000'000ull; // the preview asked for this far ahead, renewed while shown
constexpr std::uint64_t kStaleNs = 1'500'000'000ull;    // no frame for this long: no preview
} // namespace

/// The keyboard as OBS renders it, over a dark ground (where it is transparent, the scene behind would show).
class StreamLookPanel::Preview final : public juce::Component {
public:
    void take(const std::vector<std::uint8_t>& pixels, std::uint32_t w, std::uint32_t h)
    {
        if (!image.isValid() || (std::uint32_t) image.getWidth() != w || (std::uint32_t) image.getHeight() != h)
            image = juce::Image(juce::Image::ARGB, (int) w, (int) h, false);
        {
            juce::Image::BitmapData data(image, juce::Image::BitmapData::writeOnly);
            for (std::uint32_t y = 0; y < h; ++y) // OBS's BGRA, premultiplied: JUCE's ARGB in memory
                std::memcpy(data.getLinePointer((int) y), pixels.data() + (std::size_t) y * w * 4, (std::size_t) w * 4);
        }
        lastNs = pf::feedClockNs();
        repaint();
    }
    bool live() const { return lastNs > 0 && pf::feedClockNs() - lastNs < kStaleNs; }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff15181d), area.getCentreX(), area.getY(), juce::Colour(0xff0a0b0e),
                                               area.getCentreX(), area.getBottom(), false));
        g.fillRoundedRectangle(area, 6.0f);
        if (live() && image.isValid()) {
            const auto placed = juce::RectanglePlacement(juce::RectanglePlacement::centred)
                                    .appliedTo(image.getBounds().toFloat(), area.reduced(8.0f));
            g.drawImage(image, placed);
        } else {
            g.setColour(theme::dim);
            g.setFont(theme::font(14.0f));
            g.drawFittedText("The preview is OBS's own rendering: start OBS (with the PianoForge plugin) to see it here.\n"
                             "What you set here reaches OBS's keyboards as soon as it runs.",
                             getLocalBounds().reduced(24), juce::Justification::centred, 3);
        }
        g.setColour(theme::panelEdge);
        g.drawRoundedRectangle(area.reduced(0.5f), 6.0f, 1.0f);
    }

private:
    juce::Image image;
    std::uint64_t lastNs = 0;
};

/// The settings of one page: a row each (its label, its control), the rows that do not apply hidden.
class StreamLookPanel::Page final : public juce::Component {
public:
    Page(StreamLookPanel& owner, pf::look::Page page) : panel(owner)
    {
        const auto& entries = pf::look::entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            if (e.page != page)
                continue;
            auto row = std::make_unique<Row>();
            row->index = i;
            const juce::String help = juce::String::fromUTF8(e.help);
            if (e.kind == pf::look::Kind::toggle) {
                row->toggle = std::make_unique<juce::ToggleButton>(juce::String::fromUTF8(e.label));
                row->toggle->setTooltip(help);
                row->toggle->onClick = [this, i, t = row->toggle.get()] { changed(i, t->getToggleState() ? 1.0 : 0.0); };
                addAndMakeVisible(*row->toggle);
            } else {
                row->label.setText(juce::String::fromUTF8(e.label), juce::dontSendNotification);
                row->label.setTooltip(help);
                row->label.setColour(juce::Label::textColourId, theme::text);
                addAndMakeVisible(row->label);
                if (e.kind == pf::look::Kind::choice) {
                    row->combo = std::make_unique<juce::ComboBox>();
                    for (std::size_t c = 0; c < e.choices.size(); ++c)
                        row->combo->addItem(juce::String::fromUTF8(e.choices[c].label.c_str()), (int) c + 1);
                    row->combo->setTooltip(help);
                    row->combo->onChange = [this, i, c = row->combo.get()] {
                        const auto& choices = pf::look::entries()[i].choices;
                        const int at = c->getSelectedId() - 1;
                        if (at >= 0 && at < (int) choices.size())
                            changed(i, choices[(std::size_t) at].value);
                    };
                    addAndMakeVisible(*row->combo);
                } else {
                    row->slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                    row->slider->setRange(e.min, e.max, e.step);
                    row->slider->setTextValueSuffix(juce::String::fromUTF8(e.suffix));
                    row->slider->setDoubleClickReturnValue(true, e.def);
                    row->slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 22);
                    row->slider->setTooltip(help);
                    row->slider->onValueChange = [this, i, s = row->slider.get()] { changed(i, s->getValue()); };
                    addAndMakeVisible(*row->slider);
                }
            }
            rows.push_back(std::move(row));
        }
    }

    /// The controls brought to `values`, the rows that do not apply hidden.
    void show(const pf::look::Values& values)
    {
        const auto& entries = pf::look::entries();
        for (auto& row : rows) {
            const auto& e = entries[row->index];
            const double v = values.at(row->index);
            if (row->toggle != nullptr)
                row->toggle->setToggleState(v >= 0.5, juce::dontSendNotification);
            if (row->slider != nullptr)
                row->slider->setValue(v, juce::dontSendNotification);
            if (row->combo != nullptr)
                for (std::size_t c = 0; c < e.choices.size(); ++c)
                    if (e.choices[c].value == (int) std::lround(v))
                        row->combo->setSelectedId((int) c + 1, juce::dontSendNotification);
            row->shown = values.shows(e);
        }
        resized();
    }

    int height() const
    {
        int h = 12;
        for (const auto& row : rows)
            h += row->shown ? kRow : 0;
        return h + 8;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16, 10);
        for (auto& row : rows) {
            for (juce::Component* c : { (juce::Component*) &row->label, (juce::Component*) row->toggle.get(), (juce::Component*) row->slider.get(),
                                        (juce::Component*) row->combo.get() })
                if (c != nullptr)
                    c->setVisible(row->shown && (c != &row->label || row->toggle == nullptr));
            if (!row->shown)
                continue;
            auto line = area.removeFromTop(kRow).withTrimmedBottom(6);
            if (row->toggle != nullptr) {
                row->toggle->setBounds(line);
                continue;
            }
            row->label.setBounds(line.removeFromLeft(230));
            if (row->slider != nullptr)
                row->slider->setBounds(line);
            if (row->combo != nullptr)
                row->combo->setBounds(line.removeFromLeft(std::min(line.getWidth(), 320)));
        }
    }

private:
    static constexpr int kRow = 34;
    struct Row {
        std::size_t index = 0;
        juce::Label label;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ComboBox> combo;
        bool shown = true;
    };

    void changed(std::size_t index, double value)
    {
        panel.look.set(index, value);
        panel.edited();
    }

    StreamLookPanel& panel;
    std::vector<std::unique_ptr<Row>> rows;
};

StreamLookPanel::StreamLookPanel(StreamLook& l) : look(l)
{
    for (const auto& s : pf::look::styles())
        style.addItem(juce::String::fromUTF8(s.label.c_str()), s.value + 1);
    style.setSelectedId(pf::look::kCustom + 1, juce::dontSendNotification);
    style.setTooltip("A whole look at once (the size, the keys shown and the timing kept). Change anything after: the style is a "
                     "starting point.");
    style.onChange = [this] {
        const int chosen = style.getSelectedId() - 1;
        if (chosen <= pf::look::kCustom)
            return;
        pf::look::Values values = look.values();
        pf::look::applyStyle(values, chosen);
        look.setAll(values);
        refresh();
    };
    addAndMakeVisible(style);
    reset.setTooltip("Every setting back to its default, the width kept.");
    reset.onClick = [this] {
        pf::look::Values values;
        values.set("width", look.values().get("width"));
        look.setAll(values);
        style.setSelectedId(pf::look::kPianoForge + 1, juce::dontSendNotification);
        refresh();
    };
    addAndMakeVisible(reset);
    status.setFont(theme::font(13.0f));
    status.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(status);
    preview = std::make_unique<Preview>();
    addAndMakeVisible(*preview);

    tabs.setTabBarDepth(30);
    tabs.setOutline(0);
    for (int p = 0; p < pf::look::kPages; ++p) {
        auto page = std::make_unique<Page>(*this, (pf::look::Page) p);
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent(page.get(), false);
        viewport->setScrollBarsShown(true, false);
        tabs.addTab(pf::look::pageName((pf::look::Page) p), theme::panel, viewport, true);
        pages.push_back(std::move(page));
    }
    addAndMakeVisible(tabs);
    refresh();
    setSize(940, 820);
    startTimerHz(30);
}

StreamLookPanel::~StreamLookPanel()
{
    stopTimer();
    tabs.clearTabs(); // the viewports, before the pages they show
}

void StreamLookPanel::refresh()
{
    for (auto& page : pages)
        page->show(look.values());
    resized();
}

void StreamLookPanel::edited()
{
    style.setSelectedId(pf::look::kCustom + 1, juce::dontSendNotification);
    for (auto& page : pages)
        page->show(look.values()); // (a setting shows others: the hues by the colour mode...)
    resized();
}

void StreamLookPanel::timerCallback()
{
    if (!isShowing())
        return;
    auto& channel = look.channel();
    const float scale = (float) juce::Desktop::getInstance().getGlobalScaleFactor()
                        * (float) (getPeer() != nullptr ? getPeer()->getPlatformScaleFactor() : 1.0);
    channel.wantPreview(pf::feedClockNs() + kAskAheadNs, (std::uint32_t) std::lround(preview->getWidth() * scale),
                        (std::uint32_t) std::lround(preview->getHeight() * scale));
    std::uint32_t w = 0, h = 0;
    std::uint64_t made = 0;
    std::vector<std::uint8_t> pixels;
    if (channel.frame(frameSerial, pixels, w, h, made))
        preview->take(pixels, w, h);
    if (++ticks % 15 == 0) {
        const bool live = preview->live();
        status.setText(live ? "OBS: in sync, preview live" : look.obsSeen() ? "OBS: in sync" : "OBS not seen yet: it takes this look when it starts",
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, live || look.obsSeen() ? theme::good : theme::dim);
        if (!live)
            preview->repaint();
    }
}

void StreamLookPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
}

void StreamLookPanel::resized()
{
    auto area = getLocalBounds().reduced(14, 12);
    auto top = area.removeFromTop(30);
    style.setBounds(top.removeFromLeft(260));
    top.removeFromLeft(8);
    reset.setBounds(top.removeFromLeft(170));
    status.setBounds(top);
    area.removeFromTop(10);
    preview->setBounds(area.removeFromTop(juce::jmax(160, area.getHeight() * 2 / 5)));
    area.removeFromTop(10);
    tabs.setBounds(area);
    for (auto& page : pages) {
        auto* viewport = page->findParentComponentOfClass<juce::Viewport>();
        const int width = viewport != nullptr && viewport->getWidth() > 0 ? viewport->getWidth() : tabs.getWidth();
        page->setSize(juce::jmax(200, width - (viewport != nullptr ? viewport->getScrollBarThickness() : 8) - 2), page->height());
    }
}

} // namespace pf::app
