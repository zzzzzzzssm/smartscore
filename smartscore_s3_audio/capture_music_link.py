#!/usr/bin/env python3
"""Read and validate the ESP32-S3 dedicated music-link NDJSON UART."""

import argparse
import json
import sys
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit("pyserial is required: python -m pip install pyserial") from exc


VISIBLE_TYPES = {
    "hello", "status", "pong", "note_on", "note_off", "pitch", "poly", "heartbeat"
}


def build_command(name: str, sid: int) -> dict:
    if name == "start":
        return {"cmd": "start", "sid": sid}
    return {"cmd": name}


def display_message(message: dict) -> None:
    kind = message.get("type", "?")
    sid = message.get("sid", "?")
    timestamp = message.get("ts_ms", "?")
    if kind == "pitch":
        print(
            f"PITCH      sid={sid} t={timestamp} midi={message.get('midi')} "
            f"freq={message.get('freq_hz')}Hz cents={message.get('cents')} "
            f"conf={message.get('confidence')} rms={message.get('rms')}"
        )
    elif kind == "note_on":
        print(
            f"NOTE ON    sid={sid} t={timestamp} midi={message.get('midi')} "
            f"velocity={message.get('velocity')} conf={message.get('confidence')}"
        )
    elif kind == "note_off":
        print(
            f"NOTE OFF   sid={sid} t={timestamp} midi={message.get('midi')} "
            f"duration={message.get('duration_ms')}ms reason={message.get('reason')}"
        )
    elif kind == "poly":
        print(
            f"POLY       sid={sid} t={timestamp} kind={message.get('kind')} "
            f"name={message.get('name', '-')} notes={message.get('notes')} "
            f"conf={message.get('confidence')}"
        )
    elif kind in VISIBLE_TYPES:
        print(f"{kind.upper():10} {json.dumps(message, ensure_ascii=False, separators=(',', ':'))}")
    else:
        print(f"MESSAGE    {json.dumps(message, ensure_ascii=False, separators=(',', ':'))}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="UART adapter port, for example COM8")
    parser.add_argument("--baud", type=int, default=115200, help="default: 115200")
    parser.add_argument("--poll-ms", type=int, default=50, help="poll interval, default: 50")
    parser.add_argument(
        "--send", choices=("ping", "start", "stop", "stream_on", "stream_off"),
        help="send one control command after opening the port"
    )
    parser.add_argument("--sid", type=int, default=0, help="session id used with --send start")
    args = parser.parse_args()
    if not 0 <= args.sid <= 0xFFFFFFFF:
        parser.error("--sid must be between 0 and 4294967295")
    if args.poll_ms < 10:
        parser.error("--poll-ms must be at least 10")

    invalid_json = 0
    received = 0
    note_on_count = 0
    note_off_count = 0
    duplicate_note_on = 0
    unmatched_note_off = 0
    active_notes: dict[tuple[int, int], dict] = {}

    with serial.Serial(args.port, args.baud, timeout=min(args.poll_ms / 2000.0, 0.02)) as link:
        print(f"Listening on {args.port} at {args.baud} baud; Ctrl+C to stop")
        time.sleep(0.05)
        if args.send:
            command = build_command(args.send, args.sid)
            wire = json.dumps(command, separators=(",", ":")) + "\n"
            link.write(wire.encode("ascii"))
            link.flush()
            print(f"SENT       {wire.rstrip()}")

        try:
            next_poll = time.monotonic()
            while True:
                now = time.monotonic()
                if now >= next_poll:
                    link.write(b'{"cmd":"poll"}\n')
                    next_poll = now + args.poll_ms / 1000.0
                raw = link.readline()
                if not raw:
                    continue
                try:
                    line = raw.decode("utf-8").strip()
                    message = json.loads(line)
                    if not isinstance(message, dict):
                        raise ValueError("top-level JSON is not an object")
                except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
                    invalid_json += 1
                    print(f"INVALID    {raw!r} ({exc})", file=sys.stderr)
                    continue

                received += 1
                display_message(message)
                kind = message.get("type")
                try:
                    key = (int(message.get("sid", 0)), int(message.get("midi")))
                except (TypeError, ValueError):
                    key = None
                if kind == "note_on" and key is not None:
                    note_on_count += 1
                    if key in active_notes:
                        duplicate_note_on += 1
                        print(f"PAIR WARN  duplicate note_on for sid={key[0]} midi={key[1]}")
                    active_notes[key] = message
                elif kind == "note_off" and key is not None:
                    note_off_count += 1
                    if active_notes.pop(key, None) is None:
                        unmatched_note_off += 1
                        print(f"PAIR WARN  unmatched note_off for sid={key[0]} midi={key[1]}")
        except KeyboardInterrupt:
            pass

    print(
        "SUMMARY    "
        f"messages={received} invalid_json={invalid_json} note_on={note_on_count} "
        f"note_off={note_off_count} duplicate_on={duplicate_note_on} "
        f"unmatched_off={unmatched_note_off} still_active={len(active_notes)}"
    )
    if active_notes:
        print(f"ACTIVE     {sorted(active_notes)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
