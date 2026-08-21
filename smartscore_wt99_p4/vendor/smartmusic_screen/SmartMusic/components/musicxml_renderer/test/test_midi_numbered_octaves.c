#include <string.h>

#include "midi_parser.h"
#include "unity.h"

TEST_CASE("numbered notation maps the complete MIDI octave range",
          "[midi][numbered][octave]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 7;
    const uint8_t pitches[] = {0, 36, 48, 60, 72, 84, 127};
    const int8_t octaves[] = {-5, -2, -1, 0, 1, 2, 5};
    for (int i = 0; i < midi.note_count; ++i) {
        midi.notes[i].note = pitches[i];
        midi.notes[i].velocity = 100;
        midi.notes[i].staff = 1;
        midi.notes[i].start_tick = (uint32_t)i * 60U;
        midi.notes[i].duration = 240;
    }

    char text[128];
    midi_numbered_note_ref_t refs[7];
    size_t ref_count = 0;
    bool truncated = false;
    TEST_ASSERT_TRUE(midi_generate_measure_range_mapped(
        &midi, text, sizeof(text), 0, 1, 0,
        refs, 7, &ref_count, &truncated));
    TEST_ASSERT_FALSE(truncated);
    TEST_ASSERT_EQUAL_UINT32(7, ref_count);
    TEST_ASSERT_NULL(strchr(text, ','));

    for (size_t i = 0; i < ref_count; ++i) {
        TEST_ASSERT_EQUAL_UINT8(1, refs[i].byte_length);
        TEST_ASSERT_EQUAL_UINT8(1, refs[i].reduction_line_count);
        TEST_ASSERT_EQUAL_INT8(octaves[i], refs[i].octave);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)i, refs[i].note_index);
        TEST_ASSERT_TRUE(refs[i].byte_offset < strlen(text));
    }
}

TEST_CASE("numbered octave metadata is independent of duration glyph",
          "[midi][numbered][octave]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 4;
    const uint8_t pitches[] = {36, 48, 72, 84};
    const uint32_t durations[] = {480, 120, 480, 120};
    const int8_t octaves[] = {-2, -1, 1, 2};
    for (int i = 0; i < midi.note_count; ++i) {
        midi.notes[i].note = pitches[i];
        midi.notes[i].velocity = 100;
        midi.notes[i].staff = 1;
        midi.notes[i].start_tick = (uint32_t)i * 480U;
        midi.notes[i].duration = durations[i];
    }

    char text[128];
    midi_numbered_note_ref_t refs[4];
    size_t ref_count = 0;
    bool truncated = false;
    TEST_ASSERT_TRUE(midi_generate_measure_range_mapped(
        &midi, text, sizeof(text), 0, 1, 0,
        refs, 4, &ref_count, &truncated));
    TEST_ASSERT_FALSE(truncated);
    TEST_ASSERT_EQUAL_UINT32(4, ref_count);
    for (size_t i = 0; i < ref_count; ++i) {
        TEST_ASSERT_EQUAL_UINT8(durations[i] == 120U ? 2U : 0U,
                                refs[i].reduction_line_count);
        TEST_ASSERT_EQUAL_INT8(octaves[i], refs[i].octave);
    }
}

TEST_CASE("numbered notation distinguishes rests from sustained beats",
          "[midi][numbered][rest]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 2;
    midi.notes[0] = (midi_note_t){
        .note = 60,
        .velocity = 100,
        .staff = 1,
        .start_tick = 0,
        .duration = 960,
    };
    midi.notes[1] = (midi_note_t){
        .note = 64,
        .velocity = 100,
        .staff = 1,
        .start_tick = 1440,
        .duration = 480,
    };

    char text[128];
    TEST_ASSERT_TRUE(midi_generate_measure_range(
        &midi, text, sizeof(text), 0, 1, 1));
    TEST_ASSERT_NOT_NULL(strstr(text, "1 - 0 3"));
}

TEST_CASE("numbered notation reserves augmentation dots and keeps mapping",
          "[midi][numbered][dots]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 6;
    midi.time_sig_den = 3;
    midi.note_count = 1;
    midi.notes[0] = (midi_note_t){
        .note = 72,
        .velocity = 100,
        .staff = 1,
        .dots = 2,
        .start_tick = 0,
        .duration = 180,
    };

    char text[128];
    midi_numbered_note_ref_t ref = {0};
    size_t ref_count = 0;
    bool truncated = false;
    TEST_ASSERT_TRUE(midi_generate_measure_range_mapped(
        &midi, text, sizeof(text), 0, 1, 1,
        &ref, 1, &ref_count, &truncated));
    TEST_ASSERT_FALSE(truncated);
    TEST_ASSERT_EQUAL_UINT32(1, ref_count);
    TEST_ASSERT_EQUAL_UINT8(1, ref.byte_length);
    TEST_ASSERT_EQUAL_UINT8(1, ref.reduction_line_count);
    TEST_ASSERT_EQUAL_UINT16(0, ref.note_index);
    TEST_ASSERT_TRUE(strlen(text) >= 5U);
}

