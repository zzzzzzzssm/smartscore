#include <stdlib.h>

#include "music_layout.h"
#include "smufl_glyphs.h"
#include "unity.h"

/* Focused acceptance cases for rhythmic columns and dense-system breaking. */

static void init_score(music_score_t *score, int measures, int events)
{
    music_score_init(score);
    score->part_count = 1;
    score->measure_count = (uint16_t)measures;
    score->event_count = (uint16_t)events;
    score->parts[0].measure_start = 0;
    score->parts[0].measure_count = (uint16_t)measures;
    score->parts[0].staff_count = 1;
    for (int mi = 0; mi < measures; ++mi) {
        music_measure_t *measure = &score->measures[mi];
        measure->part_index = 0;
        measure->divisions = 4;
        measure->duration_divisions = 16;
        measure->staff_count = 1;
        measure->clefs[0] = (music_clef_t){
            .kind = MUSIC_CLEF_TREBLE,
            .line = 2,
        };
        measure->time = (music_time_signature_t){
            .beats = 4,
            .beat_type = 4,
        };
        measure->right_barline = MUSIC_BARLINE_SINGLE;
    }
}

static void set_note(music_event_t *event, int measure, int onset,
                     int step, int octave, int voice, bool chord,
                     music_accidental_t accidental, uint8_t dots)
{
    *event = (music_event_t){
        .kind = MUSIC_EVENT_NOTE,
        .part_index = 0,
        .measure_index = (uint16_t)measure,
        .onset_divisions = onset,
        .data.note = {
            .pitch = {
                .step = (uint8_t)step,
                .alter = accidental == MUSIC_ACCIDENTAL_SHARP ? 1 : 0,
                .octave = (int8_t)octave,
            },
            .type = MUSIC_DURATION_QUARTER,
            .accidental = accidental,
            .duration_divisions = 4,
            .dots = dots,
            .voice = (uint8_t)voice,
            .staff = 1,
            .chord = chord,
        },
    };
}

TEST_CASE("visible accidentals share onset columns across voices",
          "[music-layout][columns][accidentals]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    init_score(score, 1, 6);
    score->measures[0].event_start = 0;
    score->measures[0].event_count = 6;

    /* Two complete voices at one onset.  Input ordering must not allow the
     * second voice's accidentals to overlap the first voice. */
    set_note(&score->events[0], 0, 0, 0, 4, 1, false,
             MUSIC_ACCIDENTAL_SHARP, 1);
    set_note(&score->events[1], 0, 0, 1, 4, 1, true,
             MUSIC_ACCIDENTAL_SHARP, 1);
    set_note(&score->events[2], 0, 0, 2, 4, 1, true,
             MUSIC_ACCIDENTAL_NATURAL, 1);
    set_note(&score->events[3], 0, 0, 1, 4, 2, false,
             MUSIC_ACCIDENTAL_FLAT, 0);
    set_note(&score->events[4], 0, 0, 2, 4, 2, true,
             MUSIC_ACCIDENTAL_SHARP, 0);
    set_note(&score->events[5], 0, 0, 3, 4, 2, true,
             MUSIC_ACCIDENTAL_NATURAL, 0);

    music_layout_config_t config;
    music_layout_default_config(&config);
    char error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(
        score, &config, scene, error, sizeof(error)), error);

    music_sp_t accidental_x[6] = {0};
    bool found[6] = {false};
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        if (item->kind != MUSIC_SCENE_GLYPH ||
            item->source_event_index < 0 ||
            item->source_event_index >= 6)
            continue;
        smufl_glyph_id_t glyph = item->data.glyph.glyph;
        if (glyph < SMUFL_GLYPH_ACCIDENTAL_FLAT ||
            glyph > SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT)
            continue;
        accidental_x[item->source_event_index] = item->data.glyph.x;
        found[item->source_event_index] = true;
    }
    for (int i = 0; i < 6; ++i) TEST_ASSERT_TRUE(found[i]);
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            int distance = music_pitch_diatonic_index(
                &score->events[i].data.note.pitch) -
                music_pitch_diatonic_index(&score->events[j].data.note.pitch);
            if (distance < 0) distance = -distance;
            if (distance <= 2)
                TEST_ASSERT_TRUE(accidental_x[i] != accidental_x[j]);
        }
    }
    free(scene);
    free(score);
}

