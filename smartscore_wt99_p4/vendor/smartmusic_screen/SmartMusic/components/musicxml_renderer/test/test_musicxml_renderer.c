#include <stdlib.h>
#include <string.h>

#include "music_layout.h"
#include "musicxml_parser.h"
#include "smufl_glyphs.h"
#include "unity.h"

static const char c_major_scale_xml[] =
    "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\">"
    "<part-name>Scale</part-name></score-part></part-list><part id=\"P1\">"
    "<measure number=\"1\"><attributes><divisions>2</divisions>"
    "<key><fifths>0</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time>"
    "<clef><sign>G</sign><line>2</line></clef></attributes>"
    "<note><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">begin</beam></note>"
    "<note><pitch><step>D</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">continue</beam></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">continue</beam></note>"
    "<note><pitch><step>F</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">end</beam></note>"
    "<note><pitch><step>G</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">begin</beam></note>"
    "<note><pitch><step>A</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">continue</beam></note>"
    "<note><pitch><step>B</step><octave>4</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">continue</beam></note>"
    "<note><pitch><step>C</step><octave>5</octave></pitch><duration>1</duration><type>eighth</type><beam number=\"1\">end</beam></note>"
    "</measure></part></score-partwise>";

static const char grand_staff_xml[] =
    "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\">"
    "<part-name>Piano</part-name></score-part></part-list><part id=\"P1\">"
    "<measure number=\"1\"><attributes><divisions>4</divisions><staves>2</staves>"
    "<key><fifths>1</fifths></key><time symbol=\"common\"><beats>4</beats><beat-type>4</beat-type></time>"
    "<clef number=\"1\"><sign>G</sign><line>2</line></clef>"
    "<clef number=\"2\"><sign>F</sign><line>4</line></clef></attributes>"
    "<note><pitch><step>G</step><octave>4</octave></pitch><duration>6</duration><voice>1</voice><type>quarter</type><dot/><staff>1</staff><tie type=\"start\"/></note>"
    "<forward><duration>10</duration><staff>1</staff></forward>"
    "<backup><duration>16</duration></backup>"
    "<note><pitch><step>G</step><octave>2</octave></pitch><duration>16</duration><voice>2</voice><type>whole</type><staff>2</staff></note>"
    "<barline location=\"right\"><bar-style>light-heavy</bar-style></barline>"
    "</measure></part></score-partwise>";

static const char two_voice_unison_xml[] =
    "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\">"
    "<part-name>Two voices</part-name></score-part></part-list><part id=\"P1\">"
    "<measure number=\"1\"><attributes><divisions>1</divisions>"
    "<time><beats>4</beats><beat-type>4</beat-type></time>"
    "<clef><sign>G</sign><line>2</line></clef></attributes>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>1</voice><type>quarter</type></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>1</voice><type>quarter</type></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>1</voice><type>quarter</type></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>1</voice><type>quarter</type></note>"
    "<backup><duration>4</duration></backup>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>2</voice><type>quarter</type></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>2</voice><type>quarter</type></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>2</voice><type>quarter</type></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><voice>2</voice><type>quarter</type></note>"
    "</measure></part></score-partwise>";

static const char dense_chord_xml[] =
    "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\">"
    "<part-name>Chord</part-name></score-part></part-list><part id=\"P1\">"
    "<measure number=\"1\"><attributes><divisions>4</divisions>"
    "<time><beats>4</beats><beat-type>4</beat-type></time>"
    "<clef><sign>G</sign><line>2</line></clef></attributes>"
    "<note><pitch><step>C</step><alter>1</alter><octave>4</octave></pitch>"
    "<duration>3</duration><voice>1</voice><type>eighth</type><dot/>"
    "<accidental>sharp</accidental></note>"
    "<note><chord/><pitch><step>D</step><alter>-1</alter><octave>4</octave></pitch>"
    "<duration>3</duration><voice>1</voice><type>eighth</type><dot/>"
    "<accidental>flat</accidental></note>"
    "<note><chord/><pitch><step>D</step><octave>4</octave></pitch>"
    "<duration>3</duration><voice>1</voice><type>eighth</type><dot/></note>"
    "<note><chord/><pitch><step>C</step><octave>6</octave></pitch>"
    "<duration>3</duration><voice>1</voice><type>eighth</type><dot/></note>"
    "</measure></part></score-partwise>";

