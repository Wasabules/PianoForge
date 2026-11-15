"""What a take of the corpus holds."""

from __future__ import annotations

from pathlib import Path

_last: tuple[str, float, tuple] | None = None


def read(path) -> tuple:
    """(audio samples x channels float64, rate) of a take, the last one read kept in memory: one export layer reads
    its take three times (the body's limit, the attack residual, the partials' fades). Read-only: shared."""
    global _last
    import soundfile as sf

    p = Path(path)
    key = (str(p.resolve()), p.stat().st_mtime)
    if _last is None or _last[:2] != key:
        audio, fs = sf.read(p, dtype="float64", always_2d=True)
        audio.flags.writeable = False
        _last = (*key, (audio, fs))
    return _last[2]


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
