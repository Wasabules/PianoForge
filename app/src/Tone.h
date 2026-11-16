#pragma once

#include <pianoforge/output_eq.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <memory>

namespace pf::app {

/// An impulse response for the convolution reverb, as read from its file (any rate; mono or stereo).
struct ImpulseResponse {
    juce::AudioBuffer<float> samples;
    double sampleRate = 48000.0;
    juce::String name;
    bool stereo() const { return samples.getNumChannels() >= 2; }
};

/// Reads an impulse response (WAV, AIFF, FLAC...; at most kMaxImpulseSeconds, the first two channels). Message thread.
inline constexpr double kMaxImpulseSeconds = 12.0;
inline std::shared_ptr<const ImpulseResponse> readImpulse(const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) {
        error = "Not an audio file this application reads: " + file.getFileName();
        return nullptr;
    }
    const auto frames = (int) std::min<juce::int64>(reader->lengthInSamples, (juce::int64) (kMaxImpulseSeconds * reader->sampleRate));
    if (frames <= 0 || reader->sampleRate <= 0.0) {
        error = "The file holds no sound: " + file.getFileName();
        return nullptr;
    }
    auto ir = std::make_shared<ImpulseResponse>();
    const int channels = reader->numChannels >= 2 ? 2 : 1;
    ir->samples.setSize(channels, frames);
    reader->read(&ir->samples, 0, frames, 0, true, channels > 1);
    ir->sampleRate = reader->sampleRate;
    ir->name = file.getFileName();
    return ir;
}

/// What follows the room on the way out, for the offline renders to sound as heard: the convolution reverb (its
/// impulse response, its levels) and the tone.
struct OutputTone {
    float bassDb = 0.0f, presenceDb = 0.0f, airDb = 0.0f;
    bool convolution = false;
    float wetDb = -12.0f, dryDb = 0.0f;
    std::shared_ptr<const ImpulseResponse> impulse;
};

/// The dry level (dB) as a gain: at kDryOffDb or below, none (the reverb alone).
inline constexpr float kDryOffDb = -40.0f;
inline float dryGain(float db) noexcept { return db <= kDryOffDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

/// Offline, as live: `audio` (the room's output) at the dry level plus `dry` (the engine's sound before the room)
/// through the impulse response at the wet level; then the tone.
inline void applyOutputTone(juce::AudioBuffer<float>& audio, const juce::AudioBuffer<float>* dry, const OutputTone& tone,
                            double sampleRate)
{
    if (audio.getNumChannels() < 2)
        return;
    const int n = audio.getNumSamples();
    if (tone.convolution && tone.impulse != nullptr && dry != nullptr && dry->getNumSamples() == n) {
        juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 4096 } };
        juce::AudioBuffer<float> ir(tone.impulse->samples);
        convolution.loadImpulseResponse(std::move(ir), tone.impulse->sampleRate,
                                        tone.impulse->stereo() ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no,
                                        juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::yes);
        constexpr int kBlock = 4096;
        convolution.prepare({ sampleRate, (juce::uint32) kBlock, 2 }); // the impulse response ready from the first block
        juce::AudioBuffer<float> wet(*dry);
        for (int start = 0; start < n; start += kBlock) {
            const int count = std::min(kBlock, n - start);
            juce::dsp::AudioBlock<float> block(wet.getArrayOfWritePointers(), 2, (size_t) start, (size_t) count);
            convolution.process(juce::dsp::ProcessContextReplacing<float>(block));
        }
        const float d = dryGain(tone.dryDb), w = std::pow(10.0f, tone.wetDb / 20.0f);
        for (int c = 0; c < 2; ++c) {
            auto* out = audio.getWritePointer(c);
            const auto* x = wet.getReadPointer(c);
            for (int k = 0; k < n; ++k)
                out[k] = d * out[k] + w * x[k];
        }
    }
    OutputEq eq;
    eq.update(sampleRate, tone.bassDb, tone.presenceDb, tone.airDb);
    eq.process(audio.getWritePointer(0), audio.getWritePointer(1), n);
}

} // namespace pf::app
