// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pf {

/// The stream look's channel between PianoForge and the OBS plugin, through a shared memory of its own
/// ("PianoForgeFeedLook", or PIANOFORGE_AUDIO_FEED's value followed by "Look"; Local\ on Windows, /<name> on Linux),
/// either side creating it:
/// - each side's look (stream_look.hpp's text), with the wall-clock instant of the edit it carries: each side takes
///   the other's when it is newer, and publishes its own when it changes (the newest edit wins, on whichever side);
/// - the preview: PianoForge asks for frames (until an instant, at most so large); the plugin renders the keyboard
///   and writes its frames (BGRA, premultiplied, top row first).
/// Each part is written whole under a sequence counter (odd while being written): a reader never sees half of it.
class LookChannel {
public:
    enum class Side { app, obs };
    static constexpr std::uint32_t kTextBytes = 16384, kMaxFrameWidth = 1920, kMaxFrameHeight = 1080;

    LookChannel() = default;
    LookChannel(const LookChannel&) = delete;
    LookChannel& operator=(const LookChannel&) = delete;
    ~LookChannel() { close(); }

    /// Creates the shared memory, or opens it if the other side did.
    bool open(Side side);
    void close();
    bool isOpen() const noexcept { return header != nullptr; }

    /// Wall-clock milliseconds (since 1970): when an edit was made.
    static std::uint64_t nowMs() noexcept;

    /// This side's look (at most kTextBytes - 1 bytes), edited at `changedMs`.
    void publish(const std::string& text, std::uint64_t changedMs) noexcept;
    /// The other side's look, if published since `serial` (then updated): its text and when it was edited.
    bool other(std::uint64_t& serial, std::string& text, std::uint64_t& changedMs) const;

    /// PianoForge: frames wanted until `untilNs` (feedClockNs), at most `maxWidth` by `maxHeight`.
    void wantPreview(std::uint64_t untilNs, std::uint32_t maxWidth, std::uint32_t maxHeight) noexcept;
    /// The plugin: whether frames are wanted now, and how large at most.
    bool previewWanted(std::uint32_t& maxWidth, std::uint32_t& maxHeight) const noexcept;
    /// The plugin: a frame (rows of `width` BGRA pixels, `lineBytes` apart).
    void putFrame(const std::uint8_t* rows, std::uint32_t lineBytes, std::uint32_t width, std::uint32_t height) noexcept;
    /// PianoForge: the newest frame, if newer than `serial` (then updated), into `pixels` (width * height * 4), with
    /// the instant it was made (feedClockNs).
    bool frame(std::uint64_t& serial, std::vector<std::uint8_t>& pixels, std::uint32_t& width, std::uint32_t& height,
               std::uint64_t& madeNs) const;

private:
    struct LookChannelHeader* header = nullptr;
    void* mapping = nullptr;
    Side side = Side::app;
};

} // namespace pf
