#include "staff_display_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "music_layout.h"
#include "musicxml_parser.h"
#include "smufl_glyphs.h"

LV_FONT_DECLARE(lv_font_LelandSMuFL_72);
LV_FONT_DECLARE(lv_font_LelandSMuFL_64);
LV_FONT_DECLARE(lv_font_LelandBrace_252);
LV_FONT_DECLARE(lv_font_montserratMedium_16);

#define TEST_H_RES            1024
#define TEST_V_RES            600
#define STAFF_SPACE_PX        18.0f
#define SCENE_ORIGIN_Y_PX     70.0f
#define CURVE_SEGMENTS        20
#define LIVE_MIDI_MAX_NOTES   16
#define LIVE_NOTES_PER_MEASURE 4

static const char *TAG = "staff_musicxml_test";

typedef struct {
    music_score_t *score;
    music_scene_t *scene;
    lv_obj_t *return_screen;
    lv_obj_t *canvas;
    lv_obj_t *title;
    lv_obj_t *info;
    lv_obj_t **item_objects;
} staff_test_context_t;

typedef struct {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint32_t color_rgb;
} live_midi_note_t;

static staff_test_context_t *s_active_test_context;
static portMUX_TYPE s_live_midi_lock = portMUX_INITIALIZER_UNLOCKED;
static live_midi_note_t s_live_midi_notes[LIVE_MIDI_MAX_NOTES];
static size_t s_live_midi_note_count;
static bool s_live_midi_connected;

/* Twinkle Twinkle Little Star as a beginner piano grand staff: right hand in
 * the C4-A4 register on staff 1, left hand on bass-clef staff 2. */
static const char s_test_musicxml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<score-partwise version=\"4.0\">"
      "<work><work-title>Twinkle Twinkle Little Star</work-title></work>"
      "<part-list><score-part id=\"P1\"><part-name>Piano</part-name></score-part></part-list>"
      "<part id=\"P1\">"
        "<measure number=\"1\">"
          "<attributes><divisions>4</divisions><staves>2</staves>"
          "<key><fifths>0</fifths><mode>major</mode></key>"
          "<time><beats>4</beats><beat-type>4</beat-type></time>"
          "<clef number=\"1\"><sign>G</sign><line>2</line></clef>"
          "<clef number=\"2\"><sign>F</sign><line>4</line></clef></attributes>"
          "<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<backup><duration>16</duration></backup>"
          "<note><pitch><step>C</step><octave>3</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
          "<note><pitch><step>G</step><octave>2</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
        "</measure>"
        "<measure number=\"2\">"
          "<note><pitch><step>A</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>A</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>G</step><octave>4</octave></pitch><duration>8</duration><voice>1</voice><type>half</type><staff>1</staff></note>"
          "<backup><duration>16</duration></backup>"
          "<note><pitch><step>F</step><octave>2</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
          "<note><pitch><step>C</step><octave>3</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
        "</measure>"
        "<measure number=\"3\">"
          "<note><pitch><step>F</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>F</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>E</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>E</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<backup><duration>16</duration></backup>"
          "<note><pitch><step>F</step><octave>2</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
          "<note><pitch><step>C</step><octave>3</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
        "</measure>"
        "<measure number=\"4\">"
          "<note><pitch><step>D</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>D</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>quarter</type><staff>1</staff></note>"
          "<note><pitch><step>C</step><octave>4</octave></pitch><duration>8</duration><voice>1</voice><type>half</type><staff>1</staff></note>"
          "<backup><duration>16</duration></backup>"
          "<note><pitch><step>G</step><octave>2</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
          "<note><pitch><step>C</step><octave>3</octave></pitch><duration>8</duration><voice>2</voice><type>half</type><staff>2</staff></note>"
          "<barline location=\"right\"><bar-style>light-heavy</bar-style></barline>"
        "</measure>"
      "</part>"
    "</score-partwise>";

static int sp_to_px(music_sp_t value)
{
    return (int)lroundf(value * STAFF_SPACE_PX);
}

