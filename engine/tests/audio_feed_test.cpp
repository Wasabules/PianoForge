// Audio feed tests (shared memory, as between PianoForge and the OBS plugin, here within one process).
#include <pianoforge/audio_feed.hpp>
#include <pianoforge/drift_resampler.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s  %s\n", condition ? "[PASS]" : "[FAIL]", what);
    failures += condition ? 0 : 1;
}

} // namespace

int main()
{
    // A feed of the test's own: it runs beside a PianoForge in use.
#if defined(_WIN32)
    _putenv_s("PIANOFORGE_AUDIO_FEED", "PianoForgeAudioFeedUnitTest");
#else
    setenv("PIANOFORGE_AUDIO_FEED", "PianoForgeAudioFeedUnitTest", 1);
#endif
    pf::AudioFeedReader early;
    check(!early.open(), "no writer yet: nothing to open");

    pf::AudioFeedWriter writer;
    check(writer.create(), "the writer creates the feed");
    pf::AudioFeedWriter second;
#if defined(_WIN32)
    check(!second.create(), "a second writer is refused (its blocks would interleave)");
#endif
    pf::AudioFeedReader reader;
    check(reader.open(), "the reader opens it");
    pf::AudioFeedReader::Format format;
    check(!reader.format(format), "no format before the device starts");

    writer.setFormat(44100, 128);
    check(reader.format(format) && format.sampleRate == 44100 && format.blockSize == 128, "the format, once set");
    const auto session = format.session;

    // Blocks of a ramp, read back in other sizes.
    std::vector<float> l(128), r(128);
    const float* channels[2] = { l.data(), r.data() };
    const auto start = reader.written();
    for (int b = 0; b < 40; ++b) {
        for (int k = 0; k < 128; ++k) {
            l[(std::size_t) k] = (float) (b * 128 + k);
            r[(std::size_t) k] = -(float) (b * 128 + k);
        }
        writer.write(channels, 2, 128);
    }
    check(reader.written() - start == 40 * 128, "every frame written is counted");
    std::vector<float> out(2 * 1000);
    bool exact = true;
    std::uint64_t at = start;
    while (at < reader.written()) {
        const int n = reader.read(at, out.data(), 1000);
        if (n <= 0) {
            exact = false;
            break;
        }
        for (int k = 0; k < n; ++k)
            exact = exact && out[(std::size_t) (2 * k)] == (float) (at - start + (std::uint64_t) k)
                    && out[(std::size_t) (2 * k + 1)] == -(float) (at - start + (std::uint64_t) k);
        at += (std::uint64_t) n;
    }
    check(exact, "read back in other spans: the same frames, in order");

    // A reader left far behind is told so instead of reading frames being overwritten.
    for (int b = 0; b < 400; ++b)
        writer.write(channels, 2, 128);
    check(reader.read(start, out.data(), 100) == -1, "a reader lapped by the writer gets -1");

    writer.setFormat(48000, 256);
    check(reader.format(format) && format.sampleRate == 48000 && format.session != session, "a new format, a new session");

    writer.close();
    check(!reader.format(format), "the writer gone: no format (the reader waits)");
    pf::AudioFeedWriter again;
    check(again.create(), "a new writer joins the feed the reader still holds");
    again.setFormat(44100, 64);
    check(reader.format(format) && format.sampleRate == 44100, "and the reader sees it");

    // The resampler at equal nominal rates: frames in, the same frames out (delayed), once started.
    pf::DriftResampler resampler;
    resampler.prepare(44100.0, 44100.0, 64, 256);
    std::vector<float> interleaved(2 * 64), left(256), right(256);
    float* outputs[2] = { left.data(), right.data() };
    double worst = 0.0;
    long long produced = 0, consumed = 0;
    for (int step = 0; step < 2000; ++step) {
        for (int k = 0; k < 64; ++k) {
            const double t = (double) (produced + k) / 44100.0;
            interleaved[(std::size_t) (2 * k)] = interleaved[(std::size_t) (2 * k + 1)] = (float) std::sin(6.283185307179586 * 440.0 * t);
        }
        resampler.append(interleaved.data(), 64);
        produced += 64;
        if (step % 4 == 3) {
            resampler.render(outputs, 2, 256);
            consumed += 256;
            if (resampler.running() && step > 400)
                for (int k = 0; k < 256; ++k)
                    worst = std::fmax(worst, std::fabs((double) left[(std::size_t) k] - (double) right[(std::size_t) k]));
        }
    }
    check(resampler.running() && resampler.underruns() == 0 && worst == 0.0, "the resampler runs, both channels alike");

    std::printf(failures ? "%d failure(s)\n" : "All audio feed tests passed\n", failures);
    return failures ? 1 : 0;
}
