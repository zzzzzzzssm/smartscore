# Qwen Realtime Under-3-Second First-Audio Design

## Goal

After S3 confirms the end of an open AI utterance, P4 should begin speaker
playback within three seconds whenever Qwen delivers its first PCM within the
remaining network budget. P4 must add no large fixed buffering delay.

## Scope

Only the P4 `voice_assistant_v2` input upload path and `speaker_service` PCM
startup/rebuffer policy change. The S3 protocol, local-command priority,
Qwen model and authentication, 24 kHz/16-bit/mono output, 256 KiB PSRAM ring,
UI, scoring, MIDI, OMR, advice, Wi-Fi and BLE remain unchanged.

## Design

- Start first playback after 16 KiB of PCM (about 341 ms at 48,000 B/s).
- On the first underrun, wait for 64 KiB before resuming. On repeated
  underruns, wait for 96 KiB. Rebuffering pauses only I2S consumption and never
  blocks producers.
- While live input has little backlog, retain bounded small audio appends.
- Once S3 reports speech end, drain queued history immediately in the largest
  append that fits the existing 8 KiB JSON buffer. Do not apply the normal
  20 ms pacing delay between backlog sends.
- Keep WebSocket RX independent of I2S and preserve partial speaker writes,
  final-ring draining, stall recovery and wake re-arming.

## Error Handling

WebSocket backpressure remains bounded. Failed sends retain the pending PCM
batch for retry instead of losing it. Speaker underrun increases only the
rebuffer threshold for the current stream; the next turn starts again at the
low-latency threshold.

## Acceptance

The latency summary should show `speaker_started - speech_end <= 3000 ms` on a
healthy connection. Logs must still show complete `response.audio.done`, ring
drain and return to `IDLE`, with no requirement to compile or test as part of
this code-only change.
