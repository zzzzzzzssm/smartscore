# AI practice-advice socket-budget design

## Goal

Make repeated **AI 练习建议** requests reliable without disconnecting the
warm Doubao voice WebSocket or changing scoring, advice content, and voice
session behavior.

The current failure is local socket exhaustion. LwIP exposes ten sockets, the
HTTP server may retain several mini-program keep-alive connections, the voice
assistant retains one WebSocket, and the practice page starts three status
requests every second without checking whether the previous refresh completed.
The first long DeepSeek request therefore succeeds while polling continues to
accumulate connections; later requests can fail before TLS with HTTP status
zero and `network_interrupted`.

## Chosen approach

Use two complementary controls:

1. Bound mini-program concurrency so polling never overlaps and is paused
   during the long-running advice request.
2. Give the firmware an explicit socket budget that reserves capacity for the
   warm WebSocket, DNS, and outbound AI HTTPS traffic.

Only changing the mini-program would leave the firmware vulnerable to another
client consuming the remaining sockets. Only raising the socket limit would
hide the unbounded polling behavior without correcting it. Converting advice
generation into a separate asynchronous device API is out of scope for this
repair.

## Mini-program behavior

The practice page owns two independent state flags:

- a non-rendered in-flight flag for status refreshes;
- `isAdviceLoading`, rendered into the advice button's disabled state.

`refreshLiveStatus()` returns immediately when a refresh is already in flight
or an advice request is active. Its existing three status calls remain grouped
in one `Promise.all`, and every completion path releases the in-flight flag.
This keeps the existing API payloads and page state mapping unchanged while
limiting status traffic to one batch at a time.

When the user requests advice, the page:

1. ignores a duplicate tap;
2. sets `isAdviceLoading` and stops the status timer;
3. sends one `/api/ai/score` request;
4. displays the existing success or error modal;
5. clears the loading state in `finally`;
6. if the page is still visible, performs one immediate status refresh and
   restarts the timer.

The page tracks visibility explicitly. An advice request that completes after
`onHide` or `onUnload` must not restart polling. `onShow` remains the only path
that starts polling for a newly visible page.

Pausing polling does not stop practice, scoring, MIDI input, screen rendering,
or voice-assistant traffic.

## Firmware socket budget

Set `CONFIG_LWIP_MAX_SOCKETS=16` in the ESP32-P4 defaults. Configure the device
HTTP server with `max_open_sockets=5` instead of the ESP-IDF default of seven.

The steady-state budget is therefore bounded as follows:

- three sockets reserved internally by `esp_http_server`;
- at most five accepted mini-program HTTP clients;
- one warm Doubao WebSocket;
- one DeepSeek HTTPS connection;
- remaining capacity for DNS and short-lived network work.

`lru_purge_enable` remains enabled. The voice WebSocket is not stopped or
reconnected around an advice request.

DeepSeek error codes and request behavior remain unchanged. No automatic
retry is added because an immediate retry during resource exhaustion would
amplify the failure.

## Error handling

- Duplicate advice taps do not create duplicate device requests.
- Advice success, HTTP error, network error, and timeout all restore the UI in
  `finally`.
- A hidden or unloaded page never restarts its timer from an old request.
- Status-refresh failures retain the existing offline presentation and release
  the in-flight guard.
- The firmware logs the configured HTTP client limit and LwIP socket limit when
  the device API starts, making the runtime budget visible without exposing
  credentials.

## Validation

Per user instruction, this change is not followed by a firmware build.

Automated validation consists of mini-program unit tests and static checks:

- overlapping status ticks create only one status batch;
- advice pauses polling and disables duplicate requests;
- success and failure both restore polling while the page is visible;
- completion while hidden does not restart polling;
- existing advice formatting tests continue to pass;
- static inspection confirms the P4 socket default and HTTP server client
  limit.

Later hardware acceptance, after the user builds and flashes the firmware,
should run at least five practice/advice cycles while the Doubao WebSocket
remains connected. Each advice call must reach an HTTP status, and serial logs
must contain no `Failed to create socket` error.

## Scope boundaries

This change does not alter score calculation, DeepSeek prompts or schema,
DashScope recognition, voice protocol/session lifecycle, HTTP endpoint shapes,
or the one-second visible-page refresh cadence.