static void draw_line_px(lv_layer_t *layer, int x1, int y1, int x2, int y2,
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

static lv_color_t scene_item_color(const music_scene_item_t *item)
{
    uint32_t rgb = item && item->color_rgb != MUSIC_SCENE_COLOR_DEFAULT ?
                   item->color_rgb : UINT32_C(0x171717);
    return lv_color_hex(rgb);
}

static float cubic(float p0, float p1, float p2, float p3, float t)
{
    float u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * p1 +
           3.0f * u * t * t * p2 + t * t * t * p3;
}

static void draw_scene_cb(lv_event_t *event)
{
    staff_test_context_t *ctx = (staff_test_context_t *)lv_event_get_user_data(event);
    if (!ctx || !ctx->scene) return;
    lv_layer_t *layer = lv_event_get_layer(event);

    for (int i = 0; i < ctx->scene->item_count; ++i) {
        const music_scene_item_t *item = &ctx->scene->items[i];
        if (item->kind == MUSIC_SCENE_LINE) {
            draw_line_px(layer,
                         sp_to_px(item->data.line.x1),
                         (int)SCENE_ORIGIN_Y_PX + sp_to_px(item->data.line.y1),
                         sp_to_px(item->data.line.x2),
                         (int)SCENE_ORIGIN_Y_PX + sp_to_px(item->data.line.y2),
                         sp_to_px(item->data.line.thickness),
                         scene_item_color(item));
        } else if (item->kind == MUSIC_SCENE_BEZIER) {
            const typeof(item->data.bezier) *b = &item->data.bezier;
            float previous_x = b->x1;
            float previous_y = b->y1;
            for (int segment = 1; segment <= CURVE_SEGMENTS; ++segment) {
                float t = (float)segment / CURVE_SEGMENTS;
                float x = cubic(b->x1, b->cx1, b->cx2, b->x2, t);
                float y = cubic(b->y1, b->cy1, b->cy2, b->y2, t);
                draw_line_px(layer, sp_to_px(previous_x),
                             (int)SCENE_ORIGIN_Y_PX + sp_to_px(previous_y),
                              sp_to_px(x), (int)SCENE_ORIGIN_Y_PX + sp_to_px(y),
                              sp_to_px(b->thickness), scene_item_color(item));
                previous_x = x;
                previous_y = y;
            }
        }
    }
}

static void create_scene_glyphs(lv_obj_t *parent, staff_test_context_t *ctx)
{
    const music_scene_t *scene = ctx ? ctx->scene : NULL;
    if (!scene) return;
    for (int i = 0; i < scene->item_count; ++i) {
        const music_scene_item_t *item = &scene->items[i];
        if (item->kind != MUSIC_SCENE_GLYPH) continue;
        const smufl_glyph_info_t *glyph = smufl_glyph_info(item->data.glyph.glyph);
        char utf8[5];
        if (!glyph || !smufl_codepoint_to_utf8(glyph->codepoint, utf8)) continue;

        bool time_glyph = item->data.glyph.glyph >= SMUFL_GLYPH_TIME_0 &&
                          item->data.glyph.glyph <= SMUFL_GLYPH_TIME_CUT;
        const lv_font_t *font = item->data.glyph.glyph == SMUFL_GLYPH_BRACE ?
                                &lv_font_LelandBrace_252 :
                                time_glyph ? &lv_font_LelandSMuFL_64 :
                                             &lv_font_LelandSMuFL_72;
        int font_origin_y = font->line_height - font->base_line;

        lv_obj_t *label = lv_label_create(parent);
        lv_obj_remove_style_all(label);
        /* Some SMuFL glyphs legitimately extend below the nominal font line
         * (notably G-clef tails and upward flags). Keep transparent descent
         * room so LVGL does not clip those final antialiased rows. */
        lv_obj_set_size(label, 110, font->line_height + 16);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, scene_item_color(item), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label, utf8);
        lv_obj_set_pos(label,
                       sp_to_px(item->data.glyph.x),
                       (int)SCENE_ORIGIN_Y_PX + sp_to_px(item->data.glyph.y) -
                       font_origin_y);
        if (ctx->item_objects) ctx->item_objects[i] = label;
    }
}

static void snapshot_live_midi(live_midi_note_t *notes, size_t *note_count,
                               bool *connected)
{
    taskENTER_CRITICAL(&s_live_midi_lock);
    size_t count = s_live_midi_note_count;
    if (notes && count) memcpy(notes, s_live_midi_notes, count * sizeof(*notes));
    if (note_count) *note_count = count;
    if (connected) *connected = s_live_midi_connected;
    taskEXIT_CRITICAL(&s_live_midi_lock);
}

