#include "leland_score_view.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "music_layout.h"
#include "smufl_glyphs.h"

LV_FONT_DECLARE(lv_font_LelandSMuFL_72);
LV_FONT_DECLARE(lv_font_LelandSMuFL_64);
LV_FONT_DECLARE(lv_font_LelandBrace_252);

#define VIEW_STAFF_SPACE_PX 18.0f
#define VIEW_MIN_ORIGIN_Y_PX 14
#define VIEW_AUTO_BEAM_MAX_RUN 32
#define VIEW_CURVE_SEGMENTS 20
#define VIEW_MAX_NOTES      MAX_NOTES
#define VIEW_GUIDE_HEIGHT_PX 128
#define VIEW_GUIDE_MIN_WIDTH_PX 48
#define VIEW_GUIDE_X_PADDING_PX 8
#define VIEW_GUIDE_MAX_SHORT_LINE_PX 54

struct leland_score_view {
    lv_obj_t *parent;
    lv_obj_t *canvas;
    music_score_t *score;
    music_scene_t *scene;
    lv_obj_t **item_objects;
    int16_t note_event_indices[VIEW_MAX_NOTES];
    int note_count;
    int viewport_width;
    int viewport_height;
    int origin_y_px;
    int page_count;
    int current_page;
    int guide_first_note;
    int guide_last_note;
};

static int sp_to_px(music_sp_t value)
{
    return (int)lroundf(value * VIEW_STAFF_SPACE_PX);
}

static lv_color_t item_color(const music_scene_item_t *item)
{
    uint32_t rgb = item && item->color_rgb != MUSIC_SCENE_COLOR_DEFAULT ?
                   item->color_rgb : UINT32_C(0x171717);
    return lv_color_hex(rgb);
}
static const lv_font_t *scene_glyph_font(smufl_glyph_id_t glyph)
{
    bool time_glyph = glyph >= SMUFL_GLYPH_TIME_0 &&
                      glyph <= SMUFL_GLYPH_TIME_CUT;
    if (glyph == SMUFL_GLYPH_BRACE)
        return &lv_font_LelandBrace_252;
    if (time_glyph)
        return &lv_font_LelandSMuFL_64;
    return &lv_font_LelandSMuFL_72;
}


static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                      int width, lv_color_t color)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width > 0 ? width : 1;
    dsc.opa = LV_OPA_COVER;
    dsc.p1 = (lv_point_precise_t){.x = x1, .y = y1};
    dsc.p2 = (lv_point_precise_t){.x = x2, .y = y2};
    lv_draw_line(layer, &dsc);
}

static bool event_is_guided(const leland_score_view_t *view, int event_index)
{
    if (!view || event_index < 0 || view->guide_first_note < 0 ||
        view->guide_last_note < view->guide_first_note) {
        return false;
    }
    int last = view->guide_last_note;
    if (last >= view->note_count) last = view->note_count - 1;
    for (int note = view->guide_first_note; note <= last; ++note) {
        if (view->note_event_indices[note] == event_index) return true;
    }
    return false;
}

static void extend_bounds(int x1, int y1, int x2, int y2,
                          int *minimum_x, int *minimum_y,
                          int *maximum_x, int *maximum_y)
{
    if (x2 < x1) {
        const int swap = x1;
        x1 = x2;
        x2 = swap;
    }
    if (y2 < y1) {
        const int swap = y1;
        y1 = y2;
        y2 = swap;
    }
    if (x1 < *minimum_x) *minimum_x = x1;
    if (y1 < *minimum_y) *minimum_y = y1;
    if (x2 > *maximum_x) *maximum_x = x2;
    if (y2 > *maximum_y) *maximum_y = y2;
}

