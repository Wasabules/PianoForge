// SPDX-License-Identifier: AGPL-3.0-or-later

// The forge monitor's profile builder: a sample bank to a modal profile, by the project's tool (pianoforge-profile,
// tools/profile), started here in the background, detached (closing the monitor leaves it running; its stages in
// build.json and its log in build-log.txt, beside the profile, are what the monitor shows).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace builder {

/// The project's Python environment, which has the bench, the forge and the profile tool: found above the corpus
/// folder or the program (<project>/.venv).
struct Toolchain {
    juce::File python, project;
    bool found() const { return python.existsAsFile(); }

    static Toolchain find(const juce::File& corpus)
    {
        for (auto dir : { corpus, juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory(),
                          juce::File::getCurrentWorkingDirectory() })
            for (int up = 0; up < 10 && dir.exists(); ++up, dir = dir.getParentDirectory())
                if (dir.getChildFile("tools/profile/pfprofile").isDirectory()) {
                    Toolchain t;
                    t.project = dir;
                   #if JUCE_WINDOWS
                    t.python = dir.getChildFile(".venv/Scripts/python.exe");
                   #else
                    t.python = dir.getChildFile(".venv/bin/python");
                   #endif
                    if (t.found())
                        return t;
                }
        return {};
    }

    /// The tool's command line: python -m pfprofile.cli --corpus <corpus> <args>.
    juce::StringArray command(const juce::File& corpus, const juce::StringArray& args, bool quiet = false) const
    {
        juce::StringArray c { python.getFullPathName(), "-m", "pfprofile.cli" };
        if (quiet)
            c.add("--quiet");
        c.addArray({ "--corpus", corpus.getFullPathName() });
        c.addArray(args);
        return c;
    }

    /// Runs the tool and returns what it printed (a short command: banks, stop).
    juce::String run(const juce::File& corpus, const juce::StringArray& args, int timeoutMs = 30000) const
    {
        juce::ChildProcess p;
        if (!p.start(command(corpus, args), juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return {};
        auto out = p.readAllProcessOutput();
        p.waitForProcessToFinish(timeoutMs);
        return out;
    }

    /// Starts a build in the background, detached: nothing read from it (it writes its own log).
    bool launch(const juce::File& corpus, const juce::StringArray& args) const
    {
        auto p = std::make_unique<juce::ChildProcess>();
        if (!p->start(command(corpus, args, true), 0))
            return false;
        p.release(); // (a ChildProcess deleted leaves its process running; this one is never waited for)
        return true;
    }
};

/// What a build is asked for: a bank the tool knows, or any SFZ piano; its options.
class BuildDialog final : public juce::Component {
public:
    std::function<void(const juce::StringArray& args, const juce::String& state)> onBuild;

    BuildDialog(const Toolchain& t, const juce::File& corpusFolder) : tools(t), corpus(corpusFolder)
    {
        answer = tools.run(corpus, { "banks", "--json" });
        if (const auto* list = juce::JSON::parse(answer).getArray())
            for (const auto& b : *list)
                banks.add(b);
        heading.setText("Build a modal profile from a sample bank", juce::dontSendNotification);
        heading.setFont(juce::FontOptions(17.0f, juce::Font::bold));
        bankLabel.setText("Bank", juce::dontSendNotification);
        for (int i = 0; i < banks.size(); ++i)
            bank.addItem(banks[i].getProperty("name", "").toString() + " (" + banks[i].getProperty("piano", "").toString() + ")  -  "
                             + banks[i].getProperty("status", "").toString(),
                         i + 1);
        bank.addItem("Another SFZ piano...", kOther);
        bank.onChange = [this] { update(); };
        bank.setSelectedId(banks.isEmpty() ? kOther : 1, juce::dontSendNotification);
        chooseFile.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser>("An SFZ piano", corpus.getChildFile("sources"), "*.sfz");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& fc) {
                                     if (fc.getResult().existsAsFile()) {
                                         sfz = fc.getResult();
                                         if (state.isEmpty())
                                             state.setText(sfz.getFileNameWithoutExtension().toLowerCase().replaceCharacters(" _", "--")
                                                               .retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789-"),
                                                           juce::dontSendNotification);
                                         update();
                                     }
                                 });
        };
        for (const auto& [label, text] : std::initializer_list<std::pair<juce::Label*, const char*>> {
                 { &stateLabel, "State name" }, { &licenseLabel, "Licence" }, { &authorLabel, "Author" }, { &coresLabel, "Cores" } })
            label->setText(text, juce::dontSendNotification);
        state.setTextToShowWhenEmpty("e.g. my-grand (letters, digits, dashes)", juce::Colours::grey);
        license.setTextToShowWhenEmpty("e.g. CC-BY 4.0 (kept with the renders)", juce::Colours::grey);
        author.setTextToShowWhenEmpty("who recorded and made it", juce::Colours::grey);
        state.onTextChange = [this] { update(); };
        const int cpus = juce::SystemStats::getNumCpus();
        cores.setRange(1, cpus, 1);
        cores.setValue(juce::jmax(1, cpus - 2), juce::dontSendNotification);
        cores.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        fresh.setButtonText("Measure everything again (the renders already made are kept)");
        info.setJustificationType(juce::Justification::topLeft);
        info.setColour(juce::Label::textColourId, juce::Colour(0xffc3c2b7));
        build.onClick = [this] { start(); };
        cancel.onClick = [this] { close(); };
        for (auto* c : std::initializer_list<juce::Component*> { &heading, &bankLabel, &bank, &chooseFile, &stateLabel, &state,
                                                                 &licenseLabel, &license, &authorLabel, &author, &coresLabel, &cores,
                                                                 &fresh, &info, &build, &cancel })
            addAndMakeVisible(*c);
        update();
        setSize(640, 460);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16, 12);
        heading.setBounds(r.removeFromTop(28));
        r.removeFromTop(8);
        auto row = [&](juce::Label& label, juce::Component& field, juce::Component* side = nullptr) {
            auto line = r.removeFromTop(28);
            label.setBounds(line.removeFromLeft(90));
            if (side != nullptr) {
                side->setBounds(line.removeFromRight(120).reduced(0, 1));
                line.removeFromRight(6);
            }
            field.setBounds(line.reduced(0, 2));
            r.removeFromTop(6);
        };
        row(bankLabel, bank, &chooseFile);
        row(stateLabel, state);
        row(licenseLabel, license);
        row(authorLabel, author);
        row(coresLabel, cores);
        fresh.setBounds(r.removeFromTop(26));
        auto buttons = r.removeFromBottom(32);
        build.setBounds(buttons.removeFromRight(110));
        buttons.removeFromRight(8);
        cancel.setBounds(buttons.removeFromRight(90));
        r.removeFromBottom(8);
        info.setBounds(r.withTrimmedTop(6));
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1a1a19)); }

