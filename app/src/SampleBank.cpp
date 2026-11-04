#include "SampleBank.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace pf::app {

int SampleBank::pick(int note, int velocity) const noexcept
{
    if (note < 0 || note > 127 || byNote[(size_t) note].empty())
        return -1;
    const auto& list = byNote[(size_t) note];
    int best = list.front();
    for (const int i : list)
        if (std::abs(samples[(size_t) i].velocity - velocity) < std::abs(samples[(size_t) best].velocity - velocity))
            best = i;
    return best;
}

std::uint64_t SampleBank::bytes() const noexcept
{
    std::uint64_t n = 0;
    for (const auto& s : samples)
        n += s.pcm.size() * sizeof(std::int16_t);
    return n;
}

namespace {

struct Entry {
    juce::File audio;
    int note = 0, velocity = 0;
    float releaseDecay40 = -1.0f;
};

/// xorshift32: fast and deterministic, for dithering.
struct Random32 {
    std::uint32_t state;
    float uniform() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (state >> 8) * (1.0f / 16777216.0f);
    }
};

bool decode(juce::AudioFormatManager& formats, const Entry& entry, Sample& s, double& sampleRate)
{
    std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(entry.audio) };
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    const int n = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buffer(juce::jmax(1, (int) reader->numChannels), n);
    reader->read(&buffer, 0, n, 0, true, true);
    sampleRate = reader->sampleRate;

    s.file = entry.audio;
    s.note = entry.note;
    s.velocity = entry.velocity;
    s.releaseDecay40 = entry.releaseDecay40;
    s.length = n;
    s.peak = buffer.getMagnitude(0, n);
    s.pcm.resize((size_t) n * 2);

    const float* left = buffer.getReadPointer(0);
    const float* right = buffer.getReadPointer(buffer.getNumChannels() > 1 ? 1 : 0);
    const float scale = s.peak > 0.0f ? 32767.0f / s.peak : 0.0f;
    Random32 random { (std::uint32_t) entry.audio.getFileName().hashCode() | 1u };
    auto quantize = [&](float x) {
        const float tpdf = random.uniform() - random.uniform(); // triangular dither, +-1 LSB
        return (std::int16_t) std::clamp((long) std::lround(x * scale + tpdf), -32767L, 32767L);
    };
    for (int k = 0; k < n; ++k) {
        s.pcm[(size_t) 2 * k] = quantize(left[k]);
        s.pcm[(size_t) 2 * k + 1] = quantize(right[k]);
    }
    return true;
}

} // namespace

BankLoader::BankLoader() : juce::Thread("Corpus loader") {}

BankLoader::~BankLoader()
{
    stopThread(10000);
}

void BankLoader::load(const juce::File& newFolder, Filter newFilter)
{
    stopThread(10000);
    folder = newFolder;
    filter = std::move(newFilter);
    done = 0;
    total = 0;
    {
        const juce::ScopedLock sl(lock);
        result.reset();
        message = "Reading the index...";
    }
    startThread();
}

float BankLoader::progress() const noexcept
{
    const int t = total.load();
    return t > 0 ? (float) done.load() / (float) t : 0.0f;
}

std::unique_ptr<SampleBank> BankLoader::takeResult()
{
    const juce::ScopedLock sl(lock);
    return std::move(result);
}

juce::String BankLoader::status() const
{
    const juce::ScopedLock sl(lock);
    return message;
}

void BankLoader::run()
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    auto bank = std::make_unique<SampleBank>();

    std::vector<Entry> entries;
    for (const auto& json : folder.findChildFiles(juce::File::findFiles, false, "*.json")) {
        const auto meta = juce::JSON::parse(json);
        if (!meta.isObject() || meta["type"].toString() != "note")
            continue;
        Entry e;
        e.note = (int) meta["note"];
        e.velocity = (int) meta["velocity"];
        if (filter && !filter(e.note, e.velocity))
            continue;
        const auto t40 = meta["release_decay_40db_s"];
        e.releaseDecay40 = (t40.isDouble() || t40.isInt() || t40.isInt64()) ? (float) (double) t40 : -1.0f;
        e.audio = json.withFileExtension("flac");
        if (!e.audio.existsAsFile())
            e.audio = json.withFileExtension("wav");
        if (e.audio.existsAsFile())
            entries.push_back(e);
    }
    total = (int) entries.size();
    bank->samples.resize(entries.size());

    std::atomic<size_t> next { 0 };
    std::atomic<int> failures { 0 }, mismatchedRates { 0 };
    std::atomic<double> commonRate { 0.0 };
    const unsigned cores = std::thread::hardware_concurrency(); // 0 if unknown
    const unsigned numThreads = cores > 1 ? cores - 1 : 1;     // keep one core free for audio
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < numThreads; ++t)
        threads.emplace_back([&] {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            for (size_t i; (i = next++) < entries.size();) {
                if (threadShouldExit())
                    return;
                double rate = 0.0;
                if (!decode(formats, entries[i], bank->samples[i], rate))
                    ++failures;
                else {
                    double expected = 0.0;
                    if (!commonRate.compare_exchange_strong(expected, rate) && expected != rate)
                        ++mismatchedRates;
                }
                ++done;
            }
        });
    for (auto& t : threads)
        t.join();
    if (threadShouldExit())
        return;

    bank->sampleRate = commonRate.load();
    for (size_t i = 0; i < bank->samples.size(); ++i)
        if (bank->samples[i].length > 0)
            bank->byNote[(size_t) bank->samples[i].note].push_back((int) i);
    for (auto& list : bank->byNote)
        std::sort(list.begin(), list.end(), [&](int a, int b) {
            return bank->samples[(size_t) a].velocity < bank->samples[(size_t) b].velocity;
        });

    int notes = 0;
    for (const auto& list : bank->byNote)
        notes += list.empty() ? 0 : 1;
    auto summary = juce::String((int) entries.size() - failures.load()) + " takes, " + juce::String(notes) + " notes, "
                 + juce::String((double) bank->bytes() / 1.0e9, 2) + " GB in memory, loaded in "
                 + juce::String((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) + " s";
    if (failures > 0)
        summary << "; " << failures.load() << " unreadable files";
    if (mismatchedRates > 0)
        summary << "; WARNING: different sample rates";
    if (entries.empty())
        summary = "No take found in " + folder.getFullPathName();

    const juce::ScopedLock sl(lock);
    result = std::move(bank);
    message = summary;
}

} // namespace pf::app
