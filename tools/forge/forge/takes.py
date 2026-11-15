"""What a take of the corpus holds."""

from __future__ import annotations


def held_end(meta: dict) -> int:
    """The end (sample) of the note held in a take: its note-off, or earlier where the recording itself stops
    (sound_end_sample: an SFZ bank whose sample cuts the note short, bench/sfz.py). What follows such a cut is
    silence, not the note's decay: no analysis reads past it."""
    off = meta["note_off_sample"]
    end = meta.get("sound_end_sample")
    return off if end is None else min(off, int(end))


def sound_end(meta: dict) -> int | None:
    """The sample where the recording of a take stops (None: it runs to the take's end)."""
    end = meta.get("sound_end_sample")
    return None if end is None else int(end)