static bool guided_event_area(const leland_score_view_t *view,
                              int event_index,
                              int canvas_x, int canvas_y,
                              lv_area_t *area)
{
    if (!view || !view->scene || !area || event_index < 0) return false;
    int minimum_x = INT_MAX;
    int minimum_y = INT_MAX;
    int maximum_x = INT_MIN;
    int maximum_y = INT_MIN;

    for (int index = 0; index < view->scene->item_count; ++index) {
        const music_scene_item_t *item = &view->scene->items[index];
        if (item->source_event_index != event_index) continue;

        if (item->kind == MUSIC_SCENE_LINE) {
            const int x1 = canvas_x + sp_to_px(item->data.line.x1);
            const int x2 = canvas_x + sp_to_px(item->data.line.x2);
            /* Long horizontal/diagonal lines are beams shared with later
             * notes. Keep stems and ledger lines, but do not stretch one
             * note's guide across an entire beamed phrase. */
            if (abs(x2 - x1) > VIEW_GUIDE_MAX_SHORT_LINE_PX) continue;
            const int y1 = canvas_y + view->origin_y_px +
                           sp_to_px(item->data.line.y1);
            const int y2 = canvas_y + view->origin_y_px +
                           sp_to_px(item->data.line.y2);
            const int half_width =
                (sp_to_px(item->data.line.thickness) + 1) / 2;
            extend_bounds(x1 - half_width, y1 - half_width,
                          x2 + half_width, y2 + half_width,
                          &minimum_x, &minimum_y, &maximum_x, &maximum_y);
        } else if (item->kind == MUSIC_SCENE_GLYPH) {
            const smufl_glyph_info_t *glyph =
                smufl_glyph_info(item->data.glyph.glyph);
            if (!glyph) continue;
            const lv_font_t *font =
                scene_glyph_font(item->data.glyph.glyph);
            lv_font_glyph_dsc_t glyph_dsc;
            if (!lv_font_get_glyph_dsc(font, &glyph_dsc,
                                       glyph->codepoint, 0)) {
                continue;
            }
            const int font_origin_y = font->line_height - font->base_line;
            const int point_x = canvas_x + sp_to_px(item->data.glyph.x);
            const int point_y = canvas_y + view->origin_y_px +
                                sp_to_px(item->data.glyph.y) - font_origin_y;
            const lv_font_t *resolved = glyph_dsc.resolved_font
                                            ? glyph_dsc.resolved_font
                                            : font;
            const int glyph_x1 = point_x + glyph_dsc.ofs_x;
            const int glyph_y1 =
                point_y + (resolved->line_height - resolved->base_line) -
                glyph_dsc.box_h - glyph_dsc.ofs_y;
            extend_bounds(glyph_x1, glyph_y1,
                          glyph_x1 + glyph_dsc.box_w - 1,
                          glyph_y1 + glyph_dsc.box_h - 1,
                          &minimum_x, &minimum_y, &maximum_x, &maximum_y);
        }
    }
    if (minimum_x == INT_MAX || minimum_y == INT_MAX) return false;

    int left = minimum_x - VIEW_GUIDE_X_PADDING_PX;
    int right = maximum_x + VIEW_GUIDE_X_PADDING_PX;
    if (right - left + 1 < VIEW_GUIDE_MIN_WIDTH_PX) {
        const int center_x = (minimum_x + maximum_x) / 2;
        left = center_x - VIEW_GUIDE_MIN_WIDTH_PX / 2;
        right = left + VIEW_GUIDE_MIN_WIDTH_PX - 1;
    }
    const int center_y = (minimum_y + maximum_y) / 2;
    area->x1 = left;
    area->x2 = right;
    area->y1 = center_y - VIEW_GUIDE_HEIGHT_PX / 2;
    area->y2 = area->y1 + VIEW_GUIDE_HEIGHT_PX - 1;
    return true;
}

