// SPDX-License-Identifier: AGPL-3.0-or-later

#include <pianoforge/modal_profile.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace pf {

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
    while (lo + 2 < (int) axis.size() && (double) axis[(std::size_t) lo + 1] <= x)
        ++lo;
    w = (x - (double) axis[(std::size_t) lo]) / ((double) axis[(std::size_t) lo + 1] - (double) axis[(std::size_t) lo]);
}

/// Little-endian reader over the whole file in memory.
class Reader {
public:
    explicit Reader(std::vector<char> d) : data(std::move(d)) {}

    template <typename T>
    T get()
    {
        T v {};
        if (pos + sizeof(T) > data.size()) {
            failed = true;
            return v;
        }
        std::memcpy(&v, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }

    bool bytes(void* out, std::size_t n)
    {
        if (pos + n > data.size()) {
            failed = true;
            return false;
        }
        std::memcpy(out, data.data() + pos, n);
        pos += n;
        return true;
    }

    bool ok() const noexcept { return !failed; }
    bool atEnd() const noexcept { return pos == data.size(); }
    std::size_t position() const noexcept { return pos; }
    std::size_t length() const noexcept { return data.size(); }
    void seek(std::size_t to) noexcept
    {
        if (to > data.size())
            failed = true;
        else
            pos = to;
    }

private:
    std::vector<char> data;
    std::size_t pos = 0;
    bool failed = false;
};

/// RMS amplitude of a sum of modes over [0, T], cross terms ignored: sum |a|^2 (1 - exp(-2 sigma T)) / (2 sigma T) / 2.
float partialLevel(const ModalProfile::Mode* modes, int count, double seconds) noexcept
{
    double e = 0.0;
    for (int k = 0; k < count; ++k) {
        const auto& m = modes[k];
        const double a2 = 0.5 * ((double) std::norm(m.left) + (double) std::norm(m.right));
        const double x = 2.0 * std::max(m.decay, 1e-6) * seconds;
        e += 0.5 * a2 * (x > 1e-9 ? (1.0 - std::exp(-x)) / x : 1.0);
    }
    return (float) std::sqrt(e);
}

/// Each partial's level over the sustain window from its modes' energies alone (Layer::separateLevels): the
/// diagonal of the measure's closed form (pf::measureLayers), both channels averaged. Two real exponentials per
/// mode: a few milliseconds for a whole piano.
void measureSeparateLevels(ModalProfile& p)
{
    const double from = p.sustainFrom, to = p.sustainTo;
    for (auto& note : p.notes)
        for (auto& layer : note.layers) {
            layer.separateLevels.assign(layer.partials.size(), 0.0f);
            for (std::size_t pi = 0; pi < layer.partials.size(); ++pi) {
                const auto& part = layer.partials[pi];
                double e = 0.0;
                for (int q = 0; q < part.modeCount; ++q) {
                    const auto& m = layer.modes[(std::size_t) (part.firstMode + q)];
                    const double x = 2.0 * m.decay;
                    const double span = std::abs(x) < 1e-9 ? to - from : (std::exp(-x * from) - std::exp(-x * to)) / x;
                    e += 0.25 * ((double) std::norm(m.left) + (double) std::norm(m.right)) * span;
                }
                layer.separateLevels[pi] = (float) std::sqrt(std::max(e, 0.0) / (to - from));
            }
        }
}

} // namespace