static const char cross_system_connections_xml[] =
    "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\">"
    "<part-name>Connections</part-name></score-part></part-list><part id=\"P1\">"
    "<measure number=\"1\"><attributes><divisions>1</divisions>"
    "<time><beats>4</beats><beat-type>4</beat-type></time>"
    "<clef><sign>G</sign><line>2</line></clef></attributes>"
    "<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration>"
    "<voice>1</voice><type>whole</type><tie type=\"start\"/><notations>"
    "<slur type=\"start\" number=\"1\"/><glissando type=\"start\" number=\"2\"/>"
    "</notations></note></measure>"
    "<measure number=\"2\"><note><pitch><step>C</step><octave>4</octave></pitch>"
    "<duration>4</duration><voice>1</voice><type>whole</type>"
    "<tie type=\"stop\"/></note></measure>"
    "<measure number=\"3\"><note><pitch><step>G</step><octave>4</octave></pitch>"
    "<duration>4</duration><voice>1</voice><type>whole</type><notations>"
    "<slur type=\"stop\" number=\"1\"/><glissando type=\"stop\" number=\"2\"/>"
    "</notations></note></measure></part></score-partwise>";

static const char beamed_chords_xml[] =
    "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\">"
    "<part-name>Beamed chords</part-name></score-part></part-list><part id=\"P1\">"
    "<measure number=\"1\"><attributes><divisions>2</divisions>"
    "<time><beats>4</beats><beat-type>4</beat-type></time>"
    "<clef><sign>G</sign><line>2</line></clef></attributes>"
    "<note><pitch><step>C</step><octave>5</octave></pitch><duration>1</duration>"
    "<voice>1</voice><type>eighth</type><beam number=\"1\">begin</beam></note>"
    "<note><chord/><pitch><step>E</step><octave>5</octave></pitch><duration>1</duration>"
    "<voice>1</voice><type>eighth</type></note>"
    "<note><pitch><step>D</step><octave>5</octave></pitch><duration>1</duration>"
    "<voice>1</voice><type>eighth</type><beam number=\"1\">end</beam></note>"
    "<note><chord/><pitch><step>F</step><octave>5</octave></pitch><duration>1</duration>"
    "<voice>1</voice><type>eighth</type></note>"
    "</measure></part></score-partwise>";

TEST_CASE("MusicXML C major scale converts to staff-space scene", "[musicxml]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);

    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(c_major_scale_xml,
                                                    strlen(c_major_scale_xml),
                                                    score, &error),
                             error.message);
    TEST_ASSERT_EQUAL_UINT16(1, score->part_count);
    TEST_ASSERT_EQUAL_UINT16(1, score->measure_count);
    TEST_ASSERT_EQUAL_UINT16(8, score->event_count);
    TEST_ASSERT_EQUAL_INT(8, score->measures[0].duration_divisions);
    TEST_ASSERT_EQUAL_INT(MUSIC_DURATION_EIGHTH, score->events[0].data.note.type);

    music_layout_config_t config;
    music_layout_default_config(&config);
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);
    TEST_ASSERT_GREATER_THAN(5, scene->item_count);
    TEST_ASSERT_EQUAL_UINT16(1, scene->system_count);
    free(scene);
    free(score);
}

