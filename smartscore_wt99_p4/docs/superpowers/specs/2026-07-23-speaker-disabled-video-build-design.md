# Speaker-disabled video firmware design

## Goal

Keep the current speaker-enabled firmware available as a stable Git branch,
while producing a second firmware that never initializes the WT99 ES8311/I2S
audio path and never enables the NS4150B amplifier.

## Selected approach

Add a `SMARTSCORE_SPEAKER_ENABLED` build option in the existing
`speaker_service` Kconfig menu. The speaker-disabled branch defaults this
option to off.

When the option is off:

- `app_main` does not call `speaker_service_init()` or queue a startup tone.
- The ES8311 audio path does not claim GPIO7 and GPIO8. The screen touch
  controller still uses the same GPIO7/GPIO8 I2C bus, so this build does not
  make those pins available for UART while the touchscreen is enabled.
- GPIO53 is explicitly configured to the amplifier-off level by a small
  board-level function that does not initialize I2C, I2S, or the codec.
- Existing speaker APIs remain linked so screen and HTTP code need no
  unrelated changes; they report the service as not ready.

## Version separation

- `codex/speaker-enabled-video` points to the unchanged speaker-enabled
  baseline.
- `codex/speaker-disabled-video` contains this design and the disabled
  implementation.

## Verification

Build the complete ESP32-P4 project with ESP-IDF 5.5.3. Inspect the resulting
configuration to confirm `SMARTSCORE_SPEAKER_ENABLED` is disabled and verify
that the enabled baseline branch remains unchanged.
