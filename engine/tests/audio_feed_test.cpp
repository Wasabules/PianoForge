// Audio feed tests (shared memory, as between PianoForge and the OBS plugin, here within one process).
#include <pianoforge/audio_feed.hpp>
#include <pianoforge/drift_resampler.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
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
    _putenv_s("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedUnitTest");
#else
    setenv("PIANOFORGE_AUDIO_FEED", "PianoForgeFeedUnitTest", 1);
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

    // Three mixes in step, the events at their frames, the volume and the writer's clock.
    {
        std::vector<float> mix[3][2];
        for (int m = 0; m < 3; ++m)
            for (int c = 0; c < 2; ++c)
                mix[m][c].assign(64, (float) (10 * (m + 1) + c));
        pf::AudioFeedWriter::Block block;
        for (int m = 0; m < 3; ++m) {
            block.left[m] = mix[m][0].data();
            block.right[m] = mix[m][1].data();
        }
        block.frames = 64;
        const pf::FeedEvent events[2] = { { 5, 0x90, 60, 100, 0, 0 }, { 40, 0xB0, 64, 127, 0, 0 } };
        block.events = events;
        block.numEvents = 2;
        block.volume = 0.5f;
        const auto before = reader.written(), eventsBefore = reader.eventsWritten();
        const auto now = pf::feedClockNs();
        writer.write(block);
        bool mixes = true;
        for (int m = 0; m < 3; ++m) {
            float frame[2];
            mixes = mixes && reader.read((pf::FeedMix) m, before + 10, frame, 1) == 1 && frame[0] == (float) (10 * (m + 1))
                    && frame[1] == (float) (10 * (m + 1) + 1);
        }
        check(mixes, "three mixes of the same frames, each its own");
        block.left[1] = block.right[1] = nullptr; // a mix not rendered takes the heard one
        writer.write(block);
        float frame[2];
        check(reader.read(pf::FeedMix::speakers, before + 64 + 3, frame, 1) == 1 && frame[0] == 10.0f && frame[1] == 11.0f,
              "a mix not rendered is the heard one");
        pf::FeedEvent got[4];
        const int n = reader.readEvents(eventsBefore, got, 4);
        check(n == 4 && got[0].frame == before + 5 && got[0].status == 0x90 && got[0].data1 == 60 && got[1].frame == before + 40
                  && got[2].frame == before + 64 + 5,
              "the events, at their frames of the feed");
        std::uint64_t frames = 0, ns = 0;
        check(reader.clock(frames, ns) && frames == before + 128 && ns >= now && ns - now < 1'000'000'000ull,
              "the writer's clock: the frames written, as of a steady instant");
        check(reader.volume() == 0.5f, "the application's volume rides along");
    }

    // The state for overlays, the marks, the readers.
    {
        pf::FeedInfo info;
        std::snprintf(info.engine, sizeof(info.engine), "Modal");
        std::snprintf(info.preset, sizeof(info.preset), "Concert");
        std::snprintf(info.file, sizeof(info.file), "Gymnopédie n°1");
        info.recording = 1;
        writer.setInfo(info);
        pf::FeedInfo seen;
        check(reader.info(seen) && std::string(seen.engine) == "Modal" && std::string(seen.file) == "Gymnopédie n°1"
                  && seen.recording == 1,
              "the state for overlays, whole");
        const auto marksBefore = reader.marks();
        for (int k = 0; k < 20; ++k)
            writer.addMark(("Take " + std::to_string(k)).c_str());
        char text[128];
        check(reader.marks() == marksBefore + 20 && reader.mark(marksBefore + 19, text, sizeof(text)) && std::string(text) == "Take 19"
                  && !reader.mark(marksBefore, text, sizeof(text)),
              "marks: the last 16 kept, older ones gone");
        pf::AudioFeedReader audioReader, keyboardReader;
        audioReader.open(pf::FeedReaderKind::audio);
        keyboardReader.open(pf::FeedReaderKind::keyboard);
        audioReader.heartbeat(pf::FeedMix::speakers, 31.5f);
        pf::AudioFeedWriter::Reader seenReaders[8];
        int count = writer.readers(seenReaders, 8);
        bool found = false;
        for (int k = 0; k < count; ++k)
            found = found || (seenReaders[k].kind == pf::FeedReaderKind::audio && seenReaders[k].mix == pf::FeedMix::speakers
                              && seenReaders[k].latencyMs == 31.5f);
        check(count == 2 && found, "the readers: what each reads, and how late");
        keyboardReader.close();
        check(writer.readers(seenReaders, 8) == 1, "a reader gone frees its slot");
        audioReader.close();
    }

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
