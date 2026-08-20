# Numbered Notation Connections and Centering Design

## Goal

Bring the device numbered-notation preview closer to conventional Jianpu
engraving while preserving the existing MIDI source data, note-result coloring,
paging, and Creator behavior.

The first implementation must:

- distinguish rests (`0`) from duration-extension dashes (`-`);
- render augmentation dots carried by the source note;
- render explicit ties, slurs, and glissandi from existing MIDI metadata;
- continue ties and slurs cleanly across numbered rows and pages;
- prevent connection curves from colliding with octave dots and reduction lines;
- center each numbered row using its actual visible width;
- remain bounded by the existing `MAX_NOTES` capacities and fail open when an
  optional overlay cannot be cached.

The compatibility pass added after device testing must also make legacy local
MIDI JSON readable when it contains no notation metadata.  It is display-only:
it never rewrites the JSON, changes sounding pitches or durations, or changes
playback and scoring.

## Compatibility Decision

Three policies were considered:

1. Keep rendering authoritative metadata only.  This is semantically exact,
   but old MIDI-derived JSON continues to show no phrasing or augmentation
   marks because every metadata field is zero.
2. Aggressively guess ties, slurs, and glissandi from pitch intervals.  This
   produces more marks but can misrepresent repeated notes, articulation, and
   expressive jumps.
3. Use a conservative display-only fallback for legacy scores.  Explicit
   metadata always wins; exact rhythmic evidence may add dots and cross-bar
   ties.  Slurs and glissandi remain explicit-only because plain MIDI does not
   preserve enough authorial phrasing or channel-linked pitch-bend information
   to reconstruct them safely.

Policy 3 is selected.  It improves readability without pretending that plain
MIDI contains all authorial engraving information.

## Notation Semantics

### Ties

A tie connects equal sounding pitches. It is placed closest to the numbered
digits. `MIDI_NOTE_TIE_START` and `MIDI_NOTE_TIE_STOP` are authoritative; the
display must not infer ties merely because two adjacent notes share a pitch.
For a legacy score with no explicit tie metadata, the fallback may infer a tie
only when equal-pitch notes on the same staff and voice touch exactly across a
barline.  Ordinary repeated notes inside a measure remain separate.

### Slurs

A slur is an independent phrasing connection identified by matching non-zero
`slur_start` and `slur_stop` numbers. It uses a higher visual lane than a tie.
If a tie and a slur cover the same notes, both remain visible, with the tie on
the inner lane.

The legacy fallback never infers a slur from note timing or pitch intervals.
Only matched, non-zero source `slur_start` and `slur_stop` identifiers may draw
a slur.  This prevents ordinary legato MIDI playback from covering most of a
numbered row with guessed phrase arcs.

### Glissandi

A glissando is identified by matching non-zero `gliss_start` and `gliss_stop`
numbers. Jianpu renders it as a compact rising or falling slide mark. The mark
follows the pitch direction and is visually distinct from the curved tie/slur.

The fallback never infers a glissando from a melodic interval alone.  The
current `midi_note_t` does not retain the source channel needed to associate
pitch-bend events with a note reliably, so this compatibility pass keeps
glissandi explicit-only.

### Row and page breaks

Connections whose endpoints are on different rows are split into an outgoing
continuation at the first row edge and an incoming continuation at the next
visible row. Each visible half is clipped to its row. Connections that pass
through an off-screen page still show the appropriate incoming/outgoing
continuation on the visible page.

### Other core Jianpu marks

- Silence uses `0`; a dash is reserved for an already sounding note continuing
  through the current beat slot.
- Source `dots` are drawn as augmentation dots to the right of the digit.
- If a legacy note has `dots == 0`, its exact tick duration may supply one or
  two augmentation dots only when it matches a canonical dotted value within
  `max(1, ticks_per_quarter / 96)` ticks.  Arbitrary performed durations are
  not rounded into dotted notation.
- Octave dots remain above or below the digit and reduction lines remain below.
- Black-key accidentals may only be displayed using the score key/signature
  spelling policy; rendering never changes the MIDI pitch.
- Articulations, grace notes, dynamics, and breath marks are not guessed when
  the current intermediate representation has no authoritative data for them.

## Rendering Architecture

Before generating numbered rows, copy the bounded MIDI snapshot into the
existing display-owned working snapshot and run a pure compatibility pass over
that copy.  The pass is enabled only in the local JSON preview path; Creator
snapshots never call it.  Authority is decided independently for dots, ties,
slurs, and glissandi: if any source note already contains a mark in one
category, inference for that complete category is disabled.  Slur and
glissando inference are always disabled.  This avoids mixing guessed and
authored marks.

The pass returns counters for inferred dots and ties; inferred slur and
glissando counters remain zero.  The device log reports explicit and inferred
counts separately so a missing-data problem can be distinguished from a
drawing problem without changing the UI.