static void midi_note_to_pitch(uint8_t midi_note, music_note_t *note)
{
    static const uint8_t steps[12] = {
        0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6
    };
    static const int8_t alters[12] = {
        0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0
    };
    uint8_t pitch_class = midi_note % 12;
    note->pitch.step = steps[pitch_class];
    note->pitch.alter = alters[pitch_class];
    note->pitch.octave = (int8_t)(midi_note / 12) - 1;
    note->accidental = alters[pitch_class] ? MUSIC_ACCIDENTAL_SHARP :
                                            MUSIC_ACCIDENTAL_NONE;
}

static void build_live_midi_score(music_score_t *score,
                                  const live_midi_note_t *notes,
                                  size_t note_count)
{
    music_score_init(score);
    snprintf(score->title, sizeof(score->title), "USB MIDI LIVE INPUT");
    score->part_count = 1;
    music_part_t *part = &score->parts[0];
    snprintf(part->id, sizeof(part->id), "P1");
    snprintf(part->name, sizeof(part->name), "Piano");
    part->measure_start = 0;
    part->staff_count = 2;

    size_t measure_count = (note_count + LIVE_NOTES_PER_MEASURE - 1) /
                           LIVE_NOTES_PER_MEASURE;
    if (measure_count == 0) measure_count = 1;
    score->measure_count = (uint16_t)measure_count;
    part->measure_count = (uint16_t)measure_count;

    for (size_t mi = 0; mi < measure_count; ++mi) {
        music_measure_t *measure = &score->measures[mi];
        snprintf(measure->number, sizeof(measure->number), "%u",
                 (unsigned)(mi + 1));
        measure->part_index = 0;
        measure->event_start = score->event_count;
        measure->divisions = 4;
        measure->duration_divisions = 16;
        measure->staff_count = 2;
        measure->clefs[0] = (music_clef_t){
            .kind = MUSIC_CLEF_TREBLE, .line = 2
        };
        measure->clefs[1] = (music_clef_t){
            .kind = MUSIC_CLEF_BASS, .line = 4
        };
        measure->time = (music_time_signature_t){
            .beats = 4, .beat_type = 4, .symbol = MUSIC_TIME_NUMERIC
        };
        measure->right_barline =
            mi + 1 == measure_count ? MUSIC_BARLINE_FINAL :
                                      MUSIC_BARLINE_SINGLE;

        size_t first = mi * LIVE_NOTES_PER_MEASURE;
        size_t end = first + LIVE_NOTES_PER_MEASURE;
        if (end > note_count) end = note_count;
        for (size_t ni = first; ni < end; ++ni) {
            music_event_t *event = &score->events[score->event_count++];
            event->kind = MUSIC_EVENT_NOTE;
            event->part_index = 0;
            event->measure_index = (uint16_t)mi;
            event->onset_divisions = (int32_t)((ni - first) * 4);
            music_note_t *note = &event->data.note;
            note->type = MUSIC_DURATION_QUARTER;
            note->duration_divisions = 4;
            note->stem = MUSIC_STEM_AUTO;
            note->staff = notes[ni].note >= 60 ? 1 : 2;
            note->voice = note->staff;
            midi_note_to_pitch(notes[ni].note, note);
            measure->event_count++;
        }
    }
}

static void configure_test_layout(music_layout_config_t *config)
{
    music_layout_default_config(config);
    config->page_width = TEST_H_RES / STAFF_SPACE_PX;
    config->left_margin = 1.7f;
    config->right_margin = 1.7f;
    config->top_margin = 0.8f;
    config->staff_gap = 6.0f;
    config->minimum_note_spacing = 1.65f;
}

static void delete_scene_glyphs(staff_test_context_t *ctx)
{
    if (!ctx || !ctx->scene || !ctx->item_objects) return;
    for (int i = 0; i < ctx->scene->item_count; ++i) {
        lv_obj_t *object = ctx->item_objects[i];
        if (object && lv_obj_is_valid(object)) lv_obj_delete(object);
    }
    free(ctx->item_objects);
    ctx->item_objects = NULL;
}