TEST_CASE("MusicXML backup and grand staff preserve musical time", "[musicxml]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(grand_staff_xml,
                                                    strlen(grand_staff_xml),
                                                    score, &error),
                             error.message);
    TEST_ASSERT_EQUAL_UINT8(2, score->measures[0].staff_count);
    TEST_ASSERT_EQUAL_INT(MUSIC_CLEF_BASS, score->measures[0].clefs[1].kind);
    TEST_ASSERT_EQUAL_INT(1, score->measures[0].key.fifths);
    TEST_ASSERT_EQUAL_INT(MUSIC_TIME_COMMON, score->measures[0].time.symbol);
    TEST_ASSERT_EQUAL_UINT16(2, score->event_count);
    TEST_ASSERT_EQUAL_INT(0, score->events[0].onset_divisions);
    TEST_ASSERT_EQUAL_INT(0, score->events[1].onset_divisions);
    TEST_ASSERT_EQUAL_UINT8(2, score->events[1].data.note.staff);
    TEST_ASSERT_EQUAL_INT(16, score->measures[0].duration_divisions);

    music_layout_config_t config;
    music_layout_default_config(&config);
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);
    bool brace_found = false;
    for (int i = 0; i < scene->item_count; ++i) {
        if (scene->items[i].kind == MUSIC_SCENE_GLYPH &&
            scene->items[i].data.glyph.glyph == SMUFL_GLYPH_BRACE) {
            brace_found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(brace_found);
    free(scene);
    free(score);
}

TEST_CASE("two voices use opposing stems and resolve unison collisions", "[musicxml]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);

    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(two_voice_unison_xml,
                                                    strlen(two_voice_unison_xml),
                                                    score, &error),
                             error.message);
    TEST_ASSERT_EQUAL_UINT16(8, score->event_count);
    TEST_ASSERT_EQUAL_INT(0, score->events[0].onset_divisions);
    TEST_ASSERT_EQUAL_INT(0, score->events[4].onset_divisions);

    music_layout_config_t config;
    music_layout_default_config(&config);
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);

    bool upper_stem_found = false;
    bool lower_stem_found = false;
    bool upper_head_found = false;
    bool lower_head_found = false;
    music_sp_t upper_head_x = 0.0f;
    music_sp_t lower_head_x = 0.0f;
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        if (item->kind == MUSIC_SCENE_LINE && item->source_event_index == 0 &&
            item->data.line.y2 < item->data.line.y1)
            upper_stem_found = true;
        if (item->kind == MUSIC_SCENE_LINE && item->source_event_index == 4 &&
            item->data.line.y2 > item->data.line.y1)
            lower_stem_found = true;
        if (item->kind == MUSIC_SCENE_GLYPH && item->source_event_index == 0 &&
            item->data.glyph.glyph == SMUFL_GLYPH_NOTEHEAD_BLACK) {
            upper_head_x = item->data.glyph.x;
            upper_head_found = true;
        }
        if (item->kind == MUSIC_SCENE_GLYPH && item->source_event_index == 4 &&
            item->data.glyph.glyph == SMUFL_GLYPH_NOTEHEAD_BLACK) {
            lower_head_x = item->data.glyph.x;
            lower_head_found = true;
        }
    }
    TEST_ASSERT_TRUE(upper_stem_found);
    TEST_ASSERT_TRUE(lower_stem_found);
    TEST_ASSERT_TRUE(upper_head_found);
    TEST_ASSERT_TRUE(lower_head_found);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.5f, upper_head_x - lower_head_x);

    size_t recolored = music_scene_set_event_color(scene, 0, 0xDC2626);
    TEST_ASSERT_GREATER_THAN(0, recolored);
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        if (item->source_event_index == 0)
            TEST_ASSERT_EQUAL_HEX32(0xDC2626, item->color_rgb);
        else if (item->source_event_index < 0)
            TEST_ASSERT_EQUAL_HEX32(MUSIC_SCENE_COLOR_DEFAULT, item->color_rgb);
    }
    TEST_ASSERT_EQUAL(recolored, music_scene_clear_event_color(scene, 0));

    free(scene);
    free(score);
}

