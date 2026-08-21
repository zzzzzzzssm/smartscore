#!/usr/bin/env python3
"""Convert a standard MIDI file to SmartScore's local-score JSON format."""

from __future__ import annotations

import argparse
import bisect
import json
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path

import mido


DEFAULT_TEMPO = 500_000


@dataclass(frozen=True)
class ParsedNote:
    midi: int
    velocity: int
    channel: int
    start_tick: int
    end_tick: int


class TempoMap:
    def __init__(self, ticks_per_quarter: int, events: list[tuple[int, int]]):
        collapsed: dict[int, int] = {0: DEFAULT_TEMPO}
        for tick, tempo in events:
            collapsed[tick] = tempo
        ordered = sorted(collapsed.items())

        self._ticks: list[int] = []
        self._tempos: list[int] = []
        self._seconds: list[float] = []
        elapsed = 0.0
        previous_tick = ordered[0][0]
        previous_tempo = ordered[0][1]
        for tick, tempo in ordered:
            elapsed += mido.tick2second(
                tick - previous_tick, ticks_per_quarter, previous_tempo
            )
            self._ticks.append(tick)
            self._tempos.append(tempo)
            self._seconds.append(elapsed)
            previous_tick = tick
            previous_tempo = tempo
        self._ticks_per_quarter = ticks_per_quarter

    def seconds_at(self, tick: int) -> float:
        index = bisect.bisect_right(self._ticks, tick) - 1
        return self._seconds[index] + mido.tick2second(
            tick - self._ticks[index],
            self._ticks_per_quarter,
            self._tempos[index],
        )

    def tempo_at(self, tick: int) -> int:
        index = bisect.bisect_right(self._ticks, tick) - 1
        return self._tempos[index]


def normalized_key(key: str | None) -> str:
    if not key:
        return "C major"
    return f"{key[:-1]} minor" if key.endswith("m") else f"{key} major"


def parse_midi(path: Path) -> tuple[mido.MidiFile, list[ParsedNote], list[tuple[int, int]], list[tuple[int, int, int]], list[tuple[int, str]]]:
    midi = mido.MidiFile(path)
    if midi.ticks_per_beat <= 0:
        raise ValueError("SMPTE time-code MIDI is not supported")

    tick = 0
    active: dict[tuple[int, int], deque[tuple[int, int]]] = defaultdict(deque)
    notes: list[ParsedNote] = []
    tempos: list[tuple[int, int]] = []
    signatures: list[tuple[int, int, int]] = []
    keys: list[tuple[int, str]] = []

    for message in mido.merge_tracks(midi.tracks):
        tick += message.time
        if message.type == "set_tempo":
            tempos.append((tick, message.tempo))
        elif message.type == "time_signature":
            signatures.append((tick, message.numerator, message.denominator))
        elif message.type == "key_signature":
            keys.append((tick, message.key))
        elif message.type == "note_on" and message.velocity > 0:
            active[(message.channel, message.note)].append(
                (tick, message.velocity)
            )
        elif message.type == "note_off" or (
            message.type == "note_on" and message.velocity == 0
        ):
            pending = active.get((message.channel, message.note))
            if pending:
                start_tick, velocity = pending.popleft()
                if tick > start_tick:
                    notes.append(
                        ParsedNote(
                            midi=message.note,
                            velocity=velocity,
                            channel=message.channel,
                            start_tick=start_tick,
                            end_tick=tick,
                        )
                    )

    if not notes:
        raise ValueError("MIDI contains no complete note-on/note-off pairs")
    notes.sort(key=lambda note: (note.start_tick, note.midi, note.channel))
    return midi, notes, tempos, signatures, keys


def latest_at(events: list[tuple], tick: int, fallback: tuple) -> tuple:
    result = fallback
    for event in sorted(events):
        if event[0] > tick:
            break
        result = event
    return result


def convert(path: Path, output: Path, title: str, keep_leading_rest: bool) -> dict:
    midi, parsed_notes, tempos, signatures, keys = parse_midi(path)
    first_note_tick = parsed_notes[0].start_tick
    origin_tick = 0 if keep_leading_rest else first_note_tick
    tempo_map = TempoMap(midi.ticks_per_beat, tempos)
    origin_seconds = tempo_map.seconds_at(origin_tick)
    _, numerator, denominator = latest_at(signatures, first_note_tick, (0, 4, 4))
    _, source_key = latest_at(keys, first_note_tick, (0, ""))
    bpm = int(round(mido.tempo2bpm(tempo_map.tempo_at(first_note_tick))))

    notes = []
    for note in parsed_notes:
        start_tick = note.start_tick - origin_tick
        duration_ticks = note.end_tick - note.start_tick
        notes.append(
            {
                "midi": note.midi,
                "velocity": note.velocity,
                "start": round(tempo_map.seconds_at(note.start_tick) - origin_seconds, 6),
                "duration": round(
                    tempo_map.seconds_at(note.end_tick)
                    - tempo_map.seconds_at(note.start_tick),
                    6,
                ),
                "start_tick": start_tick,
                "duration_ticks": duration_ticks,
                "staff": 1,
                "voice": 1,
            }
        )

    document = {
        "title": title,
        "bpm": bpm,
        "time_signature": f"{numerator}/{denominator}",
        "key": normalized_key(source_key),
        "source": "midi_import",
        "source_midi": path.name,
        "ticks_per_quarter": midi.ticks_per_beat,
        "staff_mode": "treble",
        "leading_rest_removed_ticks": origin_tick,
        "notes": notes,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(output)
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--title")
    parser.add_argument("--keep-leading-rest", action="store_true")
    args = parser.parse_args()
    title = args.title or args.input.stem
    document = convert(
        args.input, args.output, title, args.keep_leading_rest
    )
    print(
        f"wrote {args.output} ({len(document['notes'])} notes, "
        f"{document['bpm']} BPM)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