static bool allocate_item_objects(staff_test_context_t *ctx)
{
    if (!ctx || !ctx->scene || !ctx->scene->item_count) return true;
    ctx->item_objects = heap_caps_calloc(ctx->scene->item_count,
                                         sizeof(*ctx->item_objects),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->item_objects) {
        ctx->item_objects = calloc(ctx->scene->item_count,
                                   sizeof(*ctx->item_objects));
    }
    return ctx->item_objects != NULL;
}

static void apply_live_note_colors(music_scene_t *scene,
                                   const live_midi_note_t *notes,
                                   size_t note_count)
{
    for (size_t i = 0; scene && i < note_count; ++i) {
        if (notes[i].color_rgb != MUSIC_SCENE_COLOR_DEFAULT) {
            music_scene_set_event_color(scene, (int)i, notes[i].color_rgb);
        }
    }
}

static void update_live_labels(staff_test_context_t *ctx, bool connected,
                               size_t note_count)
{
    if (ctx->title && lv_obj_is_valid(ctx->title)) {
        lv_label_set_text(ctx->title, connected ?
            "USB MIDI LIVE INPUT / PIANO GRAND STAFF / LELAND SMuFL" :
            "USB MIDI DISCONNECTED / LAST RECEIVED NOTES");
    }
    if (ctx->info && lv_obj_is_valid(ctx->info)) {
        char info_text[128];
        snprintf(info_text, sizeof(info_text),
                 "%s  |  %u visible notes  |  %u measures  |  staffSpace=18px",
                 connected ? "keyboard connected" : "keyboard disconnected",
                 (unsigned)note_count, (unsigned)ctx->score->measure_count);
        lv_label_set_text(ctx->info, info_text);
    }
}

/* Caller owns the BSP/LVGL display lock. */
static bool rebuild_live_scene_locked(staff_test_context_t *ctx)
{
    if (!ctx || !ctx->score || !ctx->scene || !ctx->canvas ||
        !lv_obj_is_valid(ctx->canvas)) return false;

    live_midi_note_t notes[LIVE_MIDI_MAX_NOTES];
    size_t note_count = 0;
    bool connected = false;
    snapshot_live_midi(notes, &note_count, &connected);

    delete_scene_glyphs(ctx);
    build_live_midi_score(ctx->score, notes, note_count);
    music_layout_config_t config;
    configure_test_layout(&config);
    char error_text[192] = {0};
    if (!music_layout_build(ctx->score, &config, ctx->scene,
                            error_text, sizeof(error_text))) {
        ESP_LOGE(TAG, "Unable to rebuild live MIDI scene: %s", error_text);
        if (ctx->title && lv_obj_is_valid(ctx->title))
            lv_label_set_text(ctx->title, "USB MIDI RENDER ERROR");
        lv_obj_invalidate(ctx->canvas);
        return false;
    }
    apply_live_note_colors(ctx->scene, notes, note_count);
    if (!allocate_item_objects(ctx)) {
        ESP_LOGE(TAG, "Unable to allocate live MIDI scene object map");
        return false;
    }
    create_scene_glyphs(ctx->canvas, ctx);
    update_live_labels(ctx, connected, note_count);
    lv_obj_invalidate(ctx->canvas);
    ESP_LOGI(TAG, "Live MIDI staff rebuilt: notes=%u measures=%u scene=%u",
             (unsigned)note_count, (unsigned)ctx->score->measure_count,
             (unsigned)ctx->scene->item_count);
    return true;
}

void staff_display_test_midi_connected(void)
{
    taskENTER_CRITICAL(&s_live_midi_lock);
    s_live_midi_connected = true;
    s_live_midi_note_count = 0;
    taskEXIT_CRITICAL(&s_live_midi_lock);

    bsp_display_lock(portMAX_DELAY);
    if (s_active_test_context) rebuild_live_scene_locked(s_active_test_context);
    bsp_display_unlock();
}

void staff_display_test_midi_disconnected(void)
{
    taskENTER_CRITICAL(&s_live_midi_lock);
    s_live_midi_connected = false;
    taskEXIT_CRITICAL(&s_live_midi_lock);

    bsp_display_lock(portMAX_DELAY);
    if (s_active_test_context) {
        size_t note_count = 0;
        snapshot_live_midi(NULL, &note_count, NULL);
        update_live_labels(s_active_test_context, false, note_count);
    }
    bsp_display_unlock();
}

