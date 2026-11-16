#include "Mp3Encoder.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace pf::app {

using Microsoft::WRL::ComPtr;

namespace {

std::string describe(const char* what, HRESULT hr)
{
    char text[160];
    std::snprintf(text, sizeof text, "%s failed (0x%08lx)", what, (unsigned long) hr);
    return text;
}

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), nullptr, 0);
    std::wstring w((std::size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), w.data(), n);
    return w;
}

/// COM and Media Foundation for the current thread, for the lifetime of the object.
struct MediaFoundation {
    bool com = false, mf = false;
    MediaFoundation()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr); // RPC_E_CHANGED_MODE: the thread already has COM, in another mode; it works the same
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MediaFoundation()
    {
        if (mf)
            MFShutdown();
        if (com)
            CoUninitialize();
    }
    MediaFoundation(const MediaFoundation&) = delete;
    MediaFoundation& operator=(const MediaFoundation&) = delete;
};

} // namespace

struct Mp3Encoder::Impl {
    MediaFoundation mf;
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    int rate = 0, channels = 0, kbps = 0;
    std::int64_t written = 0; // frames
    std::uint32_t seed = 0x2545F491u;
    std::vector<std::int16_t> pcm;

    float noise() noexcept // uniform in [-0.5, 0.5) LSB
    {
        seed = seed * 1664525u + 1013904223u;
        return (float) (seed >> 8) * (1.0f / 16777216.0f) - 0.5f;
    }
};

Mp3Encoder::Mp3Encoder() = default;
Mp3Encoder::~Mp3Encoder() = default;

bool Mp3Encoder::takesRate(double sampleRate) noexcept
{
    return sampleRate == 32000.0 || sampleRate == 44100.0 || sampleRate == 48000.0;
}

bool Mp3Encoder::available()
{
    static const bool found = [] {
        MediaFoundation mf;
        if (!mf.mf)
            return false;
        MFT_REGISTER_TYPE_INFO output { MFMediaType_Audio, MFAudioFormat_MP3 };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, &output, &activates, &count)))
            return false;
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
        return count > 0;
    }();
    return found;
}

int Mp3Encoder::bitRate() const noexcept
{
    return impl != nullptr ? impl->kbps : 0;
}

void Mp3Encoder::abandon() noexcept
{
    impl.reset();
}

bool Mp3Encoder::open(const std::string& path, int sampleRate, int channels, int kbps, std::string& error)
{
    impl = std::make_unique<Impl>();
    auto& s = *impl;
    if (!s.mf.mf) {
        error = "Media Foundation is not available";
        return false;
    }
    if (channels < 1 || channels > 2 || !takesRate(sampleRate)) {
        error = "the MP3 encoder takes 32, 44.1 or 48 kHz, mono or stereo";
        return false;
    }
    // The encoder's output types: the one at this rate and channel count with the highest bit rate within `kbps`.
    ComPtr<IMFCollection> types;
    HRESULT hr = MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_MP3, MFT_ENUM_FLAG_ALL, nullptr, &types);
    if (FAILED(hr)) {
        error = hr == MF_E_TRANSCODE_NO_MATCHING_ENCODER ? "no MP3 encoder on this system" : describe("listing the MP3 encoder's formats", hr);
        return false;
    }
    DWORD count = 0;
    types->GetElementCount(&count);
    ComPtr<IMFMediaType> chosen;
    UINT32 chosenBytes = 0;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IUnknown> element;
        ComPtr<IMFMediaType> type;
        if (FAILED(types->GetElement(i, &element)) || FAILED(element.As(&type)))
            continue;
        UINT32 r = 0, c = 0, bytes = 0;
        type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
        type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
        if ((int) r == sampleRate && (int) c == channels && bytes <= (UINT32) kbps * 125u && bytes > chosenBytes) {
            chosen = type;
            chosenBytes = bytes;
        }
    }
    if (chosen == nullptr) {
        error = "the MP3 encoder has no " + std::to_string(sampleRate) + " Hz format";
        return false;
    }
    s.kbps = (int) std::lround(chosenBytes / 125.0);

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3);
    const auto wide = widen(path);
    if (FAILED(hr = MFCreateSinkWriterFromURL(wide.c_str(), nullptr, attributes.Get(), &s.writer))) {
        error = describe("creating the MP3 file", hr);
        return false;
    }
    if (FAILED(hr = s.writer->AddStream(chosen.Get(), &s.stream))) {
        error = describe("adding the MP3 stream", hr);
        return false;
    }
    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
    input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32) channels);
    input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) (2 * channels));
    input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (2 * channels * sampleRate));
    input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr = s.writer->SetInputMediaType(s.stream, input.Get(), nullptr))) {
        error = describe("setting the MP3 encoder's input", hr);
        return false;
    }
    if (FAILED(hr = s.writer->BeginWriting())) {
        error = describe("starting the MP3 file", hr);
        return false;
    }
    s.rate = sampleRate;
    s.channels = channels;
    return true;
}