static void draw_note_guides(lv_layer_t *layer,
                             const leland_score_view_t *view,
                             int canvas_x, int canvas_y)
{
    if (!view || view->guide_first_note < 0 ||
        view->guide_last_note < view->guide_first_note) {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(0xBAE6FD);
    dsc.bg_opa = LV_OPA_60;
    dsc.border_color = lv_color_hex(0x0284C7);
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_width = 2;
    dsc.radius = 9;

    int last = view->guide_last_note;
    if (last >= view->note_count) last = view->note_count - 1;
    for (int note = view->guide_first_note; note <= last; ++note) {
        const int event_index = view->note_event_indices[note];
        bool already_drawn = false;
        for (int previous = view->guide_first_note;
             previous < note; ++previous) {
            if (view->note_event_indices[previous] == event_index) {
                already_drawn = true;
                break;
            }
        }
        if (already_drawn || !event_is_guided(view, event_index)) continue;
        lv_area_t area;
        if (guided_event_area(view, event_index, canvas_x, canvas_y, &area)) {
            lv_draw_rect(layer, &dsc, &area);
        }
    }
}

static float cubic(float p0, float p1, float p2, float p3, float t)
{
    float u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * p1 +
           3.0f * u * t * t * p2 + t * t * t * p3;
}

static void draw_scene_cb(lv_event_t *event)
{
    leland_score_view_t *view = lv_event_get_user_data(event);
    if (!view || !view->scene) return;
    lv_layer_t *layer = lv_event_get_layer(event);

    /* LVGL draw descriptors use display-absolute coordinates.  Scene
     * coordinates, however, are local to our canvas.  Glyph labels are
     * canvas children and LVGL translates them automatically, so failing to
     * translate custom lines made staff lines/stems lag behind the glyphs by
     * the music content container's (9, 74) screen offset. */
    lv_area_t canvas_area;
    lv_obj_get_coords(view->canvas, &canvas_area);
    const int canvas_x = canvas_area.x1;
    const int canvas_y = canvas_area.y1;

    /* Guides are rendered first so staff lines and the complete note symbol
     * remain crisp above the translucent block. */
    draw_note_guides(layer, view, canvas_x, canvas_y);

    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (item->kind == MUSIC_SCENE_LINE) {
            draw_line(layer, canvas_x + sp_to_px(item->data.line.x1),
                      canvas_y + view->origin_y_px + sp_to_px(item->data.line.y1),
                      canvas_x + sp_to_px(item->data.line.x2),
                      canvas_y + view->origin_y_px + sp_to_px(item->data.line.y2),
                      sp_to_px(item->data.line.thickness), item_color(item));
        } else if (item->kind == MUSIC_SCENE_BEZIER) {
            const typeof(item->data.bezier) *b = &item->data.bezier;
            float previous_x = b->x1;
            float previous_y = b->y1;
            for (int segment = 1; segment <= VIEW_CURVE_SEGMENTS; ++segment) {
                float t = (float)segment / VIEW_CURVE_SEGMENTS;
                float x = cubic(b->x1, b->cx1, b->cx2, b->x2, t);
                float y = cubic(b->y1, b->cy1, b->cy2, b->y2, t);
                draw_line(layer, canvas_x + sp_to_px(previous_x),
                          canvas_y + view->origin_y_px + sp_to_px(previous_y),
                          canvas_x + sp_to_px(x),
                          canvas_y + view->origin_y_px + sp_to_px(y),
                          sp_to_px(b->thickness), item_color(item));
                previous_x = x;
                previous_y = y;
            }
        } else if (item->kind == MUSIC_SCENE_GLYPH) {
            const smufl_glyph_info_t *glyph =
                smufl_glyph_info(item->data.glyph.glyph);
            if (!glyph) continue;
            const lv_font_t *font =
                scene_glyph_font(item->data.glyph.glyph);
            const int font_origin_y = font->line_height - font->base_line;
            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            dsc.font = font;
            dsc.color = item_color(item);
            dsc.opa = LV_OPA_COVER;
            lv_point_t point = {
                .x = canvas_x + sp_to_px(item->data.glyph.x),
                .y = canvas_y + view->origin_y_px +
                     sp_to_px(item->data.glyph.y) - font_origin_y,
            };
            lv_draw_character(layer, &dsc, &point, glyph->codepoint);
        }
    }
}

static void delete_glyphs(leland_score_view_t *view)
{
    if (!view || !view->scene || !view->item_objects) return;
    for (int i = 0; i < view->scene->item_count; ++i) {
        lv_obj_t *object = view->item_objects[i];
        if (object && lv_obj_is_valid(object)) lv_obj_delete(object);
    }
    free(view->item_objects);
    view->item_objects = NULL;
}

static bool allocate_object_map(leland_score_view_t *view)
{
    if (!view->scene->item_count) return true;
    view->item_objects = heap_caps_calloc(view->scene->item_count,
                                          sizeof(*view->item_objects),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view->item_objects)
        view->item_objects = calloc(view->scene->item_count,
                                    sizeof(*view->item_objects));
    return view->item_objects != NULL;
}

