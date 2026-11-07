#include "Diagnostics.h"
#include "MainComponent.h"
#include "OfflineRender.h"
#include "SelfTest.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace pf::app {

class Application final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "PianoForge"; }
    const juce::String getApplicationVersion() override { return PIANOFORGE_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        const auto arguments = getCommandLineParameterArray();
        if (arguments.contains("--selftest")) {
            setApplicationReturnValue(runSelfTest(arguments));
            quit();
            return;
        }
        if (arguments.contains("--midi-monitor")) {
            setApplicationReturnValue(runMidiMonitor(arguments));
            quit();
            return;
        }
        if (arguments.contains("--render")) {
            setApplicationReturnValue(runOfflineRender(arguments));
            quit();
            return;
        }
        if (arguments.contains("--mirror-check")) {
            setApplicationReturnValue(runMirrorCheck(arguments));
            quit();
            return;
        }
        if (arguments.contains("--ui-snapshots")) {
            setApplicationReturnValue(runUiSnapshots(arguments));
            quit();
            return;
        }
        if (arguments.contains("--audio-hold")) {
            setApplicationReturnValue(runAudioHold(arguments));
            quit();
            return;
        }
        if (arguments.contains("--audio-restore")) {
            setApplicationReturnValue(runAudioRestore(arguments));
            quit();
            return;
        }
        if (arguments.contains("--check-update")) {
            setApplicationReturnValue(runCheckUpdate(arguments));
            quit();
            return;
        }
        if (arguments.contains("--list-audio")) {
            setApplicationReturnValue(runListAudio(arguments));
            quit();
            return;
        }
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        window = std::make_unique<Window>(getApplicationName());
        for (const auto& argument : arguments) // a MIDI file on the command line ("Open with" PianoForge) goes to the player
            if (const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(argument.unquoted());
                file.existsAsFile() && file.hasFileExtension("mid;midi;smf;kar")) {
                window->content().openMidiFile(file);
                break;
            }
    }

    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }
    void systemRequestedQuit() override { quit(); }

private:
    class Window final : public juce::DocumentWindow {
    public:
        explicit Window(const juce::String& name)
            : DocumentWindow(name, juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            main = new MainComponent();
            setContentOwned(main, true);
            setMenuBar(main);
            setResizable(true, true);
            setResizeLimits(1100, 500, 4000, 1600);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        ~Window() override { setMenuBar(nullptr); }
        MainComponent& content() { return *main; }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        MainComponent* main = nullptr; // owned by the window (its content)
    };

    theme::LookAndFeel lookAndFeel;
    std::unique_ptr<Window> window;
};

} // namespace pf::app

START_JUCE_APPLICATION(pf::app::Application)
