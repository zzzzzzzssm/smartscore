# Doubao WebSocket fragmentation and 64 KiB boundary fix

## Scope

Only the P4 `voice_assistant` WebSocket receive, Doubao PCM queue, and
`speaker_service` streaming diagnostics/rebuffer path change. The existing
`IDLE -> CANDIDATE` local-command-first session lifecycle remains unchanged.
S3, UI, Wi-Fi/BLE, MIDI, scoring, score recognition, practice advice, and
unrelated business code are out of scope.

## Confirmed boundary and receive defects

The current voice receiver defines `MAX_RX_MESSAGE_BYTES` as exactly 64 KiB and
rejects any event whose `payload_len` exceeds it. The current assembly also
treats `payload_offset` as a whole-message offset, accepts only text and
continuation opcodes, and does not use the WebSocket FIN bit. ESP WebSocket
reports `payload_offset` within a frame; it can reset for a continuation frame.
Consequently a fragmented message or a message above 64 KiB can be discarded or
assembled incorrectly even though the speaker ring advertises 256 KiB.

The existing audio delta handler can call `speaker_service_stream_write()` from
the WebSocket event task. That violates the required ownership boundary and can
make receive progress depend on speaker state.

## Fragment-aware PSRAM assembler

Replace the fixed 64 KiB check with a PSRAM-backed message accumulator that can
grow geometrically up to 512 KiB. It tracks:

- the original message opcode (text or binary);
- whether a fragmented message is active;
- the current frame payload length and next frame-local offset;
- total assembled message bytes;
- FIN and end-of-current-frame independently.

For each `WEBSOCKET_EVENT_DATA`, append every non-control data segment exactly
once. A text or binary opcode starts a message. A continuation opcode requires
an active message. `payload_offset` validates order within the current frame but
is not used as the global destination offset. Completion occurs only when both
the current frame is complete and FIN is set. Invalid offsets/opcodes reset only
the receive assembler and log the precise reason.

All lengths and cumulative counters use `size_t` or `uint64_t`; no PCM length or
total uses `uint16_t`. Control frames are diagnosed but never appended to an
audio/data message.

At the raw event entrance, log:

`WS_RAW: event_no=... opcode=... fin=... data_len=... payload_len=... payload_offset=... raw_total=...`

## PCM ownership and pumping

After a complete Doubao message is parsed, decoded PCM is copied into the
existing bounded PSRAM pending queue. The WebSocket callback never waits for
I2S, calls `speaker_service_stream_write()`, or takes an unbounded lock. Its only
PCM mutation is a bounded queue copy with a zero/short timeout.

The existing voice worker is the sole producer of the 256 KiB speaker ring. It
continuously pumps pending PCM with partial-write retry. The speaker task is the
sole consumer. Underrun/rebuffer only pauses I2S consumption; it never resets or
locks out the producer. At 10 KiB buffered, or for a final short tail, the
speaker task restarts output automatically.

Diagnostics are rate-limited but cumulative:

- `DOUBAO_PCM: chunk_len=... pcm_total=...`
- `SPEAKER_WRITE: requested=... accepted=... speaker_total=... buffered=...`
- the existing one-second pipeline log for received, pumped, played, buffered,
  underrun, and session state.

## Completion and verification

`audio response done` only marks the cloud producer complete. Playback finishes
after the pending PCM queue, speaker ring, and I2S DMA drain. The 256 KiB speaker
ring is not enlarged.

Static inspection must find no active 65536/64 KiB receive cap, legacy 64 KiB
PCM ring, or 16-bit PCM cumulative counter. Runtime validation with a long
answer must show received, pumped, and played totals crossing 100 KiB and 150
KiB. A recoverable underrun must be followed by a playback-release log after the
10 KiB rebuffer threshold, and the whole response must drain normally.