Replace the octave-only position cache with one bounded geometry record for
every visible numbered note. Each record retains the existing LVGL span,
source-note index, octave, reduction-line count, and measured digit cell, plus
the source connection metadata.

After LVGL measures a row:

1. Measure every mapped note cell.
2. Calculate the row's real ink bounds, including octave dots, augmentation
   dots, accidentals, and connection endpoints.
3. Shift the row group horizontally so the complete ink bounds are centered
   inside its single-staff or grand-staff content area.
4. Convert note cells to stable row-local anchors.
5. Match connection IDs and assign non-colliding lanes.
6. Draw octave dots, augmentation dots, ties, slurs, and glissandi from the
   same cached anchors in `LV_EVENT_DRAW_MAIN_END`.

The text remains left-aligned inside the shifted group. This avoids relying on
LVGL center-alignment internals that do not necessarily expose the same offset
through span-coordinate APIs.

## Collision and Lane Rules

- A connection starts and ends near the horizontal center of its note cell.
- The tie uses the closest available arc lane.
- Slurs use successively higher lanes and receive extra clearance over upper
  octave dots in their covered range.
- Glissandi use a diagonal/bent slide path and do not share curved lanes.
- Multiple overlapping connections are ordered deterministically by type,
  start note, stop note, and connection number.
- Lower marks never overlap reduction lines or lower octave dots.
- Overlay cache exhaustion logs once and skips only excess decorations; the
  base digits remain readable.

## Generation Changes

The measure generator tracks the latest sounding end tick per staff. For a beat
slot with no onset it emits `-` only when a note is still sounding; otherwise it
emits `0`. New onsets remain visible even while an earlier note sustains.

Mapped note references continue to point to the original MIDI note, so note
coloring, playback guidance, and page following remain unchanged. The mapping
is extended only with display metadata required by augmentation dots and
connection layout.

## Audio Playback

The current local preview player builds voices only from MIDI pitch, velocity,
start, and duration.  It does not consume `slur_*`, `gliss_*`, or raw
pitch-bend events.  Therefore a displayed glissando currently plays as the two
authored discrete notes rather than a synthesized continuous pitch sweep.

This visual correction does not change that behavior.  A future audio-gliss
feature must use an explicit, note-associated pitch-bend curve; it must not
turn a visual interval or inferred connection into an invented glide.

## Centering

Each of the two numbered rows is centered independently. Grand-staff prefixes
(`高：` and `低：`) keep their fixed area; only the corresponding notation
content is centered inside the remaining width. Over-wide rows retain the full
available width and existing wrapping behavior rather than being clipped.

## Safety and Compatibility

- Do not modify Creator recording, quantization, staff layout, or saved MIDI
  pitches.
- Do not persist inferred decorations or expose them as source metadata.
- Skip an entire inferred connection if either endpoint is ambiguous; never
  leave unmatched start/stop identifiers.
- Use fixed-size arrays bounded by `MAX_NOTES`; add no large stack objects.
- Treat malformed or unmatched connection IDs as absent decorations.
- Keep the previous octave-dot fail-open behavior: missing optional geometry
  must never hide the numbered digits.
- Preserve source-note coloring and result-slot mappings.

## Verification

Static and host-test cases should cover:

- silent beat versus sustained beat (`0` versus `-`);
- single and multiple augmentation dots;
- same-pitch tie, different-pitch slur, rising and falling glissando;
- simultaneous tie and slur with distinct lanes;
- upper-octave dots under a slur without collision;
- connection continuation across rows and pages;
- single- and grand-staff rows centered by actual ink bounds;
- unmatched connection IDs and full overlay capacity failing open.
- an all-zero legacy JSON gaining exact dotted marks and a cross-bar same-pitch
  tie, but no inferred slur or glissando;
- a long continuous monophonic passage remaining free of guessed phrase arcs;
- repeated same-pitch notes inside a measure remaining untied;
- a large interval without pitch bend remaining non-glissando;
- explicit metadata suppressing the corresponding inference path;
- Creator snapshots remaining byte-for-byte unchanged by the fallback;
- bounded behavior at `MAX_NOTES`.

No firmware build is required for this change set at the user's request.

## References

- W3C MusicXML 4.0, `tied`: https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/tied/
- W3C MusicXML 4.0, `slur`: https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/slur/
- W3C MusicXML 4.0, `glissando`: https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/glissando/
- JianPu Music Editor Manual: https://musescore.org/sites/musescore.org/files/JianPu%20%5BMusic%20Editor%20Manual%5D.pdf
- People's Education Press numbered-notation textbook: https://v3.ykt.cbern.com.cn/65/document/0f200d9d9af811ec9c6bfa20200f090a/pdf.pdf
