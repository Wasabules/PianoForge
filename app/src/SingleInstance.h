#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace pf::app::instance {

/// One PianoForge per user and settings folder (Linux): a second start hands its arguments to the one running (a MIDI
/// file opened from the file manager, the menu entry's actions: --record, --gallery) and ends, as JUCE's own
/// mechanism does on Windows and macOS but not on Linux. Through a socket in the abstract namespace (it vanishes with
/// the process: nothing to clean after a crash). Elsewhere: nothing (forward() answers false).

/// The arguments for the running instance: files made absolute (its folder is not this one's).
juce::StringArray portable(const juce::StringArray& arguments);

/// From a second start: true when the one running took `arguments` (this one then quits).
bool forward(const juce::StringArray& arguments);

/// In the instance running: the arguments of later starts, on the message thread.
class Server {
public:
    explicit Server(std::function<void(const juce::StringArray&)> onArguments);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool listening() const noexcept { return socket >= 0; }

private:
    std::function<void(const juce::StringArray&)> onArguments;
    int socket = -1;
    std::atomic<bool> stopping { false };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true); // for what is posted
    std::thread thread;
    void run();
};

} // namespace pf::app::instance
