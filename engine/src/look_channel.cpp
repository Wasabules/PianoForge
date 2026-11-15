#include "pianoforge/look_channel.hpp"

#include "pianoforge/audio_feed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

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
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pf {

namespace {

constexpr std::size_t kFrameBytes = (std::size_t) LookChannel::kMaxFrameWidth * LookChannel::kMaxFrameHeight * 4;

/// One side's look, under its sequence counter.
struct LookSlot {
    std::atomic<std::uint32_t> sequence; // odd while being written
    std::uint32_t length;
    std::atomic<std::uint64_t> serial;   // bumped with each look published
    std::uint64_t changedMs;
    char text[LookChannel::kTextBytes];
};

} // namespace

struct LookChannelHeader {
    char magic[8]; // "PFLOOK01" (a zeroed channel is a valid empty one)
    LookSlot looks[2]; // Side::app, Side::obs
    std::atomic<std::uint64_t> previewUntilNs;
    std::atomic<std::uint32_t> previewMaxWidth, previewMaxHeight;
    std::atomic<std::uint32_t> frameSequence; // odd while a frame is being written
    std::uint32_t frameWidth, frameHeight;
    std::atomic<std::uint64_t> frameSerial, frameNs;
};

namespace {

constexpr char kMagic[8] = { 'P', 'F', 'L', 'O', 'O', 'K', '0', '1' };
constexpr std::size_t kFrameOffset = (sizeof(LookChannelHeader) + 4095) & ~(std::size_t) 4095;
constexpr std::size_t kBytes = kFrameOffset + kFrameBytes;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free,
              "the channel's atomics must be lock-free to be shared between processes");

/// The channel's name: the feed's ("PianoForgeFeed", or PIANOFORGE_AUDIO_FEED's value) followed by "Look".
std::string channelName()
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
    return (name.empty() ? std::string("PianoForgeFeed") : name) + "Look";
}

void* mapChannel(void*& handle)
{
#if defined(_WIN32)
    const std::string ascii = channelName();
    const std::wstring name = L"Local\\" + std::wstring(ascii.begin(), ascii.end());
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD) ((std::uint64_t) kBytes >> 32),
                                  (DWORD) (kBytes & 0xFFFFFFFFu), name.c_str()); // created, or opened as it is
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
    const std::string name = "/" + channelName();
    const int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return nullptr;
    struct stat st {}; // sized by whichever side comes first (macOS refuses to resize an existing object)
    if (fstat(fd, &st) != 0 || (st.st_size < (off_t) kBytes && ftruncate(fd, (off_t) kBytes) != 0)) {
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

void unmapChannel(void* view, void* handle)
{
#if defined(_WIN32)
    if (view != nullptr)
        UnmapViewOfFile(view);
    if (handle != nullptr)
        CloseHandle((HANDLE) handle);
#else
    (void) handle;
    if (view != nullptr)
        munmap(view, kBytes);
#endif
}

} // namespace

bool LookChannel::open(Side s)
{
    if (header != nullptr)
        return true;
    void* view = mapChannel(mapping);
    if (view == nullptr)
        return false;
    header = static_cast<LookChannelHeader*>(view);
    side = s;
    if (std::memcmp(header->magic, kMagic, sizeof(kMagic)) != 0)
        std::memcpy(header->magic, kMagic, sizeof(kMagic)); // a new channel: zeroed, valid as it is
    return true;
}

void LookChannel::close()
{
    unmapChannel(header, mapping);
    header = nullptr;
    mapping = nullptr;
}

