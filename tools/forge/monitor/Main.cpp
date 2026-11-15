// The forge monitor: follows the forge pipelines (forge pipeline, tools/forge/forge/pipeline.py) of every corpus state
// from their progress.json, once a second: each pipeline's steps, the one under way and how far it is, how long it
// should still take, and the end of its log.
//
//   PianoForgeForgeMonitor [corpus folder]   (default: the corpus folder above the working directory or the program)
//
// The time left: for the step under way, from its pace so far (items done against the time they took); for the steps
// to come, from the steps of the same kind already over (the modes of another layer...). A pipeline silent for more
// than three minutes is shown as such (stopped from outside, or the machine asleep).

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace {

namespace colours {
const juce::Colour background { 0xff1a1a19 }, card { 0xff242422 }, cardSelected { 0xff2c2c2a }, edge { 0xff3a3a37 };
const juce::Colour text { 0xfff0efec }, secondary { 0xffc3c2b7 }, muted { 0xff8d8c84 };
const juce::Colour running { 0xff3987e5 }, done { 0xff0ca30c }, failed { 0xffe66767 }, skipped { 0xff6f6e68 },
                   pending { 0xff3a3a37 }, silent { 0xffc98500 };
} // namespace colours

juce::FontOptions font(float height, bool bold = false)
{
    return juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain);
}

juce::String duration(double seconds)
{
    if (seconds < 0)
        return "?";
    const auto s = (int) std::lround(seconds);
    if (s < 60)
        return juce::String(s) + " s";
    if (s < 3600)
        return juce::String(s / 60) + " min " + juce::String(s % 60).paddedLeft('0', 2) + " s";
    return juce::String(s / 3600) + " h " + juce::String((s % 3600) / 60).paddedLeft('0', 2) + " min";
}

double now() { return (double) juce::Time::currentTimeMillis() / 1000.0; }

struct Step {
    juce::String name, status, phase, unit, error;
    double seconds = -1.0, started = 0.0, phaseStarted = 0.0;
    int done = 0, total = 0;

    juce::String kind() const { return name.upToFirstOccurrenceOf(" ", false, false); }
};

struct Pipeline {
    juce::File folder;
    juce::String state, status;
    double started = 0.0, updated = 0.0;
    std::vector<Step> steps;

    bool silent() const { return status == "running" && now() - updated > 180.0; }
    int finished() const
    {
        int n = 0;
        for (const auto& s : steps)
            n += s.status == "done" || s.status == "skipped" || s.status == "failed";
        return n;
    }
    const Step* current() const
    {
        for (const auto& s : steps)
            if (s.status == "running")
                return &s;
        return nullptr;
    }
};

double number(const juce::var& v, const char* key, double fallback = 0.0)
{
    const auto x = v.getProperty(key, fallback);
    return x.isVoid() ? fallback : (double) x;
}

bool read(const juce::File& file, Pipeline& p)
{
    const auto json = juce::JSON::parse(file.loadFileAsString());
    if (!json.isObject())
        return false;
    p.folder = file.getParentDirectory();
    p.state = json.getProperty("state", p.folder.getFileName()).toString();
    p.status = json.getProperty("status", "").toString();
    p.started = number(json, "started");
    p.updated = number(json, "updated");
    p.steps.clear();
    if (const auto* steps = json.getProperty("steps", {}).getArray())
        for (const auto& s : *steps) {
            Step step;
            step.name = s.getProperty("name", "").toString();
            step.status = s.getProperty("status", "pending").toString();
            step.phase = s.getProperty("phase", "").toString();
            step.unit = s.getProperty("unit", "").toString();
            step.error = s.getProperty("error", "").toString();
            step.seconds = number(s, "seconds", -1.0);
            step.started = number(s, "started");
            step.phaseStarted = number(s, "phase_started", step.started);
            step.done = (int) number(s, "done");
            step.total = (int) number(s, "total");
            p.steps.push_back(step);
        }
    return true;
}