void DamperCurves::evaluate(int note, double holdSeconds, float* gains, int velocity) const noexcept
{
    int n0, h0, v0 = 0;
    double wn, wh, wv = 0.0;
    bracket(notes, (double) note, n0, wn);
    bracket(logHolds, std::log(std::max(holdSeconds, 1.0e-3)), h0, wh);
    const int numHolds = (int) holds.size();
    const int n1 = std::min(n0 + 1, (int) notes.size() - 1), h1 = std::min(h0 + 1, numHolds - 1);
    // The table of the average, or those of the two measured velocities around the voice's.
    const std::size_t table = notes.size() * holds.size() * (std::size_t) length;
    const float* t0 = db.data();
    const float* t1 = db.data();
    if (velocity >= 0 && byVelocity()) {
        bracket(velocities, (double) velocity, v0, wv);
        t0 = dbByVelocity.data() + (std::size_t) v0 * table;
        t1 = dbByVelocity.data() + (std::size_t) std::min(v0 + 1, (int) velocities.size() - 1) * table;
    }
    auto at = [&](const float* t, int n, int h, int k) {
        return (double) t[((std::size_t) n * (std::size_t) numHolds + (std::size_t) h) * (std::size_t) length + (std::size_t) k];
    };
    auto value = [&](const float* t, int k) {
        const double low = at(t, n0, h0, k) * (1 - wh) + at(t, n0, h1, k) * wh;
        const double high = at(t, n1, h0, k) * (1 - wh) + at(t, n1, h1, k) * wh;
        return low * (1 - wn) + high * wn;
    };
    for (int k = 0; k < length; ++k)
        gains[k] = (float) std::pow(10.0, (value(t0, k) * (1 - wv) + value(t1, k) * wv) / 20.0);
}

void DamperCurves::evaluateGroups(int note, double holdSeconds, float* gains, int velocity) const noexcept
{
    const int count = groups();
    const std::size_t table = notes.size() * holds.size() * (std::size_t) count * (std::size_t) length;
    if (count == 0 || groupDb.size() != table)
        return;
    int n0, h0, v0 = 0;
    double wn, wh, wv = 0.0;
    bracket(notes, (double) note, n0, wn);
    bracket(logHolds, std::log(std::max(holdSeconds, 1.0e-3)), h0, wh);
    const int numHolds = (int) holds.size();
    const int n1 = std::min(n0 + 1, (int) notes.size() - 1), h1 = std::min(h0 + 1, numHolds - 1);
    const float* t0 = groupDb.data();
    const float* t1 = groupDb.data();
    if (velocity >= 0 && byVelocity() && groupDbByVelocity.size() == velocities.size() * table) {
        bracket(velocities, (double) velocity, v0, wv);
        t0 = groupDbByVelocity.data() + (std::size_t) v0 * table;
        t1 = groupDbByVelocity.data() + (std::size_t) std::min(v0 + 1, (int) velocities.size() - 1) * table;
    }
    auto at = [&](const float* t, int n, int h, int g, int k) {
        return (double) t[(((std::size_t) n * (std::size_t) numHolds + (std::size_t) h) * (std::size_t) count + (std::size_t) g)
                              * (std::size_t) length + (std::size_t) k];
    };
    auto value = [&](const float* t, int g, int k) {
        const double low = at(t, n0, h0, g, k) * (1 - wh) + at(t, n0, h1, g, k) * wh;
        const double high = at(t, n1, h0, g, k) * (1 - wh) + at(t, n1, h1, g, k) * wh;
        return low * (1 - wn) + high * wn;
    };
    for (int g = 0; g < count; ++g)
        for (int k = 0; k < length; ++k)
            gains[(std::size_t) g * (std::size_t) length + (std::size_t) k]
                = (float) std::pow(10.0, (value(t0, g, k) * (1 - wv) + value(t1, g, k) * wv) / 20.0);
}

