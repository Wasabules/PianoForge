#include "Diagnostics.h"
#include "MainComponent.h"
#include "SelfTest.h"

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
        if (arguments.contains("--list-audio")) {
            setApplicationReturnValue(runListAudio(arguments));
            quit();
            return;
        }
        window = std::make_unique<Window>(getApplicationName());
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class Window final : public juce::DocumentWindow {
    public:
        explicit Window(const juce::String& name)
            : DocumentWindow(name, juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(1000, 420, 4000, 1200);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<Window> window;
};

} // namespace pf::app

START_JUCE_APPLICATION(pf::app::Application)