/// The time the step under way still needs, and the whole pipeline (-1: unknown).
std::pair<double, double> timeLeft(const Pipeline& p)
{
    std::map<juce::String, std::pair<double, int>> byKind; // kind -> (seconds, count) of the steps done
    for (const auto& s : p.steps)
        if (s.status == "done" && s.seconds >= 0) {
            auto& k = byKind[s.kind()];
            k.first += s.seconds;
            ++k.second;
        }
    auto average = [&](const Step& s) {
        const auto it = byKind.find(s.kind());
        return it == byKind.end() ? -1.0 : it->second.first / it->second.second;
    };
    double step = -1.0, total = 0.0;
    bool known = true;
    for (const auto& s : p.steps) {
        if (s.status == "running") {
            const double t = now();
            if (s.done > 0 && s.total > 0)
                step = (t - s.phaseStarted) / s.done * (s.total - s.done);
            else if (average(s) >= 0)
                step = std::max(0.0, average(s) - (t - s.started));
            if (step < 0)
                known = false;
            else
                total += step;
        } else if (s.status == "pending") {
            const double a = average(s);
            if (a < 0)
                known = false;
            else
                total += a;
        }
    }
    return { step, known ? total : -1.0 };
}

class PipelineCard final : public juce::Component, public juce::TooltipClient {
public:
    std::function<void()> onSelect;
    Pipeline pipeline;
    bool selected = false;

    void mouseUp(const juce::MouseEvent&) override
    {
        if (onSelect)
            onSelect();
    }

    juce::String getTooltip() override
    {
        const auto x = getMouseXYRelative().x;
        if (stepsArea.contains(getMouseXYRelative()) && !pipeline.steps.empty()) {
            const auto i = juce::jlimit(0, (int) pipeline.steps.size() - 1,
                                        (int) ((x - stepsArea.getX()) * (int) pipeline.steps.size() / std::max(1, stepsArea.getWidth())));
            const auto& s = pipeline.steps[(std::size_t) i];
            return s.name + ": " + s.status + (s.seconds >= 0 ? " (" + duration(s.seconds) + ")" : juce::String())
                   + (s.error.isNotEmpty() ? "\n" + s.error : juce::String());
        }
        return {};
    }

    void paint(juce::Graphics& g) override
    {
        const auto& p = pipeline;
        auto r = getLocalBounds().reduced(6);
        g.setColour(selected ? colours::cardSelected : colours::card);
        g.fillRoundedRectangle(r.toFloat(), 8.0f);
        g.setColour(colours::edge);
        g.drawRoundedRectangle(r.toFloat(), 8.0f, 1.0f);
        r = r.reduced(14, 10);

        // Header: the state, its status, steps and time.
        auto header = r.removeFromTop(24);
        g.setColour(colours::text);
        g.setFont(font(17.0f, true));
        g.drawText(p.state, header.removeFromLeft(220), juce::Justification::centredLeft);
        const bool silent = p.silent();
        const auto status = silent ? "silent for " + duration(now() - p.updated) : p.status;
        const auto statusColour = silent ? colours::silent
                                 : p.status == "done"   ? colours::done
                                 : p.status == "failed" ? colours::failed
                                 : p.status == "running" ? colours::running : colours::skipped;
        auto chip = header.removeFromLeft(juce::jmax(90, juce::GlyphArrangement::getStringWidthInt(juce::Font(font(13.0f, true)), status) + 20));
        g.setColour(statusColour.withAlpha(0.18f));
        g.fillRoundedRectangle(chip.reduced(0, 2).toFloat(), 10.0f);
        g.setColour(statusColour);
        g.setFont(font(13.0f, true));
        g.drawText(status, chip, juce::Justification::centred);
        const auto [stepLeft, totalLeft] = timeLeft(p);
        g.setColour(colours::secondary);
        g.setFont(font(13.0f));
        juce::String times = "steps " + juce::String(p.finished()) + " / " + juce::String((int) p.steps.size())
                             + "   elapsed " + duration((p.status == "running" ? now() : p.updated) - p.started);
        if (p.status == "running")
            times << "   left " << (totalLeft >= 0 ? "~" + duration(totalLeft) : juce::String("?"));
        g.drawText(times, header, juce::Justification::centredRight);

        r.removeFromTop(8);
        // The step under way, its phase and progress.
        auto line = r.removeFromTop(20);
        g.setFont(font(14.0f));
        if (const auto* s = p.current()) {
            g.setColour(colours::text);
            juce::String what = s->name;
            if (s->phase.isNotEmpty())
                what << "  -  " << s->phase;
            if (s->total > 0)
                what << ": " << s->done << " / " << s->total << " " << s->unit;
            g.drawText(what, line, juce::Justification::centredLeft);
            g.setColour(colours::secondary);
            g.drawText(stepLeft >= 0 ? "step: ~" + duration(stepLeft) + " left" : juce::String("step: ") + duration(now() - s->started),
                       line, juce::Justification::centredRight);
            auto bar = r.removeFromTop(10).toFloat();
            g.setColour(colours::pending);
            g.fillRoundedRectangle(bar, 5.0f);
            if (s->total > 0) {
                g.setColour(colours::running);
                g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * (float) s->done / (float) s->total), 5.0f);
            }
        } else {
            juce::String what = p.status == "done" ? "every step done" : p.status;
            for (const auto& step : p.steps)
                if (step.status == "failed")
                    what = step.name + " failed: " + step.error;
            g.setColour(p.status == "failed" ? colours::failed : colours::secondary);
            g.drawText(what, line, juce::Justification::centredLeft);
            r.removeFromTop(10);
        }
        r.removeFromTop(10);
        // Every step, in order: one segment each.
        stepsArea = r.removeFromTop(12);
        const int count = (int) p.steps.size();
        for (int i = 0; i < count; ++i) {
            const auto& s = p.steps[(std::size_t) i];
            const int x0 = stepsArea.getX() + stepsArea.getWidth() * i / count;
            const int x1 = stepsArea.getX() + stepsArea.getWidth() * (i + 1) / count;
            const auto c = s.status == "done" ? colours::done
                         : s.status == "running" ? colours::running
                         : s.status == "failed" ? colours::failed
                         : s.status == "skipped" ? colours::skipped : colours::pending;
            g.setColour(c);
            g.fillRoundedRectangle(juce::Rectangle<int>(x0, stepsArea.getY(), std::max(1, x1 - x0 - 2), stepsArea.getHeight()).toFloat(), 3.0f);
        }
    }

