# SPDX-License-Identifier: AGPL-3.0-or-later

"""The SFZ player, on a small bank made here (sines of known pitch and level): python -m unittest discover -s tools/bench/tests"""

import math
import tempfile
import unittest
from pathlib import Path

import numpy as np
import soundfile as sf

from bench.protocol import parse_session
from bench.sfz import SfzHost, SfzInstrument, SfzPlayer, parse_key

FS = 48000
C4, A4 = 261.6256, 440.0

BANK = """// A test bank: two velocity layers on C4 and A4, a release region, a round robin, a pedal noise.
#define $EXT wav
#define $RES 20
<control> default_path=samples/ set_hdcc$RES=0.5 label_cc$RES=Release noise
<global> amp_veltrack=0 note_polyphony=1 off_time=0.5 ampeg_release=0.5
<master> master_label=Notes
<group> lovel=1 hivel=64 #include "data/soft.txt"
<group> lovel=65 hivel=127 group_label=loud
<region> lokey=58 hikey=63 pitch_keycenter=60 sample=c4 loud.$EXT
<region> lokey=67 hikey=71 pitch_keycenter=a4 sample=a4 loud.$EXT /* a block
comment */
<global> trigger=release amp_veltrack=0
<group> rt_decay=6 locc$RES=1 amplitude_oncc$RES=200
<region> lokey=58 hikey=71 pitch_keycenter=60 sample=release.$EXT
<global>
<group> key=100 seq_length=2 amp_veltrack=0
<region> seq_position=1 sample=c4 soft.$EXT
<region> seq_position=2 sample=c4 loud.$EXT
<group> lokey=-1 hikey=-1 on_locc64=127 on_hicc64=127 group=7 polyphony=1 amp_veltrack=0
<region> lorand=0 hirand=0.5 pitch_keycenter=60 sample=release.$EXT
<region> lorand=0.5 hirand=1 pitch_keycenter=60 sample=release.$EXT volume=-6
"""
SOFT = """#define $LAYER soft
<region> lokey=58 hikey=63 pitch_keycenter=60 sample=c4 $LAYER.$EXT
<region> lokey=67 hikey=71 pitch_keycenter=69 sample=a4 $LAYER.$EXT
"""


def sine(freq: float, level: float, seconds: float = 3.0) -> np.ndarray:
    t = np.arange(int(seconds * FS)) / FS
    x = level * np.sin(2 * np.pi * freq * t)
    return np.stack([x, x], axis=1)


def make_bank(folder: Path, text: str = BANK) -> Path:
    (folder / "samples").mkdir()
    (folder / "data").mkdir()
    sf.write(folder / "samples" / "c4 soft.wav", sine(C4, 0.1), FS, subtype="PCM_24")
    sf.write(folder / "samples" / "c4 loud.wav", sine(C4, 0.5), FS, subtype="PCM_24")
    sf.write(folder / "samples" / "a4 soft.wav", sine(A4, 0.1), FS, subtype="PCM_24")
    sf.write(folder / "samples" / "a4 loud.wav", sine(A4, 0.5), FS, subtype="PCM_24")
    noise = np.random.default_rng(1).standard_normal((int(0.3 * FS), 2)) * 0.05
    sf.write(folder / "samples" / "release.wav", noise, FS, subtype="PCM_24")
    (folder / "data" / "soft.txt").write_text(SOFT, encoding="utf-8")
    path = folder / "bank.sfz"
    path.write_text(text, encoding="utf-8")
    return path


def rms_db(x: np.ndarray) -> float:
    return 10 * math.log10(float(np.mean(x.astype(np.float64) ** 2)) + 1e-30)


def pitch(x: np.ndarray, lo: float, hi: float) -> float:
    size = 1 << 21
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), size))
    freqs = np.fft.rfftfreq(size, 1 / FS)
    band = (freqs > lo) & (freqs < hi)
    return float(freqs[band][np.argmax(spectrum[band])])


def on(t: float, key: int, vel: int):
    return (round(t * FS), (0x90, key, vel))


def off(t: float, key: int):
    return (round(t * FS), (0x80, key, 64))


def cc(t: float, number: int, value: int):
    return (round(t * FS), (0xB0, number, value))


