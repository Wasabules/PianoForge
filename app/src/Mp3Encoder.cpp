#include "Mp3Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

#else // no Media Foundation: no MP3

namespace pf::app {

struct Mp3Encoder::Impl {};
Mp3Encoder::Mp3Encoder() = default;
Mp3Encoder::~Mp3Encoder() = default;
bool Mp3Encoder::available() { return false; }
bool Mp3Encoder::takesRate(double) noexcept { return false; }
int Mp3Encoder::bitRate() const noexcept { return 0; }
void Mp3Encoder::abandon() noexcept {}
bool Mp3Encoder::open(const std::string&, int, int, int, std::string& error)
{
    error = "MP3 needs Windows' Media Foundation";
    return false;
}
bool Mp3Encoder::write(const float* const*, int, std::string& error)
{
    error = "MP3 needs Windows' Media Foundation";
    return false;
}
bool Mp3Encoder::close(std::string& error)
{
    error = "MP3 needs Windows' Media Foundation";
    return false;
}

} // namespace pf::app

#endif
