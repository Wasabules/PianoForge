// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pf::app::desktop {

/// The desktop's services on D-Bus, through GIO (libgio-2.0, loaded at run time: every desktop has it; nothing is
/// linked). Linux only: elsewhere every call answers "not available". Each object below is used from the message
/// thread.

/// Whether GIO and the session bus are there.
bool available();

/// Keeps the screen on and the computer awake while held: the desktop portal's Inhibit (GNOME, KDE and the others,
/// sandboxed or not), else GNOME's session manager, else org.freedesktop.ScreenSaver. On Windows, the thread execution
/// state does the same.
class KeepAwake {
public:
    KeepAwake();
    ~KeepAwake();
    /// Held (true) or let go (false); `reason` is shown by the desktop. Returns whether it is held.
    bool set(bool hold, const std::string& reason);
    bool held() const noexcept { return holding; }
    const std::string& how() const noexcept { return method; } // "desktop portal", or why it could not
private:
    bool holding = false;
    std::string method, handle; // the portal's request, or the cookie as text
    int kind = 0;               // 0 none, 1 portal, 2 GNOME session, 3 screen saver, 4 Windows
};

/// The "performance" power profile, held while wanted (power-profiles-daemon: the profile the desktop's quick
/// settings show; let go when PianoForge closes, even if it crashes, as the daemon watches the holder).
class PerformanceProfile {
public:
    ~PerformanceProfile();
    bool set(bool hold, const std::string& reason);
    bool held() const noexcept { return cookie != 0; }
    const std::string& how() const noexcept { return method; }
private:
    std::uint32_t cookie = 0;
    std::string method, service, path, interface;
};

/// A desktop notification (org.freedesktop.Notifications). Returns whether a notification server took it.
bool notify(const std::string& summary, const std::string& body);

/// What the desktop's media controls show and can do with the MIDI player (MPRIS: the media keys, GNOME's and KDE's
/// player widgets, a headset's buttons). The state is set by the application; the commands come back on the message
/// thread.
struct PlayerState {
    bool hasFile = false, playing = false, canPrevious = false, canNext = false;
    std::string title, path;          // the MIDI file
    std::int64_t lengthUs = 0, positionUs = 0;
    double rate = 1.0;
};

class MediaControls {
public:
    struct Commands {
        std::function<void()> play, pause, playPause, stop, next, previous, raise;
        std::function<void(std::int64_t)> seekBy, seekTo; // microseconds
        std::function<void(const std::string&)> open;     // a file path
    };
    explicit MediaControls(Commands commands);
    ~MediaControls();
    /// Registered on the session bus (org.mpris.MediaPlayer2.PianoForge).
    bool active() const noexcept;
    /// Message thread: the player's state now; what changed is announced.
    void update(const PlayerState& state);

    struct Impl;
private:
    std::unique_ptr<Impl> impl;
};

} // namespace pf::app::desktop
