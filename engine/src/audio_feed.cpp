#include <pianoforge/audio_feed.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace pf {

namespace {

constexpr std::uint32_t kMarkBytes = 128;

struct ReaderSlot {
    std::atomic<std::uint64_t> heartbeatNs; // 0: free (or not heard from for kReaderTimeoutNs)
    std::atomic<std::uint32_t> kind, mix, processId, latencyBits;
};

} // namespace

/// The shared layout (version 2). Plain data and lock-free atomics only: both processes map the same bytes. The info
/// and the clock are published under sequence counters (odd while being written; a reader copies, then checks that
/// the counter did not move).
struct AudioFeedHeader {
    char magic[8];                        // "PFFEED02"
    std::uint32_t version, headerBytes, capacity, channels, mixes, eventCapacity, marks, readerSlots;
    std::atomic<std::uint32_t> sequence;  // odd while the format changes
    std::uint32_t sampleRate, blockSize, reserved;
    std::atomic<std::uint64_t> session;   // bumped with every format
    std::atomic<std::uint64_t> written;   // frames written (each mix), monotonic
    std::atomic<std::uint32_t> clockSequence, volumeBits;
    std::atomic<std::uint64_t> clockFrames, clockNs; // under clockSequence
    std::atomic<std::uint64_t> eventsWritten;
    std::atomic<std::uint32_t> infoSequence, markCount;
    FeedInfo info;                        // under infoSequence
    char markText[AudioFeedReader::kMarks][kMarkBytes];
    ReaderSlot readers[AudioFeedWriter::kMaxReaders];
    // Added after the first release of version 2, at the end (an older reader ignores them; a newer one with an older
    // writer sees none: the header is zeroed when created).
    std::atomic<std::uint32_t> commandCount;
    std::atomic<std::uint32_t> commandCodes[AudioFeedReader::kCommands];
};

namespace {

constexpr char kMagic[8] = { 'P', 'F', 'F', 'E', 'E', 'D', '0', '2' };
constexpr std::uint32_t kVersion = 2, kHeaderBytes = 4096, kCapacity = 1u << 16, kChannels = 2; // 1.4 s at 48 kHz
constexpr std::uint32_t kEventCapacity = AudioFeedReader::kEventCapacity;
constexpr std::size_t kEventsOffset = kHeaderBytes;
constexpr std::size_t kRingsOffset = kEventsOffset + (std::size_t) kEventCapacity * sizeof(FeedEvent);
constexpr std::size_t kRingBytes = (std::size_t) kCapacity * kChannels * sizeof(float);
constexpr std::size_t kBytes = kRingsOffset + kFeedMixes * kRingBytes;
constexpr std::uint64_t kReaderTimeoutNs = 2'000'000'000ull;
static_assert(sizeof(AudioFeedHeader) <= kHeaderBytes);
static_assert(sizeof(FeedEvent) == 16);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free,
              "the feed's atomics must be lock-free to be shared between processes");

/// The feed's name: "PianoForgeFeed", or PIANOFORGE_AUDIO_FEED when set (tests run beside the application). (Version 1
/// was "PianoForgeAudioFeed": an older plugin keeping that one open does not get in the way.)
std::string feedName()
{
    std::string name;
#if defined(_WIN32)
    char* custom = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&custom, &length, "PIANOFORGE_AUDIO_FEED") == 0 && custom != nullptr) {
        name = custom;
        std::free(custom);
    }
#else
    if (const char* custom = std::getenv("PIANOFORGE_AUDIO_FEED"); custom != nullptr)
        name = custom;
#endif
    return name.empty() ? std::string("PianoForgeFeed") : name;
}

#if defined(_WIN32)
std::wstring localName(const std::string& suffix)
{
    const std::string name = feedName() + suffix; // ASCII
    return L"Local\\" + std::wstring(name.begin(), name.end());
}
#endif

std::uint32_t processId() noexcept
{
#if defined(_WIN32)
    return (std::uint32_t) GetCurrentProcessId();
#else
    return (std::uint32_t) getpid();
#endif
}

std::uint32_t floatBits(float x) noexcept
{
    std::uint32_t u;
    std::memcpy(&u, &x, sizeof(u));
    return u;
}

float bitsFloat(std::uint32_t u) noexcept
{
    float x;
    std::memcpy(&x, &u, sizeof(x));
    return x;
}