static void create_glyphs(leland_score_view_t *view)
{
    /* Glyphs are rendered by draw_scene_cb in the canvas coordinate space. */
    (void)view;
    return;
    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (item->kind != MUSIC_SCENE_GLYPH) continue;
        const smufl_glyph_info_t *glyph =
            smufl_glyph_info(item->data.glyph.glyph);
        char utf8[5];
        if (!glyph || !smufl_codepoint_to_utf8(glyph->codepoint, utf8))
            continue;

        bool time_glyph = item->data.glyph.glyph >= SMUFL_GLYPH_TIME_0 &&
                          item->data.glyph.glyph <= SMUFL_GLYPH_TIME_CUT;
        const lv_font_t *font = item->data.glyph.glyph == SMUFL_GLYPH_BRACE ?
                                &lv_font_LelandBrace_252 :
                                time_glyph ? &lv_font_LelandSMuFL_64 :
                                             &lv_font_LelandSMuFL_72;
        int font_origin_y = font->line_height - font->base_line;
        lv_obj_t *label = lv_label_create(view->canvas);
        lv_obj_remove_style_all(label);
        lv_obj_set_size(label, 110, font->line_height + 18);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, item_color(item), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label, utf8);
        lv_obj_set_pos(label, sp_to_px(item->data.glyph.x),
                       view->origin_y_px + sp_to_px(item->data.glyph.y) -
                       font_origin_y);
        view->item_objects[i] = label;
    }
}

static music_duration_kind_t duration_kind(uint32_t ticks, int tpq,
                                           uint8_t *dots)
{
    static const music_duration_kind_t kinds[] = {
        MUSIC_DURATION_WHOLE, MUSIC_DURATION_HALF, MUSIC_DURATION_QUARTER,
        MUSIC_DURATION_EIGHTH, MUSIC_DURATION_16TH, MUSIC_DURATION_32ND
    };
    static const float beats[] = {4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f};
    float value = tpq > 0 ? (float)ticks / tpq : 1.0f;
    float best_error = 1000000.0f;
    music_duration_kind_t best = MUSIC_DURATION_QUARTER;
    uint8_t best_dots = 0;
    for (int i = 0; i < 6; ++i) {
        for (int dotted = 0; dotted <= 1; ++dotted) {
            float candidate = beats[i] * (dotted ? 1.5f : 1.0f);
            float error = fabsf(value - candidate);
            if (error < best_error) {
                best_error = error;
                best = kinds[i];
                best_dots = (uint8_t)dotted;
            }
        }
    }
    if (dots) *dots = best_dots;
    return best;
}

static void key_alterations(int fifths, int8_t alter[7])
{
    static const uint8_t sharp_order[7] = {3, 0, 4, 1, 5, 2, 6};
    static const uint8_t flat_order[7] = {6, 2, 5, 1, 4, 0, 3};
    memset(alter, 0, 7);
    int count = fifths < 0 ? -fifths : fifths;
    if (count > 7) count = 7;
    for (int i = 0; i < count; ++i)
        alter[fifths < 0 ? flat_order[i] : sharp_order[i]] =
            fifths < 0 ? -1 : 1;
}