void staff_display_test_midi_note_on(uint8_t channel, uint8_t note,
                                     uint8_t velocity)
{
    bool accepted = false;
    taskENTER_CRITICAL(&s_live_midi_lock);
    if (s_live_midi_connected) {
        if (s_live_midi_note_count == LIVE_MIDI_MAX_NOTES) {
            memmove(&s_live_midi_notes[0], &s_live_midi_notes[1],
                    (LIVE_MIDI_MAX_NOTES - 1) * sizeof(s_live_midi_notes[0]));
            s_live_midi_note_count--;
        }
        s_live_midi_notes[s_live_midi_note_count++] = (live_midi_note_t){
            .channel = channel, .note = note, .velocity = velocity,
            .color_rgb = MUSIC_SCENE_COLOR_DEFAULT
        };
        accepted = true;
    }
    taskEXIT_CRITICAL(&s_live_midi_lock);
    if (!accepted) return;

    bsp_display_lock(portMAX_DELAY);
    if (s_active_test_context) rebuild_live_scene_locked(s_active_test_context);
    bsp_display_unlock();
}

static bool update_event_color(int event_index, uint32_t color_rgb, bool clear)
{
    if (event_index < 0) return false;
    bsp_display_lock(portMAX_DELAY);
    staff_test_context_t *ctx = s_active_test_context;
    if (!ctx || !ctx->scene) {
        bsp_display_unlock();
        return false;
    }

    size_t changed = clear ?
        music_scene_clear_event_color(ctx->scene, event_index) :
        music_scene_set_event_color(ctx->scene, event_index, color_rgb);
    if (changed) {
        taskENTER_CRITICAL(&s_live_midi_lock);
        if ((size_t)event_index < s_live_midi_note_count) {
            s_live_midi_notes[event_index].color_rgb = clear ?
                MUSIC_SCENE_COLOR_DEFAULT : color_rgb;
        }
        taskEXIT_CRITICAL(&s_live_midi_lock);
        for (int i = 0; i < ctx->scene->item_count; ++i) {
            const music_scene_item_t *item = &ctx->scene->items[i];
            if (item->source_event_index != event_index ||
                item->kind != MUSIC_SCENE_GLYPH || !ctx->item_objects)
                continue;
            lv_obj_t *object = ctx->item_objects[i];
            if (object && lv_obj_is_valid(object))
                lv_obj_set_style_text_color(object, scene_item_color(item), 0);
        }
        if (ctx->canvas && lv_obj_is_valid(ctx->canvas))
            lv_obj_invalidate(ctx->canvas);
    }
    bsp_display_unlock();
    return changed > 0;
}

bool staff_display_test_set_event_color(int event_index, uint32_t color_rgb)
{
    return update_event_color(event_index, color_rgb, false);
}

bool staff_display_test_clear_event_color(int event_index)
{
    return update_event_color(event_index, 0, true);
}

static void return_to_main_cb(lv_event_t *event)
{
    staff_test_context_t *ctx = (staff_test_context_t *)lv_event_get_user_data(event);
    if (ctx && ctx->return_screen && lv_obj_is_valid(ctx->return_screen)) {
        lv_screen_load_anim(ctx->return_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                            220, 0, true);
    }
}

static void test_screen_delete_cb(lv_event_t *event)
{
    staff_test_context_t *ctx = (staff_test_context_t *)lv_event_get_user_data(event);
    if (!ctx) return;
    if (s_active_test_context == ctx) s_active_test_context = NULL;
    free(ctx->item_objects);
    free(ctx->score);
    free(ctx->scene);
    free(ctx);
}

static void create_return_button(lv_obj_t *screen, staff_test_context_t *ctx)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_pos(button, 24, 528);
    lv_obj_set_size(button, 126, 48);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1769AA), 0);
    lv_obj_set_style_radius(button, 7, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, LV_SYMBOL_LEFT "  BACK");
    lv_obj_set_style_text_font(label, &lv_font_montserratMedium_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, return_to_main_cb, LV_EVENT_CLICKED, ctx);
}