TEST_CASE("legacy numbered notation infers only exact dots and crossbar ties",
          "[midi][numbered][legacy]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 7;
    midi.notes[0] = (midi_note_t){
        .note = 67, .staff = 1, .voice = 1,
        .start_tick = 0, .duration = 720,
    };
    midi.notes[1] = (midi_note_t){
        .note = 60, .staff = 1, .voice = 1,
        .start_tick = 1440, .duration = 480,
    };
    midi.notes[2] = (midi_note_t){
        .note = 60, .staff = 1, .voice = 1,
        .start_tick = 1920, .duration = 480,
    };
    midi.notes[3] = (midi_note_t){
        .note = 62, .staff = 1, .voice = 1,
        .start_tick = 2400, .duration = 480,
    };
    midi.notes[4] = (midi_note_t){
        .note = 64, .staff = 1, .voice = 1,
        .start_tick = 2880, .duration = 480,
    };
    midi.notes[5] = (midi_note_t){
        .note = 65, .staff = 1, .voice = 1,
        .start_tick = 3360, .duration = 480,
    };
    midi.notes[6] = (midi_note_t){
        .note = 77, .staff = 2, .voice = 1,
        .start_tick = 3360, .duration = 480,
    };

    midi_numbered_inference_stats_t stats = {0};
    TEST_ASSERT_TRUE(midi_numbered_infer_legacy_marks(&midi, &stats));
    TEST_ASSERT_EQUAL_UINT16(1, stats.inferred_dots);
    TEST_ASSERT_EQUAL_UINT16(1, stats.inferred_ties);
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_slurs);
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_glissandi);
    TEST_ASSERT_EQUAL_UINT8(1, midi.notes[0].dots);
    TEST_ASSERT_BITS_HIGH(MIDI_NOTE_TIE_START, midi.notes[1].tie_flags);
    TEST_ASSERT_BITS_HIGH(MIDI_NOTE_TIE_STOP, midi.notes[2].tie_flags);
    for (int index = 0; index < midi.note_count; ++index) {
        TEST_ASSERT_EQUAL_UINT8(0, midi.notes[index].slur_start);
        TEST_ASSERT_EQUAL_UINT8(0, midi.notes[index].slur_stop);
    }
}

TEST_CASE("legacy numbered notation does not tie repeated notes within a bar",
          "[midi][numbered][legacy]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 2;
    midi.notes[0] = (midi_note_t){
        .note = 60, .staff = 1, .voice = 1,
        .start_tick = 0, .duration = 480,
    };
    midi.notes[1] = (midi_note_t){
        .note = 60, .staff = 1, .voice = 1,
        .start_tick = 480, .duration = 480,
    };

    midi_numbered_inference_stats_t stats = {0};
    TEST_ASSERT_TRUE(midi_numbered_infer_legacy_marks(&midi, &stats));
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_ties);
    TEST_ASSERT_EQUAL_UINT8(0, midi.notes[0].tie_flags);
    TEST_ASSERT_EQUAL_UINT8(0, midi.notes[1].tie_flags);
}

TEST_CASE("explicit numbered metadata disables inference by category",
          "[midi][numbered][legacy]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 4;
    midi.notes[0] = (midi_note_t){
        .note = 67, .staff = 1, .voice = 1, .dots = 1,
        .slur_start = 9, .start_tick = 0, .duration = 720,
    };
    midi.notes[1] = (midi_note_t){
        .note = 60, .staff = 1, .voice = 1,
        .tie_flags = MIDI_NOTE_TIE_START,
        .start_tick = 1440, .duration = 480,
    };
    midi.notes[2] = (midi_note_t){
        .note = 60, .staff = 1, .voice = 1,
        .tie_flags = MIDI_NOTE_TIE_STOP,
        .start_tick = 1920, .duration = 480,
    };
    midi.notes[3] = (midi_note_t){
        .note = 69, .staff = 1, .voice = 1, .slur_stop = 9,
        .start_tick = 2400, .duration = 720,
    };

    midi_numbered_inference_stats_t stats = {0};
    TEST_ASSERT_TRUE(midi_numbered_infer_legacy_marks(&midi, &stats));
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_dots);
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_ties);
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_slurs);
    TEST_ASSERT_EQUAL_UINT8(0, midi.notes[3].dots);
    TEST_ASSERT_EQUAL_UINT8(9, midi.notes[0].slur_start);
    TEST_ASSERT_EQUAL_UINT8(9, midi.notes[3].slur_stop);
}

TEST_CASE("legacy numbered notation does not invent interval glissandi",
          "[midi][numbered][legacy]")
{
    midi_data_t midi = {0};
    midi.ticks_per_quarter = 480;
    midi.time_sig_num = 4;
    midi.time_sig_den = 2;
    midi.note_count = 3;
    const uint8_t pitches[] = {48, 84, 50};
    for (int index = 0; index < midi.note_count; ++index) {
        midi.notes[index] = (midi_note_t){
            .note = pitches[index], .staff = 1, .voice = 1,
            .start_tick = (uint32_t)index * 480U, .duration = 480,
        };
    }

    midi_numbered_inference_stats_t stats = {0};
    TEST_ASSERT_TRUE(midi_numbered_infer_legacy_marks(&midi, &stats));
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_glissandi);
    TEST_ASSERT_EQUAL_UINT16(0, stats.inferred_slurs);
    for (int index = 0; index < midi.note_count; ++index) {
        TEST_ASSERT_EQUAL_UINT8(0, midi.notes[index].gliss_start);
        TEST_ASSERT_EQUAL_UINT8(0, midi.notes[index].gliss_stop);
    }
}
