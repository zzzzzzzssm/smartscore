# Latest Screen UI and Mini-Program Sync Implementation Plan

Design:
`docs/superpowers/specs/2026-07-24-latest-screen-ui-miniprogram-sync-design.md`

## 1. Establish the exact UI delta

- Compare the UI-related files extracted from `SmartMusic.zip` with the
  current vendor screen tree.
- Classify each change as generated UI, font/asset, screen navigation, creator
  integration, or unrelated application logic.
- Import only the first four categories and preserve current working-tree
  changes.

## 2. Merge the latest embedded-screen UI

- Add the latest mode hub and required fonts.
- Merge generated layout, lifecycle, navigation, and label fixes.
- Integrate the mode hub into `screen_adapter`.
- Merge the creator-mode open/detached hooks into the current creator-mode
  implementation without removing its existing remote state API.

## 3. Add speaker-disabled logical control

- Keep ES8311, I2S, and amplifier initialization disabled.
- Initialize a control-only speaker service that accepts valid volume, mute,
  tone, metronome, and file state commands without producing PCM output.
- Make logical availability and physical hardware availability separately
  visible in status.
- Preserve the existing hardware-backed behavior for future
  `CONFIG_SMARTSCORE_SPEAKER_ENABLED` builds.

## 4. Complete embedded-screen synchronization

- Route screen events through the shared speaker service.
- Reconcile volume, mute, tone, metronome, beat, file selection, and playback
  controls from canonical status.
- Suppress callbacks while controls are being updated programmatically.
- Clean up timers and handlers when their owning screen is deleted.

## 5. Align mini-program synchronization

- Keep all HTTP route construction in `miniprogram/utils/api.js`.
- Verify the `0..100` mini-program to `0..80` device volume conversion.
- Poll and apply canonical device status on the audio and music pages.
- Prevent stale responses and programmatic updates from issuing duplicate
  commands.
- Display logical control availability separately from physical audio output.

## 6. Verify

- Run formatting and syntax checks on changed files.
- Run mini-program JavaScript syntax and focused state-mapping tests.
- Run the ESP-IDF build with the current speaker-disabled configuration.
- Inspect the final diff and confirm no S3 or unrelated functional modules were
  changed.
