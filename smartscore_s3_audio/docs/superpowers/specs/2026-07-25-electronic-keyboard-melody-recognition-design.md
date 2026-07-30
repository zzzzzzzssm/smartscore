# Electronic Keyboard Speaker Melody Recognition Design

## Objective

Improve the `smartscore_s3_audio` microphone path for beginner, monophonic electronic-keyboard practice. The S3 must emit reliable `note_on` and `note_off` events when a keyboard is heard through its built-in speakers, while continuing to suppress room noise, speech, transients, clipping, and unstable octave estimates.

The practical success criterion is that a clearly played single-note melody produces a continuous, correctly pitched note stream instead of only `SILENCE` and `UNKNOWN`. Absolute perfect recognition in every room is not physically guaranteeable, so the implementation prioritizes competition-demo reliability under the observed hardware and signal levels.

## Evidence and Root Cause

The P4 log confirms that the UART is bidirectional, the S3 responds to `ping`, the `audio_s3` practice session starts, and score following begins. The P4 never logs the first melody-note event.

The music S3 log shows:

- no `SINGLE` result and therefore no `note_on` event;
- many frames below the adaptive noise gate;
- periodic YIN estimates with confidence up to about `0.95` that are still rejected as `low_harmonic_ratio`;
- harmonic explained ratios around `0.02` to `0.09`, far below the current strict threshold of `0.74`;
- a repeated diagnostic that the peak remains below five percent of full scale.

The classifier therefore treats every score note as absent. The P4 correctly renders absent notes in gray.

## Scope

Only `D:\qianrushi\smartscore_s3_audio` will change.

In scope:

- ES7210 input gain for the electronic-keyboard-speaker scenario;
- monophonic candidate classification;
- temporal stability and octave-jump protection;
- diagnostics and recognition-focused tests.

Out of scope:

- P4 scoring, alignment, UI, or UART protocol changes;
- score-guided recognition or transmitting target notes to the S3;
- optimization for chords, singing, speech, or arbitrary music playback;
- unrelated refactoring of the existing dirty worktree.

## Chosen Recognition Strategy

The existing strict path remains the preferred path. A frame is still a strong single-note candidate when YIN confidence and harmonic explained ratio both satisfy the existing strict thresholds.

A second, monophonic YIN override path is added for electronic-keyboard speaker pickup. It accepts a candidate only when all of the following hold:

- YIN is valid and has strong confidence;
- the MIDI note is within the configured melody range `48..84`;
- the selected microphone is above its calibrated gate;
- the frame is not clipped;
- the estimate participates in the existing three-of-five temporal vote.

The strong override confidence will be `0.90`. A supported fallback may accept confidence from `0.80` when the strongest usable spectrum candidate agrees with the YIN pitch class. Values below `0.80` remain unknown for this scenario.

The override deliberately does not require the current `0.74` harmonic explained ratio. Electronic-keyboard speakers, enclosure response, microphone placement, and room reflections can make that ratio unrepresentative even when YIN observes a stable periodic pitch.

## Input Level

Increase the ES7210 input gain from `27 dB` to `33 dB`. The observed peaks leave adequate headroom for this six-decibel increase. Existing clipping detection remains authoritative: clipped frames are never accepted, and diagnostics continue to report sustained low peaks or clipping.

The adaptive noise gate remains enabled. Raising hardware gain is not used as a reason to lower the gate indiscriminately, because both signal and ambient noise rise together.

## Temporal and Octave Protection

The existing three-of-five vote remains the minimum stabilization requirement. Unknown or conflicting attack frames cannot immediately emit a note.

For a candidate that differs from the last stable note by exactly one octave, the classifier compares continuity and available spectral support. A weakly supported octave jump is held as unknown until it wins the normal stable vote; a sustained, newly attacked octave is allowed. This prevents a transient second harmonic from replacing the fundamental without blocking real melodic leaps.

The existing UART note-state machine continues to own release hysteresis and repeated-note handling. A stable same-pitch onset still becomes `note_off` followed by `note_on`; an unknown tail must persist for the configured timeout before release.

## Data Flow

1. ES7210 captures both microphone channels at the higher fixed gain.
2. Existing preprocessing calculates RMS, peak, clipping, and adaptive gates independently per microphone.
3. The existing selector chooses one microphone without mixing unsynchronized channels.
4. YIN and spectrum/chord analysis produce candidate evidence.
5. The classifier evaluates strict single-note, supported YIN fallback, and strong YIN override paths in that order.
6. Temporal voting and octave protection produce a stable single-note result.
7. The existing music UART link emits the unchanged NDJSON `note_on`, `pitch`, and `note_off` messages.
8. P4 receives the same protocol and requires no change.

## Failure Behavior

- Below-gate input remains `SILENCE`.
- Clipped input remains `UNKNOWN` and is logged.
- A high-confidence estimate outside MIDI `48..84` is not accepted by the monophonic override.
- Unstable pitch and unsupported octave jumps remain `UNKNOWN` until stable.
- UART queue overflow behavior and counters remain unchanged.
- If the new path is not used, strict recognition and chord behavior retain their current ordering.

## Verification

Static and host-side tests will cover:

- strict harmonic single-note acceptance;
- strong YIN override with low harmonic ratio;
- rejection below the noise gate;
- rejection of clipping;
- rejection outside the configured melody range;
- three-of-five stabilization;
- transient octave-jump suppression and sustained-octave acceptance;
- unchanged note release and same-note retrigger behavior where test seams permit.

The final hardware check is a single-note keyboard melody played through the built-in speakers. Passing logs must show stable `SINGLE` results on the S3 and `S3 melody note stream confirmed` on the P4. Recognition quality should then be evaluated using correct-note rate, false-note count during silence, octave-error count, and onset latency rather than relying only on whether any note was emitted.