bool Mp3Encoder::write(const float* const* input, int frames, std::string& error)
{
    if (impl == nullptr || impl->writer == nullptr) {
        error = "the MP3 file is not open";
        return false;
    }
    auto& s = *impl;
    if (frames <= 0)
        return true;
    // To 16 bits, triangular dither of one LSB (two uniform draws), interleaved.
    s.pcm.resize((std::size_t) frames * (std::size_t) s.channels);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < s.channels; ++c) {
            const float v = input[c][f] * 32768.0f + s.noise() + s.noise();
            s.pcm[(std::size_t) f * (std::size_t) s.channels + (std::size_t) c] = (std::int16_t) std::clamp(std::lround(v), -32768L, 32767L);
        }
    const DWORD bytes = (DWORD) (s.pcm.size() * sizeof(std::int16_t));
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(hr)) {
        error = describe("allocating an MP3 buffer", hr);
        return false;
    }
    std::memcpy(data, s.pcm.data(), bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    // Times in 100 ns units, from the frame count (no drift).
    const LONGLONG start = s.written * 10000000LL / s.rate, end = (s.written + frames) * 10000000LL / s.rate;
    sample->SetSampleTime(start);
    sample->SetSampleDuration(end - start);
    if (FAILED(hr = s.writer->WriteSample(s.stream, sample.Get()))) {
        error = describe("encoding MP3", hr);
        return false;
    }
    s.written += frames;
    return true;
}

bool Mp3Encoder::close(std::string& error)
{
    if (impl == nullptr || impl->writer == nullptr) {
        error = "the MP3 file is not open";
        return false;
    }
    const HRESULT hr = impl->writer->Finalize();
    impl->writer.Reset();
    if (FAILED(hr)) {
        error = describe("finishing the MP3 file", hr);
        return false;
    }
    return true;
}

} // namespace pf::app

#else // Linux, macOS: LAME, loaded at run time

#include <dlfcn.h>

namespace pf::app {

namespace {

/// LAME (libmp3lame, LGPL), looked for when MP3 is first asked for: no build dependency, and nothing linked into the
/// application. Its packages are everywhere (Debian and Ubuntu libmp3lame0, Fedora lame-libs, Arch lame, Homebrew
/// lame); without it, MP3 is simply not offered. Only the few functions used here are declared: LAME's interface has
/// kept them since 3.99 (2011).
struct Lame {
    using Flags = void*; // lame_global_flags*
    Flags (*init)() = nullptr;
    int (*setInSampleRate)(Flags, int) = nullptr;
    int (*setNumChannels)(Flags, int) = nullptr;
    int (*setMode)(Flags, int) = nullptr; // MPEG_mode: 1 joint stereo, 3 mono
    int (*setBitRate)(Flags, int) = nullptr;
    int (*setQuality)(Flags, int) = nullptr;
    int (*setWriteVbrTag)(Flags, int) = nullptr;
    int (*initParams)(Flags) = nullptr;
    int (*encodeFloat)(Flags, const float*, const float*, int, unsigned char*, int) = nullptr; // [-1, 1]
    int (*flush)(Flags, unsigned char*, int) = nullptr;
    std::size_t (*lameTagFrame)(Flags, unsigned char*, std::size_t) = nullptr;
    int (*close)(Flags) = nullptr;

    static const Lame* get()
    {
        static const Lame lame = [] {
            Lame l;
            void* handle = nullptr;
            for (const char* name : { "libmp3lame.so.0", "libmp3lame.so", "libmp3lame.0.dylib", "libmp3lame.dylib",
                                      "/opt/homebrew/lib/libmp3lame.dylib", "/usr/local/lib/libmp3lame.dylib" })
                if ((handle = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr)
                    break;
            if (handle == nullptr)
                return l;
            auto load = [&](auto& f, const char* symbol) {
                f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(handle, symbol));
                return f != nullptr;
            };
            const bool all = load(l.init, "lame_init") && load(l.setInSampleRate, "lame_set_in_samplerate")
                && load(l.setNumChannels, "lame_set_num_channels") && load(l.setMode, "lame_set_mode")
                && load(l.setBitRate, "lame_set_brate") && load(l.setQuality, "lame_set_quality")
                && load(l.setWriteVbrTag, "lame_set_bWriteVbrTag") && load(l.initParams, "lame_init_params")
                && load(l.encodeFloat, "lame_encode_buffer_ieee_float") && load(l.flush, "lame_encode_flush")
                && load(l.lameTagFrame, "lame_get_lametag_frame") && load(l.close, "lame_close");
            if (!all)
                l.init = nullptr; // too old a LAME: as if absent (the library stays loaded, as dlopen'd libraries may)
            return l;
        }();
        return lame.init != nullptr ? &lame : nullptr;
    }
};

/// MPEG-1 Layer III's bit rates (32, 44.1 and 48 kHz), kbit/s.
constexpr int kBitRates[] = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };

} // namespace

