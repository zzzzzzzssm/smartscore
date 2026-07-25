# MIDI sequence-first alignment design

## Problem

USB MIDI notes have exact event timestamps, but recording begins before the
performer plays. The existing scorer clamps the global start offset to two
seconds and uses absolute time during its first alignment pass. A longer wait
therefore shifts the whole sequence, creates incorrect pitch pairings, and
inflates rhythm error. The mini-program also draws corrected time by subtracting
only an offset and ignores tempo scaling.

## Design

The MIDI scorer uses three stages:

1. Align target and played notes by discrete MIDI pitch and order only. Exact
   pitches cost zero, isolated substitutions remain wrong-note matches, and a
   sequence shift is cheaper as missing/extra notes than as many substitutions.
2. Estimate `tempo_scale` and `start_offset` from exact-pitch anchors. MIDI start
   offset is not clamped to an audio-oriented two-second window.
3. Run the existing time-aware fine alignment and retain strict MIDI pitch,
   missing, extra, wrong-note, relative-rhythm, duration, completeness, and the
   existing total-score weights.

The result adds aligned played start values while preserving raw
`played_start_ms`. The mini-program renders and calculates chart statistics from
the aligned values and uses result-level `tempo_scale` and `start_offset` as a
fallback.

## Boundaries and errors

No USB transport, recorder, input-source state machine, network, provisioning,
speaker, or audio-S3 code changes. Empty performances retain the existing valid
zero-score result behavior. Allocation failures keep the existing explicit
errors and PSRAM-first allocation policy.

## Verification

- Host-side deterministic scorer cases: start delay over two seconds, tempo
  scaling, isolated wrong note, missing note, and extra note.
- Mini-program JavaScript syntax check.
- Full ESP-IDF 5.4 build without flashing.
