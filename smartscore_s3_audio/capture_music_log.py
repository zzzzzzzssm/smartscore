#!/usr/bin/env python3
"""Display ESP32 serial output and save it to repeatable analysis log files."""

from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path
import sys

try:
    import serial
except ImportError:
    print("pyserial is missing. Run this from the ESP-IDF PowerShell environment.", file=sys.stderr)
    raise SystemExit(2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture music detector serial diagnostics")
    parser.add_argument("--port", default="COM3", help="serial port, default: COM3")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud, default: 115200")
    parser.add_argument("--label", default="unlabelled", help="description of the notes being tested")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    project_dir = Path(__file__).resolve().parent
    log_dir = project_dir / "music_logs"
    log_dir.mkdir(exist_ok=True)
    started = datetime.now()
    archive_path = log_dir / f"music_capture_{started:%Y%m%d_%H%M%S}.log"
    latest_path = project_dir / "music_capture_latest.log"
    header = (
        f"# started={started.isoformat(timespec='seconds')}\n"
        f"# port={args.port} baud={args.baud} label={args.label}\n"
        "# Stop capture with Ctrl+C.\n"
    )

    print(f"Capturing {args.port} at {args.baud} baud")
    print(f"Test label: {args.label}")
    print(f"Latest log: {latest_path}")
    print(f"Archive log: {archive_path}")
    print("Press Ctrl+C after playing the test notes.\n")

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as error:
        print(f"Cannot open {args.port}: {error}", file=sys.stderr)
        print("Close idf.py monitor or any other program using this port, then retry.", file=sys.stderr)
        return 1

    try:
        with latest_path.open("w", encoding="utf-8", newline="\n") as latest, \
             archive_path.open("w", encoding="utf-8", newline="\n") as archive:
            latest.write(header)
            archive.write(header)
            latest.flush()
            archive.flush()
            while True:
                raw = port.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                print(line, flush=True)
                latest.write(line + "\n")
                archive.write(line + "\n")
                latest.flush()
                archive.flush()
    except KeyboardInterrupt:
        print("\nCapture stopped. The log files are ready for analysis.")
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