std::shared_ptr<const ModalProfile> parseModalProfile(std::vector<char> data, const std::string& source, std::string& error)
{
    Reader r(std::move(data));

    char magic[8] {};
    r.bytes(magic, 8);
    const auto version = std::memcmp(magic, "PFMODAL1", 8) == 0 ? r.get<std::uint32_t>() : 0u;
    // Version 2 adds the fade time of each partial; version 3, tagged sections after the notes.
    if (version < 1 || version > 3) {
        error = "not a modal profile (version 1 to 3): " + source;
        return nullptr;
    }
    auto p = std::make_shared<ModalProfile>();
    p->residualRate = r.get<double>();
    const auto channels = r.get<std::uint32_t>();
    if (channels != 2) {
        error = "only stereo profiles are supported";
        return nullptr;
    }

    const auto poles = r.get<std::uint32_t>();
    p->bodyFreq.resize(poles);
    p->bodyDecay.resize(poles);
    for (std::uint32_t k = 0; k < poles && r.ok(); ++k) {
        p->bodyFreq[k] = r.get<double>();
        p->bodyDecay[k] = r.get<double>();
    }

    auto& d = p->dampers;
    d.stepSeconds = r.get<double>();
    const auto dn = r.get<std::uint32_t>(), dh = r.get<std::uint32_t>(), dp = r.get<std::uint32_t>();
    if (dp > (std::uint32_t) DamperCurves::kMaxPoints || dn > 128 || dh > 64) {
        error = "damper curves too long";
        return nullptr;
    }
    d.notes.resize(dn);
    for (auto& n : d.notes)
        n = r.get<std::int32_t>();
    d.holds.resize(dh);
    for (auto& h : d.holds)
        h = r.get<double>();
    for (const double h : d.holds)
        d.logHolds.push_back(std::log(std::max(h, 1.0e-3)));
    d.length = (int) dp;
    d.db.resize((std::size_t) dn * dh * dp);
    r.bytes(d.db.data(), d.db.size() * sizeof(float));

    p->noteIndex.fill(-1);
    const auto noteCount = r.get<std::uint32_t>();
    if (noteCount > 128) {
        error = "too many notes";
        return nullptr;
    }
    for (std::uint32_t i = 0; i < noteCount && r.ok(); ++i) {
        ModalProfile::Note note;
        note.midi = (int) r.get<std::uint32_t>();
        const auto layers = r.get<std::uint32_t>();
        note.f0 = r.get<double>();
        if (note.midi < 0 || note.midi > 127 || layers > 128) {
            error = "corrupt note header";
            return nullptr;
        }
        for (std::uint32_t l = 0; l < layers && r.ok(); ++l) {
            ModalProfile::Layer layer;
            layer.velocity = (int) r.get<std::uint32_t>();
            const auto partials = r.get<std::uint32_t>(), modes = r.get<std::uint32_t>(), frames = r.get<std::uint32_t>();
            if (partials > 4096 || modes > 65536 || frames > 10'000'000) {
                error = "corrupt layer header";
                return nullptr;
            }
            int first = 0;
            for (std::uint32_t k = 0; k < partials; ++k) {
                ModalProfile::Partial part;
                part.number = (int) r.get<std::uint32_t>();
                part.modeCount = (int) r.get<std::uint32_t>();
                if (version >= 2)
                    part.fadeSeconds = r.get<float>();
                part.firstMode = first;
                first += part.modeCount;
                layer.partials.push_back(part);
            }
            if (first != (int) modes) {
                error = "partial table does not match the mode count";
                return nullptr;
            }
            layer.modes.resize(modes);
            for (auto& m : layer.modes) {
                m.freq = r.get<double>();
                m.decay = r.get<double>();
                float a[4] {}; // (left as zeros if the file ends here: the profile is then refused)
                r.bytes(a, sizeof(a));
                m.left = { a[0], a[1] };
                m.right = { a[2], a[3] };
            }
            layer.bodyLeft.resize(poles);
            layer.bodyRight.resize(poles);
            double body = 0.0;
            for (std::uint32_t k = 0; k < poles; ++k) {
                float a[4] {}; // (left as zeros if the file ends here: the profile is then refused)
                r.bytes(a, sizeof(a));
                layer.bodyLeft[k] = { a[0], a[1] };
                layer.bodyRight[k] = { a[2], a[3] };
                body += 0.5 * ((double) std::norm(layer.bodyLeft[k]) + (double) std::norm(layer.bodyRight[k]));
            }
            layer.bodyLevel = (float) std::sqrt(body);
            const float scale = r.get<float>();
            std::vector<std::int16_t> pcm((std::size_t) frames * 2);
            r.bytes(pcm.data(), pcm.size() * sizeof(std::int16_t));
            layer.residual.resize(pcm.size());
            for (std::size_t k = 0; k < pcm.size(); ++k)
                layer.residual[k] = (float) pcm[k] * scale;
            layer.residualFrames = (int) frames;

            double level = 0.0;
            for (auto& part : layer.partials) {
                part.level = partialLevel(layer.modes.data() + part.firstMode, part.modeCount, 1.0);
                level += (double) part.level * part.level;
            }
            layer.level = (float) std::sqrt(level);
            p->maxModesPerLayer = std::max(p->maxModesPerLayer, (int) modes);
            p->maxPartialsPerLayer = std::max(p->maxPartialsPerLayer, (int) layer.partials.size());
            p->bytes += layer.modes.size() * sizeof(ModalProfile::Mode) + layer.residual.size() * sizeof(float)
                        + 2 * poles * sizeof(std::complex<float>);
            note.layers.push_back(std::move(layer));
        }
        std::sort(note.layers.begin(), note.layers.end(), [](const auto& a, const auto& b) { return a.velocity < b.velocity; });
        if (!note.layers.empty()) {
            p->noteIndex[(std::size_t) note.midi] = (int) p->notes.size();
            p->notes.push_back(std::move(note));
        }
    }

    // Tagged sections (version 3): char[4] tag, u32 payload bytes, payload. Unknown tags are skipped.
    while (version >= 3 && r.ok() && !r.atEnd()) {
        char tag[4] {};
        r.bytes(tag, 4);
        const auto length = r.get<std::uint32_t>();
        const auto end = r.position() + length;
        if (!r.ok() || end > r.length()) {
            error = "corrupt section in " + source;
            return nullptr;
        }
        if (std::memcmp(tag, "SYMP", 4) == 0) {
            p->stringOnsetSeconds = std::max(0.0f, r.get<float>());
            p->stringVelocityExponent = std::clamp(r.get<float>(), 0.0f, 4.0f);
            p->stringReferenceVelocity = std::clamp(r.get<float>(), 1.0f, 127.0f);
            const auto count = r.get<std::uint32_t>();
            if (count > 128) {
                error = "too many sympathetic strings";
                return nullptr;
            }
            for (std::uint32_t s = 0; s < count && r.ok(); ++s) {
                ModalProfile::String string;
                string.midi = (int) r.get<std::uint32_t>();
                const auto resonators = r.get<std::uint32_t>();
                if (resonators > 65536) {
                    error = "corrupt sympathetic string";
                    return nullptr;
                }
                string.resonators.resize(resonators);
                for (auto& res : string.resonators) {
                    res.freq = r.get<double>();
                    res.decay = r.get<double>();
                    float a[5];
                    r.bytes(a, sizeof(a));
                    res.gain = a[0];
                    res.left = { a[1], a[2] };
                    res.right = { a[3], a[4] };
                }
                p->stringResonators += (int) resonators;
                p->strings.push_back(std::move(string));
            }
            if (r.position() + sizeof(float) <= end) // added after the strings: older profiles end here
                p->stringDriveDecaySeconds = std::max(0.0f, r.get<float>());
            if (r.position() + sizeof(std::uint32_t) <= end) { // then the registers
                const auto registers = r.get<std::uint32_t>();
                if (registers > 128) {
                    error = "corrupt sympathetic registers";
                    return nullptr;
                }
                for (std::uint32_t k = 0; k < registers && r.ok(); ++k) {
                    ModalProfile::DriveShape shape;
                    shape.firstNote = r.get<std::int32_t>();
                    shape.onsetSeconds = std::max(0.0f, r.get<float>());
                    shape.decaySeconds = std::max(0.0f, r.get<float>());
                    p->stringDriveShapes.push_back(shape);
                }
            }
            if (r.position() + sizeof(std::uint32_t) <= end) { // then each register's gain and velocity exponent
                const auto registers = r.get<std::uint32_t>();
                for (std::uint32_t k = 0; k < registers && r.ok(); ++k) {
                    const float gainDb = r.get<float>(), exponent = r.get<float>();
                    if (k < p->stringDriveShapes.size()) {
                        p->stringDriveShapes[k].gainDb = std::isfinite(gainDb) ? std::clamp(gainDb, -40.0f, 40.0f) : 0.0f;
                        p->stringDriveShapes[k].velocityExponent = std::isfinite(exponent) ? std::clamp(exponent, 0.0f, 4.0f) : -1.0f;
                    }
                }
            }
        } else if (std::memcmp(tag, "UNIS", 4) == 0) {
            // The coupled strings, per note, layer and partial; a layer that does not match the profile's is skipped.
            const auto count = r.get<std::uint32_t>();
            for (std::uint32_t n = 0; n < count && r.ok() && r.position() < end; ++n) {
                const auto midi = (int) r.get<std::uint32_t>();
                const auto layers = r.get<std::uint32_t>();
                ModalProfile::Note* note = nullptr;
                for (auto& candidate : p->notes)
                    if (candidate.midi == midi)
                        note = &candidate;
                for (std::uint32_t l = 0; l < layers && r.ok(); ++l) {
                    const auto velocity = (int) r.get<std::uint32_t>();
                    const auto partials = r.get<std::uint32_t>();
                    ModalProfile::Layer* layer = nullptr;
                    if (note != nullptr)
                        for (auto& candidate : note->layers)
                            if (candidate.velocity == velocity && candidate.partials.size() == partials)
                                layer = &candidate;
                    std::vector<ModalProfile::Unison> table(layer != nullptr ? partials : 0);
                    for (std::uint32_t k = 0; k < partials && r.ok(); ++k) {
                        std::uint8_t head[4];
                        float values[5];
                        r.bytes(head, sizeof(head));
                        r.bytes(values, sizeof(values));
                        if (layer == nullptr)
                            continue;
                        auto& u = table[k];
                        const auto& part = layer->partials[k];
                        u.strings = head[0] <= 3 ? head[0] : 0;
                        for (int i = 0; i < 3; ++i) {
                            u.modes[(std::size_t) i] = head[i + 1];
                            u.detuneHz[(std::size_t) i] = std::isfinite(values[i]) ? values[i] : 0.0f;
                        }
                        u.bridge = { values[3], values[4] };
                        for (int i = 0; i < u.strings; ++i)
                            if (u.modes[(std::size_t) i] >= part.modeCount || !std::isfinite(values[3]) || !std::isfinite(values[4]))
                                u.strings = 0;
                    }
                    if (layer != nullptr)
                        layer->unison = std::move(table);
                }
            }
        } else if (std::memcmp(tag, "LEVL", 4) == 0) {
            // The layers' measures; a layer that does not match the profile's keeps none, and without all of them
            // the profile counts as not measured.
            const float attack = r.get<float>(), from = r.get<float>(), to = r.get<float>();
            const auto count = r.get<std::uint32_t>();
            std::size_t matched = 0;
            for (std::uint32_t n = 0; n < count && r.ok() && r.position() < end; ++n) {
                const auto midi = (int) r.get<std::uint32_t>();
                const auto layers = r.get<std::uint32_t>();
                ModalProfile::Note* note = nullptr;
                for (auto& candidate : p->notes)
                    if (candidate.midi == midi)
                        note = &candidate;
                for (std::uint32_t l = 0; l < layers && r.ok(); ++l) {
                    const auto velocity = (int) r.get<std::uint32_t>();
                    const float energy = r.get<float>();
                    const auto partials = r.get<std::uint32_t>();
                    if (partials > 65536) {
                        error = "corrupt layer measures";
                        return nullptr;
                    }
                    std::vector<float> levels(partials);
                    r.bytes(levels.data(), levels.size() * sizeof(float));
                    ModalProfile::Layer* layer = nullptr;
                    if (note != nullptr)
                        for (auto& candidate : note->layers)
                            if (candidate.velocity == velocity && candidate.partials.size() == partials)
                                layer = &candidate;
                    if (layer == nullptr || !std::isfinite(energy) || energy < 0.0f)
                        continue;
                    bool finite = true;
                    for (const float v : levels)
                        finite = finite && std::isfinite(v) && v >= 0.0f;
                    if (!finite)
                        continue;
                    layer->attackEnergy = energy;
                    layer->sustainLevels = std::move(levels);
                    ++matched;
                }
            }
            std::size_t total = 0;
            for (const auto& note : p->notes)
                total += note.layers.size();
            if (r.ok() && matched == total && attack > 0.0f && to > from && from >= 0.0f) {
                p->attackSeconds = attack;
                p->sustainFrom = from;
                p->sustainTo = to;
                measureSeparateLevels(*p);
            }
        } else if (std::memcmp(tag, "PEDL", 4) == 0) {
            r.bytes(p->pedalLift.data(), p->pedalLift.size() * sizeof(float));
        } else if (std::memcmp(tag, "RELN", 4) == 0) {
            auto& noise = p->releaseNoise;
            const auto notes = r.get<std::uint32_t>(), velocities = r.get<std::uint32_t>();
            if (notes > 128 || velocities > 128) {
                error = "corrupt release noise";
                return nullptr;
            }
            noise.notes.resize(notes);
            noise.velocities.resize(velocities);
            for (auto& n : noise.notes)
                n = r.get<std::int32_t>();
            for (auto& v : noise.velocities)
                v = r.get<std::int32_t>();
            noise.levelDb.resize((std::size_t) notes * velocities);
            r.bytes(noise.levelDb.data(), noise.levelDb.size() * sizeof(float));
            const std::size_t count = (std::size_t) notes * velocities * poles;
            noise.left.resize(count);
            noise.right.resize(count);
            for (std::size_t k = 0; k < count && r.ok(); ++k) {
                float a[4] {}; // (left as zeros if the file ends here: the profile is then refused)
                r.bytes(a, sizeof(a));
                noise.left[k] = { a[0], a[1] };
                noise.right[k] = { a[2], a[3] };
            }
            if (r.position() + sizeof(std::uint32_t) <= end) { // added later: older profiles end here
                const auto holds = r.get<std::uint32_t>();
                if (holds > 64) {
                    error = "corrupt release noise holds";
                    return nullptr;
                }
                noise.holds.resize(holds);
                noise.holdDb.resize(holds);
                r.bytes(noise.holds.data(), holds * sizeof(float));
                r.bytes(noise.holdDb.data(), holds * sizeof(float));
            }
        } else if (std::memcmp(tag, "DAMP", 4) == 0) {
            auto& curves = p->dampers;
            const auto groups = r.get<std::uint32_t>();
            if (groups > (std::uint32_t) DamperCurves::kMaxGroups) {
                error = "too many damper groups";
                return nullptr;
            }
            curves.groupFirst.resize(groups);
            for (auto& first : curves.groupFirst)
                first = (int) r.get<std::uint32_t>();
            curves.groupDb.resize(curves.notes.size() * curves.holds.size() * groups * (std::size_t) curves.length);
            r.bytes(curves.groupDb.data(), curves.groupDb.size() * sizeof(float));
        } else if (std::memcmp(tag, "DAMV", 4) == 0) {
            auto& curves = p->dampers;
            const auto count = r.get<std::uint32_t>();
            if (count > 128) {
                error = "corrupt damper curves per velocity";
                return nullptr;
            }
            curves.velocities.resize(count);
            for (auto& v : curves.velocities)
                v = r.get<std::int32_t>();
            const std::size_t table = curves.notes.size() * curves.holds.size() * (std::size_t) curves.length;
            curves.dbByVelocity.resize(count * table);
            r.bytes(curves.dbByVelocity.data(), curves.dbByVelocity.size() * sizeof(float));
            const auto groups = r.get<std::uint32_t>();
            if (groups > 0) {
                if (groups != (std::uint32_t) curves.groups()) { // the groups come from DAMP, before this section
                    error = "damper groups per velocity without the same groups";
                    return nullptr;
                }
                curves.groupDbByVelocity.resize(count * table * groups);
                r.bytes(curves.groupDbByVelocity.data(), curves.groupDbByVelocity.size() * sizeof(float));
            }
        }
        if (r.position() > end) {
            error = "section longer than declared in " + source;
            return nullptr;
        }
        r.seek(end);
    }
    if (!r.ok() || !r.atEnd()) {
        error = "truncated or corrupt profile: " + source;
        return nullptr;
    }
    return p;
}