/// Maps the feed: the writer creates it; a reader opens it to read, and to write its slot.
void* mapShared(bool create, void*& handle)
{
#if defined(_WIN32)
    const std::wstring name = localName("");
    HANDLE h = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) kBytes, name.c_str())
                      : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (h == nullptr)
        return nullptr;
    void* view = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kBytes);
    if (view == nullptr) {
        CloseHandle(h);
        return nullptr;
    }
    handle = h;
    return view;
#else
    (void) create;
    const std::string name = "/" + feedName();
    const int fd = shm_open(name.c_str(), create ? (O_CREAT | O_RDWR) : O_RDWR, 0600);
    if (fd < 0)
        return nullptr;
    if (create && ftruncate(fd, (off_t) kBytes) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* view = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (view == MAP_FAILED)
        return nullptr;
    handle = nullptr;
    return view;
#endif
}

void unmapShared(const void* view, void* handle)
{
#if defined(_WIN32)
    if (view != nullptr)
        UnmapViewOfFile(view);
    if (handle != nullptr)
        CloseHandle((HANDLE) handle);
#else
    (void) handle;
    if (view != nullptr)
        munmap(const_cast<void*>(view), kBytes);
#endif
}

} // namespace

std::uint64_t feedClockNs() noexcept
{
    return (std::uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool AudioFeedWriter::create()
{
    if (header != nullptr)
        return true;
#if defined(_WIN32)
    // One writer at a time: a second PianoForge would interleave its blocks with the first one's.
    HANDLE lock = CreateMutexW(nullptr, FALSE, localName("Writer").c_str());
    if (lock == nullptr)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(lock);
        return false;
    }
    ownerLock = lock;
#endif
    void* view = mapShared(true, mapping);
    if (view == nullptr) {
        close();
        return false;
    }
    header = static_cast<AudioFeedHeader*>(view);
    base = static_cast<char*>(view);
    if (std::memcmp(header->magic, kMagic, sizeof(kMagic)) != 0 || header->version != kVersion) {
        // A new feed (readers may still hold an older one open: then it is joined as it is, slots included).
        header->sequence.store(1);
        header->version = kVersion;
        header->headerBytes = kHeaderBytes;
        header->capacity = kCapacity;
        header->channels = kChannels;
        header->mixes = kFeedMixes;
        header->eventCapacity = kEventCapacity;
        header->marks = AudioFeedReader::kMarks;
        header->readerSlots = kMaxReaders;
        header->sampleRate = header->blockSize = 0;
        header->session.store(0);
        header->written.store(0);
        header->clockSequence.store(0);
        header->clockFrames.store(0);
        header->clockNs.store(0);
        header->volumeBits.store(floatBits(1.0f));
        header->eventsWritten.store(0);
        header->infoSequence.store(0);
        header->markCount.store(0);
        header->info = FeedInfo {};
        std::memset(header->markText, 0, sizeof(header->markText));
        header->commandCount.store(0);
        for (auto& code : header->commandCodes)
            code.store(0);
        for (auto& r : header->readers) {
            r.heartbeatNs.store(0);
            r.kind.store(0);
            r.mix.store(0);
            r.processId.store(0);
            r.latencyBits.store(0);
        }
        std::memcpy(header->magic, kMagic, sizeof(kMagic));
        header->sequence.store(2);
    }
    return true;
}

void AudioFeedWriter::close()
{
    if (header != nullptr) {
        // Readers see no format any more: they stop and wait for a writer.
        header->sequence.fetch_add(1);
        header->sampleRate = 0;
        header->session.fetch_add(1);
        header->sequence.fetch_add(1);
    }
    unmapShared(header, mapping);
    header = nullptr;
    base = nullptr;
    mapping = nullptr;
#if defined(_WIN32)
    if (ownerLock != nullptr)
        CloseHandle((HANDLE) ownerLock);
#endif
    ownerLock = nullptr;
}

void AudioFeedWriter::setFormat(std::uint32_t sampleRate, std::uint32_t blockSize) noexcept
{
    if (header == nullptr)
        return;
    header->sequence.fetch_add(1); // odd: being changed
    header->sampleRate = sampleRate;
    header->blockSize = blockSize;
    header->session.fetch_add(1);
    header->sequence.fetch_add(1);
}

void AudioFeedWriter::write(const Block& b) noexcept
{
    if (header == nullptr || b.frames <= 0)
        return;
    const std::uint64_t w = header->written.load(std::memory_order_relaxed); // the only writer
    // The events first (a reader takes those up to the frames written).
    if (b.numEvents > 0 && b.events != nullptr) {
        auto* events = reinterpret_cast<FeedEvent*>(base + kEventsOffset);
        const std::uint64_t e = header->eventsWritten.load(std::memory_order_relaxed);
        for (int k = 0; k < b.numEvents; ++k) {
            FeedEvent ev = b.events[k];
            ev.frame = w + std::min<std::uint64_t>(ev.frame, (std::uint64_t) b.frames - 1);
            events[(e + (std::uint64_t) k) & (kEventCapacity - 1)] = ev;
        }
        header->eventsWritten.store(e + (std::uint64_t) b.numEvents, std::memory_order_release);
    }
    for (int m = 0; m < kFeedMixes; ++m) {
        const bool own = b.left[m] != nullptr;
        const float* l = own ? b.left[m] : b.left[0];
        const float* r = own ? b.right[m] : b.right[0];
        if (r == nullptr)
            r = l;
        float* ring = reinterpret_cast<float*>(base + kRingsOffset + (std::size_t) m * kRingBytes);
        for (int k = 0; k < b.frames; ++k) {
            float* f = ring + ((w + (std::uint64_t) k) & (kCapacity - 1)) * kChannels;
            f[0] = l != nullptr ? l[k] : 0.0f;
            f[1] = r != nullptr ? r[k] : 0.0f;
        }
    }
    header->volumeBits.store(floatBits(b.volume), std::memory_order_relaxed);
    // The clock: these frames, as of now.
    const std::uint32_t s = header->clockSequence.load(std::memory_order_relaxed);
    header->clockSequence.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    header->clockFrames.store(w + (std::uint64_t) b.frames, std::memory_order_relaxed);
    header->clockNs.store(feedClockNs(), std::memory_order_relaxed);
    header->clockSequence.store(s + 2, std::memory_order_release);
    header->written.store(w + (std::uint64_t) b.frames, std::memory_order_release);
}

void AudioFeedWriter::write(const float* const* channels, int numChannels, int numFrames) noexcept
{
    Block b;
    b.left[0] = numChannels > 0 ? channels[0] : nullptr;
    b.right[0] = numChannels > 1 ? channels[1] : b.left[0];
    b.frames = numFrames;
    write(b);
}

void AudioFeedWriter::setInfo(const FeedInfo& info) noexcept
{
    if (header == nullptr)
        return;
    const std::uint32_t s = header->infoSequence.load(std::memory_order_relaxed);
    header->infoSequence.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&header->info, &info, sizeof(FeedInfo));
    header->info.engine[sizeof(info.engine) - 1] = header->info.preset[sizeof(info.preset) - 1] = 0;
    header->info.room[sizeof(info.room) - 1] = header->info.file[sizeof(info.file) - 1] = 0;
    header->infoSequence.store(s + 2, std::memory_order_release);
}