struct Mp3Encoder::Impl {
    const Lame* lame = nullptr;
    Lame::Flags flags = nullptr;
    std::FILE* file = nullptr;
    int channels = 0, kbps = 0;
    std::vector<unsigned char> out;

    ~Impl()
    {
        if (flags != nullptr)
            lame->close(flags);
        if (file != nullptr)
            std::fclose(file);
    }
    bool put(const unsigned char* data, int bytes, std::string& error)
    {
        if (bytes > 0 && std::fwrite(data, 1, (std::size_t) bytes, file) != (std::size_t) bytes) {
            error = "writing the MP3 file failed (disk full?)";
            return false;
        }
        return true;
    }
};

Mp3Encoder::Mp3Encoder() = default;
Mp3Encoder::~Mp3Encoder() = default;

bool Mp3Encoder::available()
{
    return Lame::get() != nullptr;
}

bool Mp3Encoder::takesRate(double sampleRate) noexcept
{
    return sampleRate == 32000.0 || sampleRate == 44100.0 || sampleRate == 48000.0;
}

int Mp3Encoder::bitRate() const noexcept
{
    return impl != nullptr ? impl->kbps : 0;
}

void Mp3Encoder::abandon() noexcept
{
    impl.reset();
}

bool Mp3Encoder::open(const std::string& path, int sampleRate, int channels, int kbps, std::string& error)
{
    impl.reset();
    const Lame* lame = Lame::get();
    if (lame == nullptr) {
        error = "MP3 needs LAME (libmp3lame: the package libmp3lame0, lame-libs or lame)";
        return false;
    }
    if (channels < 1 || channels > 2 || !takesRate(sampleRate)) {
        error = "the MP3 encoder takes 32, 44.1 or 48 kHz, mono or stereo";
        return false;
    }
    auto s = std::make_unique<Impl>();
    s->lame = lame;
    s->channels = channels;
    s->kbps = kBitRates[0];
    for (const int r : kBitRates)
        if (r <= kbps)
            s->kbps = r;
    if ((s->flags = lame->init()) == nullptr) {
        error = "the MP3 encoder could not start";
        return false;
    }
    // Constant bit rate, as Media Foundation's; quality 2 (LAME's "near best"); the LAME tag (encoder delay and
    // padding: players drop them, the recording starts and ends where it did), written at close().
    lame->setInSampleRate(s->flags, sampleRate);
    lame->setNumChannels(s->flags, channels);
    lame->setMode(s->flags, channels == 1 ? 3 : 1);
    lame->setBitRate(s->flags, s->kbps);
    lame->setQuality(s->flags, 2);
    lame->setWriteVbrTag(s->flags, 1);
    if (lame->initParams(s->flags) < 0) {
        error = "the MP3 encoder refused " + std::to_string(sampleRate) + " Hz at " + std::to_string(s->kbps) + " kbit/s";
        return false;
    }
    if ((s->file = std::fopen(path.c_str(), "wb")) == nullptr) {
        error = "creating the MP3 file failed: " + std::string(std::strerror(errno));
        return false;
    }
    impl = std::move(s);
    return true;
}

bool Mp3Encoder::write(const float* const* input, int frames, std::string& error)
{
    if (impl == nullptr || impl->file == nullptr) {
        error = "the MP3 file is not open";
        return false;
    }
    auto& s = *impl;
    // LAME takes at most a few seconds per call comfortably; its output fits 1.25 x the frames + 7200 bytes.
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, 65536);
        s.out.resize((std::size_t) n * 5 / 4 + 7200);
        const float* left = input[0] + done;
        const float* right = s.channels == 2 ? input[1] + done : left;
        const int bytes = s.lame->encodeFloat(s.flags, left, right, n, s.out.data(), (int) s.out.size());
        if (bytes < 0) {
            error = "encoding MP3 failed (" + std::to_string(bytes) + ")";
            return false;
        }
        if (!s.put(s.out.data(), bytes, error))
            return false;
        done += n;
    }
    return true;
}

bool Mp3Encoder::close(std::string& error)
{
    if (impl == nullptr || impl->file == nullptr) {
        error = "the MP3 file is not open";
        return false;
    }
    auto& s = *impl;
    s.out.resize(7200);
    const int bytes = s.lame->flush(s.flags, s.out.data(), (int) s.out.size());
    bool ok = bytes >= 0 && s.put(s.out.data(), bytes, error);
    if (ok) { // the LAME tag over the empty frame LAME left at the start
        const std::size_t tag = s.lame->lameTagFrame(s.flags, s.out.data(), s.out.size());
        if (tag > 0 && tag <= s.out.size() && std::fseek(s.file, 0, SEEK_SET) == 0)
            ok = s.put(s.out.data(), (int) tag, error);
    }
    const bool closed = std::fclose(s.file) == 0;
    s.file = nullptr;
    if (ok && !closed)
        error = "finishing the MP3 file failed (disk full?)";
    else if (!ok && error.empty())
        error = "finishing the MP3 file failed";
    return ok && closed;
}

} // namespace pf::app

#endif
