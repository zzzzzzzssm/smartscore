#!/usr/bin/env python3
"""Build SmartScore's compact WT99 piano bank from Salamander Grand Piano V3.

The output is a versioned, CRC-protected 24 kHz mono PCM bank. Source files
are downloaded only when --download is supplied. Decoding uses ffmpeg when it
is available, otherwise the optional numpy + soundfile Python packages.
"""

from __future__ import annotations

import argparse
import array
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zlib
from dataclasses import dataclass
from pathlib import Path


SAMPLE_RATE = 24_000
SAMPLE_SECONDS = 0.90
LOOP_SOURCE_START_SECONDS = 0.55
LOOP_END_SECONDS = 0.85
LOOP_CROSSFADE_SECONDS = 0.03
SAMPLE_GAIN = 1.15
MAGIC = b"SSPB"
VERSION = 1
HEADER = struct.Struct("<4sHHIHHIIII")
ZONE = struct.Struct("<BBBBIIIIhHII")
SOURCE_URL = (
    "https://raw.githubusercontent.com/"
    "sfzinstruments/SalamanderGrandPiano/master/Samples/{filename}"
)
ROOT_MIDI = tuple(range(21, 109, 3))
LAYERS = (
    (4, 1, 50),
    (9, 51, 95),
    (14, 96, 127),
)
GAIN_Q15 = 32_767


@dataclass(frozen=True)
class ZoneData:
    root_midi: int
    velocity_min: int
    velocity_max: int
    samples: bytes
    loop_start: int
    loop_end: int


def midi_name(midi: int) -> str:
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    return f"{names[midi % 12]}{midi // 12 - 1}"


def source_filename(midi: int, source_layer: int) -> str:
    return f"{midi_name(midi)}v{source_layer}.flac"


def download_file(filename: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    url = SOURCE_URL.format(filename=urllib.parse.quote(filename))
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "SmartScore-PianoBankBuilder/1.0"},
    )
    temporary = destination.with_suffix(destination.suffix + ".part")
    for attempt in range(1, 6):
        try:
            print(f"download {filename} (attempt {attempt}/5)")
            with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            temporary.replace(destination)
            return
        except OSError:
            temporary.unlink(missing_ok=True)
            if attempt == 5:
                raise
            time.sleep(min(2 ** attempt, 15))


def decode_with_ffmpeg(source: Path) -> array.array:
    command = [
        "ffmpeg", "-v", "error", "-i", str(source),
        "-ac", "1", "-ar", str(SAMPLE_RATE),
        "-f", "s16le", "-acodec", "pcm_s16le", "-",
    ]
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE)
    pcm = array.array("h")
    pcm.frombytes(result.stdout)
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm


def decode_with_soundfile(source: Path) -> array.array:
    try:
        import numpy as np
        import soundfile as sf
        from scipy.signal import resample_poly
    except ImportError as exc:
        raise RuntimeError(
            "FLAC decoding requires ffmpeg, or: pip install numpy scipy soundfile"
        ) from exc

    audio, source_rate = sf.read(str(source), dtype="float32", always_2d=True)
    mono = audio.mean(axis=1)
    if source_rate != SAMPLE_RATE:
        divisor = math.gcd(source_rate, SAMPLE_RATE)
        mono = resample_poly(
            mono,
            SAMPLE_RATE // divisor,
            source_rate // divisor,
        )
    mono = np.clip(mono, -1.0, 1.0)
    values = np.rint(mono * 32767.0).astype("<i2")
    pcm = array.array("h")
    pcm.frombytes(values.tobytes())
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm


def decode_flac(source: Path) -> array.array:
    return decode_with_ffmpeg(source) if shutil.which("ffmpeg") else decode_with_soundfile(source)


def trim_and_loop(input_pcm: array.array) -> tuple[bytes, int, int]:
    if not input_pcm:
        raise ValueError("decoded sample is empty")

    peak = max(abs(value) for value in input_pcm)
    threshold = max(64, int(peak * 0.004))
    first_sound = next(
        (index for index, value in enumerate(input_pcm) if abs(value) >= threshold),
        0,
    )
    pre_roll = int(SAMPLE_RATE * 0.002)
    start = max(0, first_sound - pre_roll)
    sample_count = int(round(SAMPLE_SECONDS * SAMPLE_RATE))
    pcm = array.array("h", input_pcm[start:start + sample_count])
    if len(pcm) < sample_count:
        pcm.extend([0] * (sample_count - len(pcm)))

    for index, value in enumerate(pcm):
        amplified = int(round(value * SAMPLE_GAIN))
        pcm[index] = max(-32768, min(32767, amplified))

    fade_in = int(SAMPLE_RATE * 0.003)
    for index in range(min(fade_in, len(pcm))):
        pcm[index] = int(pcm[index] * index / max(1, fade_in - 1))

    loop_source_start = int(round(LOOP_SOURCE_START_SECONDS * SAMPLE_RATE))
    loop_end = int(round(LOOP_END_SECONDS * SAMPLE_RATE))
    crossfade = int(round(LOOP_CROSSFADE_SECONDS * SAMPLE_RATE))
    if loop_source_start + crossfade >= loop_end - crossfade:
        raise ValueError("invalid loop configuration")

    tail_start = loop_end - crossfade
    source_window = pcm[loop_source_start:loop_source_start + crossfade]
    for index in range(crossfade):
        progress = (index + 1) / crossfade
        fade_out = math.cos(progress * math.pi * 0.5) ** 2
        fade_in_weight = 1.0 - fade_out
        mixed = pcm[tail_start + index] * fade_out + source_window[index] * fade_in_weight
        pcm[tail_start + index] = max(-32768, min(32767, int(round(mixed))))

    # The crossfade ends at source_window[-1], therefore wrapping to the
    # sample immediately after that window is continuous.
    loop_start = loop_source_start + crossfade
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm.tobytes(), loop_start, loop_end