void AudioFeedWriter::addMark(const char* text) noexcept
{
    if (header == nullptr || text == nullptr)
        return;
    const std::uint32_t n = header->markCount.load(std::memory_order_relaxed);
    char* slotText = header->markText[n % AudioFeedReader::kMarks];
    std::memset(slotText, 0, kMarkBytes);
    std::size_t length = std::strlen(text);
    if (length > kMarkBytes - 1) { // cut at a whole UTF-8 character
        length = kMarkBytes - 1;
        while (length > 0 && ((unsigned char) text[length] & 0xC0) == 0x80)
            --length;
    }
    std::memcpy(slotText, text, length);
    header->markCount.store(n + 1, std::memory_order_release);
}

void AudioFeedWriter::addCommand(FeedCommand command) noexcept
{
    if (header == nullptr || command == FeedCommand::none)
        return;
    const std::uint32_t n = header->commandCount.load(std::memory_order_relaxed);
    header->commandCodes[n % AudioFeedReader::kCommands].store((std::uint32_t) command, std::memory_order_relaxed);
    header->commandCount.store(n + 1, std::memory_order_release);
}

int AudioFeedWriter::readers(Reader* out, int max) const noexcept
{
    if (header == nullptr)
        return 0;
    const std::uint64_t now = feedClockNs();
    int n = 0;
    for (const auto& r : header->readers) {
        const std::uint64_t beat = r.heartbeatNs.load(std::memory_order_acquire);
        if (beat == 0 || (now > beat && now - beat > kReaderTimeoutNs) || n >= max)
            continue;
        out[n].kind = (FeedReaderKind) r.kind.load(std::memory_order_relaxed);
        out[n].mix = (FeedMix) std::min<std::uint32_t>(r.mix.load(std::memory_order_relaxed), kFeedMixes - 1);
        out[n].latencyMs = bitsFloat(r.latencyBits.load(std::memory_order_relaxed));
        out[n].processId = r.processId.load(std::memory_order_relaxed);
        ++n;
    }
    return n;
}