class Bank(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.path = make_bank(Path(self.folder.name))
        self.bank = SfzInstrument(self.path)
        self.player = SfzPlayer(self.bank, FS)

    def tearDown(self):
        self.folder.cleanup()

    def render(self, events, seconds=2.5):
        return self.player.render(sorted(events, key=lambda e: e[0]), round(seconds * FS))

    # -- the file

    def test_keys(self):
        self.assertEqual(parse_key("c4"), 60)
        self.assertEqual(parse_key("A0"), 21)
        self.assertEqual(parse_key("c#4"), 61)
        self.assertEqual(parse_key("db4"), 61)
        self.assertEqual(parse_key("-1"), -1)
        self.assertEqual(parse_key("c4", 12), 72)

    def test_parsing(self):
        regions = self.bank.regions
        self.assertEqual(len(regions), 9)
        self.assertEqual(self.bank.unsupported, {})
        soft = [r for r in regions if r.lovel == 1 and r.hivel == 64]
        self.assertEqual([r.sample.name for r in soft], ["c4 soft.wav", "a4 soft.wav"])  # #include within a line
        loud_a4 = [r for r in regions if r.sample.name == "a4 loud.wav"][0]
        self.assertEqual(loud_a4.keycenter, 69)  # a key by name; the comment after it is gone
        self.assertEqual(loud_a4.ops["note_polyphony"], "1")  # inherited from <global>
        release = [r for r in regions if r.trigger == "release"]
        self.assertEqual(len(release), 1)
        self.assertEqual(release[0].conditions, [(20, 1 / 127, 1.0)])  # $RES substituted
        self.assertAlmostEqual(self.bank.cc[20], 0.5)
        pedal = [r for r in regions if r.cc_trigger]
        self.assertEqual([r.cc_trigger for r in pedal], [(64, 1.0, 1.0)] * 2)
        self.assertEqual([r.sample.exists() for r in regions], [True] * 9)
        self.assertEqual(len(self.bank.fingerprint()), 16)

    def test_unsupported_opcodes_are_listed(self):
        with tempfile.TemporaryDirectory() as folder:
            path = make_bank(Path(folder), BANK + "<region> key=90 sample=c4 soft.wav loop_mode=loop_continuous "
                                                  "fil_type=lpf_2p cutoff=500\n<effect> type=reverb\n")
            unsupported = SfzInstrument(path).unsupported
        self.assertEqual(unsupported, {"loop_mode=loop_continuous": 1, "fil_type": 1, "cutoff": 1, "<effect>": 1})

    # -- playing

    def test_pitch_and_layers(self):
        x = self.render([on(0.0, 62, 100), off(2.0, 62)])
        self.assertAlmostEqual(pitch(x[0, FS // 2: FS // 2 + FS], 200, 400), C4 * 2 ** (2 / 12), delta=0.05)
        loud = rms_db(x[0, FS // 2: FS])
        soft = rms_db(self.render([on(0.0, 60, 40), off(2.0, 60)])[0, FS // 2: FS])
        self.assertAlmostEqual(loud - soft, 20 * math.log10(5), delta=0.05)  # 0.5 against 0.1, veltrack 0
        a4 = self.render([on(0.0, 69, 100), off(2.0, 69)])
        self.assertAlmostEqual(pitch(a4[1, FS // 2: FS // 2 + FS], 300, 600), A4, delta=0.05)
        self.assertAlmostEqual(rms_db(a4[1, FS // 2: FS]), 20 * math.log10(0.5 / math.sqrt(2)), delta=0.05)

    def test_note_on_at_its_sample(self):
        x = self.render([on(0.1, 60, 100), off(2.0, 60)])
        start = int(np.argmax(np.abs(x[0]) > 1e-4))
        self.assertLessEqual(abs(start - round(0.1 * FS)), 2)  # at its own pitch and rate: sample for sample

    def test_release_envelope_and_region(self):
        # Without the release region (its controller at 0): the note fades by exp(-9) over ampeg_release.
        quiet = SfzPlayer(SfzInstrument(self.path, cc={20: 0.0}), FS)
        x = quiet.render([on(0.0, 60, 100), off(1.0, 60)], round(2.0 * FS))
        before = rms_db(x[0, int(0.9 * FS): FS])
        after = rms_db(x[0, int(1.24 * FS): int(1.26 * FS)])
        self.assertAlmostEqual(after - before, -9 * 0.25 / 0.5 * 20 / math.log(10), delta=1.0)
        self.assertGreater(np.max(np.abs(x[:, int(1.6 * FS):int(1.62 * FS)])), 0.0)
        self.assertEqual(float(np.max(np.abs(x[:, int(1.7 * FS):]))), 0.0)  # ended under -100 dB (0.64 s)
        # With it: the noise from the note-off, for its 0.3 s, rt_decay (6 dB) lower per second held, at
        # amplitude_oncc 200 % x 0.5.
        y = self.render([on(0.0, 60, 100), off(1.0, 60)], seconds=2.0) - x
        self.assertEqual(float(np.max(np.abs(y[:, :FS]))), 0.0)
        self.assertGreater(np.max(np.abs(y[:, FS:FS + 100])), 0.0)
        self.assertEqual(float(np.max(np.abs(y[:, int(1.3 * FS) + 1:]))), 0.0)
        noise = sf.read(Path(self.folder.name) / "samples" / "release.wav", dtype="float32", always_2d=True)[0].T
        self.assertAlmostEqual(rms_db(y[0, FS:int(1.3 * FS)]) - rms_db(noise[0]), -6.0, delta=0.05)
        held_half = self.render([on(0.0, 60, 100), off(0.5, 60)], seconds=2.0) \
            - quiet.render([on(0.0, 60, 100), off(0.5, 60)], round(2.0 * FS))
        self.assertAlmostEqual(rms_db(held_half[0, FS // 2:int(0.8 * FS)]) - rms_db(noise[0]), -3.0, delta=0.05)

    def test_sustain_pedal(self):
        x = self.render([cc(0.0, 64, 0), on(0.1, 60, 100), off(0.5, 60), cc(0.05, 64, 127), cc(1.5, 64, 0)],
                        seconds=2.5)
        held = rms_db(x[0, int(1.2 * FS): int(1.4 * FS)])
        self.assertAlmostEqual(held, 20 * math.log10(0.5 / math.sqrt(2)), delta=0.3)  # still ringing, pedal down
        # The release region waits for the pedal: nothing but the sine before 1.5 s, the noise after it.
        sine_only = self.render([cc(0.05, 64, 127), on(0.1, 60, 100), off(0.5, 60)], seconds=1.49)
        self.assertLess(np.max(np.abs(x[0, :int(1.49 * FS)] - sine_only[0])), 1e-6)
        self.assertEqual(float(np.max(np.abs(x[:, int(2.3 * FS):]))), 0.0)

    def test_pedal_noise_and_random(self):
        presses = [e for k in range(12) for e in (cc(0.5 * k, 64, 127), cc(0.5 * k + 0.4, 64, 0))]
        a = self.render(presses, seconds=6.0)
        np.testing.assert_array_equal(a, self.render(presses, seconds=6.0))  # reproducible
        levels = [round(rms_db(a[0, int(0.5 * k * FS):int((0.5 * k + 0.3) * FS)]), 1) for k in range(12)]
        self.assertEqual(len(set(levels)), 2)  # one region or the other, 6 dB apart, press after press
        self.assertAlmostEqual(max(levels) - min(levels), 6.0, delta=0.1)
        first = self.render(presses[:2], seconds=0.5)
        np.testing.assert_array_equal(first, a[:, :first.shape[1]])  # a press draws the same, whatever follows
        self.assertEqual(float(np.max(np.abs(self.render([cc(0.1, 64, 100)], seconds=1.0)))), 0.0)

    def test_a_strike_draws_the_same_in_every_take(self):
        """A round robin by lorand/hirand: the first strike of a key plays the same sample in every take, the next
        ones may not."""
        with tempfile.TemporaryDirectory() as folder:
            path = make_bank(Path(folder), BANK + "<global>\n<group> key=90 amp_veltrack=0\n"
                                                  "<region> lorand=0 hirand=0.5 sample=c4 soft.wav\n"
                                                  "<region> lorand=0.5 hirand=1 sample=c4 loud.wav\n")
            player = SfzPlayer(SfzInstrument(path), FS)
            level = lambda x, t: round(rms_db(x[0, int(t * FS):int((t + 0.1) * FS)]), 1)
            alone = player.render([on(0.0, 90, 64), off(0.3, 90)], FS // 2)
            for other in (40, 80, 100):  # another key struck first: the draw of key 90 stays
                x = player.render([on(0.0, 62, other), on(0.05, 90, 64), off(0.35, 90)], int(0.6 * FS))
                y = player.render([on(0.0, 62, other)], int(0.6 * FS))
                self.assertAlmostEqual(level(x - y, 0.1), level(alone, 0.05), delta=0.05)
            many = player.render([e for k in range(10) for e in (on(0.3 * k, 90, 64), off(0.3 * k + 0.2, 90))], 3 * FS)
            self.assertEqual(len({level(many, 0.3 * k + 0.05) for k in range(10)}), 2)

    def test_no_silence_between_random_ranges(self):
        """Ranges with a gap (0 to 0.1 and 0.9 to 1, as Piano in 162's 0.500 / 0.501): every strike sounds, the
        regions of the nearest range."""
        with tempfile.TemporaryDirectory() as folder:
            path = make_bank(Path(folder), BANK + "<global>\n<group> key=90 amp_veltrack=0\n"
                                                  "<region> hirand=0.1 sample=c4 soft.wav\n"
                                                  "<region> lorand=0.9 sample=c4 loud.wav\n")
            x = SfzPlayer(SfzInstrument(path), FS).render(
                [e for k in range(20) for e in (on(0.2 * k, 90, 64), off(0.2 * k + 0.15, 90))], 4 * FS)
            levels = [rms_db(x[0, int((0.2 * k + 0.02) * FS):int((0.2 * k + 0.1) * FS)]) for k in range(20)]
            expected = [20 * math.log10(a / math.sqrt(2)) for a in (0.1, 0.5)]
            nearest = [min(expected, key=lambda e: abs(e - level)) for level in levels]
            self.assertLess(max(abs(n - level) for n, level in zip(nearest, levels)), 0.1)  # never silent
            self.assertEqual(set(nearest), set(expected))

    def test_note_polyphony(self):
        # A second strike cuts the first over off_time (0.5 s): soon only one sine is left.
        x = self.render([on(0.0, 60, 100), on(0.5, 60, 100), off(2.0, 60)], seconds=2.2)
        self.assertAlmostEqual(rms_db(x[0, int(1.4 * FS): int(1.9 * FS)]), 20 * math.log10(0.5 / math.sqrt(2)),
                               delta=0.1)
        self.assertGreater(rms_db(x[0, int(0.52 * FS): int(0.6 * FS)]), 20 * math.log10(0.5 / math.sqrt(2)) + 1)

    def test_round_robin(self):
        x = self.render([on(0.0, 100, 64), off(0.3, 100), on(0.5, 100, 64), off(0.8, 100), on(1.0, 100, 64)],
                        seconds=1.3)
        first, second, third = (rms_db(x[0, int(t * FS): int((t + 0.2) * FS)]) for t in (0.05, 0.55, 1.05))
        self.assertAlmostEqual(second - first, 20 * math.log10(5), delta=0.2)
        self.assertAlmostEqual(third, first, delta=0.2)

    def test_aligned_attacks(self):
        """A sample starting with 50 ms of silence: played as it is, or from 3 ms before its attack, faded in."""
        late = np.concatenate([np.zeros((int(0.05 * FS), 2)), sine(C4, 0.5)])
        sf.write(Path(self.folder.name) / "samples" / "c4 loud.wav", late, FS, subtype="PCM_24")
        bank = SfzInstrument(self.path)
        events = [on(0.0, 60, 100), off(1.0, 60)]
        played = SfzPlayer(bank, FS).render(events, FS)
        aligned = SfzPlayer(bank, FS, align_ms=3.0).render(events, FS)
        first = lambda x: int(np.argmax(np.abs(x[0]) > 0.5 * 10 ** (-30 / 20))) / FS * 1000
        self.assertAlmostEqual(first(played), 50.0, delta=0.2)
        self.assertAlmostEqual(first(aligned), 3.0, delta=0.2)
        shift = int(0.05 * FS) + 1 - int(0.003 * FS)  # the sine's first sample is 0: its attack is the next one
        np.testing.assert_allclose(aligned[:, 200:int(0.9 * FS)], played[:, 200 + shift:int(0.9 * FS) + shift],
                                   atol=1e-6)
        self.assertLess(np.max(np.abs(aligned[:, :int(0.003 * FS) - 50])), 1e-4)  # the fade, then the silence kept
        soft = SfzPlayer(bank, FS, align_ms=3.0).render([on(0.0, 60, 40), off(1.0, 60)], FS)
        np.testing.assert_array_equal(soft, SfzPlayer(bank, FS).render([on(0.0, 60, 40), off(1.0, 60)], FS))

    def test_sound_end(self):
        """A note held past its sample's end: where it stops is reported; released before, nothing is."""
        host = SfzHost(self.path, FS, cc={20: 0.0})  # no release region: nothing sounds past the cut
        session = parse_session('[session]\nsample_rate = 48000\n[[pass]]\nname = "n"\ntype = "note"\n'
                                'notes = ["C4"]\nvelocities = [100]\nholds = [1.0, 4.0]\ntail = 1.0\n')
        short, long = session.takes
        host.render_take(short)
        self.assertEqual(host.last_info, {})
        audio = host.render_take(long)
        end = host.last_info["sound_end_sample"]
        self.assertEqual(end, long.meta["note_on_sample"] + 3 * FS)  # the 3 s sample, at its own pitch and rate
        self.assertGreater(np.max(np.abs(audio[:, end - 100:end])), 0.1)
        self.assertEqual(float(np.max(np.abs(audio[:, end:]))), 0.0)

    # -- the bench's host

    def test_host(self):
        host = SfzHost(self.path, 44100)
        ready = host.wait_until_loaded()
        self.assertAlmostEqual(ready["a4_hz"], 440.0, delta=0.05)  # brought to 44.1 kHz at its pitch
        session = parse_session('[session]\nsample_rate = 44100\n[[pass]]\nname = "n"\ntype = "note"\n'
                                'notes = ["C4"]\nvelocities = [100]\nhold = 1.0\ntail = 1.0\n')
        take = session.takes[0]
        audio = host.render_take(take)
        self.assertEqual(audio.shape, (2, take.duration))
        self.assertEqual(audio.dtype, np.float32)
        np.testing.assert_array_equal(audio, host.render_take(take))

    def test_bench_renders_a_bank(self):
        """prepare-sfz, then render (in two processes) and summary, as for Kontakt."""
        import contextlib
        import io
        import json

        from bench.cli import main

        with tempfile.TemporaryDirectory() as folder:
            corpus, session = Path(folder) / "corpus", Path(folder) / "probe.toml"
            session.write_text('[session]\nname = "probe"\nsample_rate = 44100\n[[pass]]\nname = "notes"\n'
                               'type = "note"\nnotes = ["C4", "D4", "A4"]\nvelocities = [40, 100]\nhold = 0.5\n'
                               'tail = 0.7\n', encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(["--corpus", str(corpus), "prepare-sfz", "--state", "test", "--sfz",
                                       str(self.path), "--hdcc", "20=0", "--license", "CC0"]), 0)
                state = json.loads((corpus / "states" / "test.json").read_text(encoding="utf-8"))
                self.assertEqual((state["player"], state["sample_rate"], state["cc"]), ("sfz", FS, {"20": 0.0}))
                self.assertEqual(main(["--corpus", str(corpus), "render", str(session), "--state", "test",
                                       "--jobs", "2"]), 0)
                rendered = corpus / "probe" / "test"
                manifest = json.loads((rendered / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["context"]["fs"], FS)  # at the bank's rate, not the session's
                self.assertEqual(manifest["instrument"]["license"], "CC0")
                takes = sorted((rendered / "notes").glob("*.flac"))
                self.assertEqual(len(takes), 6)
                audio, fs = sf.read(rendered / "notes" / "062-D4_v100_h500ms.flac", always_2d=True)
                self.assertEqual(fs, FS)
                self.assertAlmostEqual(pitch(audio[int(0.1 * FS):int(0.5 * FS), 0], 200, 400), C4 * 2 ** (2 / 12),
                                       delta=0.3)
                meta = json.loads((rendered / "notes" / "062-D4_v100_h500ms.json").read_text(encoding="utf-8"))
                self.assertLess(meta["attack_delay_ms"], 1.0)
                # Up to date: a second run renders nothing.
                self.assertEqual(main(["--corpus", str(corpus), "render", str(session), "--state", "test"]), 0)
                self.assertIn("6 already up to date", (rendered / "log.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
