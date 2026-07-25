# Creator Core

Creator Mode reuses the existing USB-MIDI producer, `midi_data_t`, music
screen, and Leland renderer.

The module is intentionally UI-independent:

- `creator_recorder` pairs Note On/Off events and stores wall-clock timing.
- `creator_quantizer` classifies whole through sixteenth notes at the selected
  BPM and reports whether the performance falls inside the tolerance.
- Recorder snapshots are ordinary `midi_data_t` values, so pitch-to-staff
  engraving remains owned by the existing Leland view.
- Grand-staff assignment uses middle C (MIDI 60) as a replaceable split point.
- Overwrite truncates at a measure boundary before starting a new timing
  segment, preventing duplicate material.

The `test` directory covers duration classes, timing fields, staff mapping,
and measure overwrite semantics.