bool AudioFeedReader::open(FeedReaderKind wanted)
{
    if (header != nullptr)
        return true;
    void* view = mapShared(false, mapping);
    if (view == nullptr)
        return false;
    auto* h = static_cast<AudioFeedHeader*>(view);
    if (std::memcmp(h->magic, kMagic, sizeof(kMagic)) != 0 || h->version != kVersion || h->capacity != kCapacity
        || h->channels != kChannels || h->mixes != (std::uint32_t) kFeedMixes || h->eventCapacity != kEventCapacity) {
        unmapShared(view, mapping);
        mapping = nullptr;
        return false;
    }
    header = h;
    base = static_cast<const char*>(view);
    kind = wanted;
    slot = -1;
    if (wanted != FeedReaderKind::none) { // a free slot, or one not heard from for kReaderTimeoutNs
        const std::uint64_t now = feedClockNs();
        for (int i = 0; i < AudioFeedWriter::kMaxReaders && slot < 0; ++i) {
            auto& r = header->readers[i];
            std::uint64_t beat = r.heartbeatNs.load(std::memory_order_acquire);
            if (beat != 0 && !(now > beat && now - beat > kReaderTimeoutNs))
                continue;
            if (r.heartbeatNs.compare_exchange_strong(beat, now, std::memory_order_acq_rel)) {
                r.kind.store((std::uint32_t) wanted, std::memory_order_relaxed);
                r.mix.store(0, std::memory_order_relaxed);
                r.processId.store(processId(), std::memory_order_relaxed);
                r.latencyBits.store(0, std::memory_order_relaxed);
                slot = i;
            }
        }
    }
    return true;
}

void AudioFeedReader::close()
{
    if (header != nullptr && slot >= 0)
        header->readers[slot].heartbeatNs.store(0, std::memory_order_release);
    unmapShared(header, mapping);
    header = nullptr;
    base = nullptr;
    mapping = nullptr;
    slot = -1;
}

bool AudioFeedReader::format(Format& out) const noexcept
{
    if (header == nullptr)
        return false;
    const std::uint32_t before = header->sequence.load(std::memory_order_acquire);
    if (before & 1u)
        return false;
    Format f;
    f.sampleRate = header->sampleRate;
    f.blockSize = header->blockSize;
    f.session = header->session.load(std::memory_order_acquire);
    if (header->sequence.load(std::memory_order_acquire) != before || f.sampleRate == 0)
        return false;
    out = f;
    return true;
}

std::uint64_t AudioFeedReader::written() const noexcept
{
    return header != nullptr ? header->written.load(std::memory_order_acquire) : 0;
}

std::uint32_t AudioFeedReader::capacity() const noexcept
{
    return kCapacity;
}

bool AudioFeedReader::clock(std::uint64_t& frames, std::uint64_t& ns) const noexcept
{
    if (header == nullptr)
        return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = header->clockSequence.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        const std::uint64_t f = header->clockFrames.load(std::memory_order_relaxed);
        const std::uint64_t t = header->clockNs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header->clockSequence.load(std::memory_order_relaxed) != before)
            continue;
        if (t == 0)
            return false;
        frames = f;
        ns = t;
        return true;
    }
    return false;
}

float AudioFeedReader::volume() const noexcept
{
    return header != nullptr ? bitsFloat(header->volumeBits.load(std::memory_order_relaxed)) : 1.0f;
}

int AudioFeedReader::read(FeedMix mix, std::uint64_t from, float* out, int maxFrames) const noexcept
{
    if (header == nullptr || maxFrames <= 0)
        return 0;
    const std::uint64_t w = header->written.load(std::memory_order_acquire);
    if (from > w || w - from > kCapacity / 2) // ahead of the writer, or too far behind (being overwritten)
        return -1;
    const int n = (int) std::min<std::uint64_t>((std::uint64_t) maxFrames, w - from);
    const auto m = (std::size_t) std::min<std::uint32_t>((std::uint32_t) mix, kFeedMixes - 1);
    const float* ring = reinterpret_cast<const float*>(base + kRingsOffset + m * kRingBytes);
    for (int k = 0; k < n; ++k) {
        const float* f = ring + ((from + (std::uint64_t) k) & (kCapacity - 1)) * kChannels;
        out[2 * k] = f[0];
        out[2 * k + 1] = f[1];
    }
    // The writer may have lapped the span while it was copied: then it is not to be trusted.
    if (header->written.load(std::memory_order_acquire) - from > kCapacity - (std::uint64_t) n)
        return -1;
    return n;
}