static void midi_to_pitch(uint8_t midi_note, int fifths, music_note_t *note)
{
    static const uint8_t sharp_steps[12] =
        {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static const int8_t sharp_alters[12] =
        {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    static const uint8_t flat_steps[12] =
        {0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6};
    static const int8_t flat_alters[12] =
        {0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0};
    uint8_t pc = midi_note % 12;
    note->pitch.step = fifths < 0 ? flat_steps[pc] : sharp_steps[pc];
    note->pitch.alter = fifths < 0 ? flat_alters[pc] : sharp_alters[pc];
    note->pitch.octave = (int8_t)(midi_note / 12) - 1;

    int8_t key_alter[7];
    key_alterations(fifths, key_alter);
    int8_t expected = key_alter[note->pitch.step];
    if (note->pitch.alter == expected) {
        note->accidental = MUSIC_ACCIDENTAL_NONE;
    } else if (note->pitch.alter > 0) {
        note->accidental = MUSIC_ACCIDENTAL_SHARP;
    } else if (note->pitch.alter < 0) {
        note->accidental = MUSIC_ACCIDENTAL_FLAT;
    } else {
        note->accidental = MUSIC_ACCIDENTAL_NATURAL;
    }
}

static uint8_t score_staff_count(const midi_data_t *midi)
{
    /* Staff membership is notation metadata, not a pitch threshold.  A low
     * melody note can still belong to the treble staff, so only an explicit
     * lower-staff assignment creates a piano grand staff. */
    for (int i = 0; i < midi->note_count; ++i) {
        if (midi->notes[i].staff == 2) return 2;
    }
    return 1;
}
static uint8_t note_beam_level(const music_note_t *note)
{
    if (!note || note->chord) return 0;
    switch (note->type) {
    case MUSIC_DURATION_EIGHTH: return 1;
    case MUSIC_DURATION_16TH:   return 2;
    case MUSIC_DURATION_32ND:   return 3;
    default:                    return 0;
    }
}

static void mark_beam_run(music_score_t *score, const int *indices,
                          int count, uint8_t levels)
{
    if (!score || !indices || count < 2 || !levels) return;
    if (levels > MUSIC_MAX_BEAM_LEVELS) levels = MUSIC_MAX_BEAM_LEVELS;
    for (uint8_t level = 0; level < levels; ++level) {
        for (int i = 0; i < count; ++i) {
            music_note_t *note = &score->events[indices[i]].data.note;
            note->beams[level] = i == 0 ? MUSIC_BEAM_BEGIN :
                                 i == count - 1 ? MUSIC_BEAM_END :
                                                  MUSIC_BEAM_CONTINUE;
        }
    }
}

static void auto_beam_measure(music_score_t *score, music_measure_t *measure,
                              int ticks_per_quarter, int numerator,
                              int denominator, uint8_t staff_count)
{
    if (!score || !measure || ticks_per_quarter <= 0) return;

    int beat_ticks = ticks_per_quarter * 4 / denominator;
    if (denominator == 8 && numerator > 3 && numerator % 3 == 0)
        beat_ticks = ticks_per_quarter * 3 / 2;
    if (beat_ticks <= 0) beat_ticks = ticks_per_quarter;

    for (uint8_t staff = 1; staff <= staff_count; ++staff) {
        for (uint8_t voice = 1; voice <= 16; ++voice) {
            int run[VIEW_AUTO_BEAM_MAX_RUN];
            int run_count = 0;
            int run_beat = -1;
            int32_t previous_end = -1;
            uint8_t run_levels = 0;

            for (int i = 0; i < measure->event_count; ++i) {
                int event_index = measure->event_start + i;
                music_event_t *event = &score->events[event_index];
                if (event->kind != MUSIC_EVENT_NOTE) continue;
                music_note_t *note = &event->data.note;
                uint8_t note_staff = note->staff ? note->staff : 1;
                uint8_t note_voice = note->voice ? note->voice : 1;
                if (note_staff != staff || note_voice != voice ||
                    note->chord)
                    continue;

                uint8_t levels = note_beam_level(note);
                int beat = event->onset_divisions / beat_ticks;
                bool continues = levels && run_count > 0 &&
                                 beat == run_beat &&
                                 levels == run_levels &&
                                 event->onset_divisions == previous_end;
                if (!continues && run_count > 0) {
                    mark_beam_run(score, run, run_count, run_levels);
                    run_count = 0;
                }
                if (!levels) {
                    run_beat = -1;
                    previous_end = -1;
                    run_levels = 0;
                    continue;
                }

                if (run_count == VIEW_AUTO_BEAM_MAX_RUN) {
                    mark_beam_run(score, run, run_count, run_levels);
                    run_count = 0;
                }
                if (run_count == 0) {
                    run_beat = beat;
                    run_levels = levels;
                }
                run[run_count++] = event_index;
                previous_end = event->onset_divisions +
                               note->duration_divisions;
            }
            mark_beam_run(score, run, run_count, run_levels);
        }
    }
}


static bool build_score(leland_score_view_t *view, const midi_data_t *midi,
                        char *error, size_t error_size)
{
    if (!midi || midi->note_count <= 0 || midi->ticks_per_quarter <= 0) {
        if (error && error_size)
            snprintf(error, error_size, "score has no playable notes");
        return false;
    }
    music_score_t *score = view->score;
    music_score_init(score);
    snprintf(score->title, sizeof(score->title), "%s",
             midi->title[0] ? midi->title : "Piano score");
    score->part_count = 1;
    music_part_t *part = &score->parts[0];
    snprintf(part->id, sizeof(part->id), "P1");
    snprintf(part->name, sizeof(part->name), "Piano");
    const uint8_t staff_count = score_staff_count(midi);
    part->staff_count = staff_count;

    int numerator = midi->time_sig_num > 0 ? midi->time_sig_num : 4;
    int denominator = 1 << (midi->time_sig_den >= 0 ? midi->time_sig_den : 2);
    if (denominator <= 0) denominator = 4;
    uint32_t measure_ticks = (uint32_t)midi->ticks_per_quarter * numerator * 4 /
                             denominator;
    if (!measure_ticks) measure_ticks = (uint32_t)midi->ticks_per_quarter * 4;
    uint32_t last_tick = 0;
    for (int i = 0; i < midi->note_count; ++i) {
        uint32_t end = midi->notes[i].start_tick + midi->notes[i].duration;
        if (end > last_tick) last_tick = end;
    }
    int measure_count = (int)((last_tick + measure_ticks - 1) / measure_ticks);
    if (measure_count < 1) measure_count = 1;
    if (measure_count > MUSIC_MAX_MEASURES) measure_count = MUSIC_MAX_MEASURES;
    score->measure_count = (uint16_t)measure_count;
    part->measure_count = (uint16_t)measure_count;
    view->note_count = 0;
    memset(view->note_event_indices, -1, sizeof(view->note_event_indices));

    for (int mi = 0; mi < measure_count; ++mi) {
        music_measure_t *measure = &score->measures[mi];
        snprintf(measure->number, sizeof(measure->number), "%d", mi + 1);
        measure->event_start = score->event_count;
        measure->divisions = midi->ticks_per_quarter;
        measure->duration_divisions = (int32_t)measure_ticks;
        measure->staff_count = staff_count;
        measure->clefs[0] = (music_clef_t){MUSIC_CLEF_TREBLE, 2, 0};
        if (staff_count == 2)
            measure->clefs[1] = (music_clef_t){MUSIC_CLEF_BASS, 4, 0};
        measure->key.fifths = (int8_t)midi->tonality_sf;
        measure->key.minor = midi->tonality_minor;
        measure->time = (music_time_signature_t){
            .beats = (uint8_t)numerator,
            .beat_type = (uint8_t)denominator,
            .symbol = MUSIC_TIME_NUMERIC,
        };
        measure->right_barline = mi + 1 == measure_count ?
                                 MUSIC_BARLINE_FINAL : MUSIC_BARLINE_SINGLE;

        uint32_t measure_start = (uint32_t)mi * measure_ticks;
        uint32_t measure_end = measure_start + measure_ticks;
        uint32_t previous_onset[2][2] = {
            {UINT32_MAX, UINT32_MAX}, {UINT32_MAX, UINT32_MAX}
        };
        for (int ni = 0; ni < midi->note_count; ++ni) {
            const midi_note_t *source = &midi->notes[ni];
            if (source->start_tick < measure_start ||
                source->start_tick >= measure_end) continue;
            if (score->event_count >= MUSIC_MAX_EVENTS) break;
            music_event_t *event = &score->events[score->event_count];
            event->kind = MUSIC_EVENT_NOTE;
            event->measure_index = (uint16_t)mi;
            event->onset_divisions = (int32_t)(source->start_tick - measure_start);
            music_note_t *note = &event->data.note;
            note->duration_divisions = (int32_t)source->duration;
            note->type = duration_kind(source->duration,
                                       midi->ticks_per_quarter, &note->dots);
            note->staff = staff_count == 2 && source->staff == 2 ? 2 : 1;
            note->voice = source->voice ? source->voice : 1;
            note->stem = MUSIC_STEM_AUTO;
            uint8_t staff_index = note->staff - 1;
            uint8_t voice_index = note->voice > 1 ? 1 : 0;
            note->chord = previous_onset[staff_index][voice_index] ==
                          source->start_tick;
            previous_onset[staff_index][voice_index] = source->start_tick;
            midi_to_pitch(source->note, midi->tonality_sf, note);
            view->note_event_indices[ni] = (int16_t)score->event_count;
            score->event_count++;
            measure->event_count++;
            if (ni + 1 > view->note_count) view->note_count = ni + 1;
        }
        auto_beam_measure(score, measure, midi->ticks_per_quarter,
                          numerator, denominator, staff_count);
    }
    return score->event_count > 0;
}

bool leland_score_view_hit_test_note(const leland_score_view_t *view,
                                     int screen_x, int screen_y,
                                     int *note_index, int *measure_number)
{
    if (!view || !view->canvas || !view->scene || !view->score ||
        !lv_obj_is_valid(view->canvas))
        return false;

    lv_area_t canvas_area;
    lv_obj_get_coords(view->canvas, &canvas_area);
    float touch_x = (float)(screen_x - canvas_area.x1) /
                    VIEW_STAFF_SPACE_PX;
    float touch_y = (float)(screen_y - canvas_area.y1 - view->origin_y_px) /
                    VIEW_STAFF_SPACE_PX;
    float best_distance = 1000000.0f;
    int best_event = -1;

    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (item->kind != MUSIC_SCENE_GLYPH ||
            item->source_event_index < 0 ||
            item->data.glyph.glyph < SMUFL_GLYPH_NOTEHEAD_WHOLE ||
            item->data.glyph.glyph > SMUFL_GLYPH_NOTEHEAD_BLACK)
            continue;

        float width = item->data.glyph.glyph == SMUFL_GLYPH_NOTEHEAD_WHOLE ?
                      1.492f : 1.300f;
        float center_x = item->data.glyph.x + width * 0.5f;
        float dx = fabsf(touch_x - center_x);
        float dy = fabsf(touch_y - item->data.glyph.y);
        if (dx > 1.65f || dy > 1.35f) continue;
        float distance = dx * dx + dy * dy;
        if (distance < best_distance) {
            best_distance = distance;
            best_event = item->source_event_index;
        }
    }
    if (best_event < 0 || best_event >= view->score->event_count)
        return false;

    int best_note = -1;
    for (int i = 0; i < view->note_count; ++i) {
        if (view->note_event_indices[i] == best_event) {
            best_note = i;
            break;
        }
    }
    if (best_note < 0) return false;
    if (note_index) *note_index = best_note;
    if (measure_number)
        *measure_number = view->score->events[best_event].measure_index + 1;
    return true;
}

leland_score_view_t *leland_score_view_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    leland_score_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->guide_first_note = -1;
    view->guide_last_note = -1;
    view->score = heap_caps_calloc(1, sizeof(*view->score),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    view->scene = heap_caps_calloc(1, sizeof(*view->scene),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view->score) view->score = calloc(1, sizeof(*view->score));
    if (!view->scene) view->scene = calloc(1, sizeof(*view->scene));
    if (!view->score || !view->scene) {
        leland_score_view_destroy(view);
        return NULL;
    }
    view->parent = parent;
    view->viewport_width = lv_obj_get_content_width(parent);
    view->viewport_height = lv_obj_get_content_height(parent);
    if (view->viewport_width <= 0) view->viewport_width = 1003;
    if (view->viewport_height <= 0) view->viewport_height = 479;
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(parent, 0, 0);
    view->canvas = lv_obj_create(parent);
    lv_obj_remove_style_all(view->canvas);
    lv_obj_set_pos(view->canvas, 0, 0);
    lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(view->canvas, draw_scene_cb, LV_EVENT_DRAW_MAIN, view);
    return view;
}

void leland_score_view_destroy(leland_score_view_t *view)
{
    if (!view) return;
    delete_glyphs(view);
    if (view->canvas && lv_obj_is_valid(view->canvas)) lv_obj_delete(view->canvas);
    free(view->score);
    free(view->scene);
    free(view);
}

bool leland_score_view_set_midi(leland_score_view_t *view,
                                const midi_data_t *midi,
                                char *error, size_t error_size)
{
    if (!view || !midi) return false;
    if (error && error_size) error[0] = '\0';
    delete_glyphs(view);
    if (!build_score(view, midi, error, error_size)) return false;

    music_layout_config_t config;
    music_layout_default_config(&config);
    config.page_width = view->viewport_width / VIEW_STAFF_SPACE_PX;
    config.left_margin = 1.35f;
    config.right_margin = 1.35f;
    config.top_margin = 0.8f;
    /* A piano grand staff needs enough room for opposing stems, ledger lines,
     * and articulations between the two staves. Keep different systems at
     * least as far apart so content from adjacent systems cannot overlap. */
    config.staff_gap = 6.0f;
    config.system_gap = 6.0f;
    config.minimum_note_spacing = 1.65f;
    if (view->score->parts[0].staff_count == 1) {
        /* The 1024x600 practice view is designed around two visible rows.
         * A monophonic four-measure melody therefore uses two measures per
         * system instead of manufacturing a bass staff from its low notes. */
        config.max_measures_per_system = 2;
        config.system_gap = 5.0f;
    }
    if (!music_layout_build(view->score, &config, view->scene,
                            error, error_size)) return false;
    int scene_height_px = sp_to_px(view->scene->height);
    view->origin_y_px = VIEW_MIN_ORIGIN_Y_PX;
    if (view->scene->system_count == 1 && scene_height_px < view->viewport_height) {
        int centered = (view->viewport_height - scene_height_px) / 2;
        if (centered > view->origin_y_px) view->origin_y_px = centered;
    }
    if (!allocate_object_map(view)) {
        if (error && error_size) snprintf(error, error_size, "out of memory");
        return false;
    }
    int content_height = view->origin_y_px + scene_height_px + 20;
    if (content_height < view->viewport_height)
        content_height = view->viewport_height;
    lv_obj_set_size(view->canvas, view->viewport_width, content_height);
    create_glyphs(view);
    lv_obj_invalidate(view->canvas);
    view->page_count = (content_height + view->viewport_height - 1) /
                       view->viewport_height;
    if (view->page_count < 1) view->page_count = 1;
    view->current_page = 0;
    lv_obj_scroll_to_y(view->parent, 0, LV_ANIM_OFF);
    return true;
}

bool leland_score_view_set_note_color(leland_score_view_t *view,
                                      int note_index, uint32_t color_rgb)
{
    if (!view || note_index < 0 || note_index >= view->note_count) return false;
    int event_index = view->note_event_indices[note_index];
    if (event_index < 0) return false;
    size_t changed = music_scene_set_event_color(view->scene, event_index,
                                                  color_rgb);
    if (!changed) return false;
    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (item->source_event_index != event_index ||
            item->kind != MUSIC_SCENE_GLYPH) continue;
        lv_obj_t *object = view->item_objects[i];
        if (object && lv_obj_is_valid(object))
            lv_obj_set_style_text_color(object, item_color(item), 0);
    }
    lv_obj_invalidate(view->canvas);
    return true;
}

