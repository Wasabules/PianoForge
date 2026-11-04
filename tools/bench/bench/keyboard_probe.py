"""MIDI keyboard probe: listens to what the piano actually sends and infers its capabilities.

Answers: is the sustain pedal a switch or continuous (half pedal)? Which velocity range does the keyboard
produce? Does it send release velocity, CC88 (high-resolution velocity prefix), other pedals (CC66
sostenuto, CC67 soft)?
"""

from __future__ import annotations

import collections
import time
from typing import Callable

from bench.protocol import midi_to_note

CC_NAMES = {1: "modulation", 7: "volume", 11: "expression", 64: "sustain pedal", 66: "sostenuto", 67: "soft pedal",
            88: "high-resolution velocity prefix", 120: "all sound off", 123: "all notes off"}


def input_ports() -> list[str]:
    import rtmidi
    return rtmidi.MidiIn().get_ports()


def pick_port(pattern: str | None) -> tuple[int, str]:
    ports = input_ports()
    if not ports:
        raise SystemExit("No MIDI input: check that the piano is switched on and connected to the PC over USB.")
    if pattern:
        found = [(i, p) for i, p in enumerate(ports) if pattern.lower() in p.lower()]
        if not found:
            raise SystemExit(f"No MIDI input contains {pattern!r}; available: {ports}")
        return found[0]
    return 0, ports[0]


def describe(message: list[int]) -> str:
    status, kind = message[0], message[0] & 0xF0
    channel = (status & 0x0F) + 1
    if kind == 0x90 and message[2] > 0:
        return f"note {message[1]} ({midi_to_note(message[1])}) on, velocity {message[2]}  [channel {channel}]"
    if kind == 0x80 or (kind == 0x90 and message[2] == 0):
        style = f"note-off, release velocity {message[2]}" if kind == 0x80 else "note-on with velocity 0"
        return f"note {message[1]} ({midi_to_note(message[1])}) off ({style})"
    if kind == 0xB0:
        return f"CC{message[1]} ({CC_NAMES.get(message[1], '?')}) = {message[2]}"
    names = {0xA0: "polyphonic aftertouch", 0xC0: "program change", 0xD0: "aftertouch", 0xE0: "pitch bend"}
    if kind in names:
        return f"{names[kind]} {message[1:]}"
    return "system exclusive" if status == 0xF0 else f"message {message}"


def listen(index: int, first_event_timeout_s: float, stop_after_silence_s: float, max_duration_s: float,
           show: Callable[[str], None] = print) -> list[tuple[float, list[int]]]:
    """Captures until `stop_after_silence_s` seconds without events (after the first), or `max_duration_s`."""
    import rtmidi
    port = rtmidi.MidiIn()
    port.ignore_types(sysex=False, timing=True, active_sense=True)
    port.open_port(index)
    capture, t, start, last = [], 0.0, time.monotonic(), None
    try:
        while True:
            now = time.monotonic()
            if last is None and now - start > first_event_timeout_s:
                show("No event received: nothing to analyze.")
                break
            if last is not None and (now - last > stop_after_silence_s or now - start > max_duration_s):
                break
            received = port.get_message()
            if received is None:
                time.sleep(0.001)
                continue
            message, delta = received
            t = 0.0 if last is None else t + delta  # driver timestamps: more precise than the arrival clock
            last = now
            capture.append((round(t, 6), list(message)))
            show(f"{t:9.3f} s  {describe(message)}")
    finally:
        port.close_port()
    return capture


def analyze_capture(capture: list[tuple[float, list[int]]]) -> dict:
    vel_on, vel_off, notes, channels = [], [], [], set()
    off_style = collections.Counter()
    ccs = collections.defaultdict(list)
    others = collections.Counter()
    for _, m in capture:
        kind = m[0] & 0xF0
        if kind in (0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0):
            channels.add((m[0] & 0x0F) + 1)
        if kind == 0x90 and m[2] > 0:
            vel_on.append(m[2])
            notes.append(m[1])
        elif kind == 0x80:
            off_style["note_off"] += 1
            vel_off.append(m[2])
        elif kind == 0x90:
            off_style["note_on_velocity_0"] += 1
        elif kind == 0xB0:
            ccs[m[1]].append(m[2])
        else:
            others[describe(m).split(" [")[0].split(" ")[0]] += 1

    res: dict = {"events": len(capture), "channels": sorted(channels)}
    if vel_on:
        res["strikes"] = {"count": len(vel_on), "velocity_min": min(vel_on), "velocity_max": max(vel_on),
                          "distinct_velocities": len(set(vel_on)),
                          "lowest_note": midi_to_note(min(notes)), "highest_note": midi_to_note(max(notes))}
    res["releases"] = dict(off_style)
    off_values = set(vel_off)
    res["release_velocity"] = (
        "absent (no note-off messages)" if not vel_off and off_style else
        "absent (fixed value)" if len(off_values - {0, 64}) == 0 else
        f"present ({len(off_values)} values, {min(vel_off)} to {max(vel_off)})" if vel_off else "not observed")
    res["controllers"] = {f"CC{n}": {"name": CC_NAMES.get(n, "?"), "messages": len(v), "distinct_values": len(set(v)),
                                     "min": min(v), "max": max(v)} for n, v in sorted(ccs.items())}
    pedal = ccs.get(64, [])
    res["sustain_pedal"] = ("not observed" if not pedal else
                            "switch (on/off)" if len(set(pedal)) <= 2 else
                            f"continuous ({len(set(pedal))} distinct values): half pedal possible")
    res["high_resolution_velocity_cc88"] = 88 in ccs
    if others:
        res["other_messages"] = dict(others)
    return res
