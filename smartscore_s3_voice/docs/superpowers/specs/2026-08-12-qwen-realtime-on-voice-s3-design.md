# SmartScore Qwen Realtime on Voice S3

## Scope

Move only the open-ended realtime voice network path from the WT99/P4 to the
voice ESP32-S3.  Existing WakeNet/MultiNet commands and IDs, P4 UI, MIDI,
scoring, OMR/advice, mini-program APIs, C5 networking and `speaker_service`
remain unchanged.

## Selected design

The voice S3 owns a warm Qwen WebSocket.  After wake it enters `CANDIDATE`,
keeps local MultiNet recognition active and streams 16 kHz S16 mono candidate
PCM to Qwen without requesting a response.  A local command clears the remote
input buffer and follows the existing command path.  An open question keeps
the same candidate, commits the input at end-of-speech and sends
`response.create`.  This avoids replaying several seconds of historical audio
while preserving local-command priority.

Only `qwen_voice_task` may connect, send WebSocket events, cancel a response or
change session state.  Microphone, local recognition, WebSocket callback and
UART tasks communicate with it through bounded queues.  The WebSocket callback
only assembles complete text messages and queues them.

## Data paths

Upstream:

`INMP441 -> AFE 16 kHz PCM -> qwen input queue -> Qwen WebSocket`

Downstream:

`response.audio.delta -> Base64 decode -> bounded S3 PSRAM ring -> UART TX task
-> P4 voice UART RX -> P4 PCM worker -> existing 256 KiB speaker_service ring
-> ES8311`

P4 does not initialize or feed its legacy Qwen realtime client.  The source is
retained for rollback but has no runtime registration.

## Qwen protocol

The implementation uses the Qwen Audio Realtime events `session.update`,
`input_audio_buffer.append`, `input_audio_buffer.clear`,
`input_audio_buffer.commit`, `response.create`, `response.cancel`,
`response.audio.delta`, `response.audio.done` and `response.done`.  The model,
endpoint, voice and instructions are build configuration.  The API key is
injected from `DASHSCOPE_API_KEY` into the private S3 component target and is
never stored in source, sdkconfig or logs.

## UART protocol

Only the voice-S3/P4 UART changes to 921600 baud.  Frames contain two-byte
magic, version, message type, flags, 32-bit sequence, 16-bit payload length,
header CRC16, payload and payload CRC16.  Maximum payload is 2048 bytes and AI
PCM frames use 1536 bytes where possible.  Control messages have independent
priority over PCM.  P4 detects CRC failures and sequence gaps and uses
`FLOW_OFF`/`FLOW_ON`; `STOP`/`STOP_ACK` cannot be starved by audio.

## State ownership

S3 internal states are `IDLE -> CANDIDATE -> LISTENING -> THINKING -> SPEAKING
-> DRAINING -> IDLE`, with `ERROR` and cancellation converging through one
cleanup path.  P4 receives only the presentation states `IDLE`, `LISTENING`,
`THINKING`, `SPEAKING`, and `ERROR`.

Entering S3 candidate/listening pauses P4 background speaker activity through
the existing audio-focus helpers.  Returning to IDLE, local-command cancel,
STOP or error restores it.  P4 starts the existing 24 kHz mono stream on
`AI_AUDIO_START`, feeds PCM incrementally, and calls stream finish only after
`AI_AUDIO_DONE`, allowing the ring to drain before restoring background audio.

## Bounded resources

The S3 Qwen input queue and decoded-output PSRAM ring are bounded.  The output
ring is 128 KiB with 96/48 KiB high/low watermarks.  The P4 keeps its existing
256 KiB speaker ring and starts with a 48 KiB jitter prebuffer.  No WebSocket
callback waits for UART or speaker playback.

## Failure handling

Wi-Fi/WebSocket errors cancel the active turn, clear bounded queues, send an
error/IDLE state to P4 and re-arm local wake.  P4 STOP stops PCM transmission,
clears the S3 output ring, serially cancels the Qwen response and returns
`STOP_ACK`.  CRC or sequence failures are counted and logged without invoking
the old P4 microphone-upload route.

