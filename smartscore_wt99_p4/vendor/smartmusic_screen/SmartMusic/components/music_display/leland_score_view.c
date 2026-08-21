#include "leland_score_view.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "midi_notation.h"
#include "music_layout.h"
#include "smufl_glyphs.h"

LV_FONT_DECLARE(lv_font_LelandSMuFL_72);
LV_FONT_DECLARE(lv_font_LelandSMuFL_64);
LV_FONT_DECLARE(lv_font_LelandBrace_252);

#define VIEW_STAFF_SPACE_PX 18.0f
#define VIEW_MIN_ORIGIN_Y_PX 14
#define VIEW_CURVE_SEGMENTS 20
#define VIEW_MAX_NOTES      MAX_NOTES
#define VIEW_GUIDE_HEIGHT_PX 128
#define VIEW_GUIDE_MIN_WIDTH_PX 48
#define VIEW_GUIDE_X_PADDING_PX 8
#define VIEW_GUIDE_MAX_SHORT_LINE_PX 54
#define VIEW_GUIDE_MAX_OVERLAYS 12

struct leland_score_view {
    lv_obj_t *parent;
    lv_obj_t *guide_layer;
    lv_obj_t *canvas;
    music_score_t *score;
    music_scene_t *scene;
    midi_data_t *midi_cache;
    uint16_t note_event_indices[VIEW_MAX_NOTES];
    int note_count;
    int viewport_width;
    int viewport_height;
    int origin_y_px;
    int page_count;
    int current_page;
    int guide_first_note;
    int guide_last_note;
    bool paginated;
    bool applied_paginated;
    lv_area_t *guide_areas;
    lv_obj_t *guide_overlays[VIEW_GUIDE_MAX_OVERLAYS];
    uint8_t guide_overlay_count;
    uint8_t visible_guide_overlays;
};

static int mapped_event_index(const leland_score_view_t *view, int note_index)
{
    if (!view || note_index < 0 || note_index >= view->note_count)
        return -1;
    const uint16_t event_index = view->note_event_indices[note_index];
    return event_index == MIDI_NOTATION_NO_EVENT ? -1 : (int)event_index;
}

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

static bool guide_area_is_valid(const lv_area_t *area)
{
    return area && area->x2 >= area->x1 && area->y2 >= area->y1;
}

