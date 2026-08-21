#include <stdlib.h>
#include <string.h>

#include "midi_notation.h"
#include "unity.h"

static void notation_test_midi_init(midi_data_t *midi)
{
    memset(midi, 0, sizeof(*midi));
    midi->ticks_per_quarter = 480;
    midi->time_sig_num = 4;
    midi->time_sig_den = 2; /* MIDI exponent: 2 means a quarter-note beat. */
    midi->bpm = 100;
}

static midi_note_t notation_test_note(uint8_t pitch, uint32_t onset,
                                      uint32_t duration)
{
    midi_note_t note = {0};
    note.note = pitch;
    note.velocity = 80;
    note.staff = 1;
    note.voice = 1;
    note.start_tick = onset;
    note.duration = duration;
    return note;
}

TEST_CASE("conservative notation analysis accepts a well-supported G major",
          "[midi_notation][key]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    TEST_ASSERT_NOT_NULL(midi);
    notation_test_midi_init(midi);
    static const uint8_t scale[] = {67, 69, 71, 72, 74, 76, 78, 79};
    for (uint32_t measure = 0; measure < 4; ++measure) {
        for (uint32_t i = 0; i < sizeof(scale); ++i) {
            midi->notes[midi->note_count++] = notation_test_note(
                scale[i], measure * 1920U + i * 240U, 240U);
        }
    }

    midi_notation_key_analysis_t analysis;
    TEST_ASSERT_TRUE(midi_notation_analyze_key(midi, &analysis));
    TEST_ASSERT_TRUE(analysis.accepted);
    TEST_ASSERT_EQUAL_INT8(1, analysis.fifths);
    TEST_ASSERT_FALSE(analysis.minor);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(8, analysis.onset_group_count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(2,
                                         analysis.completed_measure_count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(
        3, analysis.c_printed_accidentals -
               analysis.best_printed_accidentals);
    free(midi);
}

TEST_CASE("measure accidental state suppresses repeats and resets at barline",
          "[midi_notation][accidental]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(61, 0, 480);       /* C#4 */
    midi->notes[1] = notation_test_note(61, 480, 480);     /* C#4 */
    midi->notes[2] = notation_test_note(61, 1920, 480);    /* next bar */
    midi->note_count = 3;
    uint16_t mapping[3];
    midi_notation_build_options_t options = {
        .key_fifths = 0,
        .key_minor = false,
    };

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 3, NULL));
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[0]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_NONE,
                      score->events[mapping[1]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[2]].data.note.accidental);
    free(score);
    free(midi);
}

TEST_CASE("key accidental cancellation and restoration are both explicit",
          "[midi_notation][accidental]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->tonality_sf = 1;
    midi->notes[0] = notation_test_note(65, 0, 480);    /* F natural */
    midi->notes[1] = notation_test_note(66, 480, 480);  /* F sharp */
    midi->note_count = 2;
    uint16_t mapping[2];
    midi_notation_build_options_t options = {.key_fifths = 1};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 2, NULL));
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_NATURAL,
                      score->events[mapping[0]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[1]].data.note.accidental);
    free(score);
    free(midi);
}

TEST_CASE("flat keys choose flat enharmonic spelling",
          "[midi_notation][spelling]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(70, 0, 480); /* Bb4, not A#4. */
    midi->note_count = 1;
    uint16_t mapping[1];
    midi_notation_build_options_t options = {.key_fifths = -2};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 1, NULL));
    const music_note_t *note = &score->events[mapping[0]].data.note;
    TEST_ASSERT_EQUAL_UINT8(6, note->pitch.step);
    TEST_ASSERT_EQUAL_INT8(-1, note->pitch.alter);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_NONE, note->accidental);
    free(score);
    free(midi);
}

TEST_CASE("normalizer preserves MIDI and maps exact duplicates to one event",
          "[midi_notation][mapping]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    midi_data_t *before = calloc(1, sizeof(*before));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(before);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 0, 480);
    midi->notes[1] = midi->notes[0];
    midi->note_count = 2;
    *before = *midi;
    uint16_t mapping[2];
    midi_notation_build_result_t result;
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 2, &result));
    TEST_ASSERT_EQUAL_MEMORY(before, midi, sizeof(*midi));
    TEST_ASSERT_EQUAL_UINT16(mapping[0], mapping[1]);
    TEST_ASSERT_EQUAL_UINT16(1, result.score_event_count);
    TEST_ASSERT_EQUAL_UINT16(1, result.deduplicated_note_count);
    free(score);
    free(before);
    free(midi);
}

TEST_CASE("simultaneous accidentals use the pre-chord state atomically",
          "[midi_notation][accidental][voices]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(61, 0, 240);
    midi->notes[1] = notation_test_note(61, 0, 240);
    midi->notes[1].voice = 2;
    midi->notes[2] = notation_test_note(61, 480, 240);
    midi->note_count = 3;
    uint16_t mapping[3];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 3, NULL));
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[0]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[1]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_NONE,
                      score->events[mapping[2]].data.note.accidental);
    free(score);
    free(midi);
}

TEST_CASE("played chord releases normalize to one written rhythm",
          "[midi_notation][chord][rhythm]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 0, 240);
    midi->notes[1] = notation_test_note(64, 0, 480);
    midi->notes[2] = notation_test_note(67, 0, 480);
    midi->note_count = 3;
    uint16_t mapping[3];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 3, NULL));
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_INT32(
            480, score->events[mapping[i]].data.note.duration_divisions);
        TEST_ASSERT_EQUAL(MUSIC_DURATION_QUARTER,
                          score->events[mapping[i]].data.note.type);
    }
    free(score);
    free(midi);
}

