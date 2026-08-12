# Doubao single-ring playback and serialized session design

## Scope

Only the P4 `voice_assistant` and `speaker_service` implementation changes.
The S3 project and protocol, local command behavior, UI, scoring, MIDI, score
recognition, practice advice, Wi-Fi/BLE, model, authentication, and Doubao wire
protocol remain unchanged.

## Deferred cloud session creation

`voice_assistant_arm()` handles WAKE as a local candidate only. It resets and
accepts PCM into the existing PSRAM input archive, but does not request or send
`session.create`. The warm WebSocket transport may remain connected.

If S3 recognizes a local command, `voice_assistant_cancel_candidate()` discards
the candidate archive and returns to IDLE. Because no cloud conversation exists,
this path sends neither `session.create` nor `session.close`.

Only `voice_assistant_begin()`, called for S3 `AI_BEGIN`, commits the candidate,
requests the cloud session, and wakes the voice worker. The worker alone sends
`session.create`, input audio, response cancellation, and `session.close`.
WebSocket callbacks only parse server events and update bounded state; they do
not send, close, destroy, or reset a cloud session.

## Single output ring

The voice-owned 256 KiB TTS staging ring is removed. The speaker service owns a
single 256 KiB PSRAM ring. Decoded Doubao 24 kHz, signed 16-bit, mono PCM is
written directly into this ring. The speaker task independently consumes it and
drives I2S, retaining its 20 KiB startup prebuffer and 10 KiB rebuffer threshold.

The direct speaker write accepts partial blocks. WebSocket RX never waits for
I2S playback: if the ring is full, the unaccepted decoded tail is placed in a
bounded PSRAM pending buffer and the voice worker pumps it into the speaker ring.
Only one pending block exists at a time, and subsequent deltas append into the
same bounded PSRAM area. Overflow is a diagnosed transport failure rather than
silent PCM loss.

After an underrun, the speaker task checks ring occupancy continuously and calls
`board_audio_begin_output()` again as soon as 10 KiB is buffered, or immediately
for a short final tail after response done. `response.output_audio.done` only
marks input complete. Session completion waits for the pending PCM, speaker ring,
and I2S DMA to drain.

## Stack and ownership safety

Large worker-local input, output, Base64, and JSON scratch buffers move to
fixed PSRAM allocations owned by `voice_assistant`. The voice worker stack keeps
only small scalar state. Speaker control and ring reset operations are performed
by the speaker task through its existing command queue. Session transport
operations are serialized in the voice worker, preventing send/close races.

## Diagnostics

Once per second during a committed turn, logs report:

- session state;
- `tts_ring=direct` and pending decoded bytes;
- speaker ring buffered bytes;
- received, pumped, and played PCM bytes;
- underrun and backpressure counts;
- worker stack high-water mark.

The speaker service exposes a read-only stream metrics snapshot for this log.
Final logs retain total received and played bytes and the drain result.

## Verification

Build the P4 project. During runtime validation:

1. WAKE followed by a local command such as “下一页” must not log
   `session.create` or cloud close, and P4 must not reset or flash the screen.
2. WAKE followed by an open request such as “讲个笑话” must log `AI_BEGIN`, then
   and only then send `session.create`.
3. Long and bursty answers must continue playing after a recoverable underrun;
   once buffered PCM reaches 10 KiB, playback must log its release and resume.
4. The final metrics must show the whole response pumped and played before the
   session returns to IDLE, with no voice task stack protection fault.
