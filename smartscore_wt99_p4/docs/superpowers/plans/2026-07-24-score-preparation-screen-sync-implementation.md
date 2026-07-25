# Score Preparation and Screen Sync Implementation Plan

Design:
`docs/superpowers/specs/2026-07-24-score-preparation-screen-sync-design.md`

## 1. Add the canonical preparation session

- Add a screen-adapter preparation state containing the selected score,
  notation type, read-only/follow mode, input source, phase, revision, and
  error.
- Protect cross-task reads and writes with the existing screen/session locking
  pattern.
- Expose select, option update, start, and status operations without exposing
  LVGL objects to HTTP handlers.

## 2. Connect the embedded screen flow

- Route screen-side score selection into the canonical preparation session.
- Let remote score selection request the existing LVGL preparation page.
- Add read-only/follow controls to the preparation page while preserving the
  latest migrated layout.
- Reconcile notation, mode, and input selection from the canonical revision.
- Route both screen and remote start through one implementation that renders
  the requested notation and starts scoring only for follow mode.

## 3. Add preparation APIs

- Add status, select, options, and start handlers under
  `/api/practice/preparation`.
- Keep `/api/scores/sd/select` and `/api/start` compatible by delegating to the
  canonical flow where possible.
- Return stable phase, revision, option, and error fields for mini-program
  polling.

## 4. Align the mini-program

- Add preparation API wrappers in `miniprogram/utils/api.js`.
- After SD score selection, navigate to the practice page with the device
  preparation session already active.
- Add notation and mode selectors to the practice page, reusing its input
  source controls.
- Poll the preparation status, update controls without feedback loops, and
  start through the canonical preparation endpoint.

## 5. Restore physical speaker output

- Enable `CONFIG_SMARTSCORE_SPEAKER_ENABLED` in the project defaults and active
  configuration.
- Keep the control-only implementation available for deliberately
  speaker-disabled builds, but use the hardware-backed service in this build.
- Preserve existing mini-program and screen audio status synchronization.

## 6. Verify

- Add focused mini-program request and preparation-state tests.
- Run all mini-program JavaScript syntax checks and existing tests.
- Build the complete ESP32-P4 firmware with ESP-IDF 5.5.3.
- Confirm the generated configuration enables the speaker and the final diff
  contains no changes to the three S3 projects.