std::uint64_t LookChannel::nowMs() noexcept
{
    return (std::uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void LookChannel::publish(const std::string& text, std::uint64_t changedMs) noexcept
{
    if (header == nullptr)
        return;
    LookSlot& slot = header->looks[side == Side::app ? 0 : 1];
    const std::size_t n = std::min<std::size_t>(text.size(), kTextBytes - 1);
    slot.sequence.fetch_add(1, std::memory_order_acq_rel); // odd
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(slot.text, text.data(), n);
    slot.text[n] = 0;
    slot.length = (std::uint32_t) n;
    slot.changedMs = changedMs;
    std::atomic_thread_fence(std::memory_order_release);
    slot.sequence.fetch_add(1, std::memory_order_release); // even
    slot.serial.fetch_add(1, std::memory_order_release);
}

bool LookChannel::other(std::uint64_t& serial, std::string& text, std::uint64_t& changedMs) const
{
    if (header == nullptr)
        return false;
    const LookSlot& slot = header->looks[side == Side::app ? 1 : 0];
    const std::uint64_t now = slot.serial.load(std::memory_order_acquire);
    if (now == serial)
        return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = slot.sequence.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t n = std::min<std::uint32_t>(slot.length, kTextBytes - 1);
        std::string copy(slot.text, slot.text + n);
        const std::uint64_t changed = slot.changedMs;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_acquire) != before)
            continue;
        text = std::move(copy);
        changedMs = changed;
        serial = now;
        return true;
    }
    return false; // being written: next time
}

void LookChannel::wantPreview(std::uint64_t untilNs, std::uint32_t maxWidth, std::uint32_t maxHeight) noexcept
{
    if (header == nullptr)
        return;
    header->previewMaxWidth.store(std::clamp<std::uint32_t>(maxWidth, 16, kMaxFrameWidth), std::memory_order_relaxed);
    header->previewMaxHeight.store(std::clamp<std::uint32_t>(maxHeight, 16, kMaxFrameHeight), std::memory_order_relaxed);
    header->previewUntilNs.store(untilNs, std::memory_order_release);
}

bool LookChannel::previewWanted(std::uint32_t& maxWidth, std::uint32_t& maxHeight) const noexcept
{
    if (header == nullptr || feedClockNs() >= header->previewUntilNs.load(std::memory_order_acquire))
        return false;
    maxWidth = std::clamp<std::uint32_t>(header->previewMaxWidth.load(std::memory_order_relaxed), 16, kMaxFrameWidth);
    maxHeight = std::clamp<std::uint32_t>(header->previewMaxHeight.load(std::memory_order_relaxed), 16, kMaxFrameHeight);
    return true;
}

void LookChannel::putFrame(const std::uint8_t* rows, std::uint32_t lineBytes, std::uint32_t width, std::uint32_t height) noexcept
{
    if (header == nullptr || rows == nullptr || width == 0 || height == 0 || width > kMaxFrameWidth || height > kMaxFrameHeight)
        return;
    auto* pixels = reinterpret_cast<std::uint8_t*>(header) + kFrameOffset;
    header->frameSequence.fetch_add(1, std::memory_order_acq_rel); // odd
    std::atomic_thread_fence(std::memory_order_release);
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(pixels + (std::size_t) y * width * 4, rows + (std::size_t) y * lineBytes, (std::size_t) width * 4);
    header->frameWidth = width;
    header->frameHeight = height;
    header->frameNs.store(feedClockNs(), std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    header->frameSequence.fetch_add(1, std::memory_order_release); // even
    header->frameSerial.fetch_add(1, std::memory_order_release);
}

bool LookChannel::frame(std::uint64_t& serial, std::vector<std::uint8_t>& pixels, std::uint32_t& width, std::uint32_t& height,
                        std::uint64_t& madeNs) const
{
    if (header == nullptr)
        return false;
    const std::uint64_t now = header->frameSerial.load(std::memory_order_acquire);
    if (now == serial)
        return false;
    const auto* source = reinterpret_cast<const std::uint8_t*>(header) + kFrameOffset;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = header->frameSequence.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t w = std::min(header->frameWidth, kMaxFrameWidth), h = std::min(header->frameHeight, kMaxFrameHeight);
        pixels.resize((std::size_t) w * h * 4);
        std::memcpy(pixels.data(), source, pixels.size());
        const std::uint64_t made = header->frameNs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header->frameSequence.load(std::memory_order_acquire) != before)
            continue;
        width = w;
        height = h;
        madeNs = made;
        serial = now;
        return true;
    }
    return false;
}

} // namespace pf
