#include "music_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES_PER_SYSTEM 512

typedef struct {
    const music_event_t *event;
    int event_index;
    music_sp_t x;
    music_sp_t y;
    music_sp_t stem_x;
    music_sp_t stem_start_y;
    music_sp_t stem_end_y;
    music_sp_t chord_left;
    music_sp_t chord_right;
    music_sp_t chord_top;
    music_sp_t chord_bottom;
    music_stem_direction_t stem;
    uint8_t staff;
    uint8_t system_index;
    bool valid;
    bool has_stem;
    bool beamed;
} note_layout_t;

typedef struct {
    music_sp_t head_x;
    music_sp_t head_left;
    music_sp_t head_right;
    music_sp_t chord_left;
    music_sp_t chord_right;
    music_sp_t chord_top;
    music_sp_t chord_bottom;
    music_sp_t accidental_x;
    music_sp_t dot_x;
    music_sp_t dot_y;
    music_sp_t stem_x;
    music_sp_t stem_start_y;
    music_sp_t stem_end_y;
    music_stem_direction_t stem;
    bool stem_owner;
} chord_note_geometry_t;

static bool set_error(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

static music_scene_item_t *append_item(music_scene_t *scene,
                                       music_scene_item_kind_t kind,
                                       int source_event_index)
{
    if (scene->item_count >= MUSIC_SCENE_MAX_ITEMS) return NULL;
    music_scene_item_t *item = &scene->items[scene->item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->source_event_index = (int16_t)source_event_index;
    item->color_rgb = MUSIC_SCENE_COLOR_DEFAULT;
    return item;
}

static bool add_glyph(music_scene_t *scene, smufl_glyph_id_t glyph,
                      music_sp_t x, music_sp_t y, music_sp_t scale,
                      int source_event_index)
{
    if (glyph >= SMUFL_GLYPH_COUNT) return true;
    music_scene_item_t *item = append_item(scene, MUSIC_SCENE_GLYPH,
                                           source_event_index);
    if (!item) return false;
    item->data.glyph.glyph = glyph;
    item->data.glyph.x = x;
    item->data.glyph.y = y;
    item->data.glyph.scale = scale;
    return true;
}

/* Leland uses a 1000-unit em and SMuFL defines one staff space as 250 font
 * units.  These visual widths let layout X represent the optical centre,
 * while the scene command still stores the font's left-side pen origin. */
static music_sp_t leland_glyph_width(smufl_glyph_id_t glyph)
{
    switch (glyph) {
    case SMUFL_GLYPH_NOTEHEAD_WHOLE: return 1.492f;
    case SMUFL_GLYPH_NOTEHEAD_HALF:
    case SMUFL_GLYPH_NOTEHEAD_BLACK:
    case SMUFL_GLYPH_REST_WHOLE:
    case SMUFL_GLYPH_REST_HALF: return 1.300f;
    case SMUFL_GLYPH_REST_QUARTER: return 0.940f;
    case SMUFL_GLYPH_REST_8: return 1.104f;
    case SMUFL_GLYPH_REST_16: return 1.372f;
    case SMUFL_GLYPH_REST_32: return 1.564f;
    case SMUFL_GLYPH_ARTIC_ACCENT_ABOVE:
    case SMUFL_GLYPH_ARTIC_ACCENT_BELOW: return 1.444f;
    case SMUFL_GLYPH_ARTIC_STACCATO_ABOVE:
    case SMUFL_GLYPH_ARTIC_STACCATO_BELOW: return 0.314f;
    case SMUFL_GLYPH_ARTIC_TENUTO_ABOVE:
    case SMUFL_GLYPH_ARTIC_TENUTO_BELOW: return 1.264f;
    case SMUFL_GLYPH_FERMATA_ABOVE:
    case SMUFL_GLYPH_FERMATA_BELOW: return 2.488f;
    default: return 0.0f;
    }
}

static bool add_centered_glyph(music_scene_t *scene, smufl_glyph_id_t glyph,
                               music_sp_t center_x, music_sp_t y,
                               music_sp_t scale, int source_event_index)
{
    music_sp_t width = leland_glyph_width(glyph) * scale;
    return add_glyph(scene, glyph, center_x - width * 0.5f, y, scale,
                     source_event_index);
}

static bool add_line(music_scene_t *scene, music_sp_t x1, music_sp_t y1,
                     music_sp_t x2, music_sp_t y2, music_sp_t thickness,
                     int source_event_index)
{
    music_scene_item_t *item = append_item(scene, MUSIC_SCENE_LINE,
                                           source_event_index);
    if (!item) return false;
    item->data.line.x1 = x1;
    item->data.line.y1 = y1;
    item->data.line.x2 = x2;
    item->data.line.y2 = y2;
    item->data.line.thickness = thickness;
    return true;
}

static bool add_bezier(music_scene_t *scene,
                       music_sp_t x1, music_sp_t y1,
                       music_sp_t cx1, music_sp_t cy1,
                       music_sp_t cx2, music_sp_t cy2,
                       music_sp_t x2, music_sp_t y2,
                       music_sp_t thickness, int source_event_index)
{
    music_scene_item_t *item = append_item(scene, MUSIC_SCENE_BEZIER,
                                           source_event_index);
    if (!item) return false;
    item->data.bezier.x1 = x1;
    item->data.bezier.y1 = y1;
    item->data.bezier.cx1 = cx1;
    item->data.bezier.cy1 = cy1;
    item->data.bezier.cx2 = cx2;
    item->data.bezier.cy2 = cy2;
    item->data.bezier.x2 = x2;
    item->data.bezier.y2 = y2;
    item->data.bezier.thickness = thickness;
    return true;
}

void music_layout_default_config(music_layout_config_t *config)
{
    if (!config) return;
    *config = (music_layout_config_t){
        .page_width = 56.0f,
        .left_margin = 2.0f,
        .right_margin = 2.0f,
        .top_margin = 1.5f,
        .system_gap = 4.0f,
        .staff_gap = 6.0f,
        .staff_line_thickness = 0.10f,
        /* At staffSpace=18px, 0.12sp and the 0.10sp staff line both round to
         * 2px. Raster-compensate stems to 3px so they remain visibly heavier. */
        .stem_thickness = 0.16f,
        .beam_thickness = 0.50f,
        .ledger_line_thickness = 0.16f,
        .ledger_line_extension = 0.35f,
        .minimum_note_spacing = 1.65f,
        .max_measures_per_system = 0,
    };
}

size_t music_scene_set_event_color(music_scene_t *scene,
                                   int source_event_index,
                                   uint32_t color_rgb)
{
    if (!scene || source_event_index < 0) return 0;
    color_rgb &= UINT32_C(0x00FFFFFF);
    size_t changed = 0;
    for (int i = 0; i < scene->item_count; ++i) {
        music_scene_item_t *item = &scene->items[i];
        if (item->source_event_index != source_event_index) continue;
        item->color_rgb = color_rgb;
        changed++;
    }
    return changed;
}

size_t music_scene_clear_event_color(music_scene_t *scene,
                                     int source_event_index)
{
    if (!scene || source_event_index < 0) return 0;
    size_t changed = 0;
    for (int i = 0; i < scene->item_count; ++i) {
        music_scene_item_t *item = &scene->items[i];
        if (item->source_event_index != source_event_index) continue;
        item->color_rgb = MUSIC_SCENE_COLOR_DEFAULT;
        changed++;
    }
    return changed;
}

static music_sp_t staff_top_for(uint8_t staff, music_sp_t system_top,
                                const music_layout_config_t *config)
{
    return system_top + (staff > 0 ? staff - 1 : 0) *
           (4.0f + config->staff_gap);
}

static music_sp_t clef_anchor_y(const music_clef_t *clef, music_sp_t staff_top)
{
    int line = clef->line > 0 ? clef->line :
               (clef->kind == MUSIC_CLEF_BASS ? 4 :
                clef->kind == MUSIC_CLEF_ALTO ? 3 : 2);
    music_sp_t y = staff_top + (5 - line);
    /* Optical correction for the Leland G-clef at 18px staffSpace: its
     * visual mass sits about 10px too low despite the nominal line-2 anchor. */
    if (clef->kind == MUSIC_CLEF_TREBLE) y -= 0.84f;
    return y;
}

static int key_staff_position(music_clef_kind_t clef, bool flat, int order)
{
    static const int treble_sharp[7] = {8, 5, 9, 6, 3, 7, 4};
    static const int treble_flat[7]  = {4, 7, 3, 6, 2, 5, 1};
    static const int bass_sharp[7]   = {6, 3, 7, 4, 1, 5, 2};
    static const int bass_flat[7]    = {2, 5, 1, 4, 0, 3, -1};
    static const int alto_sharp[7]   = {7, 4, 8, 5, 2, 6, 3};
    static const int alto_flat[7]    = {3, 6, 2, 5, 1, 4, 0};
    if (order < 0) order = 0;
    if (order > 6) order = 6;
    if (clef == MUSIC_CLEF_BASS) return flat ? bass_flat[order] : bass_sharp[order];
    if (clef == MUSIC_CLEF_ALTO) return flat ? alto_flat[order] : alto_sharp[order];
    return flat ? treble_flat[order] : treble_sharp[order];
}

static music_sp_t render_key_signature(music_scene_t *scene,
                                       const music_key_signature_t *key,
                                       const music_clef_t *clef,
                                       music_sp_t x, music_sp_t staff_top)
{
    int cancel_count = key->cancel_fifths < 0 ? -key->cancel_fifths :
                                                key->cancel_fifths;
    bool cancel_flat = key->cancel_fifths < 0;
    for (int i = 0; i < cancel_count; ++i) {
        int position = key_staff_position(clef->kind, cancel_flat, i);
        add_glyph(scene, SMUFL_GLYPH_ACCIDENTAL_NATURAL, x + i * 0.95f,
                  staff_top + 4.0f - position * 0.5f, 1.0f, -1);
    }
    x += cancel_count * 0.95f;
    int count = key->fifths < 0 ? -key->fifths : key->fifths;
    bool flat = key->fifths < 0;
    smufl_glyph_id_t glyph = flat ? SMUFL_GLYPH_ACCIDENTAL_FLAT :
                                    SMUFL_GLYPH_ACCIDENTAL_SHARP;
    for (int i = 0; i < count; ++i) {
        int position = key_staff_position(clef->kind, flat, i);
        add_glyph(scene, glyph, x + i * 0.95f,
                  staff_top + 4.0f - position * 0.5f, 1.0f, -1);
    }
    return x + count * 0.95f;
}

static music_sp_t render_time_signature(music_scene_t *scene,
                                        const music_time_signature_t *time,
                                        music_sp_t x, music_sp_t staff_top)
{
    if (time->symbol == MUSIC_TIME_COMMON) {
        add_glyph(scene, SMUFL_GLYPH_TIME_COMMON, x, staff_top + 2.0f, 1.0f, -1);
        return x + 2.0f;
    }
    if (time->symbol == MUSIC_TIME_CUT) {
        add_glyph(scene, SMUFL_GLYPH_TIME_CUT, x, staff_top + 2.0f, 1.0f, -1);
        return x + 2.0f;
    }

    int beats = time->beats ? time->beats : 4;
    int beat_type = time->beat_type ? time->beat_type : 4;
    int numerator_digits[3], denominator_digits[3];
    int n_count = 0, d_count = 0;
    do { numerator_digits[n_count++] = beats % 10; beats /= 10; } while (beats && n_count < 3);
    do { denominator_digits[d_count++] = beat_type % 10; beat_type /= 10; } while (beat_type && d_count < 3);
    int max_count = n_count > d_count ? n_count : d_count;
    const music_sp_t digit_advance = 1.60f;
    for (int i = 0; i < n_count; ++i) {
        int digit = numerator_digits[n_count - 1 - i];
        add_glyph(scene, (smufl_glyph_id_t)(SMUFL_GLYPH_TIME_0 + digit),
                  x + (max_count - n_count) * digit_advance * 0.5f +
                  i * digit_advance,
                  staff_top + 1.0f, 1.0f, -1);
    }
    for (int i = 0; i < d_count; ++i) {
        int digit = denominator_digits[d_count - 1 - i];
        add_glyph(scene, (smufl_glyph_id_t)(SMUFL_GLYPH_TIME_0 + digit),
                  x + (max_count - d_count) * digit_advance * 0.5f +
                  i * digit_advance,
                  staff_top + 3.0f, 1.0f, -1);
    }
    return x + max_count * digit_advance + 0.45f;
}

static music_sp_t measure_ideal_width(const music_score_t *score,
                                      const music_measure_t *measure,
                                      const music_layout_config_t *config)
{
    int columns = 0;
    music_sp_t duration_weight = 0.0f;
    for (int i = 0; i < measure->event_count; ++i) {
        const music_event_t *event = &score->events[measure->event_start + i];
        if (event->kind != MUSIC_EVENT_NOTE && event->kind != MUSIC_EVENT_REST) continue;

        /* MusicXML stores voices sequentially and rewinds time with <backup>.
         * Count a simultaneous multi-voice onset as one spacing column. */
        bool onset_seen = false;
        for (int j = 0; j < i; ++j) {
            const music_event_t *previous = &score->events[measure->event_start + j];
            if ((previous->kind == MUSIC_EVENT_NOTE ||
                 previous->kind == MUSIC_EVENT_REST) &&
                previous->onset_divisions == event->onset_divisions) {
                onset_seen = true;
                break;
            }
        }
        if (onset_seen) continue;

        columns++;
        music_sp_t column_weight = 0.0f;
        for (int j = i; j < measure->event_count; ++j) {
            const music_event_t *simultaneous =
                &score->events[measure->event_start + j];
            if ((simultaneous->kind != MUSIC_EVENT_NOTE &&
                 simultaneous->kind != MUSIC_EVENT_REST) ||
                simultaneous->onset_divisions != event->onset_divisions)
                continue;
            music_duration_kind_t type = simultaneous->kind == MUSIC_EVENT_NOTE ?
                                         simultaneous->data.note.type :
                                         simultaneous->data.rest.type;
            music_sp_t weight = type == MUSIC_DURATION_WHOLE ? 2.8f :
                                type == MUSIC_DURATION_HALF ? 2.3f :
                                type == MUSIC_DURATION_QUARTER ? 1.9f :
                                type == MUSIC_DURATION_EIGHTH ? 1.55f : 1.35f;
            if (weight > column_weight) column_weight = weight;
        }
        duration_weight += column_weight;
    }
    music_sp_t by_columns = 3.0f + columns * config->minimum_note_spacing;
    music_sp_t by_duration = 2.5f + duration_weight;
    return by_columns > by_duration ? by_columns : by_duration;
}

static uint8_t rhythmic_event_voice(const music_event_t *event)
{
    uint8_t voice = event->kind == MUSIC_EVENT_NOTE ? event->data.note.voice :
                    event->kind == MUSIC_EVENT_REST ? event->data.rest.voice : 1;
    return voice ? voice : 1;
}

static uint8_t rhythmic_event_staff(const music_event_t *event)
{
    uint8_t staff = event->kind == MUSIC_EVENT_NOTE ? event->data.note.staff :
                    event->kind == MUSIC_EVENT_REST ? event->data.rest.staff : 1;
    return staff ? staff : 1;
}

static bool measure_staff_voice_range(const music_score_t *score,
                                      const music_measure_t *measure,
                                      uint8_t staff, uint8_t *upper_voice,
                                      uint8_t *lower_voice)
{
    uint8_t first = 0;
    uint8_t last = 0;
    for (int i = 0; i < measure->event_count; ++i) {
        const music_event_t *event = &score->events[measure->event_start + i];
        if ((event->kind != MUSIC_EVENT_NOTE && event->kind != MUSIC_EVENT_REST) ||
            rhythmic_event_staff(event) != staff)
            continue;
        uint8_t voice = rhythmic_event_voice(event);
        if (!first || voice < first) first = voice;
        if (voice > last) last = voice;
    }
    if (upper_voice) *upper_voice = first ? first : 1;
    if (lower_voice) *lower_voice = last ? last : 1;
    return first && last != first;
}

static music_stem_direction_t resolved_stem_direction(
    const music_score_t *score, const music_measure_t *measure,
    const music_event_t *event, int staff_position)
{
    const music_note_t *note = &event->data.note;
    if (note->stem != MUSIC_STEM_AUTO) return note->stem;

    uint8_t upper_voice = 1;
    if (measure_staff_voice_range(score, measure,
                                  note->staff ? note->staff : 1,
                                  &upper_voice, NULL)) {
        /* MusicXML voice 1 (or the lowest numbered voice) is the upper
         * voice. Professional two-voice engraving fixes its stems upward
         * and the lower voice's stems downward, independent of pitch. */
        return (note->voice ? note->voice : 1) == upper_voice ?
               MUSIC_STEM_UP : MUSIC_STEM_DOWN;
    }
    return staff_position < 4 ? MUSIC_STEM_UP : MUSIC_STEM_DOWN;
}

static bool note_matches_chord(const music_event_t *candidate,
                               const music_event_t *reference)
{
    if (candidate->kind != MUSIC_EVENT_NOTE ||
        reference->kind != MUSIC_EVENT_NOTE ||
        candidate->onset_divisions != reference->onset_divisions)
        return false;
    const music_note_t *a = &candidate->data.note;
    const music_note_t *b = &reference->data.note;
    return (a->staff ? a->staff : 1) == (b->staff ? b->staff : 1) &&
           (a->voice ? a->voice : 1) == (b->voice ? b->voice : 1);
}

static music_stem_direction_t resolved_chord_stem_direction(
    const music_score_t *score, const music_measure_t *measure,
    const music_event_t *event, const music_clef_t *clef)
{
    int min_position = 127;
    int max_position = -127;
    music_stem_direction_t explicit_stem = MUSIC_STEM_AUTO;
    for (int i = 0; i < measure->event_count; ++i) {
        const music_event_t *candidate =
            &score->events[measure->event_start + i];
        if (!note_matches_chord(candidate, event)) continue;
        int position = music_pitch_staff_position(&candidate->data.note.pitch,
                                                  clef);
        if (position < min_position) min_position = position;
        if (position > max_position) max_position = position;
        if (explicit_stem == MUSIC_STEM_AUTO &&
            candidate->data.note.stem != MUSIC_STEM_AUTO)
            explicit_stem = candidate->data.note.stem;
    }
    if (explicit_stem != MUSIC_STEM_AUTO) return explicit_stem;
    int middle = min_position <= max_position ?
                 (min_position + max_position) / 2 : 4;
    return resolved_stem_direction(score, measure, event, middle);
}

typedef struct {
    int position;
    int order;
} chord_position_t;

static int compare_chord_position(const void *left, const void *right)
{
    const chord_position_t *a = left;
    const chord_position_t *b = right;
    if (a->position != b->position)
        return (a->position > b->position) - (a->position < b->position);
    return (a->order > b->order) - (a->order < b->order);
}

static music_sp_t chord_notehead_offset(const music_score_t *score,
                                        const music_measure_t *measure,
                                        const music_event_t *event,
                                        const music_clef_t *clef,
                                        music_stem_direction_t stem)
{
    chord_position_t positions[32];
    int count = 0;
    int target_order = 0;
    for (int i = 0; i < measure->event_count && count < 32; ++i) {
        const music_event_t *candidate =
            &score->events[measure->event_start + i];
        if (!note_matches_chord(candidate, event)) continue;
        positions[count] = (chord_position_t){
            .position = music_pitch_staff_position(&candidate->data.note.pitch,
                                                   clef),
            .order = i,
        };
        if (candidate == event) target_order = i;
        count++;
    }
    if (count < 2) return 0.0f;
    qsort(positions, (size_t)count, sizeof(positions[0]),
          compare_chord_position);

    int rank = 0;
    bool collision = false;
    for (int i = 0; i < count; ++i) {
        if (positions[i].order == target_order) {
            rank = i;
            if ((i > 0 && positions[i].position - positions[i - 1].position <= 1) ||
                (i + 1 < count && positions[i + 1].position -
                                  positions[i].position <= 1))
                collision = true;
            break;
        }
    }
    if (!collision) return 0.0f;
    bool displace = stem == MUSIC_STEM_UP ? (rank & 1) != 0 :
                    ((count - 1 - rank) & 1) != 0;
    return displace ? (stem == MUSIC_STEM_UP ? 0.62f : -0.62f) : 0.0f;
}

static void chord_note_geometry(const music_score_t *score,
                                const music_measure_t *measure,
                                const music_event_t *event, int event_index,
                                const music_clef_t *clef, music_sp_t base_x,
                                music_sp_t staff_top,
                                music_stem_direction_t stem,
                                chord_note_geometry_t *geometry)
{
    music_sp_t chord_left = base_x;
    music_sp_t chord_right = base_x;
    music_sp_t chord_top = 10000.0f;
    music_sp_t chord_bottom = -10000.0f;
    int position = music_pitch_staff_position(&event->data.note.pitch, clef);
    music_sp_t y = staff_top + 4.0f - position * 0.5f;
    music_sp_t offset = chord_notehead_offset(score, measure, event, clef, stem);
    music_sp_t head_x = base_x + offset;
    music_sp_t head_width = leland_glyph_width(
        smufl_notehead_glyph(event->data.note.type));
    if (head_width <= 0.0f) head_width = 1.3f;

    int accidental_column = 0;
    int event_offset = event_index - measure->event_start;
    for (int i = 0; i < measure->event_count; ++i) {
        int candidate_index = measure->event_start + i;
        const music_event_t *candidate = &score->events[candidate_index];
        if (!note_matches_chord(candidate, event)) continue;
        int candidate_position = music_pitch_staff_position(
            &candidate->data.note.pitch, clef);
        music_sp_t candidate_y = staff_top + 4.0f - candidate_position * 0.5f;
        music_sp_t candidate_offset = chord_notehead_offset(
            score, measure, candidate, clef, stem);
        music_sp_t candidate_x = base_x + candidate_offset;
        music_sp_t width = leland_glyph_width(
            smufl_notehead_glyph(candidate->data.note.type));
        if (width <= 0.0f) width = 1.3f;
        music_sp_t left = candidate_x - width * 0.5f;
        music_sp_t right = candidate_x + width * 0.5f;
        if (left < chord_left) chord_left = left;
        if (right > chord_right) chord_right = right;
        if (candidate_y < chord_top) chord_top = candidate_y;
        if (candidate_y > chord_bottom) chord_bottom = candidate_y;

        if (i < event_offset && event->data.note.accidental != MUSIC_ACCIDENTAL_NONE &&
            candidate->data.note.accidental != MUSIC_ACCIDENTAL_NONE) {
            int distance = candidate_position - position;
            if (distance < 0) distance = -distance;
            if (distance <= 4) accidental_column++;
        }
    }

    music_sp_t dot_y = (position & 1) ? y : y - 0.5f;
    for (int i = 0; i < event_offset; ++i) {
        const music_event_t *candidate = &score->events[measure->event_start + i];
        if (!note_matches_chord(candidate, event) || !candidate->data.note.dots)
            continue;
        int candidate_position = music_pitch_staff_position(
            &candidate->data.note.pitch, clef);
        music_sp_t candidate_y = staff_top + 4.0f - candidate_position * 0.5f;
        music_sp_t candidate_dot_y = (candidate_position & 1) ?
                                     candidate_y : candidate_y - 0.5f;
        music_sp_t delta = dot_y - candidate_dot_y;
        if (delta < 0.0f) delta = -delta;
        if (delta < 0.35f) dot_y += (position >= candidate_position) ? -0.5f : 0.5f;
    }

    bool stem_owner = true;
    for (int i = 0; i < measure->event_count; ++i) {
        int candidate_index = measure->event_start + i;
        const music_event_t *candidate = &score->events[candidate_index];
        if (!note_matches_chord(candidate, event) || candidate_index == event_index)
            continue;
        int candidate_position = music_pitch_staff_position(
            &candidate->data.note.pitch, clef);
        music_sp_t candidate_y = staff_top + 4.0f - candidate_position * 0.5f;
        bool better = stem == MUSIC_STEM_UP ? candidate_y > y : candidate_y < y;
        if (better || (candidate_y == y && candidate_index < event_index)) {
            stem_owner = false;
            break;
        }
    }

    *geometry = (chord_note_geometry_t){
        .head_x = head_x,
        .head_left = head_x - head_width * 0.5f,
        .head_right = head_x + head_width * 0.5f,
        .chord_left = chord_left,
        .chord_right = chord_right,
        .chord_top = chord_top,
        .chord_bottom = chord_bottom,
        .accidental_x = chord_left - 0.55f - accidental_column * 0.82f,
        .dot_x = chord_right + 0.42f,
        .dot_y = dot_y,
        .stem_x = stem == MUSIC_STEM_UP ? chord_right - 0.08f : chord_left + 0.08f,
        .stem_start_y = stem == MUSIC_STEM_UP ? chord_bottom : chord_top,
        .stem_end_y = stem == MUSIC_STEM_UP ? chord_top - 3.5f : chord_bottom + 3.5f,
        .stem = stem,
        .stem_owner = stem_owner,
    };
}

static music_sp_t polyphonic_notehead_offset(
    const music_score_t *score, const music_measure_t *measure,
    const music_event_t *event, const music_clef_t *clef,
    music_stem_direction_t stem)
{
    const music_note_t *note = &event->data.note;
    int position = music_pitch_staff_position(&note->pitch, clef);
    uint8_t staff = note->staff ? note->staff : 1;
    uint8_t voice = note->voice ? note->voice : 1;

    for (int i = 0; i < measure->event_count; ++i) {
        const music_event_t *other = &score->events[measure->event_start + i];
        if (other == event || other->kind != MUSIC_EVENT_NOTE ||
            other->onset_divisions != event->onset_divisions ||
            (other->data.note.staff ? other->data.note.staff : 1) != staff ||
            (other->data.note.voice ? other->data.note.voice : 1) == voice)
            continue;
        int other_position = music_pitch_staff_position(&other->data.note.pitch,
                                                        clef);
        int distance = other_position - position;
        if (distance < 0) distance = -distance;
        if (distance <= 1) {
            /* For opposing stems, put the up-stem head on the right and the
             * down-stem head on the left. This keeps both stems on the outer
             * edges and resolves unisons and seconds without raw-pixel hacks. */
            return stem == MUSIC_STEM_UP ? 0.38f : -0.38f;
        }
    }
    return 0.0f;
}

static bool note_owns_ledger_lines(const music_score_t *score,
                                   const music_measure_t *measure,
                                   int event_index,
                                   const music_clef_t *clef,
                                   int position)
{
    if (position > -2 && position < 10) return false;

    const music_event_t *event = &score->events[event_index];
    uint8_t staff = event->data.note.staff ? event->data.note.staff : 1;
    for (int i = 0; i < measure->event_count; ++i) {
        int other_index = measure->event_start + i;
        if (other_index == event_index) continue;
        const music_event_t *other = &score->events[other_index];
        if (other->kind != MUSIC_EVENT_NOTE ||
            other->onset_divisions != event->onset_divisions ||
            (other->data.note.staff ? other->data.note.staff : 1) != staff)
            continue;

        int other_position = music_pitch_staff_position(&other->data.note.pitch,
                                                        clef);
        bool farther = position <= -2 ? other_position < position :
                                        other_position > position;
        if (farther || (other_position == position && other_index < event_index))
            return false;
    }
    return true;
}

static bool clef_equal(const music_clef_t *a, const music_clef_t *b)
{
    return a->kind == b->kind && a->line == b->line &&
           a->octave_change == b->octave_change;
}

static bool key_equal(const music_key_signature_t *a,
                      const music_key_signature_t *b)
{
    return a->fifths == b->fifths && a->cancel_fifths == b->cancel_fifths &&
           a->minor == b->minor;
}

static bool time_equal(const music_time_signature_t *a,
                       const music_time_signature_t *b)
{
    return a->beats == b->beats && a->beat_type == b->beat_type &&
           a->symbol == b->symbol;
}

static bool draw_staff(music_scene_t *scene, music_sp_t x1, music_sp_t x2,
                       music_sp_t top, music_sp_t thickness)
{
    for (int line = 0; line < 5; ++line) {
        if (!add_line(scene, x1, top + line, x2, top + line, thickness, -1))
            return false;
    }
    return true;
}

static bool draw_ledger_lines(music_scene_t *scene, int staff_position,
                              music_sp_t x, music_sp_t staff_top,
                              const music_layout_config_t *config,
                              int event_index)
{
    if (staff_position <= -2) {
        for (int p = -2; p >= staff_position; p -= 2) {
            music_sp_t y = staff_top + 4.0f - p * 0.5f;
            if (!add_line(scene, x - 0.75f - config->ledger_line_extension,
                          y, x + 0.75f + config->ledger_line_extension, y,
                          config->ledger_line_thickness, event_index)) return false;
        }
    } else if (staff_position >= 10) {
        for (int p = 10; p <= staff_position; p += 2) {
            music_sp_t y = staff_top + 4.0f - p * 0.5f;
            if (!add_line(scene, x - 0.75f - config->ledger_line_extension,
                          y, x + 0.75f + config->ledger_line_extension, y,
                          config->ledger_line_thickness, event_index)) return false;
        }
    }
    return true;
}

static bool render_articulations(music_scene_t *scene, const music_note_t *note,
                                 music_sp_t x, music_sp_t y,
                                 music_stem_direction_t stem, int event_index)
{
    bool below = stem == MUSIC_STEM_UP;
    music_sp_t ay = y + (below ? 1.35f : -1.35f);
    if (note->articulations & MUSIC_ARTIC_STACCATO) {
        smufl_glyph_id_t glyph = below ? SMUFL_GLYPH_ARTIC_STACCATO_BELOW :
                                         SMUFL_GLYPH_ARTIC_STACCATO_ABOVE;
        if (!add_centered_glyph(scene, glyph, x, ay, 1.0f,
                                event_index)) return false;
        ay += below ? 0.65f : -0.65f;
    }
    if (note->articulations & MUSIC_ARTIC_TENUTO) {
        smufl_glyph_id_t glyph = below ? SMUFL_GLYPH_ARTIC_TENUTO_BELOW :
                                         SMUFL_GLYPH_ARTIC_TENUTO_ABOVE;
        if (!add_centered_glyph(scene, glyph, x, ay, 1.0f,
                                event_index)) return false;
        ay += below ? 0.75f : -0.75f;
    }
    if (note->articulations & MUSIC_ARTIC_ACCENT) {
        smufl_glyph_id_t glyph = below ? SMUFL_GLYPH_ARTIC_ACCENT_BELOW :
                                         SMUFL_GLYPH_ARTIC_ACCENT_ABOVE;
        if (!add_centered_glyph(scene, glyph, x, ay, 1.0f,
                                event_index)) return false;
        ay += below ? 0.85f : -0.85f;
    }
    if (note->articulations & MUSIC_ARTIC_FERMATA) {
        smufl_glyph_id_t glyph = below ? SMUFL_GLYPH_FERMATA_BELOW :
                                         SMUFL_GLYPH_FERMATA_ABOVE;
        if (!add_centered_glyph(scene, glyph, x, ay, 1.0f,
                                event_index)) return false;
    }
    return true;
}

static bool render_barline(music_scene_t *scene, music_barline_t type,
                           music_sp_t x, music_sp_t top, music_sp_t bottom,
                           const music_layout_config_t *config)
{
    music_sp_t thin = config->staff_line_thickness * 1.25f;
    switch (type) {
    case MUSIC_BARLINE_DOUBLE:
        return add_line(scene, x - 0.35f, top, x - 0.35f, bottom, thin, -1) &&
               add_line(scene, x, top, x, bottom, thin, -1);
    case MUSIC_BARLINE_FINAL:
        return add_line(scene, x - 0.45f, top, x - 0.45f, bottom, thin, -1) &&
               add_line(scene, x, top, x, bottom, 0.34f, -1);
    case MUSIC_BARLINE_REPEAT_START:
        return add_line(scene, x, top, x, bottom, 0.34f, -1) &&
               add_line(scene, x + 0.45f, top, x + 0.45f, bottom, thin, -1) &&
               add_glyph(scene, SMUFL_GLYPH_REPEAT_DOTS, x + 0.75f,
                         top + 2.0f, 1.0f, -1);
    case MUSIC_BARLINE_REPEAT_END:
        return add_glyph(scene, SMUFL_GLYPH_REPEAT_DOTS, x - 1.0f,
                         top + 2.0f, 1.0f, -1) &&
               add_line(scene, x - 0.45f, top, x - 0.45f, bottom, thin, -1) &&
               add_line(scene, x, top, x, bottom, 0.34f, -1);
    default:
        return add_line(scene, x, top, x, bottom, thin, -1);
    }
}

static bool render_note_symbol(music_scene_t *scene, const music_event_t *event,
                               int event_index, const music_clef_t *clef,
                               music_sp_t x, music_sp_t staff_top,
                               const music_layout_config_t *config,
                               bool draw_ledger,
                               const chord_note_geometry_t *geometry,
                               note_layout_t *layout)
{
    const music_note_t *note = &event->data.note;
    int position = music_pitch_staff_position(&note->pitch, clef);
    music_sp_t y = staff_top + 4.0f - position * 0.5f;
    music_sp_t head_x = geometry->head_x;

    if (draw_ledger &&
        !draw_ledger_lines(scene, position, head_x, staff_top, config, event_index))
        return false;
    smufl_glyph_id_t accidental = smufl_accidental_glyph(note->accidental);
    if (accidental < SMUFL_GLYPH_COUNT &&
        !add_glyph(scene, accidental, geometry->accidental_x, y, 1.0f,
                   event_index)) return false;
    if (!add_centered_glyph(scene, smufl_notehead_glyph(note->type),
                            head_x, y, 1.0f, event_index)) return false;

    for (int dot = 0; dot < note->dots; ++dot) {
        if (!add_glyph(scene, SMUFL_GLYPH_AUGMENTATION_DOT,
                       geometry->dot_x + dot * 0.55f, geometry->dot_y, 1.0f,
                       event_index)) return false;
    }
    if (!render_articulations(scene, note, head_x, y, geometry->stem,
                              event_index)) return false;

    *layout = (note_layout_t){
        .event = event,
        .event_index = event_index,
        .x = head_x,
        .y = y,
        .stem_x = geometry->stem_x,
        .stem_start_y = geometry->stem_start_y,
        .stem_end_y = geometry->stem_end_y,
        .chord_left = geometry->chord_left,
        .chord_right = geometry->chord_right,
        .chord_top = geometry->chord_top,
        .chord_bottom = geometry->chord_bottom,
        .stem = geometry->stem,
        .staff = note->staff ? note->staff : 1,
        .has_stem = geometry->stem_owner && geometry->stem != MUSIC_STEM_NONE &&
                    note->type != MUSIC_DURATION_WHOLE,
    };
    return true;
}

static bool render_unbeamed_stem(music_scene_t *scene, note_layout_t *layout,
                                 const music_layout_config_t *config)
{
    if (!layout->has_stem) return true;
    if (!add_line(scene, layout->stem_x, layout->stem_start_y,
                  layout->stem_x, layout->stem_end_y,
                  config->stem_thickness, layout->event_index)) return false;
    smufl_glyph_id_t flag = smufl_flag_glyph(layout->event->data.note.type,
                                             layout->stem);
    if (flag < SMUFL_GLYPH_COUNT) {
        if (!add_glyph(scene, flag, layout->stem_x, layout->stem_end_y,
                       1.0f, layout->event_index)) return false;
    }
    return true;
}

static const music_note_t *chord_rhythm_note(note_layout_t *notes, int count,
                                             int index)
{
    const music_event_t *event = notes[index].event;
    const music_note_t *fallback = &event->data.note;
    for (int i = 0; i < count; ++i) {
        const music_event_t *candidate = notes[i].event;
        if (candidate->measure_index != event->measure_index ||
            candidate->onset_divisions != event->onset_divisions ||
            notes[i].staff != notes[index].staff ||
            (candidate->data.note.voice ? candidate->data.note.voice : 1) !=
                (fallback->voice ? fallback->voice : 1))
            continue;
        if (!candidate->data.note.chord) return &candidate->data.note;
    }
    return fallback;
}

static int chord_stem_owner_index(note_layout_t *notes, int count, int index)
{
    const music_event_t *event = notes[index].event;
    uint8_t voice = event->data.note.voice ? event->data.note.voice : 1;
    for (int i = 0; i < count; ++i) {
        const music_event_t *candidate = notes[i].event;
        if (notes[i].has_stem &&
            candidate->measure_index == event->measure_index &&
            candidate->onset_divisions == event->onset_divisions &&
            notes[i].staff == notes[index].staff &&
            (candidate->data.note.voice ? candidate->data.note.voice : 1) ==
                voice)
            return i;
    }
    return index;
}

static bool render_beams(music_scene_t *scene, note_layout_t *notes, int count,
                         const music_layout_config_t *config)
{
    for (int i = 0; i < count; ++i) {
        if (notes[i].beamed || !notes[i].has_stem) continue;
        const music_note_t *first_note = chord_rhythm_note(notes, count, i);
        if (first_note->beams[0] != MUSIC_BEAM_BEGIN) continue;

        int end = -1;
        for (int j = i + 1; j < count; ++j) {
            const music_note_t *candidate = chord_rhythm_note(notes, count, j);
            if (notes[j].staff != notes[i].staff || candidate->voice != first_note->voice) continue;
            if (candidate->beams[0] == MUSIC_BEAM_END) {
                end = chord_stem_owner_index(notes, count, j);
                break;
            }
        }
        if (end < 0) continue;

        music_stem_direction_t stem = notes[i].stem;
        for (int j = i; j <= end; ++j) {
            if (!notes[j].has_stem || notes[j].staff != notes[i].staff ||
                chord_rhythm_note(notes, count, j)->voice != first_note->voice)
                continue;
            notes[j].stem = stem;
            notes[j].stem_x = stem == MUSIC_STEM_UP ?
                              notes[j].chord_right - 0.08f :
                              notes[j].chord_left + 0.08f;
            notes[j].stem_start_y = stem == MUSIC_STEM_UP ?
                                    notes[j].chord_bottom :
                                    notes[j].chord_top;
            notes[j].stem_end_y = stem == MUSIC_STEM_UP ?
                                  notes[j].chord_top - 3.5f :
                                  notes[j].chord_bottom + 3.5f;
        }
        music_sp_t dx = notes[end].stem_x - notes[i].stem_x;
        music_sp_t raw_slope = dx != 0.0f ?
            (notes[end].stem_end_y - notes[i].stem_end_y) / dx : 0.0f;
        if (raw_slope > 0.25f) raw_slope = 0.25f;
        if (raw_slope < -0.25f) raw_slope = -0.25f;

        music_sp_t beam_y = notes[i].stem_end_y;
        for (int j = i; j <= end; ++j) {
            if (!notes[j].has_stem || notes[j].staff != notes[i].staff ||
                notes[j].event->data.note.voice != first_note->voice) continue;
            music_sp_t target = beam_y + raw_slope * (notes[j].stem_x - notes[i].stem_x);
            if (stem == MUSIC_STEM_UP && target > notes[j].stem_end_y)
                target = notes[j].stem_end_y;
            if (stem == MUSIC_STEM_DOWN && target < notes[j].stem_end_y)
                target = notes[j].stem_end_y;
            notes[j].stem_end_y = target;
            notes[j].beamed = true;
            if (!add_line(scene, notes[j].stem_x, notes[j].stem_start_y,
                          notes[j].stem_x, target, config->stem_thickness,
                          notes[j].event_index)) return false;
        }
        music_sp_t beam_end_y = beam_y + raw_slope * dx;
        if (!add_line(scene, notes[i].stem_x, beam_y,
                      notes[end].stem_x, beam_end_y,
                      config->beam_thickness, notes[i].event_index)) return false;

        for (int level = 1; level < MUSIC_MAX_BEAM_LEVELS; ++level) {
            bool level_present = false;
            for (int j = i; j <= end; ++j) {
                if (notes[j].staff == notes[i].staff &&
                    chord_rhythm_note(notes, count, j)->voice == first_note->voice &&
                    chord_rhythm_note(notes, count, j)->beams[level] !=
                        MUSIC_BEAM_NONE) {
                    level_present = true;
                    break;
                }
            }
            if (!level_present) continue;
            music_sp_t offset = (stem == MUSIC_STEM_UP ? 0.78f : -0.78f) * level;
            if (!add_line(scene, notes[i].stem_x, beam_y + offset,
                          notes[end].stem_x, beam_end_y + offset,
                          config->beam_thickness, notes[i].event_index)) return false;
        }
    }

    for (int i = 0; i < count; ++i) {
        if (!notes[i].beamed && !render_unbeamed_stem(scene, &notes[i], config))
            return false;
    }
    return true;
}

static bool add_curved_connection_segment(music_scene_t *scene,
                                          music_sp_t x1, music_sp_t y1,
                                          music_sp_t x2, music_sp_t y2,
                                          bool above, int event_index)
{
    if (x2 <= x1) return true;
    music_sp_t arch = above ? -1.25f : 1.25f;
    return add_bezier(scene, x1, y1,
                      x1 + (x2 - x1) * 0.33f, y1 + arch,
                      x1 + (x2 - x1) * 0.67f, y2 + arch,
                      x2, y2, 0.10f, event_index);
}

static bool render_connection(music_scene_t *scene,
                              const music_layout_config_t *config,
                              const note_layout_t *start_layout,
                              const note_layout_t *end_layout,
                              bool gliss)
{
    bool above = start_layout->stem == MUSIC_STEM_DOWN;
    music_sp_t side = above ? -0.75f : 0.75f;
    music_sp_t x1 = start_layout->x + 0.55f;
    music_sp_t x2 = end_layout->x - 0.55f;
    music_sp_t y1 = start_layout->y + side;
    music_sp_t y2 = end_layout->y + side;
    int source = start_layout->event_index;
    if (start_layout->system_index == end_layout->system_index) {
        return gliss ? add_line(scene, x1, start_layout->y, x2,
                                end_layout->y, 0.11f, source) :
                       add_curved_connection_segment(scene, x1, y1, x2, y2,
                                                     above, source);
    }

    music_sp_t right = config->page_width - config->right_margin - 0.25f;
    music_sp_t left = config->left_margin + 4.75f;
    if (gliss) {
        if (!add_line(scene, x1, start_layout->y, right,
                      start_layout->y, 0.11f, source)) return false;
    } else if (!add_curved_connection_segment(scene, x1, y1, right, y1,
                                               above, source)) return false;

    for (int system = start_layout->system_index + 1;
         system < end_layout->system_index; ++system) {
        music_sp_t middle_y = scene->systems[system].top + 2.0f +
            (start_layout->staff > 1 ? 4.0f + config->staff_gap : 0.0f);
        if (gliss) {
            if (!add_line(scene, left, middle_y, right, middle_y,
                          0.11f, source)) return false;
        } else if (!add_curved_connection_segment(scene, left, middle_y + side,
                                                   right, middle_y + side,
                                                   above, source)) return false;
    }

    music_sp_t incoming_left = x2 - 3.0f;
    if (incoming_left < left) incoming_left = left;
    if (gliss)
        return add_line(scene, incoming_left, end_layout->y, x2,
                        end_layout->y, 0.11f, source);
    return add_curved_connection_segment(scene, incoming_left, y2, x2, y2,
                                         above, source);
}

static bool render_connections(music_scene_t *scene,
                               const music_layout_config_t *config,
                               const music_score_t *score,
                               const note_layout_t *layouts)
{
    for (int i = 0; i < score->event_count; ++i) {
        if (!layouts[i].valid) continue;
        const music_note_t *start = &score->events[i].data.note;
        if (!(start->tie_flags & MUSIC_TIE_START) && start->slur_start == 0 &&
            start->gliss_start == 0) continue;
        bool tie_done = !(start->tie_flags & MUSIC_TIE_START);
        bool slur_done = start->slur_start == 0;
        bool gliss_done = start->gliss_start == 0;
        for (int j = i + 1; j < score->event_count; ++j) {
            if (!layouts[j].valid) continue;
            const music_note_t *end = &score->events[j].data.note;
            if (layouts[j].staff != layouts[i].staff ||
                end->voice != start->voice)
                continue;
            bool tie = !tie_done &&
                       (end->tie_flags & MUSIC_TIE_STOP) &&
                       start->pitch.step == end->pitch.step &&
                       start->pitch.octave == end->pitch.octave &&
                       start->pitch.alter == end->pitch.alter;
            bool slur = !slur_done &&
                        end->slur_stop == start->slur_start;
            bool gliss = !gliss_done &&
                         end->gliss_stop == start->gliss_start;
            if (!tie && !slur && !gliss) continue;
            if (tie && !render_connection(scene, config, &layouts[i],
                                          &layouts[j], false)) return false;
            if (slur && !render_connection(scene, config, &layouts[i],
                                           &layouts[j], false)) return false;
            if (gliss && !render_connection(scene, config, &layouts[i],
                                            &layouts[j], true)) return false;
            tie_done |= tie;
            slur_done |= slur;
            gliss_done |= gliss;
            if (tie_done && slur_done && gliss_done) break;
        }
    }
    return true;
}

static bool music_layout_build_range(const music_score_t *score,
                                     const music_layout_config_t *config,
                                     music_scene_t *scene,
                                     int range_start, int range_end,
                                     char *error,
                                     size_t error_size)
{
    if (!score || !config || !scene) return set_error(error, error_size, "invalid layout arguments");
    memset(scene, 0, sizeof(*scene));
    scene->width = config->page_width;
    if (score->part_count == 0) return set_error(error, error_size, "score has no parts");

    /* This workspace used to be a local array (~8 KiB). music_layout_build()
     * is normally called by taskLVGL, whose complete stack is only 7 KiB,
     * so entering the renderer immediately tripped the FreeRTOS stack
     * protector. Keep large, score-dependent scratch storage off task stacks. */
    note_layout_t *notes = calloc(MAX_NOTES_PER_SYSTEM, sizeof(*notes));
    note_layout_t *all_notes = calloc(score->event_count ? score->event_count : 1,
                                      sizeof(*all_notes));
    if (!notes || !all_notes) {
        free(notes);
        free(all_notes);
        return set_error(error, error_size, "unable to allocate layout workspace");
    }

#define LAYOUT_ERROR(message) do {                \
        free(notes);                              \
        free(all_notes);                          \
        return set_error(error, error_size, message); \
    } while (0)

    const music_part_t *part = &score->parts[0];
    int part_start = part->measure_start;
    int part_end = part->measure_start + part->measure_count;
    int measure_cursor = range_start >= part_start ? range_start : part_start;
    int measure_end = range_end > measure_cursor && range_end < part_end ?
                      range_end : part_end;
    music_sp_t content_width = config->page_width - config->left_margin - config->right_margin;
    music_sp_t system_top = config->top_margin;

    while (measure_cursor < measure_end) {
        int system_measure_start = measure_cursor;
        int system_item_start = scene->item_count;
        music_sp_t current_system_top = system_top;
        const music_measure_t *first = &score->measures[measure_cursor];
        uint8_t staff_count = first->staff_count ? first->staff_count : 1;
        if (staff_count > MUSIC_MAX_STAVES) staff_count = MUSIC_MAX_STAVES;

        int header_accidentals = first->key.fifths < 0 ? -first->key.fifths :
                                 first->key.fifths;
        header_accidentals += first->key.cancel_fifths < 0 ?
                              -first->key.cancel_fifths :
                              first->key.cancel_fifths;
        music_sp_t header_width = 5.0f + header_accidentals * 0.95f;
        header_width += 3.0f;
        music_sp_t available = content_width - header_width;
        int system_end = measure_cursor;
        music_sp_t ideal_sum = 0.0f;
        while (system_end < measure_end) {
            if (config->max_measures_per_system > 0 &&
                system_end - measure_cursor >= config->max_measures_per_system)
                break;
            music_sp_t ideal = measure_ideal_width(score, &score->measures[system_end], config);
            if (system_end > measure_cursor && ideal_sum + ideal > available) break;
            ideal_sum += ideal;
            system_end++;
        }
        if (system_end == measure_cursor) system_end++;
        if (ideal_sum <= 0.0f) ideal_sum = available;

        music_sp_t staff_left = config->left_margin;
        music_sp_t staff_right = config->page_width - config->right_margin;
        for (uint8_t staff = 1; staff <= staff_count; ++staff) {
            music_sp_t top = staff_top_for(staff, system_top, config);
            if (!draw_staff(scene, staff_left, staff_right, top,
                            config->staff_line_thickness))
                LAYOUT_ERROR("scene item capacity exceeded");
            const music_clef_t *clef = &first->clefs[staff - 1];
            if (!add_glyph(scene, smufl_clef_glyph(clef->kind), staff_left + 0.75f,
                           clef_anchor_y(clef, top), 1.0f, -1))
                LAYOUT_ERROR("scene item capacity exceeded");
            music_sp_t x = render_key_signature(scene, &first->key, clef,
                                                staff_left + 4.0f, top);
            render_time_signature(scene, &first->time, x + 0.7f, top);
        }
        if (staff_count == 2) {
            music_sp_t grand_top = staff_top_for(1, system_top, config);
            music_sp_t grand_bottom = staff_top_for(2, system_top, config) + 4.0f;
            if (!add_line(scene, staff_left, grand_top,
                          staff_left, grand_bottom,
                          config->staff_line_thickness * 1.6f, -1) ||
                !add_glyph(scene, SMUFL_GLYPH_BRACE, staff_left - 0.88f,
                           (grand_top + grand_bottom) * 0.5f, 1.0f, -1))
                LAYOUT_ERROR("scene item capacity exceeded");
        }

        int note_count = 0;
        music_sp_t measure_x = staff_left + header_width;
        for (int mi = measure_cursor; mi < system_end; ++mi) {
            const music_measure_t *measure = &score->measures[mi];
            music_clef_t active_clefs[MUSIC_MAX_STAVES] = {
                measure->clefs[0], measure->clefs[1]
            };
            music_sp_t ideal = measure_ideal_width(score, measure, config);
            music_sp_t measure_width = available * ideal / ideal_sum;
            music_sp_t prefix_width = 0.0f;

            if (mi > measure_cursor) {
                const music_measure_t *previous = &score->measures[mi - 1];
                music_sp_t change_x = measure_x + 0.45f;
                bool any_clef_change = false;
                for (uint8_t staff = 1; staff <= staff_count; ++staff) {
                    if (!clef_equal(&previous->clefs[staff - 1],
                                    &measure->clefs[staff - 1])) {
                        music_sp_t top = staff_top_for(staff, system_top, config);
                        add_glyph(scene, smufl_clef_glyph(measure->clefs[staff - 1].kind),
                                  change_x, clef_anchor_y(&measure->clefs[staff - 1], top),
                                  0.67f, -1);
                        any_clef_change = true;
                    }
                }
                if (any_clef_change) change_x += 2.2f;

                if (!key_equal(&previous->key, &measure->key)) {
                    music_sp_t max_x = change_x;
                    for (uint8_t staff = 1; staff <= staff_count; ++staff) {
                        music_sp_t top = staff_top_for(staff, system_top, config);
                        music_sp_t end_x = render_key_signature(scene, &measure->key,
                                                               &measure->clefs[staff - 1],
                                                               change_x, top);
                        if (end_x > max_x) max_x = end_x;
                    }
                    change_x = max_x + 0.55f;
                }
                if (!time_equal(&previous->time, &measure->time)) {
                    for (uint8_t staff = 1; staff <= staff_count; ++staff) {
                        music_sp_t top = staff_top_for(staff, system_top, config);
                        render_time_signature(scene, &measure->time, change_x, top);
                    }
                    change_x += 2.5f;
                }
                prefix_width = change_x - measure_x;
            }
            int32_t duration = measure->duration_divisions > 0 ? measure->duration_divisions :
                               measure->divisions * measure->time.beats * 4 /
                               (measure->time.beat_type ? measure->time.beat_type : 4);
            if (duration <= 0) duration = 1;

            for (int ei = 0; ei < measure->event_count; ++ei) {
                int event_index = measure->event_start + ei;
                const music_event_t *event = &score->events[event_index];
                music_sp_t ratio = (music_sp_t)event->onset_divisions / duration;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                music_sp_t usable_width = measure_width - prefix_width - 2.4f;
                if (usable_width < 1.0f) usable_width = 1.0f;
                music_sp_t x = measure_x + prefix_width + 1.2f +
                               ratio * usable_width;

                if (event->kind == MUSIC_EVENT_CLEF) {
                    uint8_t staff = event->data.clef.staff ? event->data.clef.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    active_clefs[staff - 1] = event->data.clef.value;
                    music_sp_t top = staff_top_for(staff, system_top, config);
                    if (!add_glyph(scene, smufl_clef_glyph(event->data.clef.value.kind),
                                   x, clef_anchor_y(&event->data.clef.value, top),
                                   0.67f, event_index))
                        LAYOUT_ERROR("scene item capacity exceeded");
                } else if (event->kind == MUSIC_EVENT_KEY_SIGNATURE) {
                    uint8_t staff = event->data.key.staff ? event->data.key.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, system_top, config);
                    render_key_signature(scene, &event->data.key.value,
                                         &active_clefs[staff - 1], x, top);
                } else if (event->kind == MUSIC_EVENT_TIME_SIGNATURE) {
                    uint8_t staff = event->data.time.staff ? event->data.time.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, system_top, config);
                    render_time_signature(scene, &event->data.time.value, x, top);
                } else if (event->kind == MUSIC_EVENT_NOTE) {
                    if (note_count >= MAX_NOTES_PER_SYSTEM)
                        LAYOUT_ERROR("system exceeds MAX_NOTES_PER_SYSTEM");
                    uint8_t staff = event->data.note.staff ? event->data.note.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, system_top, config);
                    int position = music_pitch_staff_position(
                        &event->data.note.pitch, &active_clefs[staff - 1]);
                    music_stem_direction_t stem = resolved_chord_stem_direction(
                        score, measure, event, &active_clefs[staff - 1]);
                    bool draw_ledger = note_owns_ledger_lines(
                        score, measure, event_index,
                        &active_clefs[staff - 1], position);
                    x += polyphonic_notehead_offset(score, measure, event,
                                                    &active_clefs[staff - 1],
                                                    stem);
                    chord_note_geometry_t geometry;
                    chord_note_geometry(score, measure, event, event_index,
                                        &active_clefs[staff - 1], x, top,
                                        stem, &geometry);
                    if (!render_note_symbol(scene, event, event_index,
                                            &active_clefs[staff - 1], x, top,
                                            config, draw_ledger, &geometry,
                                            &notes[note_count]))
                        LAYOUT_ERROR("scene item capacity exceeded");
                    notes[note_count].system_index =
                        (uint8_t)scene->system_count;
                    notes[note_count].valid = true;
                    all_notes[event_index] = notes[note_count];
                    note_count++;
                } else if (event->kind == MUSIC_EVENT_REST) {
                    uint8_t staff = event->data.rest.staff ? event->data.rest.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, system_top, config);
                    music_sp_t y = top + 2.0f;
                    uint8_t upper_voice = 1;
                    if (measure_staff_voice_range(score, measure, staff,
                                                  &upper_voice, NULL)) {
                        uint8_t voice = event->data.rest.voice ?
                                        event->data.rest.voice : 1;
                        y += voice == upper_voice ? -0.85f : 0.85f;
                    }
                    if (!add_centered_glyph(scene,
                                            smufl_rest_glyph(event->data.rest.type),
                                            x, y, 1.0f, event_index))
                        LAYOUT_ERROR("scene item capacity exceeded");
                    for (int dot = 0; dot < event->data.rest.dots; ++dot) {
                        if (!add_glyph(scene, SMUFL_GLYPH_AUGMENTATION_DOT,
                                       x + 1.0f + dot * 0.55f, y - 0.5f,
                                       1.0f, event_index))
                            LAYOUT_ERROR("scene item capacity exceeded");
                    }
                }
            }

            music_sp_t top = staff_top_for(1, system_top, config);
            music_sp_t bottom = staff_top_for(staff_count, system_top, config) + 4.0f;
            if (!render_barline(scene, measure->right_barline,
                                measure_x + measure_width, top, bottom, config))
                LAYOUT_ERROR("scene item capacity exceeded");
            measure_x += measure_width;
        }

        if (!render_beams(scene, notes, note_count, config))
            LAYOUT_ERROR("scene item capacity exceeded");

        if (scene->system_count >= MUSIC_SCENE_MAX_SYSTEMS)
            LAYOUT_ERROR("scene system capacity exceeded");
        music_sp_t system_height = 4.0f + (staff_count - 1) *
                                   (4.0f + config->staff_gap);
        music_scene_system_t *system =
            &scene->systems[scene->system_count++];
        *system = (music_scene_system_t){
            .measure_start = (uint16_t)system_measure_start,
            .measure_end = (uint16_t)system_end,
            .item_start = (uint16_t)system_item_start,
            .item_count = (uint16_t)(scene->item_count - system_item_start),
            .top = current_system_top,
            .bottom = current_system_top + system_height,
        };
        system_top += system_height + config->system_gap;
        measure_cursor = system_end;
    }

    if (!render_connections(scene, config, score, all_notes))
        LAYOUT_ERROR("scene item capacity exceeded");
    if (scene->system_count) {
        music_scene_system_t *last =
            &scene->systems[scene->system_count - 1];
        last->item_count = (uint16_t)(scene->item_count - last->item_start);
    }

    /* system_top has already advanced past the last system. Excluding the
     * trailing inter-system gap makes single-system centering use the actual
     * grand-staff height (the old value shifted it upward by about 14 px). */
    scene->height = system_top - config->system_gap;
    if (error && error_size) error[0] = '\0';
    free(notes);
    free(all_notes);