TEST_CASE("hard measure widths give dense measures their own systems",
          "[music-layout][columns][line-breaking]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    init_score(score, 2, 12);

    for (int mi = 0; mi < 2; ++mi) {
        music_measure_t *measure = &score->measures[mi];
        measure->event_start = (uint16_t)(mi * 6);
        measure->event_count = 6;
        for (int note = 0; note < 6; ++note) {
            int event_index = mi * 6 + note;
            set_note(&score->events[event_index], mi, 0, note % 7,
                     4 + note / 7, 1, note > 0,
                     MUSIC_ACCIDENTAL_SHARP, 1);
        }
    }

    music_layout_config_t config;
    music_layout_default_config(&config);
    config.page_width = 24.0f;
    char error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(
        score, &config, scene, error, sizeof(error)), error);
    TEST_ASSERT_EQUAL_UINT16(2, scene->system_count);
    TEST_ASSERT_EQUAL_UINT16(0, scene->systems[0].measure_start);
    TEST_ASSERT_EQUAL_UINT16(1, scene->systems[0].measure_end);
    TEST_ASSERT_EQUAL_UINT16(1, scene->systems[1].measure_start);
    TEST_ASSERT_EQUAL_UINT16(2, scene->systems[1].measure_end);

    free(scene);
    free(score);
}

TEST_CASE("paginated layout moves complete systems across page boundaries",
          "[music-layout][pagination]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    init_score(score, 3, 3);
    for (int mi = 0; mi < 3; ++mi) {
        score->measures[mi].event_start = (uint16_t)mi;
        score->measures[mi].event_count = 1;
        set_note(&score->events[mi], mi, 0, 0, 4, 1, false,
                 MUSIC_ACCIDENTAL_NONE, 0);
    }

    music_layout_config_t config;
    music_layout_default_config(&config);
    config.max_measures_per_system = 1;
    config.system_gap = 3.0f;
    config.page_height = 15.0f;
    config.page_top_padding = 0.5f;
    config.page_bottom_padding = 0.5f;
    char error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(
        score, &config, scene, error, sizeof(error)), error);
    TEST_ASSERT_EQUAL_UINT16(3, scene->system_count);

    for (int i = 0; i < scene->system_count; ++i) {
        const music_scene_system_t *system = &scene->systems[i];
        const int page = (int)(system->top / config.page_height);
        const music_sp_t page_bottom =
            (page + 1) * config.page_height - config.page_bottom_padding;
        TEST_ASSERT_TRUE(system->bottom <= page_bottom + 0.001f);
    }

    int group_start = 0;
    while (group_start < scene->system_count) {
        const int page = (int)(scene->systems[group_start].ink_top /
                               config.page_height);
        music_sp_t group_top = scene->systems[group_start].ink_top;
        music_sp_t group_bottom = scene->systems[group_start].ink_bottom;
        int group_end = group_start + 1;
        while (group_end < scene->system_count &&
               (int)(scene->systems[group_end].ink_top /
                     config.page_height) == page) {
            if (scene->systems[group_end].ink_bottom > group_bottom)
                group_bottom = scene->systems[group_end].ink_bottom;
            group_end++;
        }
        const music_sp_t group_center = (group_top + group_bottom) * 0.5f;
        const music_sp_t page_center =
            page * config.page_height + config.page_height * 0.5f;
        TEST_ASSERT_FLOAT_WITHIN(0.001f, page_center, group_center);
        group_start = group_end;
    }

    free(scene);
    free(score);
}