TEST_CASE("same-voice seconds and wide dotted chord share one enclosing stem",
          "[musicxml][chord]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);

    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(dense_chord_xml,
                                                    strlen(dense_chord_xml),
                                                    score, &error),
                             error.message);
    TEST_ASSERT_EQUAL_UINT16(4, score->event_count);
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_INT(0, score->events[i].onset_divisions);

    music_layout_config_t config;
    music_layout_default_config(&config);
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);

    int long_vertical_stems = 0;
    int flags = 0;
    music_sp_t head_x[4] = {0};
    music_sp_t dot_x[4] = {0};
    bool head_found[4] = {false};
    bool dot_found[4] = {false};
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        int source = item->source_event_index;
        if (item->kind == MUSIC_SCENE_LINE && source >= 0 &&
            item->data.line.x1 == item->data.line.x2) {
            music_sp_t length = item->data.line.y2 - item->data.line.y1;
            if (length < 0.0f) length = -length;
            if (length >= 3.5f) long_vertical_stems++;
        }
        if (source < 0 || source >= 4 || item->kind != MUSIC_SCENE_GLYPH)
            continue;
        if (item->data.glyph.glyph ==
                smufl_flag_glyph(MUSIC_DURATION_EIGHTH, MUSIC_STEM_UP) ||
            item->data.glyph.glyph ==
                smufl_flag_glyph(MUSIC_DURATION_EIGHTH, MUSIC_STEM_DOWN))
            flags++;
        if (item->data.glyph.glyph == SMUFL_GLYPH_NOTEHEAD_BLACK) {
            head_x[source] = item->data.glyph.x;
            head_found[source] = true;
        } else if (item->data.glyph.glyph == SMUFL_GLYPH_AUGMENTATION_DOT) {
            dot_x[source] = item->data.glyph.x;
            dot_found[source] = true;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, long_vertical_stems);
    TEST_ASSERT_EQUAL_INT(1, flags);
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(head_found[i]);
        TEST_ASSERT_TRUE(dot_found[i]);
        TEST_ASSERT_GREATER_THAN_FLOAT(head_x[i], dot_x[i]);
    }
    TEST_ASSERT_TRUE(head_x[0] != head_x[1]);
    TEST_ASSERT_TRUE(head_x[1] != head_x[2]);

    free(scene);
    free(score);
}

TEST_CASE("tie slur and gliss split cleanly across systems",
          "[musicxml][connections]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(
        cross_system_connections_xml, strlen(cross_system_connections_xml),
        score, &error), error.message);
    TEST_ASSERT_EQUAL_UINT8(1, score->events[0].data.note.slur_start);
    TEST_ASSERT_EQUAL_UINT8(2, score->events[0].data.note.gliss_start);
    TEST_ASSERT_EQUAL_UINT8(2, score->events[2].data.note.gliss_stop);

    music_layout_config_t config;
    music_layout_default_config(&config);
    config.max_measures_per_system = 1;
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);
    TEST_ASSERT_EQUAL_UINT16(3, scene->system_count);
    int curved_segments = 0;
    int gliss_segments = 0;
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        if (item->source_event_index != 0) continue;
        if (item->kind == MUSIC_SCENE_BEZIER) curved_segments++;
        if (item->kind == MUSIC_SCENE_LINE &&
            item->data.line.thickness == 0.11f)
            gliss_segments++;
    }
    TEST_ASSERT_TRUE(curved_segments >= 4);
    TEST_ASSERT_TRUE(gliss_segments >= 3);

    free(scene);
    free(score);
}