#undef LAYOUT_ERROR
    return true;
}

bool music_layout_build(const music_score_t *score,
                        const music_layout_config_t *config,
                        music_scene_t *scene,
                        char *error,
                        size_t error_size)
{
    return music_layout_build_range(score, config, scene, -1, -1,
                                    error, error_size);
}

bool music_layout_rebuild_from_measure(const music_score_t *score,
                                       const music_layout_config_t *config,
                                       music_scene_t *scene,
                                       uint16_t first_dirty_measure,
                                       char *error,
                                       size_t error_size)
{
    if (!score || !config || !scene || !scene->system_count)
        return music_layout_build(score, config, scene, error, error_size);

    int dirty_system = -1;
    for (int i = 0; i < scene->system_count; ++i) {
        if (first_dirty_measure < scene->systems[i].measure_end) {
            dirty_system = i;
            break;
        }
    }
    if (dirty_system < 0) dirty_system = scene->system_count - 1;
    if (dirty_system > 0) dirty_system--;

    const music_scene_system_t boundary = scene->systems[dirty_system];
    const int prefix_item_count = boundary.item_start;
    music_layout_config_t tail_config = *config;
    tail_config.top_margin = boundary.top;

    music_scene_t *tail = calloc(1, sizeof(*tail));
    if (!tail) return set_error(error, error_size,
                                "unable to allocate dirty layout workspace");
    bool built = music_layout_build_range(
        score, &tail_config, tail, boundary.measure_start, -1,
        error, error_size);
    if (!built) {
        free(tail);
        return false;
    }
    if (prefix_item_count + tail->item_count > MUSIC_SCENE_MAX_ITEMS ||
        dirty_system + tail->system_count > MUSIC_SCENE_MAX_SYSTEMS) {
        free(tail);
        return set_error(error, error_size,
                         "dirty layout exceeds scene capacity");
    }

    memcpy(&scene->items[prefix_item_count], tail->items,
           (size_t)tail->item_count * sizeof(tail->items[0]));
    for (int i = 0; i < tail->system_count; ++i) {
        scene->systems[dirty_system + i] = tail->systems[i];
        scene->systems[dirty_system + i].item_start +=
            (uint16_t)prefix_item_count;
    }
    scene->item_count = (uint16_t)(prefix_item_count + tail->item_count);
    scene->system_count = (uint16_t)(dirty_system + tail->system_count);
    scene->width = tail->width;
    scene->height = tail->height;
    free(tail);
    if (error && error_size) error[0] = '\0';
    return true;
}
