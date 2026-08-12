# Doubao PCM backpressure design

## Scope

Only the P4 `voice_assistant` and `speaker_service` streaming-PCM path changes.
The format remains 24 kHz, signed 16-bit, mono. S3, local commands, scoring,
UI, MIDI, sheet recognition, and unrelated network services are unchanged.

## Data flow

WebSocket callbacks decode each Doubao PCM delta into the existing PSRAM TTS
staging buffer and return without waiting for the speaker. The voice worker
moves data from staging to a speaker-owned 64 KiB PSRAM stream ring. If the
speaker ring accepts only part of a block, the worker retains the block and its
offset and retries later. Full or partial writes are backpressure, not session
errors.

The speaker starts at 20 KiB buffered PCM and, after a real underrun, waits for
10 KiB before resuming. It consumes the ring independently and preserves all
existing non-stream speaker sources.

## Completion and observability

`response.output_audio.done` does not finish playback until the TTS staging
buffer is empty, the worker has no retained partial block, the speaker ring is
empty, and I2S has drained. Logs report ring capacity, current and maximum
buffering, underruns, total PCM received, and total PCM played.