private:
    juce::Rectangle<int> stepsArea;
};

class MonitorComponent final : public juce::Component, private juce::Timer {
public:
    explicit MonitorComponent(juce::File corpusFolder) : corpus(std::move(corpusFolder))
    {
        addAndMakeVisible(title);
        title.setFont(font(15.0f, true));
        title.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(choose);
        choose.onClick = [this] { chooseCorpus(); };
        addAndMakeVisible(openFolder);
        openFolder.onClick = [this] {
            if (const auto* c = selectedCard())
                c->pipeline.folder.startAsProcess();
        };
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&list, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(log);
        log.setMultiLine(true);
        log.setReadOnly(true);
        log.setScrollbarsShown(true);
        log.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        log.setColour(juce::TextEditor::backgroundColourId, colours::card);
        log.setColour(juce::TextEditor::textColourId, colours::secondary);
        log.setColour(juce::TextEditor::outlineColourId, colours::edge);
        addAndMakeVisible(empty);
        empty.setJustificationType(juce::Justification::centred);
        empty.setColour(juce::Label::textColourId, colours::muted);
        refresh();
        startTimer(1000);
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        auto top = r.removeFromTop(30);
        openFolder.setBounds(top.removeFromRight(130));
        top.removeFromRight(8);
        choose.setBounds(top.removeFromRight(150));
        title.setBounds(top);
        r.removeFromTop(8);
        log.setBounds(r.removeFromBottom(juce::jmax(140, r.getHeight() / 3)));
        r.removeFromBottom(8);
        viewport.setBounds(r);
        empty.setBounds(r);
        layoutCards();
    }

private:
    juce::File corpus;
    juce::Label title, empty;
    juce::TextButton choose { "Choose corpus..." }, openFolder { "Open its folder" };
    juce::Viewport viewport;
    juce::Component list;
    juce::TextEditor log;
    std::vector<std::unique_ptr<PipelineCard>> cards;
    juce::String selectedState;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 300 };

    PipelineCard* selectedCard() const
    {
        for (const auto& c : cards)
            if (c->pipeline.state == selectedState)
                return c.get();
        return nullptr;
    }

    void chooseCorpus()
    {
        chooser = std::make_unique<juce::FileChooser>("The corpus folder", corpus);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser& fc) {
                                 if (fc.getResult().isDirectory()) {
                                     corpus = fc.getResult();
                                     refresh();
                                 }
                             });
    }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        title.setText("Forge pipelines in " + corpus.getFullPathName(), juce::dontSendNotification);
        std::vector<Pipeline> found;
        for (const auto& session : corpus.findChildFiles(juce::File::findDirectories, false))
            for (const auto& state : session.findChildFiles(juce::File::findDirectories, false)) {
                const auto file = state.getChildFile("progress.json");
                Pipeline p;
                if (file.existsAsFile() && read(file, p))
                    found.push_back(std::move(p));
            }
        std::sort(found.begin(), found.end(), [](const Pipeline& a, const Pipeline& b) {
            const bool ra = a.status == "running", rb = b.status == "running";
            return ra != rb ? ra : a.updated > b.updated;
        });
        while (cards.size() > found.size()) {
            list.removeChildComponent(cards.back().get());
            cards.pop_back();
        }
        while (cards.size() < found.size()) {
            auto card = std::make_unique<PipelineCard>();
            auto* raw = card.get();
            card->onSelect = [this, raw] {
                selectedState = raw->pipeline.state;
                refresh();
            };
            list.addAndMakeVisible(*card);
            cards.push_back(std::move(card));
        }
        if (selectedState.isEmpty() && !found.empty())
            selectedState = found.front().state;
        for (std::size_t i = 0; i < found.size(); ++i) {
            cards[i]->pipeline = std::move(found[i]);
            cards[i]->selected = cards[i]->pipeline.state == selectedState;
            cards[i]->repaint();
        }
        empty.setText(cards.empty() ? "No forge pipeline in this corpus yet (forge pipeline --corpus corpus/reference/<state>)"
                                    : juce::String(), juce::dontSendNotification);
        empty.setVisible(cards.empty());
        layoutCards();
        showLog();
    }

    void layoutCards()
    {
        const int height = 118, width = viewport.getMaximumVisibleWidth();
        list.setSize(width, (int) cards.size() * height);
        for (std::size_t i = 0; i < cards.size(); ++i)
            cards[i]->setBounds(0, (int) i * height, width, height);
    }

    void showLog()
    {
        const auto* c = selectedCard();
        if (c == nullptr) {
            log.setText({}, false);
            return;
        }
        const auto file = c->pipeline.folder.getChildFile("forge-log.txt");
        juce::StringArray lines;
        lines.addLines(file.loadFileAsString());
        lines.removeEmptyStrings();
        const int from = juce::jmax(0, lines.size() - 40);
        juce::StringArray tail;
        for (int i = from; i < lines.size(); ++i)
            tail.add(lines[i]);
        const auto text = c->pipeline.state + "  -  " + file.getFullPathName() + "\n" + tail.joinIntoString("\n");
        if (text != log.getText()) {
            log.setText(text, false);
            log.moveCaretToEnd();
        }
    }
};

