#include "MidiFilePlayer.h"

namespace pf::app {

std::shared_ptr<const MidiFilePlayer::Sequence> MidiFilePlayer::read(const juce::File& file, juce::String& error)
{
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes)) {
        error = "cannot read " + file.getFileName();
        return nullptr;
    }
    std::string why;
    auto sequence = pf::readMidiFile(static_cast<const std::uint8_t*>(bytes.getData()), bytes.getSize(), why);
    if (sequence == nullptr) {
        error = file.getFileName() + ": " + juce::String::fromUTF8(why.c_str());
        return nullptr;
    }
    sequence->name = file.getFileNameWithoutExtension().toStdString();
    sequence->path = file.getFullPathName().toStdString();
    return sequence;
}

void MidiFilePlayer::render(juce::MidiBuffer& midi, int numSamples, double sampleRate, int stride, int offset) noexcept
{
    const int count = pf::SequencePlayer::render(scratch.data(), (int) scratch.size(), numSamples, sampleRate);
    for (int i = 0; i < count; ++i) {
        const auto& e = scratch[(std::size_t) i];
        midi.addEvent(juce::MidiMessage(e.status, e.data1, e.data2), offset + (int) e.offset * stride);
    }
}

} // namespace pf::app
