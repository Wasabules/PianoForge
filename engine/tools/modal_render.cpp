// Offline renders and CPU measurements of the modal engine, without the application.
//
//   modal_render <profile> render <out.wav> <note> <velocity> <hold s> [tail s] [rate]
//       one note: note-on at 0, note-off after <hold s>, <tail s> more (default 3), 32-bit float stereo WAV;
//       --no-body / --no-residual (anywhere on the line) leave out the soundboard or the attack residual
//   modal_render <profile> events <out.wav> <events.txt> <seconds> [rate]
//       a sequence: one event per line, "<sample> <status> <data1> <data2>" (a take's MIDI, at the given rate);
//       --no-sympathetic, --no-held-keys, --no-catch, --no-release-noise (anywhere on the line) switch those engine
//       settings off;
//       --soft <0..1>: the soft pedal held through a render; --unison <spread>: the unison's detunings (1: measured)
//   modal_render <profile> bench [voices] [rate] [block]
//       strikes <voices> notes spread over the keyboard and times process() against the real-time budget
#include <pianoforge/modal_engine.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int rate)
{
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto frames = (std::uint32_t) l.size(), bytes = frames * 8;
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32((std::uint32_t) rate);
    u32((std::uint32_t) rate * 8);
    u16(8);
    u16(32);
    out.write("data", 4);
    u32(bytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        out.write(reinterpret_cast<const char*>(&l[i]), 4);
        out.write(reinterpret_cast<const char*>(&r[i]), 4);
    }
}

int mostVoices = 0; // the most voices active at once in the last run()

void run(pf::ModalEngine& engine, std::vector<pf::Event> events, int frames, int block, std::vector<float>& l,
         std::vector<float>& r, double* seconds = nullptr, double* worst = nullptr)
{
    mostVoices = 0;
    l.assign((std::size_t) frames, 0.0f);
    r.assign((std::size_t) frames, 0.0f);
    std::size_t next = 0;
    std::vector<pf::Event> local;
    double total = 0.0, peak = 0.0;
    for (int s = 0; s < frames; s += block) {
        const int n = std::min(block, frames - s);
        local.clear();
        while (next < events.size() && (int) events[next].offset < s + n) {
            auto e = events[next++];
            e.offset -= (std::uint32_t) s;
            local.push_back(e);
        }
        float* out[2] = { l.data() + s, r.data() + s };
        const auto t0 = std::chrono::steady_clock::now();
        engine.process(out, 2, n, local.data(), (int) local.size());
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        total += dt;
        peak = std::max(peak, dt);
        mostVoices = std::max(mostVoices, engine.activeVoices.load());
    }
    if (seconds != nullptr)
        *seconds = total;
    if (worst != nullptr)
        *worst = peak;
}

} // namespace