void measureLayers(ModalProfile& profile, double attackSeconds, double sustainFrom, double sustainTo)
{
    constexpr double kTwoPi = 6.283185307179586;
    const double rate = profile.residualRate > 0.0 ? profile.residualRate : 44100.0;
    const int frames = std::max(1, (int) std::lround(attackSeconds * rate));
    std::vector<double> x((std::size_t) frames);
    // A mode's mono sound over the attack, added to x (at the residual's rate; inaudible past its Nyquist).
    auto addMode = [&](double freq, double decay, std::complex<float> left, std::complex<float> right) {
        if (freq >= 0.5 * rate)
            return;
        const auto pole = std::polar(std::exp(-decay / rate), kTwoPi * freq / rate);
        auto z = 0.5 * (std::complex<double>(left) + std::complex<double>(right));
        for (auto& v : x) {
            v += z.real();
            z *= pole;
        }
    };
    // The energy of sum_k Re(a_k e^(s_k t)), s_k = -decay + i 2 pi freq, over [t0, t1], in closed form:
    // 1/2 sum_j sum_k Re(a_j a_k I(s_j + s_k) + a_j conj(a_k) I(s_j + conj(s_k))), I(s) = (e^(s t1) - e^(s t0)) / s.
    const double t0 = sustainFrom, t1 = sustainTo;
    auto integral = [t0, t1](std::complex<double> s) {
        if (std::abs(s) < 1e-9)
            return std::complex<double>(t1 - t0);
        return (std::exp(s * t1) - std::exp(s * t0)) / s;
    };
    const bool body = profile.bodyDecay.size() == profile.bodyFreq.size();
    for (auto& note : profile.notes)
        for (auto& layer : note.layers) {
            // The attack as recorded: the residual cancels the modes and the soundboard's resonances extrapolated
            // back to the note-on (at the softest layers of the bass, most of the soundboard's).
            for (int i = 0; i < frames; ++i)
                x[(std::size_t) i] = i < layer.residualFrames ? 0.5 * ((double) layer.residual[(std::size_t) (2 * i)]
                                                                       + (double) layer.residual[(std::size_t) (2 * i + 1)])
                                                              : 0.0;
            for (const auto& part : layer.partials) // the modes the engine plays (those of the partials)
                for (int q = 0; q < part.modeCount; ++q) {
                    const auto& m = layer.modes[(std::size_t) (part.firstMode + q)];
                    addMode(m.freq, m.decay, m.left, m.right);
                }
            if (body && layer.bodyLeft.size() == profile.bodyFreq.size() && layer.bodyRight.size() == profile.bodyFreq.size())
                for (std::size_t b = 0; b < profile.bodyFreq.size(); ++b)
                    addMode(profile.bodyFreq[b], profile.bodyDecay[b], layer.bodyLeft[b], layer.bodyRight[b]);
            double e = 0.0;
            for (const double v : x)
                e += v * v;
            layer.attackEnergy = (float) (e / frames);

            // Each partial over the sustain: its RMS, both channels averaged (as the profile's level, which is the
            // j = k terms over the first second).
            layer.sustainLevels.assign(layer.partials.size(), 0.0f);
            for (std::size_t pi = 0; pi < layer.partials.size(); ++pi) {
                const auto& part = layer.partials[pi];
                double sum = 0.0;
                for (int a = 0; a < part.modeCount; ++a) {
                    const auto& ma = layer.modes[(std::size_t) (part.firstMode + a)];
                    const std::complex<double> sa(-ma.decay, kTwoPi * ma.freq), la(ma.left), ra(ma.right);
                    for (int b = 0; b < part.modeCount; ++b) {
                        const auto& mb = layer.modes[(std::size_t) (part.firstMode + b)];
                        const std::complex<double> sb(-mb.decay, kTwoPi * mb.freq), lb(mb.left), rb(mb.right);
                        sum += 0.25 * ((la * lb + ra * rb) * integral(sa + sb)
                                       + (la * std::conj(lb) + ra * std::conj(rb)) * integral(sa + std::conj(sb))).real();
                    }
                }
                layer.sustainLevels[pi] = (float) std::sqrt(std::max(sum, 0.0) / (t1 - t0));
            }
        }
    profile.attackSeconds = attackSeconds;
    profile.sustainFrom = sustainFrom;
    profile.sustainTo = sustainTo;
    measureSeparateLevels(profile);
}

} // namespace pf
