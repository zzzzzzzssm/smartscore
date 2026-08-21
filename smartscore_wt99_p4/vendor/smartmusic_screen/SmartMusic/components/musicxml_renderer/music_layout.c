#include "music_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES_PER_SYSTEM 512

/* Horizontal engraving clearances are expressed in staff spaces.  Keep
 * these independent from the page scale so the same collision rules apply
 * to the on-device and exported views. */
#define RHYTHMIC_COLUMN_CLEARANCE 0.45f
#define ACCIDENTAL_NOTE_CLEARANCE 0.35f
#define ACCIDENTAL_COLUMN_CLEARANCE 0.15f
#define ACCIDENTAL_VERTICAL_CLEARANCE 0.20f
#define MEASURE_EDGE_PADDING 1.20f

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

typedef struct {
    int32_t onset;
    music_sp_t left_extent;
    music_sp_t right_extent;
    music_sp_t hard_x;
    music_sp_t x;
    music_sp_t spacing_weight;
} rhythmic_column_t;

typedef struct {
    uint16_t column_start;
    uint16_t column_count;
    music_sp_t hard_width;
    music_sp_t ideal_width;
    music_sp_t ink_top;
    music_sp_t ink_bottom;
} measure_spacing_t;

typedef struct {
    int16_t column_index;
    music_clef_t clef;
    music_sp_t head_left;
    music_sp_t accidental_x;
    bool has_clef;
    bool has_accidental_x;
} event_spacing_t;

typedef struct {
    int event_index;
    int staff_position;
    int assigned_column;
    music_sp_t y;
    music_sp_t half_height;
    music_sp_t glyph_width;
} accidental_layout_t;

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
    case SMUFL_GLYPH_AUGMENTATION_DOT: return 0.314f;
    case SMUFL_GLYPH_ACCIDENTAL_FLAT: return 0.760f;
    case SMUFL_GLYPH_ACCIDENTAL_NATURAL: return 0.720f;
    case SMUFL_GLYPH_ACCIDENTAL_SHARP: return 1.020f;
    case SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP: return 1.120f;
    case SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT: return 1.460f;
    case SMUFL_GLYPH_FLAG_8_UP:
    case SMUFL_GLYPH_FLAG_8_DOWN: return 1.050f;
    case SMUFL_GLYPH_FLAG_16_UP:
    case SMUFL_GLYPH_FLAG_16_DOWN: return 1.180f;
    case SMUFL_GLYPH_FLAG_32_UP:
    case SMUFL_GLYPH_FLAG_32_DOWN: return 1.300f;
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

