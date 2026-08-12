# Doubao WebSocket Handshake Header Buffer Design

## Problem

The ESP-IDF WebSocket transport uses `CONFIG_WS_BUFFER_SIZE`, not the
`esp_websocket_client_config_t.buffer_size` field, to construct the HTTP
Upgrade request and receive the Upgrade response headers. The existing 1024
byte transport buffer fills before the response header terminator is received,
so the transport cannot parse an HTTP status or complete the WebSocket
connection.

## Design

- Set `CONFIG_WS_BUFFER_SIZE` to 4096 bytes for the ESP32-P4 build.
- Enable `CONFIG_WS_DYNAMIC_BUFFER` so the transport releases this temporary
  handshake buffer after a successful connection.
- Apply both values to the current generated `sdkconfig` and to
  `sdkconfig.defaults.esp32p4`, ensuring that reconfiguration keeps the fix.
- Do not modify the WebSocket client component, Doubao protocol, credentials,
  audio buffering, scoring, UI, MIDI, or board communication logic.

## Expected Result

The WebSocket transport can receive the complete Upgrade response and proceed
to either `WebSocket connected` or a real HTTP authentication/protocol error.
Once the session becomes ready, buffered microphone audio is drained normally;
the existing retry and overrun protection remains unchanged.

## Verification

- Confirm the two configuration symbols have exactly one active value.
- Confirm no source or ESP-IDF managed-component file changed for this fix.
- Reconfigure and build P4 before flashing so the transport component is
  rebuilt with the new compile-time buffer size.