def build_zones(source_dir: Path, allow_download: bool) -> list[ZoneData]:
    zones: list[ZoneData] = []
    for root_midi in ROOT_MIDI:
        for source_layer, velocity_min, velocity_max in LAYERS:
            filename = source_filename(root_midi, source_layer)
            source = source_dir / filename
            if not source.exists():
                if not allow_download:
                    raise FileNotFoundError(
                        f"missing {source}; rerun with --download or provide the source FLAC files"
                    )
                download_file(filename, source)
            samples, loop_start, loop_end = trim_and_loop(decode_flac(source))
            zones.append(
                ZoneData(
                    root_midi=root_midi,
                    velocity_min=velocity_min,
                    velocity_max=velocity_max,
                    samples=samples,
                    loop_start=loop_start,
                    loop_end=loop_end,
                )
            )
            print(f"prepare {filename}: {len(samples) // 2} samples")
    return zones


def write_bank(zones: list[ZoneData], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    payload_offset = HEADER.size + len(zones) * ZONE.size
    offset = payload_offset
    entries: list[bytes] = []
    for zone in zones:
        sample_count = len(zone.samples) // 2
        entries.append(
            ZONE.pack(
                zone.root_midi,
                zone.velocity_min,
                zone.velocity_max,
                0,
                offset,
                sample_count,
                zone.loop_start,
                zone.loop_end,
                GAIN_Q15,
                0,
                zlib.crc32(zone.samples) & 0xFFFFFFFF,
                0,
            )
        )
        offset += len(zone.samples)
    index = b"".join(entries)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER.size,
        SAMPLE_RATE,
        len(zones),
        ZONE.size,
        zlib.crc32(index) & 0xFFFFFFFF,
        payload_offset,
        offset,
        0,
    )

    with tempfile.NamedTemporaryFile(
        mode="wb", prefix=output.name + ".", suffix=".tmp",
        dir=output.parent, delete=False,
    ) as temporary:
        temporary_path = Path(temporary.name)
        temporary.write(header)
        temporary.write(index)
        for zone in zones:
            temporary.write(zone.samples)
    temporary_path.replace(output)
    print(f"wrote {output} ({output.stat().st_size} bytes, {len(zones)} zones)")


def verify_bank(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("bank is shorter than its header")
    magic, version, header_size, rate, zone_count, entry_size, index_crc, payload_offset, file_size, _ = HEADER.unpack_from(data)
    if (magic, version, header_size, rate, entry_size, file_size) != (
        MAGIC, VERSION, HEADER.size, SAMPLE_RATE, ZONE.size, len(data)
    ):
        raise ValueError("bank header validation failed")
    if payload_offset != HEADER.size + zone_count * ZONE.size:
        raise ValueError("bank payload offset is invalid")
    index = data[HEADER.size:payload_offset]
    if zlib.crc32(index) & 0xFFFFFFFF != index_crc:
        raise ValueError("bank index CRC mismatch")
    for zone_index in range(zone_count):
        entry = ZONE.unpack_from(index, zone_index * ZONE.size)
        data_offset, sample_count, loop_start, loop_end = entry[4:8]
        sample_crc = entry[10]
        end = data_offset + sample_count * 2
        if not (payload_offset <= data_offset < end <= len(data)):
            raise ValueError(f"zone {zone_index} points outside the bank")
        if not (0 <= loop_start < loop_end < sample_count):
            raise ValueError(f"zone {zone_index} has invalid loop points")
        if zlib.crc32(data[data_offset:end]) & 0xFFFFFFFF != sample_crc:
            raise ValueError(f"zone {zone_index} sample CRC mismatch")
    print(f"verified {path} ({zone_count} zones, {len(data)} bytes)")


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "cache" / "salamander",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "sdcard" / "smartscore" / "piano.pbank",
    )
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.verify_only:
        verify_bank(args.output)
        return 0
    zones = build_zones(args.source_dir, args.download)
    write_bank(zones, args.output)
    verify_bank(args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
