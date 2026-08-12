# DashScope Qwen3.8-Max full-page OMR design

## Goal

Replace only the existing Doubao sheet-recognition path with a direct ESP32-P4
request to Alibaba Cloud Model Studio in Beijing. One camera-produced JPEG
containing the complete page is sent once to `qwen3.8-max`. The firmware parses
the OpenAI-compatible completion envelope, validates the compact score JSON,
and converts it into owned internal score events without inventing musical
content.

The existing camera/image acquisition path, C5-hosted network interface,
Doubao practice-advice path, scoring service, and unrelated voice assistant
remain unchanged.

## Non-goals

- No staff or numbered-notation line detection.
- No crop-per-system or crop-per-line processing.
- No OCR-first or title-based completion.
- No self-hosted gateway or OpenAI SDK.
- No inferred notes, rests, accidentals, repeats, ties, slurs, or cross-system
  relationships.
- No change to the camera S3 UART protocol, which currently carries gesture
  commands rather than JPEG frames.

## Components

### `score_capture`

`score_capture` creates a non-owning or explicitly owned full-page JPEG object
from the existing raw image upload. It verifies the JPEG signature and header,
records width, height, pixel count, byte length, and the recognition task ID,
and rejects unsupported or unsafe inputs. It never detects or crops score
lines. A future camera driver that exposes its JPEG framebuffer can use the
same interface without changing the cloud or parser components.

The input JPEG remains the only full-size encoded image buffer. The upload
component must not create a second full-image Base64 allocation. Images are
sent at their captured resolution; the request's `max_pixels=8388608` provides
the documented proportional model-side limit. The firmware does not
proactively reduce a clear image below that limit.

### `dashscope_omr`

`dashscope_omr` owns the HTTPS request and retry policy. Its fixed endpoint is:

`https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`

The request uses `qwen3.8-max`, the exact approved OMR prompt, JSON object
response format, temperature zero, thinking disabled, 8000 output tokens, and
non-streaming output. The JPEG is represented as a Base64 Data URL.

The compile-time credential entry has this safe default:

```c
#define DASHSCOPE_API_KEY "__DASHSCOPE_API_KEY__"
```

The placeholder is treated as unconfigured. No request headers, credentials,
or Base64 body are logged.

The client precomputes `4 * ((jpeg_len + 2) / 3)` and the full JSON content
length, calls `esp_http_client_open()`, then writes the JSON prefix, 3-byte
aligned Base64 chunks, and JSON suffix with `esp_http_client_write()`. HTTPS
uses `esp_crt_bundle_attach`; common-name verification is never disabled. The
total timeout is at least 180 seconds.

Only one OMR request may execute at a time. A task receives one unique ID, and
all retry attempts keep that ID. Network interruptions and HTTP 429, 500, 502,
503, and 504 are retried at most twice after approximately two and five
seconds. HTTP 401 and 403 fail immediately. A task result is committed only
once, after every response check succeeds.

### `dashscope_omr_response`

The response decoder is a portable C/cJSON unit shared by firmware and host
tests. It requires:

1. HTTP status 200.
2. A valid outer JSON object.
3. `choices[0].finish_reason == "stop"`.
4. A string at `choices[0].message.content`.
5. A valid compact score object parsed from that string.

`finish_reason == "length"` is an explicit truncation error and cannot enter
the score or playback stores. Usage input, output, and total tokens are copied
when present. Malformed usage data does not replace the more important
completion/schema error, but is recorded as a response-format error when the
rest of the envelope would otherwise succeed.

### `compact_score`

`compact_score` is an owned, hierarchical internal representation. It
preserves public metadata, systems, staves, bars, bar flags, volta numbers,
event flags, voices, lyrics, numbered-notation pitch fields, staff-notation
MIDI chord arrays, relative starts, tick durations, deterministic absolute
starts, and stable event indices.

Validation includes at least:

