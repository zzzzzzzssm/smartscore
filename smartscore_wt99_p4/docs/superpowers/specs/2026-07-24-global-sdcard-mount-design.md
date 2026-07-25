# Global MicroSD Mount Design

## Goal

Mount the WT99 MicroSD card independently of the optional speaker service so
both speaker-enabled and speaker-disabled firmware expose the card at
`/sdcard`.

## Architecture

`app_main()` owns the one-time MicroSD mount. It calls
`board_sdcard_mount()` after NVS initialization and before starting the
speaker service, display, or any other SD-card consumer.

`speaker_service_init()` no longer mounts the card. It may continue to read
the card status and use `/sdcard/wav`, but it does not own storage
initialization.

The existing board driver remains responsible for:

- enabling on-chip LDO channel 4;
- initializing SDMMC slot 0 in four-bit mode on GPIO39 through GPIO44;
- mounting a FAT/FAT32 filesystem at `/sdcard`;
- recording mount state, capacity, and the last error.

## Startup and Error Handling

The application attempts the mount once per boot. A successful mount logs the
slot, pin mapping, and capacity. A failed mount logs a `BOARD_SD` warning and
does not block the rest of startup. Display, Wi-Fi, BLE, USB MIDI, and other
services continue to initialize.

The firmware does not format a card when mounting fails. Runtime hot-plug and
automatic retries are outside this change. If a card is inserted after the
mount attempt, the device must be restarted to try again.

## Compatibility

The change applies identically to speaker-enabled and speaker-disabled builds.
The board mount function remains idempotent, so an accidental later call after
a successful mount returns `ESP_OK` without reinitializing the controller.

## Verification

- Confirm the global mount call occurs before `speaker_service_init()` and
  `screen_adapter_start()`.
- Confirm `speaker_service_init()` no longer calls `board_sdcard_mount()`.
- Build the current speaker-disabled configuration.
- Build or compile-check the speaker-enabled configuration if it can be done
  without overwriting unrelated user configuration.
- Inspect the resulting startup path for a non-blocking warning when no card
  is present and an `SD mounted` message when mounting succeeds.