static music_sp_t accidental_half_height(smufl_glyph_id_t glyph)
{
    switch (glyph) {
    case SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP: return 0.72f;
    case SMUFL_GLYPH_ACCIDENTAL_FLAT: return 1.58f;
    case SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT: return 1.62f;
    case SMUFL_GLYPH_ACCIDENTAL_NATURAL:
    case SMUFL_GLYPH_ACCIDENTAL_SHARP: return 1.48f;
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
        .page_height = 0.0f,
        .page_vertical_offset = 0.0f,
        .page_top_padding = 0.0f,
        .page_bottom_padding = 0.0f,
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

static music_sp_t duration_spacing_weight(music_duration_kind_t type)
{
    return type == MUSIC_DURATION_WHOLE ? 2.80f :
           type == MUSIC_DURATION_HALF ? 2.30f :
           type == MUSIC_DURATION_QUARTER ? 1.90f :
           type == MUSIC_DURATION_EIGHTH ? 1.55f : 1.35f;
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

static music_sp_t polyphonic_notehead_offset(
    const music_score_t *score, const music_measure_t *measure,
    const music_event_t *event, const music_clef_t *clef,
    music_stem_direction_t stem);

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
                                music_sp_t accidental_x,
                                chord_note_geometry_t *geometry)
{
    music_sp_t chord_left = base_x;
    music_sp_t chord_right = base_x;
    music_sp_t chord_top = 10000.0f;
    music_sp_t chord_bottom = -10000.0f;
    int position = music_pitch_staff_position(&event->data.note.pitch, clef);
    music_sp_t y = staff_top + 4.0f - position * 0.5f;
    music_sp_t offset = polyphonic_notehead_offset(
        score, measure, event, clef, stem) +
        chord_notehead_offset(score, measure, event, clef, stem);
    music_sp_t head_x = base_x + offset;
    music_sp_t head_width = leland_glyph_width(
        smufl_notehead_glyph(event->data.note.type));
    if (head_width <= 0.0f) head_width = 1.3f;

    int event_offset = event_index - measure->event_start;
    for (int i = 0; i < measure->event_count; ++i) {
        int candidate_index = measure->event_start + i;
        const music_event_t *candidate = &score->events[candidate_index];
        if (!note_matches_chord(candidate, event)) continue;
        int candidate_position = music_pitch_staff_position(
            &candidate->data.note.pitch, clef);
        music_sp_t candidate_y = staff_top + 4.0f - candidate_position * 0.5f;
        music_sp_t candidate_offset = polyphonic_notehead_offset(
            score, measure, candidate, clef, stem) +
            chord_notehead_offset(score, measure, candidate, clef, stem);
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
        .accidental_x = accidental_x,
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

static music_sp_t min_sp(music_sp_t a, music_sp_t b)
{
    return a < b ? a : b;
}

static music_sp_t max_sp(music_sp_t a, music_sp_t b)
{
    return a > b ? a : b;
}

static int compare_rhythmic_column(const void *left, const void *right)
{
    const rhythmic_column_t *a = left;
    const rhythmic_column_t *b = right;
    return (a->onset > b->onset) - (a->onset < b->onset);
}

static int compare_accidental_layout(const void *left, const void *right)
{
    const accidental_layout_t *a = left;
    const accidental_layout_t *b = right;
    if (a->staff_position != b->staff_position)
        return (a->staff_position > b->staff_position) -
               (a->staff_position < b->staff_position);
    return (a->event_index > b->event_index) -
           (a->event_index < b->event_index);
}

static music_clef_t measure_clef_at_onset(const music_score_t *score,
                                           const music_measure_t *measure,
                                           uint8_t staff, int32_t onset)
{
    if (!staff) staff = 1;
    if (staff > MUSIC_MAX_STAVES) staff = MUSIC_MAX_STAVES;
    music_clef_t clef = measure->clefs[staff - 1];
    int32_t selected_onset = -1;
    int selected_order = -1;
    for (int i = 0; i < measure->event_count; ++i) {
        const music_event_t *event =
            &score->events[measure->event_start + i];
        uint8_t event_staff = event->kind == MUSIC_EVENT_CLEF ?
                              event->data.clef.staff : 0;
        if (event->kind != MUSIC_EVENT_CLEF ||
            (event_staff ? event_staff : 1) != staff ||
            event->onset_divisions > onset)
            continue;
        if (event->onset_divisions > selected_onset ||
            (event->onset_divisions == selected_onset && i > selected_order)) {
            clef = event->data.clef.value;
            selected_onset = event->onset_divisions;
            selected_order = i;
        }
    }
    return clef;
}

static music_sp_t note_articulation_half_width(const music_note_t *note)
{
    music_sp_t width = 0.0f;
    if (note->articulations & MUSIC_ARTIC_STACCATO)
        width = max_sp(width, leland_glyph_width(
            SMUFL_GLYPH_ARTIC_STACCATO_ABOVE));
    if (note->articulations & MUSIC_ARTIC_TENUTO)
        width = max_sp(width, leland_glyph_width(
            SMUFL_GLYPH_ARTIC_TENUTO_ABOVE));
    if (note->articulations & MUSIC_ARTIC_ACCENT)
        width = max_sp(width, leland_glyph_width(
            SMUFL_GLYPH_ARTIC_ACCENT_ABOVE));
    if (note->articulations & MUSIC_ARTIC_FERMATA)
        width = max_sp(width, leland_glyph_width(
            SMUFL_GLYPH_FERMATA_ABOVE));
    return width * 0.5f;
}

static bool analyze_measure_spacing(
    const music_score_t *score, const music_measure_t *measure,
    const music_layout_config_t *config, rhythmic_column_t *columns,
    int column_capacity, int *next_column, event_spacing_t *event_spacing,
    accidental_layout_t *accidentals,
    music_sp_t *accidental_column_widths, measure_spacing_t *metrics)
{
    int event_end = measure->event_start + measure->event_count;
    if (measure->event_start > score->event_count ||
        event_end > score->event_count || !next_column || !metrics)
        return false;

    int column_start = *next_column;
    for (int event_index = measure->event_start;
         event_index < event_end; ++event_index) {
        const music_event_t *event = &score->events[event_index];
        event_spacing[event_index].column_index = -1;
        if (event->kind != MUSIC_EVENT_NOTE &&
            event->kind != MUSIC_EVENT_REST)
            continue;

        int found = -1;
        for (int ci = column_start; ci < *next_column; ++ci) {
            if (columns[ci].onset == event->onset_divisions) {
                found = ci;
                break;
            }
        }
        if (found < 0) {
            if (*next_column >= column_capacity) return false;
            found = (*next_column)++;
            columns[found] = (rhythmic_column_t){
                .onset = event->onset_divisions,
                .left_extent = 10000.0f,
                .right_extent = -10000.0f,
            };
        }
    }

    int column_count = *next_column - column_start;
    qsort(&columns[column_start], (size_t)column_count,
          sizeof(columns[0]), compare_rhythmic_column);

    for (int event_index = measure->event_start;
         event_index < event_end; ++event_index) {
        const music_event_t *event = &score->events[event_index];
        if (event->kind != MUSIC_EVENT_NOTE &&
            event->kind != MUSIC_EVENT_REST)
            continue;
        for (int ci = column_start; ci < *next_column; ++ci) {
            if (columns[ci].onset == event->onset_divisions) {
                event_spacing[event_index].column_index = (int16_t)ci;
                break;
            }
        }
    }

    uint8_t staff_count = measure->staff_count ? measure->staff_count : 1;
    if (staff_count > MUSIC_MAX_STAVES) staff_count = MUSIC_MAX_STAVES;
    music_sp_t staff_height = 4.0f + (staff_count - 1) *
                              (4.0f + config->staff_gap);
    metrics->ink_top = 0.0f;
    metrics->ink_bottom = staff_height;

    for (int event_index = measure->event_start;
         event_index < event_end; ++event_index) {
        const music_event_t *event = &score->events[event_index];
        int ci = event_spacing[event_index].column_index;
        if (ci < column_start || ci >= *next_column) continue;
        rhythmic_column_t *column = &columns[ci];

        if (event->kind == MUSIC_EVENT_NOTE) {
            const music_note_t *note = &event->data.note;
            uint8_t staff = note->staff ? note->staff : 1;
            if (staff > staff_count) staff = staff_count;
            music_sp_t staff_top = staff_top_for(staff, 0.0f, config);
            music_clef_t clef = measure_clef_at_onset(
                score, measure, staff, event->onset_divisions);
            event_spacing[event_index].clef = clef;
            event_spacing[event_index].has_clef = true;
            event_spacing[event_index].has_accidental_x = false;

            music_stem_direction_t stem = resolved_chord_stem_direction(
                score, measure, event, &clef);
            chord_note_geometry_t geometry;
            chord_note_geometry(score, measure, event, event_index, &clef,
                                0.0f, staff_top, stem, 0.0f, &geometry);
            event_spacing[event_index].head_left = geometry.chord_left;

            music_sp_t left = geometry.chord_left;
            music_sp_t right = geometry.chord_right;
            int position = music_pitch_staff_position(&note->pitch, &clef);
            if ((position <= -2 || position >= 10) &&
                note_owns_ledger_lines(score, measure, event_index, &clef,
                                       position)) {
                music_sp_t ledger_half = 0.75f +
                                         config->ledger_line_extension;
                left = min_sp(left, geometry.head_x - ledger_half);
                right = max_sp(right, geometry.head_x + ledger_half);
            }
            if (note->dots) {
                music_sp_t dot_width = leland_glyph_width(
                    SMUFL_GLYPH_AUGMENTATION_DOT);
                right = max_sp(right, geometry.dot_x +
                    (note->dots - 1) * 0.55f + dot_width);
            }
            smufl_glyph_id_t flag = smufl_flag_glyph(note->type, stem);
            if (note->beams[0] == MUSIC_BEAM_NONE &&
                flag < SMUFL_GLYPH_COUNT) {
                music_sp_t flag_width = leland_glyph_width(flag);
                left = min_sp(left, geometry.stem_x - flag_width);
                right = max_sp(right, geometry.stem_x + flag_width);
            }
            music_sp_t articulation_half = note_articulation_half_width(note);
            left = min_sp(left, geometry.head_x - articulation_half);
            right = max_sp(right, geometry.head_x + articulation_half);
            column->left_extent = min_sp(column->left_extent, left);
            column->right_extent = max_sp(column->right_extent, right);
            column->spacing_weight = max_sp(
                column->spacing_weight, duration_spacing_weight(note->type));

            music_sp_t top = min_sp(geometry.chord_top,
                                    geometry.stem_end_y);
            music_sp_t bottom = max_sp(geometry.chord_bottom,
                                       geometry.stem_end_y);
            music_sp_t note_y = staff_top + 4.0f - position * 0.5f;
            smufl_glyph_id_t accidental =
                smufl_accidental_glyph(note->accidental);
            if (accidental < SMUFL_GLYPH_COUNT) {
                music_sp_t half_height = accidental_half_height(accidental);
                top = min_sp(top, note_y - half_height);
                bottom = max_sp(bottom, note_y + half_height);
            }
            if (note->articulations) {
                if (stem == MUSIC_STEM_UP) bottom = max_sp(bottom, note_y + 3.5f);
                else top = min_sp(top, note_y - 3.5f);
            }
            metrics->ink_top = min_sp(metrics->ink_top, top);
            metrics->ink_bottom = max_sp(metrics->ink_bottom, bottom);
        } else if (event->kind == MUSIC_EVENT_REST) {
            const music_rest_t *rest = &event->data.rest;
            uint8_t staff = rest->staff ? rest->staff : 1;
            if (staff > staff_count) staff = staff_count;
            music_sp_t y = staff_top_for(staff, 0.0f, config) + 2.0f;
            uint8_t upper_voice = 1;
            if (measure_staff_voice_range(score, measure, staff,
                                          &upper_voice, NULL)) {
                uint8_t voice = rest->voice ? rest->voice : 1;
                y += voice == upper_voice ? -0.85f : 0.85f;
            }
            music_sp_t width = leland_glyph_width(smufl_rest_glyph(rest->type));
            if (width <= 0.0f) width = 1.3f;
            column->left_extent = min_sp(column->left_extent, -width * 0.5f);
            music_sp_t right = width * 0.5f;
            if (rest->dots)
                right = max_sp(right, 1.0f + (rest->dots - 1) * 0.55f +
                    leland_glyph_width(SMUFL_GLYPH_AUGMENTATION_DOT));
            column->right_extent = max_sp(column->right_extent, right);
            column->spacing_weight = max_sp(
                column->spacing_weight, duration_spacing_weight(rest->type));
            metrics->ink_top = min_sp(metrics->ink_top, y - 1.2f);
            metrics->ink_bottom = max_sp(metrics->ink_bottom, y + 1.2f);
        }
    }

    /* Accidentals belong to an onset+staff, not to an input voice.  Assign
     * every visible accidental to the nearest collision-free column using a
     * deterministic high/low alternating order, then right-align each glyph
     * within its column so mixed sharp/flat widths remain compact. */
    for (int ci = column_start; ci < *next_column; ++ci) {
        rhythmic_column_t *column = &columns[ci];
        if (column->left_extent > column->right_extent) {
            column->left_extent = -0.65f;
            column->right_extent = 0.65f;
        }
        for (uint8_t staff = 1; staff <= staff_count; ++staff) {
            int accidental_count = 0;
            music_sp_t note_left = 10000.0f;
            for (int event_index = measure->event_start;
                 event_index < event_end; ++event_index) {
                const music_event_t *event = &score->events[event_index];
                if (event->kind != MUSIC_EVENT_NOTE ||
                    event_spacing[event_index].column_index != ci)
                    continue;
                uint8_t event_staff = event->data.note.staff ?
                                      event->data.note.staff : 1;
                if (event_staff > staff_count) event_staff = staff_count;
                if (event_staff != staff) continue;
                note_left = min_sp(note_left,
                    event_spacing[event_index].head_left);
                smufl_glyph_id_t glyph =
                    smufl_accidental_glyph(event->data.note.accidental);
                if (glyph >= SMUFL_GLYPH_COUNT) continue;
                const music_clef_t *clef = &event_spacing[event_index].clef;
                int position = music_pitch_staff_position(
                    &event->data.note.pitch, clef);
                music_sp_t width = leland_glyph_width(glyph);
                if (width <= 0.0f) width = 1.0f;
                accidentals[accidental_count++] = (accidental_layout_t){
                    .event_index = event_index,
                    .staff_position = position,
                    .assigned_column = -1,
                    .y = staff_top_for(staff, 0.0f, config) +
                         4.0f - position * 0.5f,
                    .half_height = accidental_half_height(glyph),
                    .glyph_width = width,
                };
            }
            if (!accidental_count || note_left >= 9999.0f) continue;

            qsort(accidentals, (size_t)accidental_count,
                  sizeof(accidentals[0]), compare_accidental_layout);
            for (int i = 0; i < accidental_count; ++i)
                accidental_column_widths[i] = 0.0f;

            int left_index = 0;
            int right_index = accidental_count - 1;
            int assigned_count = 0;
            int accidental_columns = 0;
            bool take_high = true;
            while (assigned_count < accidental_count) {
                int current = take_high ? right_index-- : left_index++;
                take_high = !take_high;
                assigned_count++;

                int selected_column = accidental_columns;
                for (int candidate_column = 0;
                     candidate_column < accidental_columns;
                     ++candidate_column) {
                    bool collision = false;
                    for (int placed = 0; placed < accidental_count; ++placed) {
                        if (accidentals[placed].assigned_column != candidate_column)
                            continue;
                        music_sp_t distance = accidentals[current].y -
                                              accidentals[placed].y;
                        if (distance < 0.0f) distance = -distance;
                        if (distance < accidentals[current].half_height +
                                       accidentals[placed].half_height +
                                       ACCIDENTAL_VERTICAL_CLEARANCE) {
                            collision = true;
                            break;
                        }
                    }
                    if (!collision) {
                        selected_column = candidate_column;
                        break;
                    }
                }
                if (selected_column == accidental_columns)
                    accidental_columns++;
                accidentals[current].assigned_column = selected_column;
                accidental_column_widths[selected_column] = max_sp(
                    accidental_column_widths[selected_column],
                    accidentals[current].glyph_width);
            }

            music_sp_t column_right = note_left -
                                      ACCIDENTAL_NOTE_CLEARANCE;
            for (int accidental_column = 0;
                 accidental_column < accidental_columns;
                 ++accidental_column) {
                music_sp_t column_left = column_right -
                    accidental_column_widths[accidental_column];
                for (int i = 0; i < accidental_count; ++i) {
                    if (accidentals[i].assigned_column != accidental_column)
                        continue;
                    event_spacing[accidentals[i].event_index].accidental_x =
                        column_right - accidentals[i].glyph_width;
                    event_spacing[accidentals[i].event_index].has_accidental_x =
                        true;
                }
                column->left_extent = min_sp(column->left_extent, column_left);
                column_right = column_left - ACCIDENTAL_COLUMN_CLEARANCE;
            }
        }
    }

    metrics->column_start = (uint16_t)column_start;
    metrics->column_count = (uint16_t)column_count;
    if (!column_count) {
        metrics->hard_width = MEASURE_EDGE_PADDING * 2.0f;
        metrics->ideal_width = 3.0f;
        return true;
    }

    rhythmic_column_t *first = &columns[column_start];
    first->hard_x = MEASURE_EDGE_PADDING - first->left_extent;
    music_sp_t ideal_x = first->hard_x;
    for (int i = 1; i < column_count; ++i) {
        rhythmic_column_t *previous = &columns[column_start + i - 1];
        rhythmic_column_t *current = &columns[column_start + i];
        music_sp_t hard_delta = previous->right_extent -
                                current->left_extent +
                                RHYTHMIC_COLUMN_CLEARANCE;
        current->hard_x = previous->hard_x + hard_delta;
        music_sp_t desired_delta = max_sp(hard_delta,
            max_sp(config->minimum_note_spacing,
                   (previous->spacing_weight + current->spacing_weight) * 0.5f));
        ideal_x += desired_delta;
    }
    rhythmic_column_t *last = &columns[column_start + column_count - 1];
    metrics->hard_width = last->hard_x + last->right_extent +
                          MEASURE_EDGE_PADDING;
    metrics->ideal_width = ideal_x + last->right_extent +
                           MEASURE_EDGE_PADDING;
    if (metrics->ideal_width < metrics->hard_width)
        metrics->ideal_width = metrics->hard_width;
    return true;
}

static bool analyze_score_spacing(
    const music_score_t *score, const music_layout_config_t *config,
    rhythmic_column_t *columns, int column_capacity,
    event_spacing_t *event_spacing, measure_spacing_t *measure_spacing,
    accidental_layout_t *accidentals,
    music_sp_t *accidental_column_widths)
{
    int next_column = 0;
    for (int mi = 0; mi < score->measure_count; ++mi) {
        if (!analyze_measure_spacing(
                score, &score->measures[mi], config, columns,
                column_capacity, &next_column, event_spacing, accidentals,
                accidental_column_widths, &measure_spacing[mi]))
            return false;
    }
    return true;
}

static bool position_measure_columns(const music_measure_t *measure,
                                     const measure_spacing_t *metrics,
                                     rhythmic_column_t *columns,
                                     music_sp_t content_left,
                                     music_sp_t content_width)
{
    if (content_width + 0.001f < metrics->hard_width) return false;
    if (!metrics->column_count) return true;
    int32_t duration = measure->duration_divisions > 0 ?
                       measure->duration_divisions :
                       measure->divisions * measure->time.beats * 4 /
                       (measure->time.beat_type ? measure->time.beat_type : 4);
    music_sp_t extra = content_width - metrics->hard_width;
    for (int i = 0; i < metrics->column_count; ++i) {
        rhythmic_column_t *column =
            &columns[metrics->column_start + i];
        music_sp_t phase = duration > 0 ?
            (music_sp_t)column->onset / duration :
            (metrics->column_count > 1 ?
             (music_sp_t)i / (metrics->column_count - 1) : 0.0f);
        if (phase < 0.0f) phase = 0.0f;
        if (phase > 1.0f) phase = 1.0f;
        column->x = content_left + column->hard_x + extra * phase;
    }
    return true;
}

static music_sp_t measure_boundary_prefix_width(
    const music_measure_t *previous, const music_measure_t *current)
{
    music_sp_t width = 0.45f;
    uint8_t staff_count = current->staff_count ? current->staff_count : 1;
    uint8_t previous_staff_count = previous->staff_count ?
                                   previous->staff_count : 1;
    if (previous_staff_count > staff_count) staff_count = previous_staff_count;
    if (staff_count > MUSIC_MAX_STAVES) staff_count = MUSIC_MAX_STAVES;
    for (uint8_t staff = 0; staff < staff_count; ++staff) {
        if (!clef_equal(&previous->clefs[staff], &current->clefs[staff])) {
            width += 2.2f;
            break;
        }
    }
    if (!key_equal(&previous->key, &current->key)) {
        int accidental_count = current->key.fifths < 0 ?
                               -current->key.fifths : current->key.fifths;
        accidental_count += current->key.cancel_fifths < 0 ?
                            -current->key.cancel_fifths :
                            current->key.cancel_fifths;
        width += accidental_count * 0.95f + 0.55f;
    }
    if (!time_equal(&previous->time, &current->time)) width += 2.5f;
    return width;
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

static void translate_scene_item_y(music_scene_item_t *item, music_sp_t dy)
{
    if (!item || dy == 0.0f) return;
    switch (item->kind) {
    case MUSIC_SCENE_GLYPH:
        item->data.glyph.y += dy;
        break;
    case MUSIC_SCENE_LINE:
        item->data.line.y1 += dy;
        item->data.line.y2 += dy;
        break;
    case MUSIC_SCENE_BEZIER:
        item->data.bezier.y1 += dy;
        item->data.bezier.cy1 += dy;
        item->data.bezier.cy2 += dy;
        item->data.bezier.y2 += dy;
        break;
    }
}

static void translate_system_y(music_scene_t *scene,
                               note_layout_t *layouts,
                               int layout_count,
                               int system_index,
                               music_sp_t dy)
{
    if (!scene || system_index < 0 ||
        system_index >= scene->system_count || dy == 0.0f)
        return;

    music_scene_system_t *system = &scene->systems[system_index];
    const int item_end = system->item_start + system->item_count;
    for (int i = system->item_start;
         i < item_end && i < scene->item_count; ++i)
        translate_scene_item_y(&scene->items[i], dy);

    system->top += dy;
    system->bottom += dy;
    system->ink_top += dy;
    system->ink_bottom += dy;

    for (int i = 0; layouts && i < layout_count; ++i) {
        note_layout_t *layout = &layouts[i];
        if (!layout->valid || layout->system_index != system_index) continue;
        layout->y += dy;
        layout->stem_start_y += dy;
        layout->stem_end_y += dy;
        layout->chord_top += dy;
        layout->chord_bottom += dy;
    }
}

/* Line breaking first guarantees that no system crosses a fixed page.  This
 * second pass then treats all systems on a page as one visual block and
 * centers that block inside the page's safe area.  Centering by real ink
 * bounds keeps pages with one or two short systems from appearing pinned to
 * the top while still protecting tall stems and ledger lines. */
static void center_paginated_systems(music_scene_t *scene,
                                     const music_layout_config_t *config,
                                     note_layout_t *layouts,
                                     int layout_count)
{
    if (!scene || !config || scene->system_count == 0 ||
        config->page_height <= 0.0f)
        return;

    const music_sp_t usable_height =
        config->page_height - config->page_top_padding -
        config->page_bottom_padding;
    if (usable_height <= 0.0f) return;

    int group_start = 0;
    while (group_start < scene->system_count) {
        music_scene_system_t *first = &scene->systems[group_start];
        music_sp_t page_coordinate =
            first->ink_top + config->page_vertical_offset;
        if (page_coordinate < 0.0f) page_coordinate = 0.0f;
        const int page_index = (int)(page_coordinate / config->page_height);
        int group_end = group_start + 1;
        music_sp_t ink_top = first->ink_top;
        music_sp_t ink_bottom = first->ink_bottom;

        while (group_end < scene->system_count) {
            music_scene_system_t *candidate = &scene->systems[group_end];
            music_sp_t candidate_coordinate =
                candidate->ink_top + config->page_vertical_offset;
            if (candidate_coordinate < 0.0f) candidate_coordinate = 0.0f;
            const int candidate_page =
                (int)(candidate_coordinate / config->page_height);
            if (candidate_page != page_index) break;
            if (candidate->ink_top < ink_top) ink_top = candidate->ink_top;
            if (candidate->ink_bottom > ink_bottom)
                ink_bottom = candidate->ink_bottom;
            group_end++;
        }

        const music_sp_t occupied_height = ink_bottom - ink_top;
        if (occupied_height <= usable_height + 0.001f) {
            const music_sp_t page_top =
                page_index * config->page_height -
                config->page_vertical_offset + config->page_top_padding;
            const music_sp_t target_top =
                page_top + (usable_height - occupied_height) * 0.5f;
            const music_sp_t dy = target_top - ink_top;
            for (int system = group_start; system < group_end; ++system)
                translate_system_y(scene, layouts, layout_count, system, dy);
        }
        group_start = group_end;
    }
}

static bool music_layout_build_range(const music_score_t *score,
                                     const music_layout_config_t *config,
                                     music_scene_t *scene,
                                     int range_start, int range_end,
                                     char *error,
                                     size_t error_size)
{
    if (!score || !config || !scene) return set_error(error, error_size, "invalid layout arguments");
    if (score->event_count > MUSIC_MAX_EVENTS ||
        score->measure_count > MUSIC_MAX_MEASURES)
        return set_error(error, error_size, "score exceeds layout limits");
    memset(scene, 0, sizeof(*scene));
    scene->width = config->page_width;
    if (score->part_count == 0) return set_error(error, error_size, "score has no parts");

    /* music_layout_build() normally runs on taskLVGL's 7 KiB stack.  Every
     * score-dependent workspace therefore stays on the heap, is bounded by
     * MUSIC_MAX_EVENTS/MUSIC_MAX_MEASURES, and is released through the common
     * error path.  ESP-IDF's configured >4 KiB allocation policy routes these
     * arrays to PSRAM while the same calloc code remains host-test portable. */
    note_layout_t *notes = calloc(MAX_NOTES_PER_SYSTEM, sizeof(*notes));
    note_layout_t *all_notes = calloc(score->event_count ? score->event_count : 1,
                                      sizeof(*all_notes));
    rhythmic_column_t *columns = calloc(
        score->event_count ? score->event_count : 1, sizeof(*columns));
    event_spacing_t *event_spacing = calloc(
        score->event_count ? score->event_count : 1, sizeof(*event_spacing));
    measure_spacing_t *measure_spacing = calloc(
        score->measure_count ? score->measure_count : 1,
        sizeof(*measure_spacing));
    accidental_layout_t *accidentals = calloc(
        score->event_count ? score->event_count : 1, sizeof(*accidentals));
    music_sp_t *accidental_column_widths = calloc(
        score->event_count ? score->event_count : 1,
        sizeof(*accidental_column_widths));
    if (!notes || !all_notes || !columns || !event_spacing ||
        !measure_spacing || !accidentals || !accidental_column_widths) {
        free(notes);
        free(all_notes);
        free(columns);
        free(event_spacing);
        free(measure_spacing);
        free(accidentals);
        free(accidental_column_widths);
        return set_error(error, error_size, "unable to allocate layout workspace");
    }

#define LAYOUT_ERROR(message) do {                     \
        free(notes);                                   \
        free(all_notes);                               \
        free(columns);                                 \
        free(event_spacing);                           \
        free(measure_spacing);                         \
        free(accidentals);                             \
        free(accidental_column_widths);                \
        return set_error(error, error_size, message); \
    } while (0)

    if (!analyze_score_spacing(
            score, config, columns,
            score->event_count ? score->event_count : 1,
            event_spacing, measure_spacing, accidentals,
            accidental_column_widths))
        LAYOUT_ERROR("invalid rhythmic spacing input");

    const music_part_t *part = &score->parts[0];
    int part_start = part->measure_start;
    int part_end = part->measure_start + part->measure_count;
    if (part_start > score->measure_count || part_end > score->measure_count)
        LAYOUT_ERROR("part measure range exceeds score");
    int measure_cursor = range_start >= part_start ? range_start : part_start;
    int measure_end = range_end > measure_cursor && range_end < part_end ?
                      range_end : part_end;
    music_sp_t content_width = config->page_width - config->left_margin - config->right_margin;
    music_sp_t system_top = config->top_margin;
    /* Incremental rebuild passes the preserved dirty system's staff top as
     * top_margin. Do not apply its above-staff skyline a second time, or the
     * same tail would drift downward on every live refresh. */
    bool preserve_first_system_staff_top = range_start >= part_start;

    while (measure_cursor < measure_end) {
        int system_measure_start = measure_cursor;
        int system_item_start = scene->item_count;
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
        if (available <= 0.0f)
            LAYOUT_ERROR("page is too narrow for notation header");
        int system_end = measure_cursor;
        int measure_limit = config->max_measures_per_system > 0 ?
                            config->max_measures_per_system :
                            measure_end - measure_cursor;
        music_sp_t hard_sum = 0.0f;
        music_sp_t ideal_sum = 0.0f;
        while (system_end < measure_end) {
            if (system_end - measure_cursor >= measure_limit)
                break;
            music_sp_t prefix = system_end > measure_cursor ?
                measure_boundary_prefix_width(
                    &score->measures[system_end - 1],
                    &score->measures[system_end]) : 0.0f;
            music_sp_t hard = measure_spacing[system_end].hard_width + prefix;
            music_sp_t ideal = measure_spacing[system_end].ideal_width + prefix;
            if (system_end == measure_cursor && hard > available + 0.001f)
                LAYOUT_ERROR("measure exceeds hard layout width");
            if (system_end > measure_cursor && hard_sum + hard > available)
                break;
            hard_sum += hard;
            ideal_sum += ideal;
            system_end++;
        }
        if (system_end == measure_cursor) system_end++;

        music_sp_t system_height = 4.0f + (staff_count - 1) *
                                   (4.0f + config->staff_gap);
        music_sp_t system_ink_top = 0.0f;
        music_sp_t system_ink_bottom = system_height;
        for (int mi = measure_cursor; mi < system_end; ++mi) {
            system_ink_top = min_sp(system_ink_top,
                                    measure_spacing[mi].ink_top);
            system_ink_bottom = max_sp(system_ink_bottom,
                                       measure_spacing[mi].ink_bottom);
        }
        music_sp_t current_system_top = preserve_first_system_staff_top ?
            system_top : system_top - min_sp(system_ink_top, 0.0f);
        preserve_first_system_staff_top = false;

        /* A screen page must never cut through a staff system. Continuous
         * layout remains the default; an on-device preview can opt into a
         * fixed page grid and move the whole system when its ink would cross
         * the lower page boundary. */
        const music_sp_t page_usable_height =
            config->page_height - config->page_top_padding -
            config->page_bottom_padding;
        if (config->page_height > 0.0f && page_usable_height > 0.0f) {
            const music_sp_t ink_top = current_system_top + system_ink_top;
            const music_sp_t ink_bottom = current_system_top +
                max_sp(system_height, system_ink_bottom);
            const music_sp_t ink_height = ink_bottom - ink_top;
            music_sp_t page_coordinate =
                ink_top + config->page_vertical_offset;
            if (page_coordinate < 0.0f) page_coordinate = 0.0f;
            const int page_index =
                (int)(page_coordinate / config->page_height);
            const music_sp_t page_bottom =
                (page_index + 1) * config->page_height -
                config->page_vertical_offset -
                config->page_bottom_padding;
            if (ink_height <= page_usable_height &&
                ink_bottom > page_bottom + 0.001f) {
                const music_sp_t next_page_top =
                    (page_index + 1) * config->page_height -
                    config->page_vertical_offset +
                    config->page_top_padding;
                current_system_top += next_page_top - ink_top;
            }
        }
        music_sp_t flexible_sum = ideal_sum - hard_sum;

        music_sp_t staff_left = config->left_margin;
        music_sp_t staff_right = config->page_width - config->right_margin;
        for (uint8_t staff = 1; staff <= staff_count; ++staff) {
            music_sp_t top = staff_top_for(staff, current_system_top, config);
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
            music_sp_t grand_top = staff_top_for(1, current_system_top, config);
            music_sp_t grand_bottom = staff_top_for(2, current_system_top, config) + 4.0f;
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
            music_sp_t estimated_prefix = mi > measure_cursor ?
                measure_boundary_prefix_width(&score->measures[mi - 1],
                                              measure) : 0.0f;
            music_sp_t measure_hard = measure_spacing[mi].hard_width +
                                      estimated_prefix;
            music_sp_t measure_flexible = measure_spacing[mi].ideal_width -
                                          measure_spacing[mi].hard_width;
            if (measure_flexible < 0.0f) measure_flexible = 0.0f;
            music_sp_t system_extra = available - hard_sum;
            music_sp_t measure_width;
            if (flexible_sum > 0.001f) {
                measure_width = measure_hard + system_extra *
                                measure_flexible / flexible_sum;
            } else {
                measure_width = measure_hard + system_extra /
                    (system_end - measure_cursor);
            }
            music_sp_t prefix_width = 0.0f;

            if (mi > measure_cursor) {
                const music_measure_t *previous = &score->measures[mi - 1];
                music_sp_t change_x = measure_x + 0.45f;
                bool any_clef_change = false;
                for (uint8_t staff = 1; staff <= staff_count; ++staff) {
                    if (!clef_equal(&previous->clefs[staff - 1],
                                    &measure->clefs[staff - 1])) {
                        music_sp_t top = staff_top_for(staff, current_system_top, config);
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
                        music_sp_t top = staff_top_for(staff, current_system_top, config);
                        music_sp_t end_x = render_key_signature(scene, &measure->key,
                                                               &measure->clefs[staff - 1],
                                                               change_x, top);
                        if (end_x > max_x) max_x = end_x;
                    }
                    change_x = max_x + 0.55f;
                }
                if (!time_equal(&previous->time, &measure->time)) {
                    for (uint8_t staff = 1; staff <= staff_count; ++staff) {
                        music_sp_t top = staff_top_for(staff, current_system_top, config);
                        render_time_signature(scene, &measure->time, change_x, top);
                    }
                    change_x += 2.5f;
                }
                prefix_width = change_x - measure_x;
            }
            if (!position_measure_columns(
                    measure, &measure_spacing[mi], columns,
                    measure_x + prefix_width, measure_width - prefix_width))
                LAYOUT_ERROR("measure exceeds assigned hard layout width");
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
                int rhythmic_column = event_spacing[event_index].column_index;
                if (rhythmic_column >= measure_spacing[mi].column_start &&
                    rhythmic_column < measure_spacing[mi].column_start +
                                      measure_spacing[mi].column_count)
                    x = columns[rhythmic_column].x;

                if (event->kind == MUSIC_EVENT_CLEF) {
                    uint8_t staff = event->data.clef.staff ? event->data.clef.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    active_clefs[staff - 1] = event->data.clef.value;
                    music_sp_t top = staff_top_for(staff, current_system_top, config);
                    if (!add_glyph(scene, smufl_clef_glyph(event->data.clef.value.kind),
                                   x, clef_anchor_y(&event->data.clef.value, top),
                                   0.67f, event_index))
                        LAYOUT_ERROR("scene item capacity exceeded");
                } else if (event->kind == MUSIC_EVENT_KEY_SIGNATURE) {
                    uint8_t staff = event->data.key.staff ? event->data.key.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, current_system_top, config);
                    render_key_signature(scene, &event->data.key.value,
                                         &active_clefs[staff - 1], x, top);
                } else if (event->kind == MUSIC_EVENT_TIME_SIGNATURE) {
                    uint8_t staff = event->data.time.staff ? event->data.time.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, current_system_top, config);
                    render_time_signature(scene, &event->data.time.value, x, top);
                } else if (event->kind == MUSIC_EVENT_NOTE) {
                    if (note_count >= MAX_NOTES_PER_SYSTEM)
                        LAYOUT_ERROR("system exceeds MAX_NOTES_PER_SYSTEM");
                    uint8_t staff = event->data.note.staff ? event->data.note.staff : 1;
                    if (staff > staff_count) staff = staff_count;
                    music_sp_t top = staff_top_for(staff, current_system_top, config);
                    const music_clef_t *note_clef =
                        event_spacing[event_index].has_clef ?
                        &event_spacing[event_index].clef :
                        &active_clefs[staff - 1];
                    int position = music_pitch_staff_position(
                        &event->data.note.pitch, note_clef);
                    music_stem_direction_t stem = resolved_chord_stem_direction(
                        score, measure, event, note_clef);
                    bool draw_ledger = note_owns_ledger_lines(
                        score, measure, event_index, note_clef, position);
                    music_sp_t accidental_x =
                        event_spacing[event_index].has_accidental_x ?
                        x + event_spacing[event_index].accidental_x : x;
                    chord_note_geometry_t geometry;
                    chord_note_geometry(score, measure, event, event_index,
                                        note_clef, x, top, stem,
                                        accidental_x, &geometry);
                    if (!render_note_symbol(scene, event, event_index,
                                            note_clef, x, top,
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
                    music_sp_t top = staff_top_for(staff, current_system_top, config);
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

            music_sp_t top = staff_top_for(1, current_system_top, config);
            music_sp_t bottom = staff_top_for(staff_count, current_system_top, config) + 4.0f;
            if (!render_barline(scene, measure->right_barline,
                                measure_x + measure_width, top, bottom, config))
                LAYOUT_ERROR("scene item capacity exceeded");
            measure_x += measure_width;
        }

        if (!render_beams(scene, notes, note_count, config))
            LAYOUT_ERROR("scene item capacity exceeded");

        if (scene->system_count >= MUSIC_SCENE_MAX_SYSTEMS)
            LAYOUT_ERROR("scene system capacity exceeded");
        music_scene_system_t *system =
            &scene->systems[scene->system_count++];
        *system = (music_scene_system_t){
            .measure_start = (uint16_t)system_measure_start,
            .measure_end = (uint16_t)system_end,
            .item_start = (uint16_t)system_item_start,
            .item_count = (uint16_t)(scene->item_count - system_item_start),
            .top = current_system_top,
            .bottom = current_system_top +
                      max_sp(system_height, system_ink_bottom),
            .ink_top = current_system_top + system_ink_top,
            .ink_bottom = current_system_top +
                          max_sp(system_height, system_ink_bottom),
        };
        music_sp_t ink_bottom = current_system_top + system_ink_bottom;
        music_sp_t staff_bottom = current_system_top + system_height;
        system_top = max_sp(ink_bottom, staff_bottom) + config->system_gap;
        measure_cursor = system_end;
    }

    center_paginated_systems(scene, config, all_notes,
                             score->event_count);

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
    if (config->page_height > 0.0f && scene->system_count > 0) {
        scene->height = 0.0f;
        for (int i = 0; i < scene->system_count; ++i) {
            const music_scene_system_t *system = &scene->systems[i];
            if (system->ink_bottom > scene->height)
                scene->height = system->ink_bottom;
        }
    } else {
        scene->height = system_top - config->system_gap;
    }
    if (error && error_size) error[0] = '\0';
    free(notes);
    free(all_notes);
    free(columns);
    free(event_spacing);
    free(measure_spacing);
    free(accidentals);
    free(accidental_column_widths);
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
    /* Page centering is a whole-page constraint; rebuilding an isolated tail
     * could move its systems without moving the preserved page prefix. */
    if (config->page_height > 0.0f)
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
    if (dirty_system == 0)
        return music_layout_build(score, config, scene, error, error_size);

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