static void show_error(lv_obj_t *screen, const char *message)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, 880);
    lv_obj_set_style_text_font(label, &lv_font_montserratMedium_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, message);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void staff_display_test_show(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, TEST_H_RES, TEST_V_RES);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFDF7), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    staff_test_context_t *ctx = heap_caps_calloc(1, sizeof(*ctx),
                                                  MALLOC_CAP_SPIRAM |
                                                  MALLOC_CAP_8BIT);
    if (!ctx) ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        show_error(screen, "Unable to allocate MusicXML renderer context");
        lv_screen_load(screen);
        return;
    }
    ctx->return_screen = lv_screen_active();
    ctx->score = heap_caps_calloc(1, sizeof(*ctx->score),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->scene = heap_caps_calloc(1, sizeof(*ctx->scene),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->score) ctx->score = calloc(1, sizeof(*ctx->score));
    if (!ctx->scene) ctx->scene = calloc(1, sizeof(*ctx->scene));
    lv_obj_add_event_cb(screen, test_screen_delete_cb, LV_EVENT_DELETE, ctx);

    ctx->title = lv_label_create(screen);
    lv_label_set_text(ctx->title,
                      "TWINKLE TWINKLE / PIANO GRAND STAFF / LELAND SMuFL");
    lv_obj_set_style_text_font(ctx->title, &lv_font_montserratMedium_16, 0);
    lv_obj_set_style_text_color(ctx->title, lv_color_hex(0x34302B), 0);
    lv_obj_align(ctx->title, LV_ALIGN_TOP_MID, 0, 17);

    bool ready = ctx->score && ctx->scene;
    bool live_connected = false;
    size_t live_note_count = 0;
    live_midi_note_t live_notes[LIVE_MIDI_MAX_NOTES];
    snapshot_live_midi(live_notes, &live_note_count, &live_connected);
    char error_text[192] = {0};
    if (ready) {
        if (live_connected) {
            build_live_midi_score(ctx->score, live_notes, live_note_count);
        } else {
            musicxml_error_t xml_error;
            if (!musicxml_parse_buffer(s_test_musicxml, strlen(s_test_musicxml),
                                       ctx->score, &xml_error)) {
                snprintf(error_text, sizeof(error_text),
                         "MusicXML error at %u:%u: %s",
                         (unsigned)xml_error.line, (unsigned)xml_error.column,
                         xml_error.message);
                ready = false;
            }
        }
    }

    if (ready) {
        music_layout_config_t config;
        configure_test_layout(&config);
        if (!music_layout_build(ctx->score, &config, ctx->scene,
                                error_text, sizeof(error_text))) ready = false;
        if (ready && live_connected)
            apply_live_note_colors(ctx->scene, live_notes, live_note_count);
        if (ready && !allocate_item_objects(ctx)) ready = false;
    }

    if (ready) {
        lv_obj_t *canvas = lv_obj_create(screen);
        lv_obj_remove_style_all(canvas);
        lv_obj_set_pos(canvas, 0, 0);
        lv_obj_set_size(canvas, TEST_H_RES, 515);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(canvas, draw_scene_cb, LV_EVENT_DRAW_MAIN, ctx);
        ctx->canvas = canvas;
        create_scene_glyphs(canvas, ctx);
        s_active_test_context = ctx;

        ctx->info = lv_label_create(screen);
        char info_text[120];
        snprintf(info_text, sizeof(info_text),
                 "%u measures  |  %u events  |  %u systems  |  staffSpace=18px",
                 (unsigned)ctx->score->measure_count,
                 (unsigned)ctx->score->event_count,
                 (unsigned)ctx->scene->system_count);
        lv_label_set_text(ctx->info, info_text);
        lv_obj_set_style_text_font(ctx->info, &lv_font_montserratMedium_16, 0);
        lv_obj_set_style_text_color(ctx->info, lv_color_hex(0x6B6259), 0);
        lv_obj_set_pos(ctx->info, 175, 548);
        if (live_connected) update_live_labels(ctx, true, live_note_count);
        ESP_LOGI(TAG, "MusicXML converted: measures=%u events=%u scene=%u",
                 (unsigned)ctx->score->measure_count,
                 (unsigned)ctx->score->event_count,
                 (unsigned)ctx->scene->item_count);
    } else {
        show_error(screen, error_text[0] ? error_text :
                   "Unable to allocate MusicXML score/scene buffers");
        ESP_LOGE(TAG, "%s", error_text);
    }

    create_return_button(screen, ctx);
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}