private:
    static constexpr int kOther = 1000;

    bool other() const { return bank.getSelectedId() == kOther; }
    juce::var chosen() const { return other() ? juce::var() : banks[bank.getSelectedId() - 1]; }

    void update()
    {
        const bool o = other();
        for (auto* c : std::initializer_list<juce::Component*> { &chooseFile, &state, &license, &author })
            c->setEnabled(o);
        juce::String text;
        bool ready = true;
        if (o) {
            if (banks.isEmpty()) // the tool did not list its banks: what it said
                text << "The profile tool did not list its banks (" << tools.python.getFullPathName() << "): "
                     << (answer.trim().isEmpty() ? juce::String("no answer") : answer.trim().substring(0, 400)) << "\n\n";
            text << (sfz.existsAsFile() ? "SFZ: " + sfz.getFullPathName() : juce::String("Choose the bank's .sfz file (a piano: the 88 keys)."))
                 << "\n\nIts velocity layers and pedal-down samples are found in it; it is rendered through the bench's sessions, "
                    "then measured by the forge. Keep the bank's licence: it goes with the renders and the profile.";
            ready = sfz.existsAsFile() && state.getText().trim().isNotEmpty();
        } else {
            const auto b = chosen();
            text << b.getProperty("name", "").toString() << ": " << b.getProperty("license", "").toString() << "\n"
                 << "Source: " << b.getProperty("source", "").toString() << " (" << b.getProperty("size", "").toString() << ")\n"
                 << "State: " << b.getProperty("state", "").toString() << ", " << b.getProperty("status", "").toString() << "\n\n";
            if (!(bool) b.getProperty("present", false)) {
                if (b.getProperty("fetch", "").toString() == "github")
                    text << "Not downloaded yet: the build downloads it first (each file checked).";
                else {
                    text << b.getProperty("instructions", "").toString();
                    ready = false;
                }
            } else {
                text << "A build takes 15 minutes to an hour (the renders, then the forge's measures of every layer); a build "
                        "stopped resumes where it was.";
            }
        }
        info.setText(text, juce::dontSendNotification);
        build.setEnabled(ready);
    }

    void start()
    {
        juce::StringArray args { "build" };
        juce::String name;
        if (other()) {
            name = state.getText().trim();
            args.addArray({ "--sfz", sfz.getFullPathName(), "--state", name });
            if (license.getText().trim().isNotEmpty())
                args.addArray({ "--license", license.getText().trim() });
            if (author.getText().trim().isNotEmpty())
                args.addArray({ "--author", author.getText().trim() });
        } else {
            const auto b = chosen();
            args.add(b.getProperty("id", "").toString());
            name = b.getProperty("state", "").toString();
        }
        args.addArray({ "--jobs", juce::String((int) cores.getValue()) });
        if (fresh.getToggleState())
            args.add("--fresh");
        if (onBuild)
            onBuild(args, name);
        close();
    }

    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState(0);
    }

    const Toolchain& tools;
    juce::File corpus, sfz;
    juce::Array<juce::var> banks;
    juce::String answer; // what the tool said when asked for its banks
    juce::Label heading, bankLabel, stateLabel, licenseLabel, authorLabel, coresLabel, info;
    juce::ComboBox bank;
    juce::TextButton chooseFile { "SFZ file..." }, build { "Build" }, cancel { "Cancel" };
    juce::TextEditor state, license, author;
    juce::Slider cores;
    juce::ToggleButton fresh;
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace builder