void leland_score_view_set_note_guide(leland_score_view_t *view,
                                      int first_note_index,
                                      int last_note_index)
{
    if (!view) return;
    if (first_note_index < 0 || last_note_index < first_note_index ||
        first_note_index >= view->note_count) {
        first_note_index = -1;
        last_note_index = -1;
    } else if (last_note_index >= view->note_count) {
        last_note_index = view->note_count - 1;
    }
    if (view->guide_first_note == first_note_index &&
        view->guide_last_note == last_note_index) {
        return;
    }
    view->guide_first_note = first_note_index;
    view->guide_last_note = last_note_index;
    if (view->canvas && lv_obj_is_valid(view->canvas)) {
        lv_obj_invalidate(view->canvas);
    }
}

int leland_score_view_page_count(const leland_score_view_t *view)
{
    return view && view->page_count > 0 ? view->page_count : 1;
}

bool leland_score_view_show_page(leland_score_view_t *view, int page,
                                 bool animated)
{
    if (!view || page < 0 || page >= view->page_count) return false;
    view->current_page = page;
    lv_obj_scroll_to_y(view->parent, page * view->viewport_height,
                       animated ? LV_ANIM_ON : LV_ANIM_OFF);
    return true;
}

void leland_score_view_set_hidden(leland_score_view_t *view, bool hidden)
{
    if (!view || !view->canvas || !lv_obj_is_valid(view->canvas)) return;
    if (hidden)
        lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
}

int leland_score_view_get_last_note_on_page(const leland_score_view_t *view,
                                            int page)
{
    if (!view || page < 0 || page >= view->page_count) return -1;
    if (view->note_count <= 0 || !view->scene) return -1;

    int page_start_y = page * view->viewport_height;
    int page_end_y = (page + 1) * view->viewport_height;
    int last_note = -1;

    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (item->kind != MUSIC_SCENE_GLYPH) continue;
        int event_index = item->source_event_index;
        if (event_index < 0) continue;

        int note_index = -1;
        for (int n = 0; n < view->note_count; ++n) {
            if (view->note_event_indices[n] == event_index) {
                note_index = n;
                break;
            }
        }
        if (note_index < 0) continue;

        int pixel_y = sp_to_px(item->data.glyph.y) + view->origin_y_px;
        if (pixel_y >= page_start_y && pixel_y < page_end_y &&
            note_index > last_note) {
            last_note = note_index;
        }
    }
    return last_note;
}
