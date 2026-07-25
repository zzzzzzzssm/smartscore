# MusicXML to Leland conversion layer

This component deliberately separates notation semantics from output pixels:

```text
MusicXML 4.0
  -> Expat SAX parser
  -> music_score_t (parts / measures / voices / events)
  -> music_layout_build() (staff-space coordinates)
  -> music_scene_t (glyph / line / cubic Bezier commands)
  -> LVGL today; SVG or high-DPI backends can consume the same scene later
```

All musical symbols are resolved by SMuFL name and canonical code point in
`smufl_glyphs.c`. Staff lines, stems, beams, barlines, ledger lines, ties, and
slurs remain scalable geometry, as required by conventional engraving.

## Current conversion coverage

- `score-partwise`, parts, measures, divisions, multiple staves
- notes, rests, chords, grace markers, voices, `backup`, and `forward`
- treble, bass, and alto clefs
- numeric, common, and cut time
- circle-of-fifths key signatures
- whole through 32nd durations, dots, stems, and three beam levels
- standard accidentals through double-flat/double-sharp
- staccato, accent, tenuto, fermata, ties, and numbered slurs
- single, double, final, and repeat barlines
- multiple systems and grand-staff geometry
- two voices on one staff with opposing automatic stems, voice-separated
  beams/ties, displaced unisons/seconds, and vertically separated rests
- per-MusicXML-event RGB color overrides for recognition/correction feedback;
  every affected scene item retains its `source_event_index`

The model and scene use `music_sp_t` staff spaces. Pixel conversion belongs
only to an output backend.

References:

- https://www.w3.org/2021/06/musicxml40/
- https://w3c-cg.github.io/smufl/latest/
