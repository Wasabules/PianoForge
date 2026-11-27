# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a take holds: where its note held ends (python -m unittest discover -s tools/forge/tests)."""

import unittest

from forge.takes import held_end, sound_end


class HeldEnd(unittest.TestCase):
    def test_note_off(self):
        meta = {"note_on_sample": 100, "note_off_sample": 5000}
        self.assertEqual(held_end(meta), 5000)
        self.assertIsNone(sound_end(meta))

    def test_cut_short_by_its_sample(self):
        meta = {"note_on_sample": 100, "note_off_sample": 5000, "sound_end_sample": 3000}
        self.assertEqual(held_end(meta), 3000)
        self.assertEqual(sound_end(meta), 3000)

    def test_cut_after_the_release(self):
        meta = {"note_on_sample": 100, "note_off_sample": 5000, "sound_end_sample": 9000}
        self.assertEqual(held_end(meta), 5000)


if __name__ == "__main__":
    unittest.main()