- version 1, notation kind `n` or `s`, PPQ 24;
- non-empty systems and at least one event;
- required metadata types, including nullable time signature and tempo;
- seven-item numbered events with degree 0-7, octave integer, ticks greater
  than zero, accidental 0-5, and defined flag bits only;
- six-item staff events with MIDI values 0-127, ticks greater than zero,
  positive voice, and defined flag bits only;
- bar flags and volta values limited to the defined representation;
- non-negative integer starts and no `start + ticks` overflow;
- no event beyond a known normal measure length
  `numerator * 24 * 4 / denominator`;
- deterministic ordering within each bar and voice. Model events that are out
  of order are sorted without changing or creating events;
- complete interior measures end at the known measure length for every voice
  that occurs in the measure. A first-measure pickup and final incomplete
  measure may be shorter.

When the time signature is known, absolute bar positions advance by the
normal measure length. This establishes timing coordinates but does not create
implicit rest events. With an unknown time signature, the next bar begins at
the greatest confirmed event end in the current bar/system.

## Legacy playback bridge

The complete compact document remains the authoritative recognition result.
A separate deterministic bridge creates the legacy onset/duration note view
needed by the current scoring and screen preparation code:

- Staff notation emits every MIDI pitch in each chord at the same onset.
- Explicit rests advance the timeline but do not become sounding notes.
- Numbered notation is converted only when `meta.nkey` identifies the tonic;
  otherwise the compact document is retained and marked not playback-ready.
- A nullable model BPM remains null in the compact document. A user-selected
  or playback-layer tempo may be used for derived milliseconds; it is not
  written back as model metadata.
- Lyrics, repeats, ties, slurs, staff identity, voice identity, and all source
  flags stay in the compact document even where the old flat playback view
  cannot express them.

No legacy conversion may add a note or rest that was absent from the model
output.

## Device API behavior

The existing `POST /api/ai/sheet_to_score` route remains the local JPEG test
and product entry point. It accepts exactly one raw JPEG page, creates a unique
task, performs the DashScope request synchronously under the single-request
guard, validates both JSON layers, stores the internal document, and only then
updates compatible score/playback state.

The response includes an explicit success flag, task ID, model name, compact
score, playback readiness, event count, JPEG byte count, elapsed time, HTTP
status, and available token usage. Error responses retain stable error codes
for missing credentials, HTTP 401/403/429/5xx, timeout/network failure,
truncation, invalid outer JSON, invalid inner JSON, schema errors, and
unplayable-but-valid notation.

The mini-program may keep its background task UI. It stores the full compact
score alongside any derived legacy notes so that local persistence does not
discard musical information.

## Observability and security

One completion log line records task ID, JPEG bytes, elapsed milliseconds,
HTTP status, input/output token counts, event count, attempt count, and final
error code. It never records the API key, Authorization header, request body,
Base64 data, full image, or full model response.

The implementation must not contain the credential supplied in conversation.
Repository scans verify that only the placeholder appears in source and test
fixtures.

## Tests and verification

Portable host tests compile the production compact-score and completion
response parsers with cJSON. At minimum they cover:

- valid numbered notation;
- valid two-staff piano notation with a chord;
- deterministic sorting;
- measure overflow;
- invalid event array lengths and flag bits;
- `finish_reason=length`;
- malformed outer JSON;
- malformed compact inner JSON;
- a valid completion envelope and usage counters.

A local-file API script posts one JPEG to the existing device endpoint without
containing any cloud credential. HTTP policy tests use injected/mock transport
results for success, 401, 429 followed by success/failure, timeout, and the
response-parser cases above.

Verification consists of the host parser test command, the relevant
mini-program tests, a full ESP-IDF 5.4 firmware build, and a secret scan. Real
cloud and camera hardware tests are reported as not run when no provisioned
device/API key is available.

## Existing-work preservation

The repository already contains unrelated uncommitted voice, speaker,
network, screen, and mini-program changes. Implementation uses new components
and minimal targeted edits. Existing hunks in overlapping files are preserved
and are not reformatted or reverted.