TEST_CASE("tie continuation suppresses a repeated barline accidental",
          "[midi_notation][accidental][tie]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(61, 0, 1920);
    midi->notes[0].tie_flags = MIDI_NOTE_TIE_START;
    midi->notes[1] = notation_test_note(61, 1920, 480);
    midi->notes[1].tie_flags = MIDI_NOTE_TIE_STOP;
    midi->note_count = 2;
    uint16_t mapping[2];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 2, NULL));
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[0]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_NONE,
                      score->events[mapping[1]].data.note.accidental);
    free(score);
    free(midi);
}

TEST_CASE("tie continuation does not alter later accidental state",
          "[midi_notation][accidental][tie]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(61, 0, 1920);
    midi->notes[0].tie_flags = MIDI_NOTE_TIE_START;
    midi->notes[1] = notation_test_note(61, 1920, 240);
    midi->notes[1].tie_flags = MIDI_NOTE_TIE_STOP;
    midi->notes[2] = notation_test_note(61, 2400, 240);
    midi->note_count = 3;
    uint16_t mapping[3];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 3, NULL));
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_NONE,
                      score->events[mapping[1]].data.note.accidental);
    TEST_ASSERT_EQUAL(MUSIC_ACCIDENTAL_SHARP,
                      score->events[mapping[2]].data.note.accidental);
    free(score);
    free(midi);
}

TEST_CASE("semantic differences prevent unison deduplication",
          "[midi_notation][mapping]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 0, 240);
    midi->notes[1] = notation_test_note(60, 0, 480);
    midi->note_count = 2;
    uint16_t mapping[2];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 2, NULL));
    TEST_ASSERT_NOT_EQUAL(mapping[0], mapping[1]);
    TEST_ASSERT_EQUAL_UINT16(2, score->event_count);
    free(score);
    free(midi);
}

TEST_CASE("zero-duration source notes are rejected",
          "[midi_notation][validation]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 0, 0);
    midi->note_count = 1;
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_INVALID_ARGUMENT,
                      midi_notation_build_score(
                          midi, &options, score, NULL, 0, NULL));
    free(score);
    free(midi);
}

TEST_CASE("unsplit cross-measure duration is split into tied segments",
          "[midi_notation][preview][tie]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 0, 2000);
    midi->note_count = 1;
    uint16_t mapping[1];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 1, NULL));
    TEST_ASSERT_EQUAL_UINT16(2, score->measure_count);
    TEST_ASSERT_EQUAL_UINT16(2, score->event_count);
    TEST_ASSERT_EQUAL_UINT16(0, mapping[0]);
    TEST_ASSERT_EQUAL_UINT16(0, score->events[0].measure_index);
    TEST_ASSERT_EQUAL_INT32(0, score->events[0].onset_divisions);
    TEST_ASSERT_EQUAL_INT32(1920,
                            score->events[0].data.note.duration_divisions);
    TEST_ASSERT_EQUAL_UINT8(MIDI_NOTE_TIE_START,
                            score->events[0].data.note.tie_flags);
    TEST_ASSERT_EQUAL_UINT16(1, score->events[1].measure_index);
    TEST_ASSERT_EQUAL_INT32(0, score->events[1].onset_divisions);
    TEST_ASSERT_EQUAL_INT32(80,
                            score->events[1].data.note.duration_divisions);
    TEST_ASSERT_EQUAL_UINT8(MIDI_NOTE_TIE_STOP,
                            score->events[1].data.note.tie_flags);
    free(score);
    free(midi);
}

TEST_CASE("barline splitting preserves incoming and outgoing ties",
          "[midi_notation][preview][tie]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 1800, 240);
    midi->notes[0].tie_flags = MIDI_NOTE_TIE_STOP | MIDI_NOTE_TIE_START;
    midi->note_count = 1;
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_UINT16(2, score->event_count);
    TEST_ASSERT_EQUAL_INT32(120,
                            score->events[0].data.note.duration_divisions);
    TEST_ASSERT_EQUAL_UINT8(MIDI_NOTE_TIE_STOP | MIDI_NOTE_TIE_START,
                            score->events[0].data.note.tie_flags);
    TEST_ASSERT_EQUAL_INT32(120,
                            score->events[1].data.note.duration_divisions);
    TEST_ASSERT_EQUAL_UINT8(MIDI_NOTE_TIE_STOP | MIDI_NOTE_TIE_START,
                            score->events[1].data.note.tie_flags);
    free(score);
    free(midi);
}

TEST_CASE("double-dotted duration keeps a matching written value",
          "[midi_notation][duration]")
{
    midi_data_t *midi = calloc(1, sizeof(*midi));
    music_score_t *score = calloc(1, sizeof(*score));
    TEST_ASSERT_NOT_NULL(midi);
    TEST_ASSERT_NOT_NULL(score);
    notation_test_midi_init(midi);
    midi->notes[0] = notation_test_note(60, 0, 840);
    midi->notes[0].dots = 2;
    midi->note_count = 1;
    uint16_t mapping[1];
    midi_notation_build_options_t options = {.key_fifths = 0};

    TEST_ASSERT_EQUAL(MIDI_NOTATION_OK, midi_notation_build_score(
        midi, &options, score, mapping, 1, NULL));
    TEST_ASSERT_EQUAL(MUSIC_DURATION_QUARTER,
                      score->events[mapping[0]].data.note.type);
    TEST_ASSERT_EQUAL_UINT8(2,
                            score->events[mapping[0]].data.note.dots);
    free(score);
    free(midi);
}
