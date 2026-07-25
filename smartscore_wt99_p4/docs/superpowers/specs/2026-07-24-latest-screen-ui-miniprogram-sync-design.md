# Latest Screen UI and Mini-Program Synchronization Design

## Goal

Migrate the latest embedded-screen UI from
`C:\Users\17762\Downloads\SmartMusic.zip` into the current
`smartscore_wt99_p4` firmware without replacing unrelated project work, then
make the embedded screen and mini-program share synchronized controls for:

- metronome tempo, meter, running, paused, and stopped state;
- WAV selection, playback, pause, and stop state;
- volume and mute state;
- calibration-tone frequency, running, and stopped state.

This iteration is intended for UI and interface testing. The onboard speaker
hardware remains disabled and the three S3 communication paths are outside the
scope of this change.

## Scope

### Included

- Import the current screen assets, fonts, generated UI corrections, and mode
  pages from `SmartMusic.zip`.
- Merge those changes surgically with the current working tree instead of
  replacing the complete vendor tree.
- Preserve current scoring, creator-mode, SD-card, networking, and device API
  additions.
- Provide a logical audio-control mode when speaker hardware is disabled so
  screen and mini-program synchronization can be tested without sound.
- Align mini-program requests and responses with the device HTTP API.
- Add focused tests for interface payloads and bidirectional state mapping.
- Build the ESP-IDF firmware and run available mini-program/static tests.

### Excluded

- Music, camera, or voice S3 protocol implementation and testing.
- Enabling ES8311, I2S, or the onboard amplifier.
- PCB pin changes.
- Changes to scoring algorithms, networking/provisioning behavior, SD score
  storage, or other unrelated product features.
- Wholesale replacement of the repository, vendor subtree, or Git metadata
  from the ZIP archive.

## Migration Strategy

The ZIP is treated as the authoritative source only for its latest screen UI.
Relevant source files are compared with the current project before merging.

The migration will:

1. Add the new screen mode hub and its required fonts/assets.
2. Apply generated UI fixes that affect layout, page lifecycle, labels, and
   navigation.
3. Merge creator-mode entry hooks into the current creator-mode implementation
   so its existing remote-control and state APIs remain intact.
4. Add only the screen initialization and navigation hooks required to expose
   the latest UI.

Large files containing unrelated scoring or follower changes will not be
copied wholesale. Only UI-specific hunks will be ported.

## Shared Control Model

The firmware owns the canonical audio-control state. Both control surfaces use
the same service:

```text
Embedded screen ─┐
                 ├─> logical audio-control state ─> device status
Mini-program  ───┘
```

Screen events update the service directly. Mini-program events update it
through the device HTTP API. Both surfaces periodically read the canonical
status and reconcile their controls, which prevents one side from displaying
stale local state.

Programmatic UI updates must not re-send commands. Screen callbacks therefore
distinguish user interaction from status reconciliation, and the mini-program
must avoid request loops while applying polled device state.

## Speaker-Disabled Test Mode

The current firmware configuration has
`CONFIG_SMARTSCORE_SPEAKER_ENABLED` disabled. In this configuration:

- ES8311, I2S, and amplifier initialization remain disabled.
- Control commands update the logical state and return success when their
  parameters are valid.
- No PCM samples are generated or written to hardware.
- Device status identifies hardware output as disabled while reporting the
  current logical control state.
- Playback and calibration-tone state are simulated for synchronization tests;
  they do not produce sound.
- Metronome running and paused state, tempo, meter, and current beat remain
  observable without audio output.

When the existing speaker configuration is enabled later, the same control API
continues to drive the hardware-backed implementation.

## Interface Contract

The existing device routes remain the public contract:

- `GET /api/audio/status`
- `POST /api/audio/volume`
- `POST /api/audio/mute`
- `POST /api/audio/tone`
- `POST /api/audio/stop`
- `POST /api/metronome/start`
- `POST /api/metronome/pause`
- `POST /api/metronome/stop`
- `GET /api/audio/files`
- `POST /api/audio/file/play`
- `POST /api/audio/file/pause`
- `POST /api/audio/file/stop`

The mini-program presents volume as `0..100`; the firmware contract remains
`0..80`. Conversion is centralized in `miniprogram/utils/api.js`, and status
values use the inverse conversion before updating the mini-program UI. The
embedded screen uses the firmware `0..80` value directly.

Status contains enough information for both clients to reconstruct the full
control surface, including hardware availability, volume, mute, tone
frequency, metronome parameters/state/beat, playback state, and selected file.

Invalid parameters continue to return explicit client errors. Valid logical
commands in speaker-disabled mode must not be reported as hardware failures.

## Screen Synchronization

The screen status timer reconciles all visible controls:

- volume slider and mute presentation;
- selected calibration frequency and running indicator;
- metronome BPM, meter, action buttons, and beat indicator;
- selected WAV file and play/pause/stop presentation.

Navigation and page deletion flags from the generated UI must be handled with
the updated pointer-based lifecycle API from the ZIP. Timers and callbacks are
created once and cleaned up when their owning screen is deleted.

## Mini-Program Synchronization

The mini-program uses the device API rather than maintaining an independent
audio state. It:

- sends user changes to the corresponding audio route;
- polls device status while relevant pages are visible;
- applies status from screen-originated changes;
- cancels polling when pages are hidden;
- displays the difference between logical control availability and disabled
  physical audio output;
- preserves the current offline fallback where already required, without
  presenting fallback state as confirmed device state.

## Error Handling

- HTTP handlers validate JSON types and numeric ranges before modifying state.
- The last control error remains available in device status.
- UI callbacks log rejected commands and restore controls from canonical state
  on the next reconciliation pass.
- Missing SD storage produces an empty/unavailable WAV list without breaking
  the other controls.
- A failure to initialize optional audio hardware does not prevent the screen,
  mini-program API, or network services from starting.

## Verification

Verification is limited to the approved scope:

1. Compare migrated UI files with the ZIP source and document intentionally
   omitted unrelated changes.
2. Build the ESP-IDF firmware using the current configuration.
3. Run focused host/static tests for:
   - screen-to-service event mapping;
   - API-to-service state changes;
   - status-to-screen reconciliation;
   - status-to-mini-program reconciliation;
   - loop suppression during programmatic updates;
   - speaker-disabled logical mode.
4. Run available mini-program JavaScript checks.
5. Inspect the final Git diff to confirm that S3 and unrelated modules were not
   changed.

Hardware validation for actual sound and S3 communication is deferred to a
later iteration.