/// The corpus folder: the argument, or a folder named corpus (with a reference session) above the working directory
/// or the program.
juce::File findCorpus(const juce::StringArray& args)
{
    for (const auto& a : args)
        if (juce::File(a).isDirectory())
            return juce::File(a);
    for (auto dir : { juce::File::getCurrentWorkingDirectory(),
                      juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory() }) {
        for (int up = 0; up < 8 && dir.exists(); ++up, dir = dir.getParentDirectory()) {
            const auto candidate = dir.getChildFile("corpus");
            if (candidate.getChildFile("reference").isDirectory())
                return candidate;
            if (dir.getFileName() == "corpus" && dir.getChildFile("reference").isDirectory())
                return dir;
        }
    }
    return juce::File::getCurrentWorkingDirectory();
}

class Application final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "PianoForge forge monitor"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        auto& laf = juce::LookAndFeel::getDefaultLookAndFeel();
        laf.setColour(juce::TextButton::buttonColourId, colours::card);
        laf.setColour(juce::TextButton::textColourOffId, colours::text);
        laf.setColour(juce::ResizableWindow::backgroundColourId, colours::background);
        window = std::make_unique<Window>(findCorpus(getCommandLineParameterArray()));
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class Window final : public juce::DocumentWindow {
    public:
        explicit Window(juce::File corpus)
            : DocumentWindow("PianoForge - forge monitor", colours::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MonitorComponent(std::move(corpus)), false);
            setResizable(true, true);
            setResizeLimits(640, 420, 4000, 3000);
            centreWithSize(980, 680);
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};

} // namespace

START_JUCE_APPLICATION(Application)
