#pragma once

#include <cstdint>

namespace pf {

struct AudioFeedHeader;

/// A one-way stereo audio feed from PianoForge to another process on the same machine (the OBS plugin), through
/// shared memory: no driver, no system mixer, no virtual cable in between.
///
/// The memory ("PianoForgeAudioFeed": Local\ on Windows, /pianoforge-audio-feed on Linux) holds a header and a
/// ring of interleaved float frames. The writer (PianoForge's audio thread) copies each block into the ring and
/// then publishes the total of frames written (release); a reader copies any span still in the ring and checks
/// afterwards that the writer has not lapped it. Writing is a memory copy and one atomic store: no system call, no
/// lock, nothing the audio thread could wait on. The format (sample rate, block size) changes under a sequence
/// counter, while the audio device is stopped.
class AudioFeedWriter {
public:
    AudioFeedWriter() = default;
    AudioFeedWriter(const AudioFeedWriter&) = delete;
    AudioFeedWriter& operator=(const AudioFeedWriter&) = delete;
    ~AudioFeedWriter() { close(); }

    /// Creates (or joins) the shared memory. Fails when another PianoForge already writes to it.
    bool create();
    void close();
    bool isOpen() const noexcept { return header != nullptr; }

    /// Before the first block, whenever the audio device (re)starts (not while write() may run).
    void setFormat(std::uint32_t sampleRate, std::uint32_t blockSize) noexcept;
    /// Audio thread: the block just sent to the device. No allocation, lock or system call.
    void write(const float* const* channels, int numChannels, int numFrames) noexcept;

private:
    struct AudioFeedHeader* header = nullptr;
    float* ring = nullptr;
    void* mapping = nullptr; // platform handles
    void* ownerLock = nullptr;
};

class AudioFeedReader {
public:
    struct Format {
        std::uint32_t sampleRate = 0, blockSize = 0;
        std::uint64_t session = 0; // changes whenever the format does
    };

    AudioFeedReader() = default;
    AudioFeedReader(const AudioFeedReader&) = delete;
    AudioFeedReader& operator=(const AudioFeedReader&) = delete;
    ~AudioFeedReader() { close(); }

    /// Opens the shared memory if a writer created it.
    bool open();
    void close();
    bool isOpen() const noexcept { return header != nullptr; }

    /// false while the writer changes it, or before it set one.
    bool format(Format& out) const noexcept;
    /// Frames written since the feed was created.
    std::uint64_t written() const noexcept;
    std::uint32_t capacity() const noexcept;
    /// Copies frames [from, from + n) as interleaved stereo into `out`, n <= maxFrames and up to what is written.
    /// Returns n, or -1 when the span is no longer in the ring (the reader fell behind: start again nearer).
    int read(std::uint64_t from, float* out, int maxFrames) const noexcept;

private:
    const struct AudioFeedHeader* header = nullptr;
    const float* ring = nullptr;
    void* mapping = nullptr;
};

} // namespace pf
