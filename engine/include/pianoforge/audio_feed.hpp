#pragma once

#include <cstdint>

namespace pf {

struct AudioFeedHeader;

/// A one-way feed from PianoForge to other processes on the same machine (the OBS plugin), through shared memory: no
/// driver, no system mixer, no virtual cable in between.
///
/// The memory ("PianoForgeAudioFeed", or the value of PIANOFORGE_AUDIO_FEED for tests running beside the
/// application: Local\ on Windows, /<name> on Linux) holds:
/// - the sound, as three mixes of the same frames (FeedMix), before the application's volume and limiter: as heard
///   (on headphones, the binaural rendering), as on speakers (the room without the head; the heard one when the
///   room is heard on speakers, or when no reader wants it), and dry (the engine's sound, no room). Rings of
///   interleaved stereo float frames, written in step. The application's volume rides along (a reader may follow
///   it);
/// - the writer's clock: the frames written as of a steady-clock instant (feedClockNs, the same clock in every
///   process of the machine), published with each block: a reader knows how old the audio it reads is, and how far
///   it is between the writer's blocks;
/// - the MIDI events that reached the engine, at their frame (a keyboard display shows them with the sound);
/// - the application's state for overlays (FeedInfo), and marks (chapters: a take saved, a recording started...);
/// - the readers (eight slots): each says what it reads and how late, every few hundred milliseconds; the
///   application shows them, and renders the speakers mix only while one takes it.
///
/// The writer (PianoForge's audio thread) copies each block into the rings and then publishes the total of frames
/// written (release); a reader copies any span still in a ring and checks afterwards that the writer has not lapped
/// it. Writing is memory copies and atomic stores: no lock, nothing the audio thread could wait on. The format
/// (sample rate, block size) changes under a sequence counter, while the audio device is stopped.
enum class FeedMix : std::uint32_t { heard = 0, speakers = 1, dry = 2 };
constexpr int kFeedMixes = 3;

/// A MIDI event as the engine received it, at its frame of the feed.
struct FeedEvent {
    std::uint64_t frame = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0, flags = 0;
    std::uint32_t reserved = 0;
};

/// The application's state, for overlays (UTF-8, each string zero-terminated).
struct FeedInfo {
    char engine[32] {};   // "Modal" or "Reference"
    char preset[96] {};   // the instrument preset (empty: none chosen)
    char room[64] {};     // the room's preset ("off" when the room is off)
    char file[192] {};    // the MIDI file loaded (empty: none)
    std::uint32_t filePlaying = 0, recording = 0;
};

enum class FeedReaderKind : std::uint32_t { none = 0, audio = 1, keyboard = 2, text = 3 };

/// The steady clock the feed's instants are on (nanoseconds; the same in every process of the machine).
std::uint64_t feedClockNs() noexcept;

class AudioFeedWriter {
public:
    static constexpr int kMaxReaders = 8;

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

    /// A block: each mix's two channels (a mix whose left channel is nullptr takes the heard one), the events of the
    /// block (FeedEvent::frame: their offset in it), and the application's volume (linear).
    struct Block {
        const float* left[kFeedMixes] {};
        const float* right[kFeedMixes] {};
        int frames = 0;
        const FeedEvent* events = nullptr;
        int numEvents = 0;
        float volume = 1.0f;
    };
    /// Audio thread: the block just sent to the device. No allocation, lock or system call.
    void write(const Block& block) noexcept;
    /// The same sound in every mix, no event, volume 1 (tests).
    void write(const float* const* channels, int numChannels, int numFrames) noexcept;

    /// Message thread: the state for overlays (published whole: a reader never sees half of it).
    void setInfo(const FeedInfo& info) noexcept;
    /// Message thread: a mark (a chapter of an OBS recording), at most 127 bytes of UTF-8.
    void addMark(const char* text) noexcept;

    /// Message thread: the readers heard from in the last two seconds.
    struct Reader {
        FeedReaderKind kind = FeedReaderKind::none;
        FeedMix mix = FeedMix::heard;
        float latencyMs = 0.0f; // an audio reader: from the key (as the feed's clock has it) to OBS
        std::uint32_t processId = 0;
    };
    int readers(Reader* out, int max) const noexcept;

private:
    struct AudioFeedHeader* header = nullptr;
    char* base = nullptr;
    void* mapping = nullptr; // platform handles
    void* ownerLock = nullptr;
};

class AudioFeedReader {
public:
    struct Format {
        std::uint32_t sampleRate = 0, blockSize = 0;
        std::uint64_t session = 0; // changes whenever the format does
    };
    static constexpr std::uint32_t kEventCapacity = 4096, kMarks = 16;

    AudioFeedReader() = default;
    AudioFeedReader(const AudioFeedReader&) = delete;
    AudioFeedReader& operator=(const AudioFeedReader&) = delete;
    ~AudioFeedReader() { close(); }

    /// Opens the shared memory if a writer created it, and takes a reader's slot for `kind` (none: no slot).
    bool open(FeedReaderKind kind = FeedReaderKind::none);
    void close();
    bool isOpen() const noexcept { return header != nullptr; }

    /// false while the writer changes it, or before it set one.
    bool format(Format& out) const noexcept;
    /// Frames written since the feed was created.
    std::uint64_t written() const noexcept;
    std::uint32_t capacity() const noexcept;
    /// The writer's clock: `frames` written as of `ns` (feedClockNs). false before the first block.
    bool clock(std::uint64_t& frames, std::uint64_t& ns) const noexcept;
    /// The application's volume (linear), as of the last block.
    float volume() const noexcept;
    /// Copies frames [from, from + n) of `mix` as interleaved stereo into `out`, n <= maxFrames and up to what is
    /// written. Returns n, or -1 when the span is no longer in the ring (the reader fell behind: start again nearer).
    int read(FeedMix mix, std::uint64_t from, float* out, int maxFrames) const noexcept;
    int read(std::uint64_t from, float* out, int maxFrames) const noexcept { return read(FeedMix::heard, from, out, maxFrames); }

    /// Events written since the feed was created.
    std::uint64_t eventsWritten() const noexcept;
    /// Copies events [from, from + n) into `out`, n <= maxEvents. Returns n, or -1 when they are no longer in the
    /// ring.
    int readEvents(std::uint64_t from, FeedEvent* out, int maxEvents) const noexcept;

    /// The state for overlays; false while it is being changed (try again).
    bool info(FeedInfo& out) const noexcept;
    /// Marks added since the feed was created; mark `index` (if still kept: the last kMarks) into `out`.
    std::uint32_t marks() const noexcept;
    bool mark(std::uint32_t index, char* out, int size) const noexcept;

    /// What this reader reads and how late it is (every few hundred milliseconds: a slot not heard from for two
    /// seconds counts as free).
    void heartbeat(FeedMix mix, float latencyMs) noexcept;

private:
    struct AudioFeedHeader* header = nullptr;
    const char* base = nullptr;
    void* mapping = nullptr;
    int slot = -1;
    FeedReaderKind kind = FeedReaderKind::none;
};

} // namespace pf
