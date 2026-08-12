# Doubao streaming session lifecycle design

## Scope

Only the P4 real-time voice assistant PCM path changes. Authentication, model,
WebSocket protocol, S3 wire protocol, local command priority, Wi-Fi/BLE, MIDI,
scoring, UI, score recognition, and practice advice remain unchanged.

## Session lifecycle

The single-turn state is explicit:

`IDLE -> LISTENING -> WAIT_RESPONSE -> SPEAKING -> DRAINING -> IDLE`

- `LISTENING` starts at S3 wake and accepts candidate PCM while the existing S3
  local recognizer continues independently.
- A cloud ASR final/endpoint stops P4 input acceptance immediately and removes
  unsent silence from the upload queue. It does not commit cloud output or
  bypass the existing local-command decision.
- `AI,BEGIN` commits an open question, stops input if ASR final has not already
  done so, keeps at most 400 ms of already queued tail PCM, asks S3 to stop the
  AI input route through the existing command, and moves to `WAIT_RESPONSE`.
- The P4 S3 receiver continues ACKing and discarding the bounded UART tail so a
  route transition does not create corrupt-frame or NACK loops.
- The first committed TTS stream moves the session to `SPEAKING`.
- `response.output_audio.done` moves it to `DRAINING`; session completion is
  emitted only after all decoded PCM and the speaker/I2S pipeline are empty.

Input statistics retain total accepted bytes separately from queue occupancy so
trimming unsent silence cannot hide how much audio arrived. Logs include the
stop reason, total accepted, sent/queued, and discarded bytes.

## TTS streaming pipeline

WebSocket RX decodes each 24 kHz, signed 16-bit, mono PCM delta and writes it to
a PSRAM-backed 256 KiB staging ring. This ring absorbs network bursts; it is not
an answer-sized accumulator. An existing independent voice worker continuously
moves data into the speaker service's 64 KiB PSRAM ring. The speaker service
starts after its 20 KiB prebuffer (about 426 ms), resumes after a 10 KiB
rebuffer, and consumes the ring in its own task.

Both transfer boundaries support partial writes. If the staging or speaker ring
is temporarily full, the producer retains the unwritten tail and retries after
the consumer frees space. Backpressure is counted and logged, but it never
becomes `tts-staging-overflow`, `speaker-buffer-rejected`, or a session failure.
The WebSocket callback waits only in short bounded intervals when the PSRAM
staging ring is full and rechecks session cancellation between attempts; it
never performs speaker I/O.

## Completion, recovery, and metrics

TTS `done` only marks the producer complete. The worker waits for the staging
ring, retained partial block, speaker ring, and I2S DMA to drain before closing
the turn and restoring the existing audio focus/WakeNet path. Cancellation or a
real transport/protocol error still stops the stream through existing recovery.

Per-turn logs report:

- every session state transition and reason;
- input PCM total and upload stop reason;
- TTS PCM received, staging current/maximum occupancy, and backpressure count;
- speaker ring capacity/current/maximum occupancy, actual played bytes,
  underruns, and backpressure count;
- final drain completion or the real terminal error reason.

## Verification

Build only the P4 project. At runtime, test short and long open questions such as
"讲个笑话". ASR final or `AI,BEGIN` must stop growth of the input archive before
256000 bytes. Playback should begin after roughly 300-500 ms of buffered output,
continue while PCM arrives, and finish the whole response after TTS `done`.
Normal runs must not log input capacity exhaustion, staging overflow, immediate
speaker rejection, or premature session completion.