std::uint64_t AudioFeedReader::eventsWritten() const noexcept
{
    return header != nullptr ? header->eventsWritten.load(std::memory_order_acquire) : 0;
}

int AudioFeedReader::readEvents(std::uint64_t from, FeedEvent* out, int maxEvents) const noexcept
{
    if (header == nullptr || maxEvents <= 0)
        return 0;
    const std::uint64_t e = header->eventsWritten.load(std::memory_order_acquire);
    if (from > e || e - from > kEventCapacity / 2)
        return -1;
    const int n = (int) std::min<std::uint64_t>((std::uint64_t) maxEvents, e - from);
    const auto* events = reinterpret_cast<const FeedEvent*>(base + kEventsOffset);
    for (int k = 0; k < n; ++k)
        out[k] = events[(from + (std::uint64_t) k) & (kEventCapacity - 1)];
    if (header->eventsWritten.load(std::memory_order_acquire) - from > kEventCapacity - (std::uint64_t) n)
        return -1;
    return n;
}

bool AudioFeedReader::info(FeedInfo& out) const noexcept
{
    if (header == nullptr)
        return false;
    const std::uint32_t before = header->infoSequence.load(std::memory_order_acquire);
    if (before & 1u)
        return false;
    FeedInfo copy;
    std::memcpy(&copy, &header->info, sizeof(FeedInfo));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (header->infoSequence.load(std::memory_order_relaxed) != before)
        return false;
    copy.engine[sizeof(copy.engine) - 1] = copy.preset[sizeof(copy.preset) - 1] = 0;
    copy.room[sizeof(copy.room) - 1] = copy.file[sizeof(copy.file) - 1] = 0;
    out = copy;
    return true;
}

std::uint32_t AudioFeedReader::marks() const noexcept
{
    return header != nullptr ? header->markCount.load(std::memory_order_acquire) : 0;
}

bool AudioFeedReader::mark(std::uint32_t index, char* out, int size) const noexcept
{
    if (header == nullptr || out == nullptr || size <= 0)
        return false;
    const std::uint32_t count = header->markCount.load(std::memory_order_acquire);
    if (index >= count || count - index > kMarks)
        return false;
    char copy[kMarkBytes];
    std::memcpy(copy, header->markText[index % kMarks], kMarkBytes);
    if (header->markCount.load(std::memory_order_acquire) - index > kMarks) // overwritten meanwhile
        return false;
    copy[kMarkBytes - 1] = 0;
    const std::size_t n = std::min<std::size_t>(std::strlen(copy), (std::size_t) size - 1);
    std::memcpy(out, copy, n);
    out[n] = 0;
    return true;
}

std::uint32_t AudioFeedReader::commands() const noexcept
{
    return header != nullptr ? header->commandCount.load(std::memory_order_acquire) : 0;
}

FeedCommand AudioFeedReader::command(std::uint32_t index) const noexcept
{
    if (header == nullptr)
        return FeedCommand::none;
    const std::uint32_t count = header->commandCount.load(std::memory_order_acquire);
    if (index >= count || count - index > kCommands)
        return FeedCommand::none;
    const auto code = header->commandCodes[index % kCommands].load(std::memory_order_relaxed);
    if (header->commandCount.load(std::memory_order_acquire) - index > kCommands)
        return FeedCommand::none;
    return code <= (std::uint32_t) FeedCommand::obsPreviousScene ? (FeedCommand) code : FeedCommand::none;
}

void AudioFeedReader::heartbeat(FeedMix mix, float latencyMs) noexcept
{
    if (header == nullptr || slot < 0)
        return;
    auto& r = header->readers[slot];
    r.mix.store((std::uint32_t) mix, std::memory_order_relaxed);
    r.latencyBits.store(floatBits(latencyMs), std::memory_order_relaxed);
    r.kind.store((std::uint32_t) kind, std::memory_order_relaxed);
    r.processId.store(processId(), std::memory_order_relaxed);
    r.heartbeatNs.store(feedClockNs(), std::memory_order_release);
}

} // namespace pf
