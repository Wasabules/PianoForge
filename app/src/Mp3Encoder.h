// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pf::app {

/// MP3 files, constant bit rate, 32, 44.1 or 48 kHz, mono or stereo.
/// - Windows: Media Foundation's MP3 encoder (Windows 8 and later; absent from the "N" editions without the Media
///   Feature Pack). The float input is brought to 16 bits (what the encoder takes) with a triangular dither.
/// - Linux, macOS: LAME (libmp3lame), loaded at run time when installed (Debian/Ubuntu libmp3lame0, Fedora
///   lame-libs, Arch lame, Homebrew lame); it takes the float input as it is, and the file gets a LAME tag (the
///   encoder's delay and padding, which players remove).
///
/// One encoder per file, on one thread (on Windows it initialises COM and Media Foundation on it). No JUCE here: the
/// system headers stay out of the rest of the application.
class Mp3Encoder {
public:
    Mp3Encoder();
    ~Mp3Encoder();
    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;

    /// Whether this system has the encoder (checked once).
    static bool available();
    /// Whether the encoder takes this rate (32, 44.1 and 48 kHz).
    static bool takesRate(double sampleRate) noexcept;

    /// Creates the file (UTF-8 path); the bit rate is the encoder's highest at or below `kbps`.
    bool open(const std::string& path, int sampleRate, int channels, int kbps, std::string& error);
    /// `frames` frames of each channel (`channels` pointers).
    bool write(const float* const* channels, int frames, std::string& error);
    /// Finishes the file. The destructor of an encoder left open abandons it (the file is unfinished).
    bool close(std::string& error);
    /// Drops an unfinished file's encoder (releases the file, which can then be removed).
    void abandon() noexcept;
    /// The bit rate chosen at open(), in kbit/s.
    int bitRate() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace pf::app
