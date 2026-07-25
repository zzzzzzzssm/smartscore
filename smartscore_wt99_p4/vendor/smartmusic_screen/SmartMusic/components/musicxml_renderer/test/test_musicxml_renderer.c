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