static void hide_note_guide_overlays(leland_score_view_t *view)
{
    if (!view) return;
    for (uint8_t index = 0; index < view->visible_guide_overlays; ++index) {
        lv_obj_t *overlay = view->guide_overlays[index];
        if (overlay && lv_obj_is_valid(overlay)) {
            lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
    view->visible_guide_overlays = 0;
}

static void create_note_guide_overlays(leland_score_view_t *view)
{
    if (!view || !view->guide_layer) return;
    for (int index = 0; index < VIEW_GUIDE_MAX_OVERLAYS; ++index) {
        lv_obj_t *overlay = lv_obj_create(view->guide_layer);
        if (!overlay) {
            for (uint8_t created = 0;
                 created < view->guide_overlay_count; ++created) {
                lv_obj_delete(view->guide_overlays[created]);
                view->guide_overlays[created] = NULL;
            }
            view->guide_overlay_count = 0;
            return;
        }
        lv_obj_remove_style_all(overlay);
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0xBAE6FD), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
        lv_obj_set_style_border_color(overlay, lv_color_hex(0x0284C7), 0);
        lv_obj_set_style_border_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(overlay, 2, 0);
        lv_obj_set_style_radius(overlay, 9, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        view->guide_overlays[view->guide_overlay_count++] = overlay;
    }
}

static bool ensure_note_guide_cache(leland_score_view_t *view)
{
    if (!view) return false;
    if (view->guide_areas) return true;
    view->guide_areas = heap_caps_calloc(VIEW_MAX_NOTES,
                                          sizeof(*view->guide_areas),
                                          MALLOC_CAP_SPIRAM |
                                              MALLOC_CAP_8BIT);
    if (!view->guide_areas) {
        view->guide_areas = calloc(VIEW_MAX_NOTES,
                                   sizeof(*view->guide_areas));
    }
    if (!view->guide_areas) return false;
    for (int note = 0; note < VIEW_MAX_NOTES; ++note) {
        view->guide_areas[note].x1 = 1;
        view->guide_areas[note].x2 = 0;
    }
    return true;
}

static void invalidate_note_guide_cache(leland_score_view_t *view)
{
    if (!view) return;
    hide_note_guide_overlays(view);
    if (!view->guide_areas) return;
    for (int note = 0; note < VIEW_MAX_NOTES; ++note) {
        view->guide_areas[note].x1 = 1;
        view->guide_areas[note].x2 = 0;
    }
}

static void show_note_guide_area(leland_score_view_t *view, int overlay_index,
                                 const lv_area_t *area)
{
    if (!view || overlay_index < 0 ||
        overlay_index >= view->guide_overlay_count ||
        !guide_area_is_valid(area)) {
        return;
    }
    lv_obj_t *overlay = view->guide_overlays[overlay_index];
    if (!overlay || !lv_obj_is_valid(overlay)) return;
    lv_obj_set_pos(overlay, area->x1, area->y1);
    lv_obj_set_size(overlay, area->x2 - area->x1 + 1,
                    area->y2 - area->y1 + 1);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_note_guide_overlays(leland_score_view_t *view)
{
    if (!view) return;
    hide_note_guide_overlays(view);
    if (view->guide_overlay_count == 0 || !view->guide_areas ||
        view->guide_first_note < 0 ||
        view->guide_last_note < view->guide_first_note ||
        view->guide_first_note >= view->note_count) {
        return;
    }

    int last = view->guide_last_note;
    if (last >= view->note_count) last = view->note_count - 1;
    lv_area_t areas[VIEW_GUIDE_MAX_OVERLAYS];
    int event_indices[VIEW_GUIDE_MAX_OVERLAYS];
    int area_count = 0;
    bool merged_valid = false;
    lv_area_t merged = {0};

    for (int note = view->guide_first_note; note <= last; ++note) {
        const lv_area_t *area = &view->guide_areas[note];
        if (!guide_area_is_valid(area)) continue;
        const int event_index = mapped_event_index(view, note);
        if (event_index < 0) continue;
        bool duplicate = false;
        const int remembered = area_count < VIEW_GUIDE_MAX_OVERLAYS
                                   ? area_count
                                   : VIEW_GUIDE_MAX_OVERLAYS;
        for (int index = 0; index < remembered; ++index) {
            if (event_indices[index] == event_index) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        if (!merged_valid) {
            merged = *area;
            merged_valid = true;
        } else {
            if (area->x1 < merged.x1) merged.x1 = area->x1;
            if (area->y1 < merged.y1) merged.y1 = area->y1;
            if (area->x2 > merged.x2) merged.x2 = area->x2;
            if (area->y2 > merged.y2) merged.y2 = area->y2;
        }
        if (area_count < VIEW_GUIDE_MAX_OVERLAYS) {
            areas[area_count] = *area;
            event_indices[area_count] = event_index;
        }
        area_count++;
    }

    if (area_count > VIEW_GUIDE_MAX_OVERLAYS) {
        show_note_guide_area(view, 0, &merged);
        view->visible_guide_overlays = merged_valid ? 1 : 0;
        return;
    }
    for (int index = 0; index < area_count; ++index) {
        show_note_guide_area(view, index, &areas[index]);
    }
    view->visible_guide_overlays = (uint8_t)area_count;
}

static void rebuild_note_guide_cache(leland_score_view_t *view,
                                     int canvas_width,
                                     int canvas_height)
{
    if (!view || !view->scene || canvas_width <= 0 || canvas_height <= 0 ||
        !ensure_note_guide_cache(view)) {
        return;
    }
    invalidate_note_guide_cache(view);
    for (int note = 0; note < view->note_count; ++note) {
        const int event_index = mapped_event_index(view, note);
        if (event_index < 0) continue;
        lv_area_t area;
        if (!guided_event_area(view, event_index, 0, 0, &area)) continue;
        if (area.x2 < 0 || area.y2 < 0 || area.x1 >= canvas_width ||
            area.y1 >= canvas_height) {
            continue;
        }
        if (area.x1 < 0) area.x1 = 0;
        if (area.y1 < 0) area.y1 = 0;
        if (area.x2 >= canvas_width) area.x2 = canvas_width - 1;
        if (area.y2 >= canvas_height) area.y2 = canvas_height - 1;
        view->guide_areas[note] = area;
    }
    refresh_note_guide_overlays(view);
}

static float cubic(float p0, float p1, float p2, float p3, float t)
{
    float u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * p1 +
           3.0f * u * t * t * p2 + t * t * t * p3;
}

static bool scene_item_intersects_clip(const leland_score_view_t *view,
                                       const music_scene_item_t *item,
                                       int canvas_x, int canvas_y,
                                       const lv_area_t *clip)
{
    if (!view || !item || !clip) return false;
    lv_area_t area = {0};

    if (item->kind == MUSIC_SCENE_LINE) {
        const int x1 = canvas_x + sp_to_px(item->data.line.x1);
        const int x2 = canvas_x + sp_to_px(item->data.line.x2);
        const int y1 = canvas_y + view->origin_y_px +
                       sp_to_px(item->data.line.y1);
        const int y2 = canvas_y + view->origin_y_px +
                       sp_to_px(item->data.line.y2);
        const int width = sp_to_px(item->data.line.thickness);
        const int padding = width > 0 ? width : 1;
        area.x1 = (x1 < x2 ? x1 : x2) - padding;
        area.x2 = (x1 > x2 ? x1 : x2) + padding;
        area.y1 = (y1 < y2 ? y1 : y2) - padding;
        area.y2 = (y1 > y2 ? y1 : y2) + padding;
    } else if (item->kind == MUSIC_SCENE_BEZIER) {
        const typeof(item->data.bezier) *b = &item->data.bezier;
        const float minimum_x = fminf(fminf(b->x1, b->cx1),
                                      fminf(b->cx2, b->x2));
        const float maximum_x = fmaxf(fmaxf(b->x1, b->cx1),
                                      fmaxf(b->cx2, b->x2));
        const float minimum_y = fminf(fminf(b->y1, b->cy1),
                                      fminf(b->cy2, b->y2));
        const float maximum_y = fmaxf(fmaxf(b->y1, b->cy1),
                                      fmaxf(b->cy2, b->y2));
        const int width = sp_to_px(b->thickness);
        const int padding = width > 0 ? width : 1;
        area.x1 = canvas_x + sp_to_px(minimum_x) - padding;
        area.x2 = canvas_x + sp_to_px(maximum_x) + padding;
        area.y1 = canvas_y + view->origin_y_px +
                  sp_to_px(minimum_y) - padding;
        area.y2 = canvas_y + view->origin_y_px +
                  sp_to_px(maximum_y) + padding;
    } else if (item->kind == MUSIC_SCENE_GLYPH) {
        const smufl_glyph_info_t *glyph =
            smufl_glyph_info(item->data.glyph.glyph);
        if (!glyph) return false;
        const lv_font_t *font = scene_glyph_font(item->data.glyph.glyph);
        lv_font_glyph_dsc_t glyph_dsc;
        if (!lv_font_get_glyph_dsc(font, &glyph_dsc,
                                   glyph->codepoint, 0)) {
            return false;
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
        const int glyph_x2 = glyph_x1 + glyph_dsc.box_w;
        const int glyph_y2 = glyph_y1 + glyph_dsc.box_h;
        area.x1 = glyph_x1 < point_x ? glyph_x1 : point_x;
        area.y1 = glyph_y1 < point_y ? glyph_y1 : point_y;
        area.x2 = glyph_x2 > point_x + glyph_dsc.adv_w
                      ? glyph_x2
                      : point_x + glyph_dsc.adv_w;
        area.y2 = glyph_y2 > point_y + font->line_height
                      ? glyph_y2
                      : point_y + font->line_height;
    } else {
        return false;
    }

    return area.x1 <= clip->x2 && area.x2 >= clip->x1 &&
           area.y1 <= clip->y2 && area.y2 >= clip->y1;
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

    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (!scene_item_intersects_clip(view, item, canvas_x, canvas_y,
                                        &layer->_clip_area)) {
            continue;
        }
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

static bool midi_has_lower_staff(const midi_data_t *midi)
{
    if (!midi) return false;
    for (int i = 0; i < midi->note_count; ++i)
        if (midi->notes[i].staff == 2) return true;
    return false;
}

static bool midi_measure_ticks(const midi_data_t *midi,
                               uint32_t *measure_ticks)
{
    if (!midi || !measure_ticks || midi->ticks_per_quarter <= 0)
        return false;
    const int numerator = midi->time_sig_num > 0 ? midi->time_sig_num : 4;
    if (numerator > UINT8_MAX) return false;
    int denominator_power = midi->time_sig_den;
    if (denominator_power < 0 || denominator_power > 6)
        denominator_power = 2;
    const uint32_t denominator = UINT32_C(1) << denominator_power;
    const uint64_t ticks = (uint64_t)(uint32_t)midi->ticks_per_quarter *
                           (uint32_t)numerator * 4U / denominator;
    if (!ticks || ticks > INT32_MAX) return false;
    *measure_ticks = (uint32_t)ticks;
    return true;
}

static int first_dirty_measure(const leland_score_view_t *view,
                               const midi_data_t *midi)
{
    if (!view || !view->midi_cache || !midi) return -1;
    const midi_data_t *old = view->midi_cache;
    if (old->ticks_per_quarter != midi->ticks_per_quarter ||
        old->time_sig_num != midi->time_sig_num ||
        old->time_sig_den != midi->time_sig_den ||
        old->tonality_sf != midi->tonality_sf ||
        old->tonality_minor != midi->tonality_minor ||
        old->tonality_forced != midi->tonality_forced)
        return -1;
    /* Staff count is score-wide. If the lower staff appears or disappears,
     * every preserved measure header and system geometry becomes stale. */
    if (midi_has_lower_staff(old) != midi_has_lower_staff(midi)) return -1;

    int shared = old->note_count < midi->note_count ?
                 old->note_count : midi->note_count;
    int changed = 0;
    while (changed < shared &&
           memcmp(&old->notes[changed], &midi->notes[changed],
                  sizeof(midi->notes[changed])) == 0) {
        changed++;
    }
    if (changed == shared && old->note_count == midi->note_count &&
        strcmp(old->title, midi->title) == 0)
        return INT_MAX;

    uint32_t dirty_tick = UINT32_MAX;
    /* Do not assume the public midi_data_t arrays are source-sorted. Any
     * changed suffix note may be the earliest affected rhythmic column. */
    for (int i = changed; i < old->note_count; ++i)
        if (old->notes[i].start_tick < dirty_tick)
            dirty_tick = old->notes[i].start_tick;
    for (int i = changed; i < midi->note_count; ++i)
        if (midi->notes[i].start_tick < dirty_tick)
            dirty_tick = midi->notes[i].start_tick;
    if (dirty_tick == UINT32_MAX) dirty_tick = 0;

    uint32_t measure_ticks = 0;
    if (!midi_measure_ticks(midi, &measure_ticks)) return -1;
    return (int)(dirty_tick / measure_ticks);
}

static bool midi_has_connections(const midi_data_t *midi)
{
    if (!midi) return false;
    for (int i = 0; i < midi->note_count; ++i) {
        const midi_note_t *note = &midi->notes[i];
        if (note->tie_flags || note->slur_start || note->slur_stop ||
            note->gliss_start || note->gliss_stop)
            return true;
    }
    return false;
}
static bool build_score(const midi_data_t *midi, music_score_t *score,
                        uint16_t *note_event_indices,
                        size_t note_event_capacity, int *note_count,
                        char *error, size_t error_size)
{
    if (!midi || !score || !note_event_indices || !note_count) return false;
    *note_count = 0;
    for (size_t i = 0; i < note_event_capacity; ++i)
        note_event_indices[i] = MIDI_NOTATION_NO_EVENT;

    midi_notation_build_options_t options;
    midi_notation_build_options_from_midi(midi, &options);
    midi_notation_build_result_t result;
    const midi_notation_status_t status = midi_notation_build_score(
        midi, &options, score, note_event_indices,
        note_event_capacity, &result);
    if (status != MIDI_NOTATION_OK) {
        if (error && error_size) {
            snprintf(error, error_size, "notation: %s",
                     midi_notation_status_name(status));
        }
        return false;
    }

    *note_count = midi->note_count;
    if (*note_count > (int)note_event_capacity)
        *note_count = (int)note_event_capacity;
    return result.score_event_count > 0;
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
        if (mapped_event_index(view, i) == best_event) {
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
    view->midi_cache = heap_caps_calloc(1, sizeof(*view->midi_cache),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view->score) view->score = calloc(1, sizeof(*view->score));
    if (!view->scene) view->scene = calloc(1, sizeof(*view->scene));
    if (!view->midi_cache)
        view->midi_cache = calloc(1, sizeof(*view->midi_cache));
    if (!view->score || !view->scene || !view->midi_cache) {
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
    /* The guide layer is created first, so the score canvas remains above
     * it in LVGL's sibling draw order. Staff lines and note glyphs therefore
     * stay crisp while moving a guide only dirties its small rectangle. */
    view->guide_layer = lv_obj_create(parent);
    lv_obj_remove_style_all(view->guide_layer);
    lv_obj_set_pos(view->guide_layer, 0, 0);
    lv_obj_clear_flag(view->guide_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(view->guide_layer, LV_OBJ_FLAG_CLICKABLE);
    create_note_guide_overlays(view);

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
    if (view->canvas && lv_obj_is_valid(view->canvas)) lv_obj_delete(view->canvas);
    if (view->guide_layer && lv_obj_is_valid(view->guide_layer))
        lv_obj_delete(view->guide_layer);
    free(view->guide_areas);
    free(view->score);
    free(view->scene);
    free(view->midi_cache);
    free(view);
}

void leland_score_view_set_paginated(leland_score_view_t *view,
                                     bool paginated)
{
    if (!view) return;
    view->paginated = paginated;
}

bool leland_score_view_set_midi(leland_score_view_t *view,
                                const midi_data_t *midi,
                                char *error, size_t error_size)
{
    if (!view || !midi) return false;
    if (error && error_size) error[0] = '\0';
    uint32_t checked_measure_ticks = 0;
    if (midi->note_count <= 0 || midi->note_count > VIEW_MAX_NOTES ||
        !memchr(midi->title, '\0', sizeof(midi->title)) ||
        !midi_measure_ticks(midi, &checked_measure_ticks)) {
        if (error && error_size)
            snprintf(error, error_size, "invalid MIDI score");
        return false;
    }
    (void)checked_measure_ticks;
    /* Page centering depends on every system sharing a page.  Paginated
     * preview therefore rebuilds atomically as a complete scene; Creator is
     * continuous (paginated=false) and keeps its incremental fast path. */
    int dirty_measure = (!view->paginated &&
                         view->paginated == view->applied_paginated) ?
                        first_dirty_measure(view, midi) : -1;
    if (dirty_measure == INT_MAX) return true;
    /* Connection geometry may begin in a preserved system and finish in the
     * dirty tail. Rebuild the whole scene in that uncommon case; ordinary
     * dense input remains on the dirty-system fast path. */
    if (midi_has_connections(midi)) dirty_measure = -1;

    /* Normalize and lay out into a complete staging snapshot. The currently
     * visible score remains authoritative until every fallible step has
     * succeeded, so an OOM or malformed input cannot leave a torn page. */
    music_score_t *next_score = heap_caps_calloc(
        1, sizeof(*next_score), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    music_scene_t *next_scene = heap_caps_calloc(
        1, sizeof(*next_scene), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *next_note_event_indices = heap_caps_malloc(
        sizeof(view->note_event_indices),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next_note_event_indices)
        next_note_event_indices = malloc(sizeof(view->note_event_indices));
    if (!next_score || !next_scene || !next_note_event_indices) {
        free(next_score);
        free(next_scene);
        free(next_note_event_indices);
        if (error && error_size) snprintf(error, error_size, "out of memory");
        return false;
    }
    if (dirty_measure >= 0) *next_scene = *view->scene;

    int next_note_count = 0;
    if (!build_score(midi, next_score, next_note_event_indices,
                     VIEW_MAX_NOTES, &next_note_count,
                     error, error_size)) {
        free(next_score);
        free(next_scene);
        free(next_note_event_indices);
        return false;
    }

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
    /* The device page is optimized for two measures per system. A dense
     * measure can still claim the whole system when its hard ink bounds make
     * a second measure unsafe. Apply this to both single and grand staff. */
    config.max_measures_per_system = 2;
    if (view->paginated) {
        config.page_height =
            (music_sp_t)view->viewport_height / VIEW_STAFF_SPACE_PX;
        config.page_vertical_offset =
            (music_sp_t)VIEW_MIN_ORIGIN_Y_PX / VIEW_STAFF_SPACE_PX;
        config.page_top_padding = 0.55f;
        config.page_bottom_padding = 1.10f;
    }
    if (next_score->parts[0].staff_count == 1) {
        /* The 1024x600 practice view is designed around two visible rows.
         * A monophonic four-measure melody therefore uses two measures per
         * system instead of manufacturing a bass staff from its low notes. */
        config.system_gap = 5.0f;
    }
    bool layout_ok = dirty_measure >= 0 ?
        music_layout_rebuild_from_measure(next_score, &config, next_scene,
                                          (uint16_t)dirty_measure,
                                          error, error_size) :
        music_layout_build(next_score, &config, next_scene,
                           error, error_size);
    if (!layout_ok) {
        free(next_score);
        free(next_scene);
        free(next_note_event_indices);
        return false;
    }

    music_score_t *old_score = view->score;
    music_scene_t *old_scene = view->scene;
    view->score = next_score;
    view->scene = next_scene;
    memcpy(view->note_event_indices, next_note_event_indices,
           sizeof(view->note_event_indices));
    view->note_count = next_note_count;
    *view->midi_cache = *midi;
    view->applied_paginated = view->paginated;
    free(next_note_event_indices);
    free(old_score);
    free(old_scene);

    view->guide_first_note = -1;
    view->guide_last_note = -1;
    invalidate_note_guide_cache(view);
    int scene_height_px = sp_to_px(view->scene->height);
    view->origin_y_px = VIEW_MIN_ORIGIN_Y_PX;
    if (!view->paginated && view->scene->system_count == 1 &&
        scene_height_px < view->viewport_height) {
        int centered = (view->viewport_height - scene_height_px) / 2;
        if (centered > view->origin_y_px) view->origin_y_px = centered;
    }
    int content_height = view->origin_y_px + scene_height_px +
                         (view->paginated ? 2 : 20);
    if (content_height < view->viewport_height)
        content_height = view->viewport_height;
    view->page_count = (content_height + view->viewport_height - 1) /
                       view->viewport_height;
    if (view->page_count < 1) view->page_count = 1;
    /* Make a short final logical page fully scrollable. Otherwise LVGL
     * clamps it upward and can expose the preceding system under the fixed
     * bottom controls. */
    if (view->paginated)
        content_height = view->page_count * view->viewport_height;
    lv_obj_set_size(view->canvas, view->viewport_width, content_height);
    if (view->guide_layer && lv_obj_is_valid(view->guide_layer)) {
        lv_obj_set_size(view->guide_layer, view->viewport_width,
                        content_height);
    }
    lv_obj_invalidate(view->canvas);
    /* LVGL applies object sizes during its next layout pass. Use the known
     * dimensions here so a freshly loaded multi-page score is not clipped
     * against the canvas's previous/default size. */
    rebuild_note_guide_cache(view, view->viewport_width, content_height);
    view->current_page = 0;
    lv_obj_scroll_to_y(view->parent, 0, LV_ANIM_OFF);
    return true;
}

bool leland_score_view_set_note_color(leland_score_view_t *view,
                                      int note_index, uint32_t color_rgb)
{
    if (!view || note_index < 0 || note_index >= view->note_count) return false;
    int event_index = mapped_event_index(view, note_index);
    if (event_index < 0) return false;
    size_t changed = music_scene_set_event_color(view->scene, event_index,
                                                  color_rgb);
    if (!changed) return false;
    lv_obj_invalidate(view->canvas);
    return true;
}

void leland_score_view_clear_note_colors(leland_score_view_t *view)
{
    if (!view || !view->scene) return;
    for (int i = 0; i < view->scene->item_count; ++i) {
        music_scene_item_t *item = &view->scene->items[i];
        if (item->source_event_index < 0 ||
            item->color_rgb == MUSIC_SCENE_COLOR_DEFAULT) {
            continue;
        }
        item->color_rgb = MUSIC_SCENE_COLOR_DEFAULT;
    }
    if (view->canvas && lv_obj_is_valid(view->canvas)) {
        lv_obj_invalidate(view->canvas);
    }
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
    refresh_note_guide_overlays(view);
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
    if (hidden) {
        lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
        if (view->guide_layer && lv_obj_is_valid(view->guide_layer))
            lv_obj_add_flag(view->guide_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (view->guide_layer && lv_obj_is_valid(view->guide_layer))
            lv_obj_clear_flag(view->guide_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

int leland_score_view_get_last_note_on_page(const leland_score_view_t *view,
                                            int page)
{
    if (!view || page < 0 || page >= view->page_count) return -1;
    if (view->note_count <= 0 || !view->scene) return -1;

    int last_note = -1;

    /* Guide geometry is cached when the score is laid out. Reuse its centre
     * point for paging instead of scanning every scene item and then doing a
     * second linear lookup through all notes for every playback tick. */
    if (view->guide_areas) {
        for (int note = 0; note < view->note_count; ++note) {
            const lv_area_t *area = &view->guide_areas[note];
            if (!guide_area_is_valid(area)) continue;
            const int center_y = area->y1 + (area->y2 - area->y1) / 2;
            if (center_y / view->viewport_height == page) last_note = note;
        }
        if (last_note >= 0) return last_note;
    }

    const int page_start_y = page * view->viewport_height;
    const int page_end_y = (page + 1) * view->viewport_height;

    for (int i = 0; i < view->scene->item_count; ++i) {
        const music_scene_item_t *item = &view->scene->items[i];
        if (item->kind != MUSIC_SCENE_GLYPH) continue;
        int event_index = item->source_event_index;
        if (event_index < 0) continue;

        int note_index = -1;
        for (int n = 0; n < view->note_count; ++n) {
            if (mapped_event_index(view, n) == event_index) {
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

bool leland_score_view_get_page_start_tick(const leland_score_view_t *view,
                                           int page,
                                           uint64_t *start_tick)
{
    if (!view || !start_tick || !view->scene || !view->score ||
        !view->midi_cache || page < 0 || page >= view->page_count ||
        view->viewport_height <= 0 ||
        view->midi_cache->ticks_per_quarter <= 0)
        return false;

    int first_measure = -1;
    for (int i = 0; i < view->scene->system_count; ++i) {
        const music_scene_system_t *system = &view->scene->systems[i];
        const music_sp_t center = (system->ink_top + system->ink_bottom) * 0.5f;
        const int center_y = sp_to_px(center) + view->origin_y_px;
        if (center_y / view->viewport_height == page) {
            first_measure = system->measure_start;
            break;
        }
    }
    if (first_measure < 0 || first_measure > view->score->measure_count)
        return false;

    uint64_t tick = 0;
    const uint64_t target_tpq =
        (uint64_t)view->midi_cache->ticks_per_quarter;
    for (int i = 0; i < first_measure; ++i) {
        const music_measure_t *measure = &view->score->measures[i];
        if (measure->divisions <= 0 || measure->duration_divisions <= 0)
            return false;
        if ((uint64_t)measure->duration_divisions >
            UINT64_MAX / target_tpq)
            return false;
        const uint64_t scaled =
            (uint64_t)measure->duration_divisions * target_tpq;
        const uint64_t divisor = (uint64_t)measure->divisions;
        const uint64_t rounded = scaled / divisor +
            ((scaled % divisor) * 2U >= divisor ? 1U : 0U);
        if (tick > UINT64_MAX - rounded) return false;
        tick += rounded;
    }
    *start_tick = tick;
    return true;
}
