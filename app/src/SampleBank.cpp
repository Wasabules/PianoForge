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

namespace {
/// Index `lo` and weight `w` such that x lies between axis[lo] and axis[lo + 1] (clamped at both ends).
template <typename T>
void bracket(const std::vector<T>& axis, double x, int& lo, double& w) noexcept
{
    lo = 0;
    w = 0.0;
    if (axis.size() < 2 || x <= (double) axis.front())
        return;
    if (x >= (double) axis.back()) {
        lo = (int) axis.size() - 2;
        w = 1.0;
        return;
    }
    while (lo + 2 < (int) axis.size() && (double) axis[(size_t) lo + 1] <= x)
        ++lo;
    w = (x - (double) axis[(size_t) lo]) / ((double) axis[(size_t) lo + 1] - (double) axis[(size_t) lo]);
}
} // namespace

void ReleaseCurves::evaluate(int note, double holdSeconds, float* gains) const noexcept
{
    // Called on the audio thread at each key release: no allocation here.
    int n0, h0;
    double wn, wh;
    bracket(notes, (double) note, n0, wn);
    bracket(logHolds, std::log(std::max(holdSeconds, 1.0e-3)), h0, wh);

    const int numHolds = (int) holds.size();
    const int n1 = std::min(n0 + 1, (int) notes.size() - 1), h1 = std::min(h0 + 1, numHolds - 1);
    auto at = [&](int n, int h, int k) { return db[((size_t) n * (size_t) numHolds + (size_t) h) * (size_t) length + (size_t) k]; };
    for (int k = 0; k < length; ++k) {
        const double low = at(n0, h0, k) * (1 - wh) + at(n0, h1, k) * wh;
        const double high = at(n1, h0, k) * (1 - wh) + at(n1, h1, k) * wh;
        gains[k] = (float) std::pow(10.0, (low * (1 - wn) + high * wn) / 20.0);
    }
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
    std::int64_t noteOnFrame = 0;
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
    s.noteOnFrame = juce::jlimit<std::int64_t>(0, n - 1, entry.noteOnFrame);
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
        e.noteOnFrame = (juce::int64) meta["note_on_sample"]; // missing in the JSON: 0
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

    // Damper curves measured by the forge, next to the corpus passes (optional).
    juce::String damperInfo = "no damper curves (exponential approximation)";
    if (const auto curvesFile = folder.getParentDirectory().getChildFile("release-curves.json"); curvesFile.existsAsFile()) {
        const auto json = juce::JSON::parse(curvesFile);
        auto& rc = bank->releaseCurves;
        rc.stepSeconds = (double) json["step_s"];
        for (const auto& n : *json["notes"].getArray())
            rc.notes.push_back((int) n);
        for (const auto& h : *json["holds_s"].getArray()) {
            rc.holds.push_back((double) h);
            rc.logHolds.push_back(std::log((double) h));
        }
        const auto* perNote = json["curves_db"].getArray();
        rc.length = 0;
        if (perNote != nullptr && !perNote->isEmpty() && (*perNote)[0].getArray() != nullptr
            && !(*perNote)[0].getArray()->isEmpty())
            rc.length = juce::jmin(ReleaseCurves::kMaxPoints, (*(*perNote)[0].getArray())[0].getArray()->size());
        if (perNote != nullptr && rc.length > 0 && perNote->size() == (int) rc.notes.size()) {
            for (const auto& noteCurves : *perNote)
                for (const auto& curve : *noteCurves.getArray())
                    for (int k = 0; k < rc.length; ++k)
                        rc.db.push_back((float) (double) (*curve.getArray())[k]);
        }
        if (rc.db.size() != rc.notes.size() * rc.holds.size() * (size_t) rc.length || rc.stepSeconds <= 0.0)
            rc = {}; // malformed file: keep the fallback
        else
            damperInfo = "damper curves for " + juce::String((int) rc.notes.size()) + " measured notes";
    }

    int notes = 0;
    for (const auto& list : bank->byNote)
        notes += list.empty() ? 0 : 1;
    auto summary = juce::String((int) entries.size() - failures.load()) + " takes, " + juce::String(notes) + " notes, "
                 + juce::String((double) bank->bytes() / 1.0e9, 2) + " GB in memory, loaded in "
                 + juce::String((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) + " s, " + damperInfo;
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
