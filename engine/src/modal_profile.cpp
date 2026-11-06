#include <pianoforge/modal_profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

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

} // namespace

void DamperCurves::evaluate(int note, double holdSeconds, float* gains) const noexcept
{
    int n0, h0;
    double wn, wh;
    bracket(notes, (double) note, n0, wn);
    bracket(logHolds, std::log(std::max(holdSeconds, 1.0e-3)), h0, wh);
    const int numHolds = (int) holds.size();
    const int n1 = std::min(n0 + 1, (int) notes.size() - 1), h1 = std::min(h0 + 1, numHolds - 1);
    auto at = [&](int n, int h, int k) {
        return db[((std::size_t) n * (std::size_t) numHolds + (std::size_t) h) * (std::size_t) length + (std::size_t) k];
    };
    for (int k = 0; k < length; ++k) {
        const double low = at(n0, h0, k) * (1 - wh) + at(n0, h1, k) * wh;
        const double high = at(n1, h0, k) * (1 - wh) + at(n1, h1, k) * wh;
        gains[k] = (float) std::pow(10.0, (low * (1 - wn) + high * wn) / 20.0);
    }
}

void DamperCurves::evaluateGroups(int note, double holdSeconds, float* gains) const noexcept
{
    const int count = groups();
    if (count == 0 || groupDb.size() != notes.size() * holds.size() * (std::size_t) count * (std::size_t) length)
        return;
    int n0, h0;
    double wn, wh;
    bracket(notes, (double) note, n0, wn);
    bracket(logHolds, std::log(std::max(holdSeconds, 1.0e-3)), h0, wh);
    const int numHolds = (int) holds.size();
    const int n1 = std::min(n0 + 1, (int) notes.size() - 1), h1 = std::min(h0 + 1, numHolds - 1);
    auto at = [&](int n, int h, int g, int k) {
        return groupDb[(((std::size_t) n * (std::size_t) numHolds + (std::size_t) h) * (std::size_t) count + (std::size_t) g)
                           * (std::size_t) length + (std::size_t) k];
    };
    for (int g = 0; g < count; ++g)
        for (int k = 0; k < length; ++k) {
            const double low = at(n0, h0, g, k) * (1 - wh) + at(n0, h1, g, k) * wh;
            const double high = at(n1, h0, g, k) * (1 - wh) + at(n1, h1, g, k) * wh;
            gains[(std::size_t) g * (std::size_t) length + (std::size_t) k] = (float) std::pow(10.0, (low * (1 - wn) + high * wn) / 20.0);
        }
}

std::shared_ptr<const ModalProfile> loadModalProfile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    std::vector<char> data((std::size_t) in.tellg());
    in.seekg(0);
    in.read(data.data(), (std::streamsize) data.size());
    Reader r(std::move(data));

    char magic[8] {};
    r.bytes(magic, 8);
    const auto version = std::memcmp(magic, "PFMODAL1", 8) == 0 ? r.get<std::uint32_t>() : 0u;
    // Version 2 adds the fade time of each partial; version 3, tagged sections after the notes.
    if (version < 1 || version > 3) {
        error = "not a modal profile (version 1 to 3): " + path.string();
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
                float a[4];
                r.bytes(a, sizeof(a));
                m.left = { a[0], a[1] };
                m.right = { a[2], a[3] };
            }
            layer.bodyLeft.resize(poles);
            layer.bodyRight.resize(poles);
            double body = 0.0;
            for (std::uint32_t k = 0; k < poles; ++k) {
                float a[4];
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
            error = "corrupt section in " + path.string();
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
                float a[4];
                r.bytes(a, sizeof(a));
                noise.left[k] = { a[0], a[1] };
                noise.right[k] = { a[2], a[3] };
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
        }
        if (r.position() > end) {
            error = "section longer than declared in " + path.string();
            return nullptr;
        }
        r.seek(end);
    }
    if (!r.ok() || !r.atEnd()) {
        error = "truncated or corrupt profile: " + path.string();
        return nullptr;
    }
    return p;
}

} // namespace pf
