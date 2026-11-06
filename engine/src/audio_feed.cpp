#include <pianoforge/audio_feed.hpp>

#include <algorithm>
#include <atomic>
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

/// The shared layout (version 1). Plain data and lock-free atomics only: both processes map the same bytes.
struct AudioFeedHeader {
    char magic[8];                        // "PFFEED01"
    std::uint32_t version, headerBytes, capacity, channels;
    std::atomic<std::uint32_t> sequence;  // odd while the format changes
    std::uint32_t sampleRate, blockSize, reserved;
    std::atomic<std::uint64_t> session;   // bumped with every format
    std::atomic<std::uint64_t> written;   // frames written, monotonic
};

namespace {

constexpr char kMagic[8] = { 'P', 'F', 'F', 'E', 'E', 'D', '0', '1' };
constexpr std::uint32_t kVersion = 1, kHeaderBytes = 128, kCapacity = 1u << 16, kChannels = 2; // 1.4 s at 48 kHz
constexpr std::size_t kBytes = kHeaderBytes + (std::size_t) kCapacity * kChannels * sizeof(float);
static_assert(sizeof(AudioFeedHeader) <= kHeaderBytes);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free,
              "the feed's atomics must be lock-free to be shared between processes");

/// The feed's name: "PianoForgeAudioFeed", or PIANOFORGE_AUDIO_FEED when set (tests run beside the application).
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
    return name.empty() ? std::string("PianoForgeAudioFeed") : name;
}

#if defined(_WIN32)
std::wstring localName(const std::string& suffix)
{
    const std::string name = feedName() + suffix; // ASCII
    return L"Local\\" + std::wstring(name.begin(), name.end());
}
#endif

void* mapShared(bool create, void*& handle)
{
#if defined(_WIN32)
    const std::wstring name = localName("");
    HANDLE h = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) kBytes, name.c_str())
                      : OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (h == nullptr)
        return nullptr;
    void* view = MapViewOfFile(h, create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, kBytes);
    if (view == nullptr) {
        CloseHandle(h);
        return nullptr;
    }
    handle = h;
    return view;
#else
    const std::string name = "/" + feedName();
    const int fd = shm_open(name.c_str(), create ? (O_CREAT | O_RDWR) : O_RDONLY, 0600);
    if (fd < 0)
        return nullptr;
    if (create && ftruncate(fd, (off_t) kBytes) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* view = mmap(nullptr, kBytes, create ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, 0);
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
    ring = reinterpret_cast<float*>(static_cast<char*>(view) + kHeaderBytes);
    if (std::memcmp(header->magic, kMagic, sizeof(kMagic)) != 0 || header->version != kVersion) {
        // A new feed (a reader may still hold an older one open: then it is joined as it is).
        header->sequence.store(1);
        header->version = kVersion;
        header->headerBytes = kHeaderBytes;
        header->capacity = kCapacity;
        header->channels = kChannels;
        header->sampleRate = header->blockSize = 0;
        header->session.store(0);
        header->written.store(0);
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
    ring = nullptr;
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

void AudioFeedWriter::write(const float* const* channels, int numChannels, int numFrames) noexcept
{
    if (header == nullptr || numFrames <= 0)
        return;
    const std::uint64_t w = header->written.load(std::memory_order_relaxed); // the only writer
    const float* l = numChannels > 0 ? channels[0] : nullptr;
    const float* r = numChannels > 1 ? channels[1] : l;
    for (int k = 0; k < numFrames; ++k) {
        float* f = ring + ((w + (std::uint64_t) k) & (kCapacity - 1)) * kChannels;
        f[0] = l != nullptr ? l[k] : 0.0f;
        f[1] = r != nullptr ? r[k] : 0.0f;
    }
    header->written.store(w + (std::uint64_t) numFrames, std::memory_order_release);
}

bool AudioFeedReader::open()
{
    if (header != nullptr)
        return true;
    const void* view = mapShared(false, mapping);
    if (view == nullptr)
        return false;
    const auto* h = static_cast<const AudioFeedHeader*>(view);
    if (std::memcmp(h->magic, kMagic, sizeof(kMagic)) != 0 || h->version != kVersion || h->capacity != kCapacity
        || h->channels != kChannels) {
        unmapShared(view, mapping);
        mapping = nullptr;
        return false;
    }
    header = h;
    ring = reinterpret_cast<const float*>(static_cast<const char*>(view) + kHeaderBytes);
    return true;
}

void AudioFeedReader::close()
{
    unmapShared(header, mapping);
    header = nullptr;
    ring = nullptr;
    mapping = nullptr;
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

int AudioFeedReader::read(std::uint64_t from, float* out, int maxFrames) const noexcept
{
    if (header == nullptr || maxFrames <= 0)
        return 0;
    const std::uint64_t w = header->written.load(std::memory_order_acquire);
    if (from > w || w - from > kCapacity / 2) // ahead of the writer, or too far behind (being overwritten)
        return -1;
    const int n = (int) std::min<std::uint64_t>((std::uint64_t) maxFrames, w - from);
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

} // namespace pf