TEST_CASE("beamed chords use one enclosing stem per onset",
          "[musicxml][chord][beam]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(
        beamed_chords_xml, strlen(beamed_chords_xml), score, &error),
        error.message);
    music_layout_config_t config;
    music_layout_default_config(&config);
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);
    int stems = 0;
    int beams = 0;
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        if (item->kind != MUSIC_SCENE_LINE || item->source_event_index < 0)
            continue;
        music_sp_t dx = item->data.line.x2 - item->data.line.x1;
        music_sp_t dy = item->data.line.y2 - item->data.line.y1;
        if (dx == 0.0f && (dy >= 3.5f || dy <= -3.5f)) stems++;
        if ((dx >= 1.0f || dx <= -1.0f) &&
            item->data.line.thickness == config.beam_thickness)
            beams++;
    }
    TEST_ASSERT_EQUAL_INT(2, stems);
    TEST_ASSERT_EQUAL_INT(1, beams);
    free(scene);
    free(score);
}

TEST_CASE("dirty layout preserves systems before the changed tail",
          "[musicxml][incremental]")
{
    music_score_t *score = calloc(1, sizeof(*score));
    music_scene_t *scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(score);
    TEST_ASSERT_NOT_NULL(scene);
    musicxml_error_t error;
    TEST_ASSERT_TRUE_MESSAGE(musicxml_parse_buffer(
        cross_system_connections_xml, strlen(cross_system_connections_xml),
        score, &error), error.message);

    /* Remove connectors so this test isolates the dirty-system path. */
    score->events[0].data.note.tie_flags = 0;
    score->events[0].data.note.slur_start = 0;
    score->events[0].data.note.gliss_start = 0;
    score->events[1].data.note.tie_flags = 0;
    score->events[2].data.note.slur_stop = 0;
    score->events[2].data.note.gliss_stop = 0;

    music_layout_config_t config;
    music_layout_default_config(&config);
    config.max_measures_per_system = 1;
    char layout_error[96];
    TEST_ASSERT_TRUE_MESSAGE(music_layout_build(score, &config, scene,
                                                 layout_error,
                                                 sizeof(layout_error)),
                             layout_error);
    TEST_ASSERT_EQUAL_UINT16(3, scene->system_count);
    music_scene_item_t preserved = scene->items[0];
    uint16_t preserved_count = scene->systems[0].item_count;
    music_sp_t rebuilt_boundary_top = scene->systems[1].top;

    score->events[2].data.note.pitch.step = 5;
    TEST_ASSERT_TRUE_MESSAGE(music_layout_rebuild_from_measure(
        score, &config, scene, 2, layout_error, sizeof(layout_error)),
        layout_error);
    TEST_ASSERT_EQUAL_UINT16(3, scene->system_count);
    TEST_ASSERT_EQUAL_UINT16(preserved_count, scene->systems[0].item_count);
    TEST_ASSERT_EQUAL_MEMORY(&preserved, &scene->items[0], sizeof(preserved));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, rebuilt_boundary_top,
                             scene->systems[1].top);

    free(scene);
    free(score);
}

TEST_CASE("SMuFL resolver uses canonical Leland mappings", "[smufl]")
{
    TEST_ASSERT_EQUAL_HEX32(0xE000, smufl_glyph_info(SMUFL_GLYPH_BRACE)->codepoint);
    TEST_ASSERT_EQUAL_HEX32(0xE050, smufl_glyph_info(SMUFL_GLYPH_G_CLEF)->codepoint);
    TEST_ASSERT_EQUAL_HEX32(0xE062, smufl_glyph_info(SMUFL_GLYPH_F_CLEF)->codepoint);
    TEST_ASSERT_EQUAL_HEX32(0xE1E7, smufl_glyph_info(SMUFL_GLYPH_AUGMENTATION_DOT)->codepoint);
    TEST_ASSERT_EQUAL_HEX32(0xE4E3, smufl_glyph_info(SMUFL_GLYPH_REST_WHOLE)->codepoint);
    TEST_ASSERT_EQUAL_HEX32(0xE4E5, smufl_glyph_info(SMUFL_GLYPH_REST_QUARTER)->codepoint);
    TEST_ASSERT_EQUAL_HEX32(0xE263, smufl_glyph_info(SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP)->codepoint);
}