int main(int argc, char** argv)
{
    bool body = true, residual = true, sympathetic = true, heldKeys = true, pedalCatch = true, releaseNoise = true; // options
    double soft = 0.0, unison = 1.0, absorb = -1.0;
    bool nearest = false;
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--nearest") == 0)
            nearest = true;
        else if (std::strcmp(argv[i], "--absorb") == 0 && i + 1 < argc)
            absorb = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--soft") == 0 && i + 1 < argc)
            soft = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--unison") == 0 && i + 1 < argc)
            unison = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--no-body") == 0)
            body = false;
        else if (std::strcmp(argv[i], "--no-residual") == 0)
            residual = false;
        else if (std::strcmp(argv[i], "--no-sympathetic") == 0)
            sympathetic = false;
        else if (std::strcmp(argv[i], "--no-held-keys") == 0)
            heldKeys = false;
        else if (std::strcmp(argv[i], "--no-catch") == 0)
            pedalCatch = false;
        else if (std::strcmp(argv[i], "--no-release-noise") == 0)
            releaseNoise = false;
        else
            argv[kept++] = argv[i];
    }
    argc = kept;
    if (argc < 3) {
        std::fprintf(stderr, "usage: modal_render <profile> render <out.wav> <note> <velocity> <hold s> [tail s] [rate]\n"
                             "                    [--no-body] [--no-residual] [--soft <0..1>] [--unison <spread>]\n"
                             "       modal_render <profile> events <out.wav> <events.txt> <seconds> [rate]\n"
                             "                    [--no-sympathetic] [--no-held-keys] [--no-catch] [--no-release-noise]\n"
                             "                    [--absorb <0..1>] [--nearest: the nearest layer, not blended]\n"
                             "       modal_render <profile> bench [voices] [rate] [block]\n");
        return 2;
    }
    std::string error;
    auto profile = pf::loadModalProfile(argv[1], error);
    if (profile == nullptr) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    pf::ModalEngine engine;
    engine.install(profile);
    engine.bodyEnabled = body;
    engine.residualEnabled = residual;
    engine.sympatheticEnabled = sympathetic;
    engine.heldKeysResonate = heldKeys;
    engine.pedalCatch = pedalCatch;
    engine.releaseNoiseEnabled = releaseNoise;
    engine.unisonSpread = (float) unison;
    if (absorb >= 0.0)
        engine.restrikeAbsorption = (float) absorb;
    engine.blendLayers = !nearest;
    const std::string mode = argv[2];

    if (mode == "events" && argc >= 6) {
        std::ifstream in(argv[4]);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", argv[4]);
            return 1;
        }
        std::vector<pf::Event> events;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            long long sample = 0;
            int status = 0, d1 = 0, d2 = 0;
            if (fields >> sample >> status >> d1 >> d2)
                events.push_back({ (std::uint32_t) sample, (std::uint8_t) status, (std::uint8_t) d1, (std::uint8_t) d2 });
        }
        const int rate = argc > 6 ? std::atoi(argv[6]) : 44100, block = 64;
        engine.prepare(rate, block);
        const int frames = (int) (std::atof(argv[5]) * rate);
        std::vector<float> l, r;
        run(engine, events, frames, block, l, r);
        writeWav(argv[3], l, r, rate);
        std::printf("%s: %zu events, %.1f s, at most %d voices, %u stolen\n", argv[3], events.size(), (double) frames / rate,
                    mostVoices, engine.stolenVoices.load());
        return 0;
    }

    if (mode == "render" && argc >= 7) {
        const int note = std::atoi(argv[4]), velocity = std::atoi(argv[5]);
        const double hold = std::atof(argv[6]), tail = argc > 7 ? std::atof(argv[7]) : 3.0;
        const int rate = argc > 8 ? std::atoi(argv[8]) : 44100, block = 64;
        engine.prepare(rate, block);
        const int off = (int) (hold * rate), frames = (int) ((hold + tail) * rate);
        std::vector<pf::Event> events { { 0, 0xB0, 67, (std::uint8_t) std::lround(std::clamp(soft, 0.0, 1.0) * 127.0) },
                                        { 0, 0x90, (std::uint8_t) note, (std::uint8_t) velocity },
                                        { (std::uint32_t) off, 0x80, (std::uint8_t) note, 0 } };
        std::vector<float> l, r;
        run(engine, events, frames, block, l, r);
        writeWav(argv[3], l, r, rate);
        std::printf("%s: note %d velocity %d (layer %d), %.1f s\n", argv[3], note, velocity,
                    engine.lastLayerVelocity.load(), (double) frames / rate);
        return 0;
    }
    if (mode == "bench") {
        const int voices = argc > 3 ? std::atoi(argv[3]) : 32, rate = argc > 4 ? std::atoi(argv[4]) : 48000;
        const int block = argc > 5 ? std::atoi(argv[5]) : 64;
        engine.prepare(rate, block);
        // Sustain pedal down, then <voices> notes struck 10 ms apart, from the bass up: the worst case is the
        // bass, with the most partials.
        std::vector<pf::Event> events { { 0, 0xB0, 64, 127 } };
        for (int i = 0; i < voices; ++i)
            events.push_back({ (std::uint32_t) (i * rate / 100), 0x90, (std::uint8_t) (21 + (i * 7) % 88), 80 });
        const int frames = (int) (4.0 * rate);
        std::vector<float> l, r;
        double total = 0.0, worst = 0.0;
        run(engine, events, frames, block, l, r, &total, &worst);
        const double audio = (double) frames / rate, budget = (double) block / rate;
        std::printf("%d voices, %d modes at the end, %d Hz, blocks of %d: %.1f %% of real time on average, "
                    "worst block %.0f us of %.0f us\n",
                    voices, engine.activeModes.load(), rate, block, 100.0 * total / audio, worst * 1e6, budget * 1e6);
        return 0;
    }
    if (mode == "kernel") { // the resonator bank alone: cost per mode and per sample
        const int modes = argc > 3 ? std::atoi(argv[3]) : 2048, block = argc > 4 ? std::atoi(argv[4]) : 64;
        pf::ModeBank bank;
        bank.reserve(modes);
        for (int k = 0; k < modes; ++k)
            bank.add(pf::modePole(30.0 + 7.3 * k, 0.5, 48000.0), { 1e-3f, 0.0f }, { 0.0f, 1e-3f });
        std::vector<float> l((std::size_t) block), r((std::size_t) block);
        const int blocks = 48000 * 4 / block;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
            bank.process(l.data(), r.data(), block);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("kernel: %d modes, blocks of %d: %.3f ns per mode and sample (%.0f modes in real time at 48 kHz)\n",
                    modes, block, 1e9 * s / ((double) blocks * block * modes), (double) modes * 4.0 / s);
        return 0;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
