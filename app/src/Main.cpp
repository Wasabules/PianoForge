// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Diagnostics.h"
#include "LinuxAudio.h"
#include "MainComponent.h"
#include "OfflineRender.h"
#include "PipeWireAudio.h"
#include "SelfTest.h"
#include "SettingsLocation.h"
#include "SingleInstance.h"
#include "ui/DesktopPanel.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#if PIANOFORGE_WINDOW_ICON
#include <BinaryData.h>
#endif

namespace pf::app {

class Application final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "PianoForge"; }
    const juce::String getApplicationVersion() override { return PIANOFORGE_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        const auto arguments = getCommandLineParameterArray();
#if JUCE_LINUX
        { // before any audio device type is made (JUCE loads libjack then, once): JACK through PipeWire, and whether the
          // devices that play through PipeWire ask its graph for their block (Audio > PipeWire)
            linuxaudio::prepare();
            const juce::PropertiesFile settings(settingsOptions());
            pipewire::forceQuantum() = settings.getBoolValue("pipewireForceQuantum", true);
        }
#endif
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
        if (arguments.contains("--desktop-check")) {
            setApplicationReturnValue(runDesktopCheck(arguments));
            quit();
            return;
        }
        if (arguments.contains("--audio-check")) {
            setApplicationReturnValue(runAudioCheck(arguments));
            quit();
            return;
        }
        if (arguments.contains("--list-audio")) {
            setApplicationReturnValue(runListAudio(arguments));
            quit();
            return;
        }
        const juce::PropertiesFile settings(settingsOptions());
#if JUCE_LINUX
        // One PianoForge (Settings > Desktop): a second start hands its arguments to the one running, and ends.
        const bool single = settings.getBoolValue("singleInstance", true);
        if (single && instance::forward(arguments)) {
            quit();
            return;
        }
#endif
        if (const double scale = DesktopPanel::savedScale(settings); scale > 0.0) // Settings > Desktop: the interface's size
            juce::Desktop::getInstance().setGlobalScaleFactor((float) scale);
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        window = std::make_unique<Window>(getApplicationName());
#if JUCE_LINUX
        if (single) {
            server = std::make_unique<instance::Server>([this](const juce::StringArray& later) {
                if (window != nullptr)
                    window->content().handleArguments(later, true);
            });
            window->content().setInstanceAnswering(server->listening());
        }
#endif
        window->content().handleArguments(arguments, false); // a MIDI file ("Open with PianoForge"), --record, --gallery
    }

    void shutdown() override
    {
        server.reset();
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
#if PIANOFORGE_WINDOW_ICON // Linux: the icon the window manager shows (Windows and macOS take the executable's). With the
                           // native title bar, DocumentWindow::setIcon() draws nothing: the peer takes it.
            if (const auto logo = juce::Drawable::createFromImageData(PianoForgeAppData::logo_svg, PianoForgeAppData::logo_svgSize);
                logo != nullptr && getPeer() != nullptr) {
                juce::Image icon(juce::Image::ARGB, 256, 256, true);
                {
                    juce::Graphics g(icon);
                    logo->drawWithin(g, icon.getBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
                }
                getPeer()->setIcon(icon);
            }
#endif
        }
        ~Window() override { setMenuBar(nullptr); }
        MainComponent& content() { return *main; }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        MainComponent* main = nullptr; // owned by the window (its content)
    };

    theme::LookAndFeel lookAndFeel;
    std::unique_ptr<Window> window;
    std::unique_ptr<instance::Server> server; // Linux: later starts' arguments
};

} // namespace pf::app

START_JUCE_APPLICATION(pf::app::Application)
