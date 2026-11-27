// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <pianoforge/look_channel.hpp>
#include <pianoforge/stream_look.hpp>

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>

namespace pf::app {

/// The look of the stream's keyboard (the OBS plugin's "PianoForge keyboard" source), shared with OBS through the
/// look channel (pianoforge/look_channel.hpp): an edit here reaches every keyboard synchronised in OBS, an edit there
/// comes here. Each look carries the wall-clock instant of its edit; the newest wins (at a tie, OBS's: a look set in
/// OBS before this window existed is kept). Kept in the settings ("streamLook", "streamLookChanged") for OBS's next
/// start.
class StreamLook {
public:
    explicit StreamLook(juce::PropertiesFile& settings);

    /// Message thread, a few times a second: OBS's look taken when it is the newest; ours sent again when it is.
    void poll();

    const pf::look::Values& values() const noexcept { return current; }
    /// An edit here: one setting, or the whole look (a style, the defaults).
    void set(std::size_t index, double value);
    void setAll(const pf::look::Values& values);

    /// The look changed from OBS's side (the window shows it).
    std::function<void()> onChanged;
    /// OBS has answered in this session (its plugin is there).
    bool obsSeen() const noexcept { return seen; }
    std::uint64_t changedMs() const noexcept { return changed; }

    pf::LookChannel& channel() noexcept { return link; }

private:
    void edited();
    void save();

    juce::PropertiesFile& settings;
    pf::LookChannel link;
    pf::look::Values current;
    std::uint64_t changed = 0, obsSerial = 0;
    bool seen = false, dirty = false;
};

} // namespace pf::app
