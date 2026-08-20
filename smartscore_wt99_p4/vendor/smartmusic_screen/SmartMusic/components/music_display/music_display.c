#include "music_display.h"
#include "app_font.h"
#include "midi_parser.h"
#include "leland_score_view.h"
#include "gui_guider.h"
#include "score_ui_flow.h"
#include "creator_snapshot_mailbox.h"

/* GUI Guider 生成的字体声明 (未包含在 gui_guider.h 中的补齐) */
LV_FONT_DECLARE(lv_font_SimpMusicBasePSMTModified_90)
LV_FONT_DECLARE(lv_font_SimpMusicBasePSMTModified_72)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "music_display";

extern lv_ui guider_ui;

/* ── 队列 ── */
static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_snapshot_mutex;
static creator_snapshot_mailbox_t s_snapshot_mailbox;
static int s_latest_snapshot_notation = MUSIC_DISPLAY_NOTATION_STAFF;
static bool s_latest_snapshot_allow_empty;

/* ── 内部状态 ── */
static midi_data_t s_midi_data;
static bool s_midi_parsed = false;
static bool s_numbered_inference_evaluated;
static uint8_t s_numbered_inferred_categories;
static int s_current_page = 0;
static int s_total_pages = 1;
static leland_score_view_t *s_staff_view;
static int s_notation_type = MUSIC_DISPLAY_NOTATION_STAFF;
static volatile bool s_creator_active;
static volatile bool s_read_only_active;
static volatile bool s_practice_active;
static volatile bool s_practice_paused;
static lv_obj_t *s_navigation_event_target;
static lv_obj_t *s_read_only_back_button;
static lv_obj_t *s_read_only_notation_button;
static lv_obj_t *s_read_only_notation_label;
static lv_obj_t *s_read_only_playback_button;
static lv_obj_t *s_read_only_playback_label;
static music_display_playback_state_t s_read_only_playback_state;
static music_display_playback_button_cb_t s_playback_button_callback;
static void *s_playback_button_user_data;
static music_display_note_selection_cb_t s_note_selection_callback;
static void *s_note_selection_user_data;

#define MUSIC_MAX_RESULT_SLOTS 1024
#define MUSIC_LINES_PER_PAGE 2
#define MUSIC_MEASURES_PER_SYSTEM 2
#define MUSIC_MAX_NOTE_SPANS_PER_PAGE MAX_NOTES
#define MUSIC_MAX_NUMBERED_MARKERS_PER_PAGE MAX_NOTES
#define MUSIC_NUMBERED_TEXT_BUFFER_SIZE 2048
#define MUSIC_NUMBERED_MAX_OCTAVE_DOTS 5
#define MUSIC_NUMBERED_MAX_REDUCTION_LINES 3
#define MUSIC_NUMBERED_MAX_AUGMENTATION_DOTS 3
#define MUSIC_NUMBERED_DOT_RADIUS_PX 3
#define MUSIC_NUMBERED_DOT_GAP_PX 4
#define MUSIC_NUMBERED_DOT_STEP_PX 10
#define MUSIC_NUMBERED_AUGMENTATION_GAP_PX 6
#define MUSIC_NUMBERED_AUGMENTATION_STEP_PX 10
#define MUSIC_NUMBERED_CONNECTION_SEGMENTS 14
#define MUSIC_NUMBERED_CONNECTION_TYPES 3
#define MUSIC_NUMBERED_ROW_HEIGHT_PX 190
#define MUSIC_NUMBERED_ROW_PADDING_Y_PX 44
/* Stable reference planes for the 90 px SimpMusic font. Digits 1-7 share
 * top=10 and bottom=59 even though their individual bitmap boxes differ. */
#define MUSIC_NUMBERED_DIGIT_TOP_FROM_LINE_PX 10
#define MUSIC_NUMBERED_DIGIT_BOTTOM_FROM_LINE_PX 59
#define MUSIC_NUMBERED_REDUCTION_LINE_DEPTH_PX 7
#define MUSIC_STATUS_UNKNOWN 0xff
#define MUSIC_PAGE_TURN_LOOKAHEAD_MS 250
#define MUSIC_DISPLAY_GREEN_PITCH_TOLERANCE 2
#define MUSIC_DISPLAY_RED_PITCH_THRESHOLD 6

enum {
    MUSIC_NUMBERED_INFERRED_DOTS = 1U << 0,
    MUSIC_NUMBERED_INFERRED_TIES = 1U << 1,
};

_Static_assert(
    MUSIC_NUMBERED_ROW_PADDING_Y_PX +
    MUSIC_NUMBERED_DIGIT_TOP_FROM_LINE_PX -
    MUSIC_NUMBERED_DOT_GAP_PX -
    2 * MUSIC_NUMBERED_DOT_RADIUS_PX -
    (MUSIC_NUMBERED_MAX_OCTAVE_DOTS - 1) *
        MUSIC_NUMBERED_DOT_STEP_PX >= 0,
    "upper octave dots must remain inside the numbered row");
_Static_assert(
    MUSIC_NUMBERED_ROW_PADDING_Y_PX +
    MUSIC_NUMBERED_DIGIT_BOTTOM_FROM_LINE_PX +
    MUSIC_NUMBERED_MAX_REDUCTION_LINES *
        MUSIC_NUMBERED_REDUCTION_LINE_DEPTH_PX +
    MUSIC_NUMBERED_DOT_GAP_PX +
    2 * MUSIC_NUMBERED_DOT_RADIUS_PX +
    (MUSIC_NUMBERED_MAX_OCTAVE_DOTS - 1) *
        MUSIC_NUMBERED_DOT_STEP_PX < MUSIC_NUMBERED_ROW_HEIGHT_PX,
    "lower octave dots must remain inside the numbered row");

static uint8_t s_note_status[MUSIC_MAX_RESULT_SLOTS];
static char s_numbered_text_buffer[MUSIC_NUMBERED_TEXT_BUFFER_SIZE];
static midi_numbered_note_ref_t s_numbered_note_refs[MAX_NOTES];
static uint32_t s_numbered_page_end_tick;

/* ── 逐音符 span 追踪 ── */
typedef struct {
    lv_obj_t *group;
    lv_span_t *span;
    int slot_index;
} music_note_span_ref_t;

typedef struct {
    lv_obj_t *group;
    lv_span_t *span;
    int slot_index;
    int8_t octave;
    uint8_t reduction_line_count;
    int16_t cell_left_x;
    int16_t cell_right_x;
    int16_t center_x;
    int16_t digit_top_y;
    int16_t digit_bottom_y;
    int16_t upper_ink_y;
    int16_t lower_ink_y;
    int16_t nearest_center_y;
    bool positioned;
} music_numbered_marker_ref_t;

enum {
    MUSIC_NUMBERED_CONNECTION_TIE = 0,
    MUSIC_NUMBERED_CONNECTION_SLUR,
    MUSIC_NUMBERED_CONNECTION_GLISS,
};

static music_note_span_ref_t s_page_note_spans[MUSIC_MAX_NOTE_SPANS_PER_PAGE];
static music_numbered_marker_ref_t
    s_page_numbered_markers[MUSIC_MAX_NUMBERED_MARKERS_PER_PAGE];
static int16_t s_page_marker_by_note[MAX_NOTES];
static int16_t s_numbered_connection_stop
    [MUSIC_NUMBERED_CONNECTION_TYPES][MAX_NOTES];
static int16_t s_numbered_open_connection[2][UINT8_MAX + 1];
static uint32_t s_numbered_row_start_tick[MUSIC_LINES_PER_PAGE];
static uint32_t s_numbered_row_end_tick[MUSIC_LINES_PER_PAGE];
static uint8_t s_numbered_row_staff[MUSIC_LINES_PER_PAGE];
static int s_page_note_span_count;
static int s_page_numbered_marker_count;
static bool s_page_numbered_marker_overflow_logged;
static int s_page_first_slot = -1;
static int s_page_last_slot = -1;
static int s_expected_first_slot = -1;
static int s_expected_last_slot = -1;

static bool score_measure_ticks_locked(uint64_t *measure_ticks);
static void draw_numbered_overlays_cb(lv_event_t *event);

/* ── 动态乐谱行 spangroup（创建在 music_screen_cont_1 内）── */
static lv_obj_t *s_line_groups[MUSIC_LINES_PER_PAGE];
static lv_obj_t *s_line_prefixes[MUSIC_LINES_PER_PAGE];

static void ensure_line_groups(void)
{
    if (s_line_groups[0] && lv_obj_is_valid(s_line_groups[0])) return;
    memset(s_line_groups, 0, sizeof(s_line_groups));
    memset(s_line_prefixes, 0, sizeof(s_line_prefixes));

    lv_obj_t *parent = guider_ui.music_screen_cont_1;
    if (!parent) parent = guider_ui.music_screen;
    if (!parent) return;

    for (int i = 0; i < MUSIC_LINES_PER_PAGE; i++) {
        s_line_groups[i] = lv_spangroup_create(parent);
        lv_obj_set_pos(s_line_groups[i], 42, 25 + i * 220);
        lv_obj_set_size(s_line_groups[i], 929,
                        MUSIC_NUMBERED_ROW_HEIGHT_PX);
        lv_spangroup_set_align(s_line_groups[i], LV_TEXT_ALIGN_LEFT);
        lv_spangroup_set_overflow(s_line_groups[i], LV_SPAN_OVERFLOW_CLIP);
        lv_spangroup_set_mode(s_line_groups[i], LV_SPAN_MODE_BREAK);
        lv_obj_set_style_bg_opa(s_line_groups[i], 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_line_groups[i], 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_line_groups[i], 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        /* Reserve vertical ink space for the full MIDI octave range. Five
         * stacked dots remain inside the row instead of relying on an
         * ancestor's clipping policy. */
        lv_obj_set_style_pad_top(s_line_groups[i],
                                 MUSIC_NUMBERED_ROW_PADDING_Y_PX,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(s_line_groups[i],
                                    MUSIC_NUMBERED_ROW_PADDING_Y_PX,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_flag(s_line_groups[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_event_cb(s_line_groups[i],
                            draw_numbered_overlays_cb,
                            LV_EVENT_DRAW_MAIN_END, NULL);
        lv_obj_add_flag(s_line_groups[i], LV_OBJ_FLAG_HIDDEN);

        s_line_prefixes[i] = lv_label_create(parent);
        lv_label_set_text(s_line_prefixes[i], i == 0 ? "高：" : "低：");
        lv_obj_set_pos(s_line_prefixes[i], 15, 82 + i * 220);
        lv_obj_set_size(s_line_prefixes[i], 70, 36);
        lv_obj_set_style_bg_opa(s_line_prefixes[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_line_prefixes[i],
                                    lv_color_hex(0x171717), 0);
        const lv_font_t *prefix_font = app_font_chinese_22();
        if (prefix_font)
            lv_obj_set_style_text_font(s_line_prefixes[i], prefix_font, 0);
        lv_obj_add_flag(s_line_prefixes[i], LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "line spangroups created inside music_screen_cont_1");
}

/* ── 辅助：按起始 tick 排序音符 ── */
static int compare_midi_note_start(const void *a, const void *b)
{
    const midi_note_t *na = (const midi_note_t *)a;
    const midi_note_t *nb = (const midi_note_t *)b;
    if (na->start_tick < nb->start_tick) return -1;
    if (na->start_tick > nb->start_tick) return 1;
    if (na->note < nb->note) return -1;
    if (na->note > nb->note) return 1;
    return 0;
}

/* ── 前向声明 ── */
static void music_display_task(void *arg);
static void update_notation_display(void);
static void apply_midi_data_to_ui(void);
static void navigation_gesture_event_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;

    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    bool accepted = false;
    if (direction == LV_DIR_LEFT) {
        accepted = music_display_request_page_turn(
            MUSIC_DISPLAY_PAGE_NEXT, MUSIC_DISPLAY_PAGE_SOURCE_TOUCH);
    } else if (direction == LV_DIR_RIGHT) {
        accepted = music_display_request_page_turn(
            MUSIC_DISPLAY_PAGE_PREVIOUS, MUSIC_DISPLAY_PAGE_SOURCE_TOUCH);
    }
    if (accepted)
        lv_indev_wait_release(indev);
}

static void read_only_back_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_playback_button_callback) {
        s_playback_button_callback(MUSIC_DISPLAY_PLAYBACK_STOP,
                                   s_playback_button_user_data);
    }
    s_read_only_active = false;
    if (s_read_only_back_button &&
        lv_obj_is_valid(s_read_only_back_button))
        lv_obj_add_flag(s_read_only_back_button, LV_OBJ_FLAG_HIDDEN);
    if (s_read_only_notation_button &&
        lv_obj_is_valid(s_read_only_notation_button))
        lv_obj_add_flag(s_read_only_notation_button, LV_OBJ_FLAG_HIDDEN);
    if (s_read_only_playback_button &&
        lv_obj_is_valid(s_read_only_playback_button))
        lv_obj_add_flag(s_read_only_playback_button, LV_OBJ_FLAG_HIDDEN);
    if (!score_ui_flow_return_to_preparation(&guider_ui)) {
        score_ui_flow_open_choose(&guider_ui);
    }
}

static void read_only_notation_event_cb(lv_event_t *event)
{
    (void)event;
    if (!s_read_only_active || s_creator_active || !s_midi_parsed) return;
    if (s_playback_button_callback) {
        s_playback_button_callback(MUSIC_DISPLAY_PLAYBACK_STOP,
                                   s_playback_button_user_data);
    }

    s_notation_type =
        s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF
            ? MUSIC_DISPLAY_NOTATION_NUMBERED
            : MUSIC_DISPLAY_NOTATION_STAFF;
    s_current_page = 0;
    apply_midi_data_to_ui();
}

static void read_only_playback_event_cb(lv_event_t *event)
{
    (void)event;
    if (!s_read_only_active || s_creator_active ||
        !s_playback_button_callback) {
        return;
    }
    s_playback_button_callback(MUSIC_DISPLAY_PLAYBACK_TOGGLE,
                               s_playback_button_user_data);
}

static void update_read_only_playback_label(void)
{
    if (!s_read_only_playback_label ||
        !lv_obj_is_valid(s_read_only_playback_label)) {
        return;
    }
    const char *text = "播放";
    if (s_read_only_playback_state == MUSIC_DISPLAY_PLAYBACK_PREPARING) {
        text = "准备中";
    } else if (s_read_only_playback_state ==
               MUSIC_DISPLAY_PLAYBACK_PLAYING) {
        text = "暂停";
    } else if (s_read_only_playback_state ==
               MUSIC_DISPLAY_PLAYBACK_PAUSED) {
        text = "继续";
    }
    lv_label_set_text(s_read_only_playback_label, text);
    if (s_read_only_playback_button &&
        lv_obj_is_valid(s_read_only_playback_button)) {
        if (s_read_only_playback_state ==
            MUSIC_DISPLAY_PLAYBACK_PREPARING) {
            lv_obj_add_state(s_read_only_playback_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_read_only_playback_button,
                               LV_STATE_DISABLED);
        }
    }
}

static void update_read_only_back_button(void)
{
    if (!guider_ui.music_screen) return;

    if (!s_read_only_back_button ||
        !lv_obj_is_valid(s_read_only_back_button)) {
        s_read_only_back_button = lv_button_create(guider_ui.music_screen);
        lv_obj_set_pos(s_read_only_back_button, 12, 10);
        lv_obj_set_size(s_read_only_back_button, 120, 48);
        lv_obj_set_style_radius(s_read_only_back_button, 8, 0);
        lv_obj_set_style_bg_color(s_read_only_back_button,
                                  lv_color_hex(0x3F2204), 0);
        lv_obj_set_style_border_width(s_read_only_back_button, 0, 0);

        lv_obj_t *label = lv_label_create(s_read_only_back_button);
        lv_label_set_text(label, LV_SYMBOL_LEFT " 返回");
        const lv_font_t *font = app_font_chinese_22();
        if (font) lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(s_read_only_back_button,
                            read_only_back_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    if (!s_read_only_notation_button ||
        !lv_obj_is_valid(s_read_only_notation_button)) {
        s_read_only_notation_button =
            lv_button_create(guider_ui.music_screen);
        lv_obj_set_pos(s_read_only_notation_button, 144, 10);
        lv_obj_set_size(s_read_only_notation_button, 166, 48);
        lv_obj_set_style_radius(s_read_only_notation_button, 8, 0);
        lv_obj_set_style_bg_color(s_read_only_notation_button,
                                  lv_color_hex(0xF4E6D0), 0);
        lv_obj_set_style_border_width(s_read_only_notation_button, 1, 0);
        lv_obj_set_style_border_color(s_read_only_notation_button,
                                      lv_color_hex(0xB88A5B), 0);
        s_read_only_notation_label =
            lv_label_create(s_read_only_notation_button);
        const lv_font_t *font = app_font_chinese_22();
        if (font)
            lv_obj_set_style_text_font(s_read_only_notation_label, font, 0);
        lv_obj_set_style_text_color(s_read_only_notation_label,
                                    lv_color_hex(0x3F2204), 0);
        lv_obj_center(s_read_only_notation_label);
        lv_obj_add_event_cb(s_read_only_notation_button,
                            read_only_notation_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    if (!s_read_only_playback_button ||
        !lv_obj_is_valid(s_read_only_playback_button)) {
        s_read_only_playback_button =
            lv_button_create(guider_ui.music_screen);
        /* The score title occupies the centre of the top bar. Keep playback
         * in the otherwise unused lower-left footer so long titles remain
         * readable in read-only mode. */
        lv_obj_set_pos(s_read_only_playback_button, 16, 552);
        lv_obj_set_size(s_read_only_playback_button, 120, 48);
        lv_obj_set_style_radius(s_read_only_playback_button, 8, 0);
        lv_obj_set_style_bg_color(s_read_only_playback_button,
                                  lv_color_hex(0x0284C7), 0);
        lv_obj_set_style_border_width(s_read_only_playback_button, 0, 0);
        s_read_only_playback_label =
            lv_label_create(s_read_only_playback_button);
        const lv_font_t *font = app_font_chinese_22();
        if (font)
            lv_obj_set_style_text_font(s_read_only_playback_label, font, 0);
        lv_obj_set_style_text_color(s_read_only_playback_label,
                                    lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(s_read_only_playback_label);
        lv_obj_add_event_cb(s_read_only_playback_button,
                            read_only_playback_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    if (s_read_only_active && !s_creator_active) {
        lv_obj_clear_flag(s_read_only_back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_read_only_notation_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_read_only_playback_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(
            s_read_only_notation_label,
            s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF
                ? "\xE5\x88\x87\xE6\x8D\xA2\xE7\xAE\x80\xE8\xB0\xB1"
                : "\xE5\x88\x87\xE6\x8D\xA2\xE4\xBA\x94\xE7\xBA\xBF\xE8\xB0\xB1");
        update_read_only_playback_label();
    } else {
        lv_obj_add_flag(s_read_only_back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_read_only_notation_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_read_only_playback_button, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool score_has_lower_staff(void);
static void configure_numbered_rows(bool grand_staff);
static void staff_scroll_event_cb(lv_event_t *event);
static void handle_midi_data(const uint8_t *data, size_t len);
static void handle_parsed_midi(midi_data_t *md);
static void handle_midi_snapshot(midi_data_t *md, int notation_type,
                                 bool allow_empty);
static bool take_latest_snapshot(midi_data_t **snapshot, int *notation_type,
                                 bool *allow_empty);
static void handle_gesture(music_msg_type_t gesture);
static void render_colored_line(lv_obj_t *group, const char *line,
                                const midi_numbered_note_ref_t *refs,
                                size_t ref_count);
static bool update_note_span_color(int slot_index, uint8_t status);
static void reset_page_span_index(void);
static void remember_page_note_span(lv_obj_t *group, lv_span_t *span, int slot_index);
static bool current_page_may_contain_slot(int slot_index);
static lv_obj_t *line_group_for_index(int line_idx);

QueueHandle_t music_display_get_queue(void) { return s_queue; }

static void reset_note_status(void)
{
    memset(s_note_status, MUSIC_STATUS_UNKNOWN, sizeof(s_note_status));
}

static int score_to_bar_blocks(float score)
{
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    int blocks = (int)(score / 100.0f * 22.0f + 0.5f);
    if (blocks < 0) blocks = 0;
    if (blocks > 22) blocks = 22;
    return blocks;
}

static void make_score_bar(float score, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    int blocks = score_to_bar_blocks(score);
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < 22 && used < out_size - 1; ++i) {
        int w = snprintf(out + used, out_size - used, "%s", i < blocks ? "█" : "░");
        if (w <= 0) break;
        if ((size_t)w >= out_size - used) { used = out_size - 1; break; }
        used += (size_t)w;
    }
    out[out_size - 1] = '\0';
}

static void set_score_bar_label(lv_obj_t *label, float score)
{
    if (!label) return;
    char bar[100];
    make_score_bar(score, bar, sizeof(bar));
    lv_label_set_text(label, bar);
}

/* ── 逐音符着色辅助函数 ── */

static lv_color_t color_for_status(uint8_t status)
{
    switch (status) {
    case 1:  return lv_color_hex(0x16a34a); /* correct 绿 */
    case 2:  return lv_color_hex(0xca8a04); /* partial 黄 */
    case 3:  return lv_color_hex(0xdc2626); /* wrong 红 */
    case 4:  return lv_color_hex(0x9ca3af); /* missing 灰 */
    default: return lv_color_hex(0x000000); /* unknown 黑 */
    }
}

static lv_color_t numbered_note_display_color(int slot_index)
{
    const uint8_t status =
        slot_index >= 0 && slot_index < MUSIC_MAX_RESULT_SLOTS ?
        s_note_status[slot_index] : MUSIC_STATUS_UNKNOWN;
    if (status == MUSIC_STATUS_UNKNOWN &&
        slot_index >= s_expected_first_slot &&
        slot_index <= s_expected_last_slot)
        return lv_color_hex(0x0284C7);
    return color_for_status(status);
}

static bool span_area_has_size(const lv_area_t *area)
{
    return area && area->x2 > area->x1 && area->y2 > area->y1;
}

static void consider_note_cell_area(const lv_area_t *candidate,
                                    int expected_width,
                                    int expected_height,
                                    lv_area_t *best,
                                    int *best_score)
{
    if (!span_area_has_size(candidate) || !best || !best_score) return;
    const int width = candidate->x2 - candidate->x1;
    const int height = candidate->y2 - candidate->y1;
    const int width_error = width > expected_width ?
                            width - expected_width : expected_width - width;
    const int height_error = height > expected_height ?
                             height - expected_height : expected_height - height;
    const int score = width_error * 4 + height_error;
    if (score >= *best_score) return;
    *best = *candidate;
    *best_score = score;
}

static bool span_note_cell_area(lv_obj_t *group, const lv_span_t *span,
                                int expected_width, int expected_height,
                                lv_area_t *area)
{
    if (!group || !span || expected_width <= 0 || expected_height <= 0 ||
        !area)
        return false;
    const lv_span_coords_t coords =
        lv_spangroup_get_span_coords(group, span);

    /* A mapped note owns exactly one ASCII glyph. At a wrap boundary LVGL
     * can expose several span regions, so select the region closest to one
     * glyph cell instead of blindly preferring trailing/middle/heading. */
    int best_score = INT_MAX;
    consider_note_cell_area(&coords.heading, expected_width,
                            expected_height, area, &best_score);
    consider_note_cell_area(&coords.trailing, expected_width,
                            expected_height, area, &best_score);
    consider_note_cell_area(&coords.middle, expected_width,
                            expected_height, area, &best_score);
    return best_score != INT_MAX;
}

static void refresh_numbered_marker_positions(lv_obj_t *group)
{
    if (!group) return;
    lv_obj_update_layout(group);
    const int content_width = lv_obj_get_content_width(group);
    if (content_width <= 0) return;
    /* In LVGL 9 a fixed-height spangroup does not necessarily run its
     * self-size calculation during lv_obj_update_layout(). Span coordinates
     * depend on trailing_pos, which is populated by this explicit measure.
     * Without it every newly-created note span can report a zero area. */
    (void)lv_spangroup_get_expand_height(group, content_width);
    const lv_font_t *font = &lv_font_SimpMusicBasePSMTModified_90;
    lv_font_glyph_dsc_t reference = {0};
    if (!lv_font_get_glyph_dsc(font, &reference, '5', 0) ||
        reference.adv_w <= 0)
        return;
    for (int i = 0; i < s_page_numbered_marker_count; ++i) {
        music_numbered_marker_ref_t *marker =
            &s_page_numbered_markers[i];
        if (marker->group != group || !marker->span) continue;
        marker->positioned = false;
        lv_area_t cell;
        if (!span_note_cell_area(group, marker->span, reference.adv_w,
                                 font->line_height, &cell))
            continue;
        const int cell_width = cell.x2 - cell.x1;
        const int digit_top =
            cell.y1 + MUSIC_NUMBERED_DIGIT_TOP_FROM_LINE_PX;
        uint8_t reduction_lines = marker->reduction_line_count;
        if (reduction_lines > MUSIC_NUMBERED_MAX_REDUCTION_LINES)
            reduction_lines = MUSIC_NUMBERED_MAX_REDUCTION_LINES;
        const int visible_bottom =
            cell.y1 + MUSIC_NUMBERED_DIGIT_BOTTOM_FROM_LINE_PX +
            reduction_lines * MUSIC_NUMBERED_REDUCTION_LINE_DEPTH_PX;
        int octave_dots = marker->octave < 0 ? -marker->octave :
                          marker->octave;
        if (octave_dots > MUSIC_NUMBERED_MAX_OCTAVE_DOTS)
            octave_dots = MUSIC_NUMBERED_MAX_OCTAVE_DOTS;
        marker->cell_left_x = (int16_t)cell.x1;
        marker->cell_right_x = (int16_t)cell.x2;
        marker->center_x = (int16_t)(cell.x1 + cell_width / 2);
        marker->digit_top_y = (int16_t)digit_top;
        marker->digit_bottom_y = (int16_t)(cell.y1 +
            MUSIC_NUMBERED_DIGIT_BOTTOM_FROM_LINE_PX);
        marker->nearest_center_y = (int16_t)(marker->octave > 0 ?
            digit_top - MUSIC_NUMBERED_DOT_GAP_PX -
                MUSIC_NUMBERED_DOT_RADIUS_PX :
            visible_bottom + MUSIC_NUMBERED_DOT_GAP_PX +
                MUSIC_NUMBERED_DOT_RADIUS_PX);
        marker->upper_ink_y = (int16_t)(marker->octave > 0 ?
            marker->nearest_center_y - MUSIC_NUMBERED_DOT_RADIUS_PX -
                (octave_dots - 1) * MUSIC_NUMBERED_DOT_STEP_PX :
            digit_top);
        marker->lower_ink_y = (int16_t)(marker->octave < 0 ?
            marker->nearest_center_y + MUSIC_NUMBERED_DOT_RADIUS_PX +
                (octave_dots - 1) * MUSIC_NUMBERED_DOT_STEP_PX :
            visible_bottom);
        marker->positioned = true;
    }
}

static int numbered_row_index(lv_obj_t *group)
{
    for (int i = 0; i < MUSIC_LINES_PER_PAGE; ++i) {
        if (s_line_groups[i] == group) return i;
    }
    return -1;
}

static const music_numbered_marker_ref_t *numbered_marker_for_note(
    int note_index, lv_obj_t *group)
{
    if (note_index < 0 || note_index >= MAX_NOTES) return NULL;
    const int marker_index = s_page_marker_by_note[note_index];
    if (marker_index < 0 || marker_index >= s_page_numbered_marker_count)
        return NULL;
    const music_numbered_marker_ref_t *marker =
        &s_page_numbered_markers[marker_index];
    return marker->group == group && marker->positioned ? marker : NULL;
}

static void draw_numbered_line(lv_layer_t *layer, int x1, int y1,
                               int x2, int y2, int width,
                               lv_color_t color)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = width > 0 ? width : 1;
    line.opa = LV_OPA_COVER;
    line.p1 = (lv_point_precise_t){.x = x1, .y = y1};
    line.p2 = (lv_point_precise_t){.x = x2, .y = y2};
    lv_draw_line(layer, &line);
}

static void draw_numbered_curve(lv_layer_t *layer, int x1, int y1,
                                int x2, int y2, int apex_y,
                                lv_color_t color)
{
    int previous_x = x1;
    int previous_y = y1;
    for (int segment = 1; segment <= MUSIC_NUMBERED_CONNECTION_SEGMENTS;
         ++segment) {
        const int inverse = MUSIC_NUMBERED_CONNECTION_SEGMENTS - segment;
        const int denominator = MUSIC_NUMBERED_CONNECTION_SEGMENTS *
                                MUSIC_NUMBERED_CONNECTION_SEGMENTS;
        const int x = x1 + (x2 - x1) * segment /
                           MUSIC_NUMBERED_CONNECTION_SEGMENTS;
        const int y = (inverse * inverse * y1 +
                       2 * inverse * segment * apex_y +
                       segment * segment * y2) / denominator;
        draw_numbered_line(layer, previous_x, previous_y, x, y, 2, color);
        previous_x = x;
        previous_y = y;
    }
}

static void draw_numbered_dot(lv_layer_t *layer, int center_x, int center_y,
                              int radius, lv_color_t color)
{
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_opa = LV_OPA_COVER;
    dot.bg_color = color;
    dot.border_opa = LV_OPA_TRANSP;
    dot.radius = LV_RADIUS_CIRCLE;
    const lv_area_t area = {
        .x1 = center_x - radius,
        .y1 = center_y - radius,
        .x2 = center_x + radius - 1,
        .y2 = center_y + radius - 1,
    };
    lv_draw_rect(layer, &dot, &area);
}

static void numbered_connection_ink_bounds(lv_obj_t *group, int x1, int x2,
                                           int *top, int *bottom)
{
    if (!top || !bottom) return;
    *top = MUSIC_NUMBERED_ROW_HEIGHT_PX;
    *bottom = 0;
    if (x2 < x1) {
        const int swap = x1;
        x1 = x2;
        x2 = swap;
    }
    for (int i = 0; i < s_page_numbered_marker_count; ++i) {
        const music_numbered_marker_ref_t *marker =
            &s_page_numbered_markers[i];
        if (marker->group != group || !marker->positioned ||
            marker->center_x < x1 || marker->center_x > x2)
            continue;
        if (marker->upper_ink_y < *top) *top = marker->upper_ink_y;
        if (marker->lower_ink_y > *bottom) *bottom = marker->lower_ink_y;
    }
    if (*top == MUSIC_NUMBERED_ROW_HEIGHT_PX) {
        *top = MUSIC_NUMBERED_ROW_PADDING_Y_PX +
               MUSIC_NUMBERED_DIGIT_TOP_FROM_LINE_PX;
        *bottom = MUSIC_NUMBERED_ROW_PADDING_Y_PX +
                  MUSIC_NUMBERED_DIGIT_BOTTOM_FROM_LINE_PX;
    }
}

static uint8_t normalized_numbered_staff(const midi_note_t *note)
{
    return note && note->staff == 2 ? 2 : 1;
}

static uint8_t normalized_numbered_voice(const midi_note_t *note)
{
    return note && note->voice != 0 ? note->voice : 1;
}

static void build_numbered_connection_index(void)
{
    memset(s_numbered_connection_stop, 0xff,
           sizeof(s_numbered_connection_stop));
    memset(s_numbered_open_connection, 0xff,
           sizeof(s_numbered_open_connection));
    int16_t *slur_start = s_numbered_open_connection[0];
    int16_t *gliss_start = s_numbered_open_connection[1];

    int note_count = s_midi_data.note_count;
    if (note_count > MAX_NOTES) note_count = MAX_NOTES;
    for (int i = 0; i < note_count; ++i) {
        const midi_note_t *note = &s_midi_data.notes[i];

        if (note->slur_stop != 0 && slur_start[note->slur_stop] >= 0) {
            s_numbered_connection_stop[MUSIC_NUMBERED_CONNECTION_SLUR]
                                        [slur_start[note->slur_stop]] =
                (int16_t)i;
            slur_start[note->slur_stop] = -1;
        }
        if (note->slur_start != 0)
            slur_start[note->slur_start] = (int16_t)i;

        if (note->gliss_stop != 0 && gliss_start[note->gliss_stop] >= 0) {
            s_numbered_connection_stop[MUSIC_NUMBERED_CONNECTION_GLISS]
                                        [gliss_start[note->gliss_stop]] =
                (int16_t)i;
            gliss_start[note->gliss_stop] = -1;
        }
        if (note->gliss_start != 0)
            gliss_start[note->gliss_start] = (int16_t)i;

        if ((note->tie_flags & MIDI_NOTE_TIE_START) == 0) continue;
        for (int j = i + 1; j < note_count; ++j) {
            const midi_note_t *stop = &s_midi_data.notes[j];
            if ((stop->tie_flags & MIDI_NOTE_TIE_STOP) == 0 ||
                stop->note != note->note ||
                normalized_numbered_staff(stop) !=
                    normalized_numbered_staff(note) ||
                normalized_numbered_voice(stop) !=
                    normalized_numbered_voice(note))
                continue;
            s_numbered_connection_stop[MUSIC_NUMBERED_CONNECTION_TIE][i] =
                (int16_t)j;
            break;
        }
    }
}

static void draw_numbered_connections(lv_obj_t *group, lv_layer_t *layer,
                                      const lv_area_t *group_area)
{
    const int row = numbered_row_index(group);
    if (row < 0 || !group_area ||
        s_numbered_row_end_tick[row] <= s_numbered_row_start_tick[row])
        return;
    const int content_width = lv_obj_get_content_width(group);
    const lv_color_t color = lv_color_hex(0x171717);
    int note_count = s_midi_data.note_count;
    if (note_count > MAX_NOTES) note_count = MAX_NOTES;

    for (int type = 0; type < MUSIC_NUMBERED_CONNECTION_TYPES; ++type) {
        for (int start_index = 0; start_index < note_count; ++start_index) {
            const int stop_index =
                s_numbered_connection_stop[type][start_index];
            if (stop_index < 0 || stop_index >= note_count) continue;
            const midi_note_t *start = &s_midi_data.notes[start_index];
            const midi_note_t *stop = &s_midi_data.notes[stop_index];
            const uint8_t start_staff = normalized_numbered_staff(start);
            const uint8_t stop_staff = normalized_numbered_staff(stop);
            if ((start_staff != s_numbered_row_staff[row] &&
                 stop_staff != s_numbered_row_staff[row]) ||
                start->start_tick >= s_numbered_row_end_tick[row] ||
                stop->start_tick < s_numbered_row_start_tick[row])
                continue;

            const music_numbered_marker_ref_t *start_marker =
                numbered_marker_for_note(start_index, group);
            const music_numbered_marker_ref_t *stop_marker =
                numbered_marker_for_note(stop_index, group);
            int x1 = start_marker ? start_marker->center_x : 4;
            int x2 = stop_marker ? stop_marker->center_x : content_width - 4;
            if (x2 <= x1 + 3) continue;

            if (type == MUSIC_NUMBERED_CONNECTION_GLISS) {
                int y1 = start_marker ? start_marker->digit_top_y - 2 :
                         MUSIC_NUMBERED_ROW_PADDING_Y_PX + 12;
                int direction = stop->note > start->note ? -1 :
                                stop->note < start->note ? 1 : 0;
                int y2 = stop_marker ? stop_marker->digit_top_y - 2 : y1;
                y2 += direction * 10;
                const int absolute_x1 = group_area->x1 + x1;
                const int absolute_y1 = group_area->y1 + y1;
                const int absolute_x2 = group_area->x1 + x2;
                const int absolute_y2 = group_area->y1 + y2;
                draw_numbered_line(layer, absolute_x1, absolute_y1,
                                   absolute_x2, absolute_y2, 2, color);
                if (stop_marker) {
                    draw_numbered_line(layer, absolute_x2, absolute_y2,
                                       absolute_x2 - 7,
                                       absolute_y2 - direction * 5 - 3,
                                       2, color);
                    draw_numbered_line(layer, absolute_x2, absolute_y2,
                                       absolute_x2 - 7,
                                       absolute_y2 - direction * 5 + 3,
                                       2, color);
                }
                continue;
            }

            int ink_top = 0;
            int ink_bottom = 0;
            numbered_connection_ink_bounds(group, x1, x2,
                                           &ink_top, &ink_bottom);
            int lane = type == MUSIC_NUMBERED_CONNECTION_TIE ? 0 :
                       1 + ((start->slur_start != 0 ?
                             start->slur_start : start_index) & 1);
            const int needed = 10 + lane * 8;
            const int top_clearance = ink_top - 2;
            const int bottom_clearance =
                MUSIC_NUMBERED_ROW_HEIGHT_PX - ink_bottom - 2;
            const bool below = top_clearance < needed &&
                               bottom_clearance > top_clearance;
            int y1 = below ? ink_bottom + 3 : ink_top - 3;
            if (y1 < 2) y1 = 2;
            if (y1 > MUSIC_NUMBERED_ROW_HEIGHT_PX - 3)
                y1 = MUSIC_NUMBERED_ROW_HEIGHT_PX - 3;
            int y2 = y1;
            int apex_y = below ? y1 + needed : y1 - needed;
            if (apex_y < 2) apex_y = 2;
            if (apex_y > MUSIC_NUMBERED_ROW_HEIGHT_PX - 3)
                apex_y = MUSIC_NUMBERED_ROW_HEIGHT_PX - 3;
            draw_numbered_curve(layer,
                group_area->x1 + x1, group_area->y1 + y1,
                group_area->x1 + x2, group_area->y1 + y2,
                group_area->y1 + apex_y, color);
        }
    }
}

static void draw_numbered_overlays_cb(lv_event_t *event)
{
    lv_obj_t *group = lv_event_get_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!group || !layer) return;

    lv_area_t group_area;
    lv_obj_get_coords(group, &group_area);

    for (int i = 0; i < s_page_numbered_marker_count; ++i) {
        const music_numbered_marker_ref_t *marker =
            &s_page_numbered_markers[i];
        if (marker->group != group || !marker->positioned)
            continue;
        const int center_x = group_area.x1 + marker->center_x;
        int dot_count = marker->octave > 0 ? marker->octave :
                        -marker->octave;
        if (dot_count > MUSIC_NUMBERED_MAX_OCTAVE_DOTS)
            dot_count = MUSIC_NUMBERED_MAX_OCTAVE_DOTS;
        const int direction = marker->octave > 0 ? -1 : 1;
        const int nearest_center =
            group_area.y1 + marker->nearest_center_y;

        const lv_color_t note_color =
            numbered_note_display_color(marker->slot_index);
        for (int level = 0; level < dot_count; ++level) {
            const int center_y = nearest_center + direction * level *
                                 MUSIC_NUMBERED_DOT_STEP_PX;
            draw_numbered_dot(layer, center_x, center_y,
                              MUSIC_NUMBERED_DOT_RADIUS_PX, note_color);
        }

        if (marker->reduction_line_count > 1) {
            uint8_t line_count = marker->reduction_line_count;
            if (line_count > MUSIC_NUMBERED_MAX_REDUCTION_LINES)
                line_count = MUSIC_NUMBERED_MAX_REDUCTION_LINES;
            for (uint8_t line = 2; line <= line_count; ++line) {
                const int y = group_area.y1 + marker->digit_bottom_y +
                    line * MUSIC_NUMBERED_REDUCTION_LINE_DEPTH_PX;
                draw_numbered_line(layer,
                    group_area.x1 + marker->cell_left_x + 3, y,
                    group_area.x1 + marker->cell_right_x - 3, y,
                    2, note_color);
            }
        }

        if (marker->slot_index >= 0 &&
            marker->slot_index < s_midi_data.note_count) {
            uint8_t augmentation_dots =
                s_midi_data.notes[marker->slot_index].dots;
            if (augmentation_dots > MUSIC_NUMBERED_MAX_AUGMENTATION_DOTS)
                augmentation_dots = MUSIC_NUMBERED_MAX_AUGMENTATION_DOTS;
            const int y = group_area.y1 +
                (marker->digit_top_y + marker->digit_bottom_y) / 2;
            for (uint8_t dot = 0; dot < augmentation_dots; ++dot) {
                const int x = group_area.x1 + marker->cell_right_x +
                    MUSIC_NUMBERED_AUGMENTATION_GAP_PX +
                    dot * MUSIC_NUMBERED_AUGMENTATION_STEP_PX;
                draw_numbered_dot(layer, x, y,
                                  MUSIC_NUMBERED_DOT_RADIUS_PX, note_color);
            }
        }
    }

    draw_numbered_connections(group, layer, &group_area);
}

static void clear_spangroup(lv_obj_t *group)
{
    if (!group) return;
    while (lv_spangroup_get_span_count(group) > 0) {
        lv_span_t *span = lv_spangroup_get_child(group, 0);
        if (!span) break;
        lv_spangroup_delete_span(group, span);
    }
}

static lv_span_t *add_text_span(lv_obj_t *group, const char *text, lv_color_t color)
{
    if (!group || !text) return NULL;
    lv_span_t *span = lv_spangroup_new_span(group);
    if (!span) return NULL;
    lv_span_set_text(span, text);
    lv_style_set_text_font(lv_span_get_style(span), &lv_font_SimpMusicBasePSMTModified_90);
    lv_style_set_text_color(lv_span_get_style(span), color);
    return span;
}

static lv_span_t *add_text_span_range(lv_obj_t *group, const char *text,
                                      size_t length, lv_color_t color)
{
    lv_span_t *last = NULL;
    while (text && length > 0) {
        char chunk[64];
        size_t chunk_length = length;
        if (chunk_length >= sizeof(chunk)) chunk_length = sizeof(chunk) - 1U;
        memcpy(chunk, text, chunk_length);
        chunk[chunk_length] = '\0';
        last = add_text_span(group, chunk, color);
        text += chunk_length;
        length -= chunk_length;
    }
    return last;
}

static bool token_is_note_glyph(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static bool numbered_note_ref_matches_text(
    const char *line, size_t line_length,
    const midi_numbered_note_ref_t *ref)
{
    if (!line || !ref || ref->byte_length == 0) return false;
    size_t offset = ref->byte_offset;
    size_t length = ref->byte_length;
    if (offset > line_length || length > line_length - offset) return false;
    return length == 1U && token_is_note_glyph(line[offset]);
}

static lv_obj_t *line_group_for_index(int line_idx)
{
    ensure_line_groups();
    if (line_idx < 0 || line_idx >= MUSIC_LINES_PER_PAGE) return NULL;
    return s_line_groups[line_idx];
}

static void reset_page_span_index(void)
{
    s_page_note_span_count = 0;
    s_page_numbered_marker_count = 0;
    s_page_numbered_marker_overflow_logged = false;
    memset(s_page_marker_by_note, 0xff, sizeof(s_page_marker_by_note));
    memset(s_numbered_connection_stop, 0xff,
           sizeof(s_numbered_connection_stop));
    memset(s_numbered_row_start_tick, 0,
           sizeof(s_numbered_row_start_tick));
    memset(s_numbered_row_end_tick, 0,
           sizeof(s_numbered_row_end_tick));
    memset(s_numbered_row_staff, 0, sizeof(s_numbered_row_staff));
    s_page_first_slot = -1;
    s_page_last_slot = -1;
    s_numbered_page_end_tick = 0;
}

static void remember_page_note_span(lv_obj_t *group, lv_span_t *span, int slot_index)
{
    if (slot_index < 0) return;
    if (s_page_first_slot < 0 || slot_index < s_page_first_slot)
        s_page_first_slot = slot_index;
    if (slot_index > s_page_last_slot) s_page_last_slot = slot_index;
    if (!group || !span) return;
    if (s_page_note_span_count >= MUSIC_MAX_NOTE_SPANS_PER_PAGE) return;
    s_page_note_spans[s_page_note_span_count++] = (music_note_span_ref_t){
        .group = group, .span = span, .slot_index = slot_index };
}

static void remember_numbered_marker(lv_obj_t *group, lv_span_t *span,
                                     int slot_index, int8_t octave,
                                     uint8_t reduction_line_count)
{
    if (!group || !span) return;
    if (s_page_numbered_marker_count >=
        MUSIC_MAX_NUMBERED_MARKERS_PER_PAGE) {
        if (!s_page_numbered_marker_overflow_logged) {
            ESP_LOGW(TAG, "numbered marker cache full");
            s_page_numbered_marker_overflow_logged = true;
        }
        return;
    }
    const int marker_index = s_page_numbered_marker_count++;
    s_page_numbered_markers[marker_index] =
        (music_numbered_marker_ref_t){
            .group = group,
            .span = span,
            .slot_index = slot_index,
            .octave = octave,
            .reduction_line_count = reduction_line_count,
        };
    if (slot_index >= 0 && slot_index < MAX_NOTES)
        s_page_marker_by_note[slot_index] = (int16_t)marker_index;
}

static bool current_page_may_contain_slot(int slot_index)
{
    return s_page_first_slot >= 0 && slot_index >= s_page_first_slot && slot_index <= s_page_last_slot;
}

static void apply_note_span_style(music_note_span_ref_t *ref,
                                  uint8_t status)
{
    if (!ref || !ref->span) return;
    const bool guided = status == MUSIC_STATUS_UNKNOWN &&
                        ref->slot_index >= s_expected_first_slot &&
                        ref->slot_index <= s_expected_last_slot;
    lv_style_t *style = lv_span_get_style(ref->span);
    lv_style_set_text_color(style, guided ? lv_color_hex(0x0284C7)
                                          : color_for_status(status));
    lv_style_set_text_decor(style, guided ? LV_TEXT_DECOR_UNDERLINE
                                          : LV_TEXT_DECOR_NONE);
}

static bool update_note_span_color(int slot_index, uint8_t status)
{
    bool updated = false;
    for (int i = 0; i < s_page_note_span_count; ++i) {
        music_note_span_ref_t *ref = &s_page_note_spans[i];
        if (ref->slot_index != slot_index || !ref->span) continue;
        apply_note_span_style(ref, status);
        if (ref->group) { lv_spangroup_refr_mode(ref->group); lv_obj_invalidate(ref->group); }
        updated = true;
    }
    return updated;
}

/* ── 逐 Token 着色渲染一行简谱 ── */
static void render_colored_line(lv_obj_t *group, const char *line,
                                const midi_numbered_note_ref_t *refs,
                                size_t ref_count)
{
    clear_spangroup(group);
    if (!group) return;
    if (!line || line[0] == '\0') {
        add_text_span(group, " ", lv_color_hex(0x000000));
        lv_spangroup_refr_mode(group);
        return;
    }

    const size_t line_length = strlen(line);
    size_t text_cursor = 0;
    for (size_t i = 0; refs && i < ref_count; ++i) {
        const midi_numbered_note_ref_t *ref = &refs[i];
        const size_t offset = ref->byte_offset;
        const size_t length = ref->byte_length;
        const int note_idx = ref->note_index;

        /* A malformed entry stays plain text.  Do not consume the cursor so
         * a later valid offset can still be rendered with the right index. */
        if (offset < text_cursor || note_idx >= s_midi_data.note_count ||
            !numbered_note_ref_matches_text(line, line_length, ref)) {
            continue;
        }
        add_text_span_range(group, line + text_cursor,
                            offset - text_cursor,
                            lv_color_hex(0x000000));

        uint8_t status = note_idx < MUSIC_MAX_RESULT_SLOTS ?
                         s_note_status[note_idx] :
                         MUSIC_STATUS_UNKNOWN;
        lv_span_t *span = add_text_span_range(
            group, line + offset, length, color_for_status(status));
        if (span && status == MUSIC_STATUS_UNKNOWN &&
            note_idx >= s_expected_first_slot &&
            note_idx <= s_expected_last_slot) {
            lv_style_t *style = lv_span_get_style(span);
            lv_style_set_text_color(style, lv_color_hex(0x0284C7));
            lv_style_set_text_decor(style, LV_TEXT_DECOR_UNDERLINE);
        }
        remember_page_note_span(group, span, note_idx);
        remember_numbered_marker(group, span, note_idx, ref->octave,
                                 ref->reduction_line_count);
        text_cursor = offset + length;
    }
    add_text_span_range(group, line + text_cursor,
                        line_length - text_cursor,
                        lv_color_hex(0x000000));

    lv_spangroup_refr_mode(group);
}

static void center_numbered_row(lv_obj_t *group, int line_index,
                                bool grand_staff)
{
    if (!group || line_index < 0 || line_index >= MUSIC_LINES_PER_PAGE)
        return;
    const int base_x = grand_staff ? 90 : 42;
    const int maximum_width = grand_staff ? 875 : 929;
    uint32_t used_width =
        lv_spangroup_get_expand_width(group, (uint32_t)maximum_width);
    int row_width = used_width >= (uint32_t)maximum_width ?
                    maximum_width : (int)used_width + 2;
    if (row_width < 1) row_width = 1;
    if (row_width > maximum_width) row_width = maximum_width;
    const int row_x = base_x + (maximum_width - row_width) / 2;
    lv_obj_set_pos(group, row_x, 25 + line_index * 220);
    lv_obj_set_size(group, row_width, MUSIC_NUMBERED_ROW_HEIGHT_PX);
    lv_obj_update_layout(group);
    const int content_width = lv_obj_get_content_width(group);
    if (content_width > 0)
        (void)lv_spangroup_get_expand_height(group, content_width);
}

static void reset_numbered_legacy_inference_state(void)
{
    s_numbered_inference_evaluated = false;
    s_numbered_inferred_categories = 0;
}

static void clear_numbered_legacy_inference(void)
{
    if (s_numbered_inferred_categories == 0) return;
    for (int index = 0; index < s_midi_data.note_count; ++index) {
        midi_note_t *note = &s_midi_data.notes[index];
        if ((s_numbered_inferred_categories &
             MUSIC_NUMBERED_INFERRED_DOTS) != 0) {
            note->dots = 0;
        }
        if ((s_numbered_inferred_categories &
             MUSIC_NUMBERED_INFERRED_TIES) != 0) {
            note->tie_flags = 0;
        }
    }
    reset_numbered_legacy_inference_state();
}

static void prepare_numbered_legacy_inference(void)
{
    if (s_numbered_inference_evaluated ||
        !s_midi_data.numbered_legacy_inference_allowed) {
        return;
    }

    midi_numbered_inference_stats_t inferred = {0};
    s_numbered_inference_evaluated = true;
    if (!midi_numbered_infer_legacy_marks(&s_midi_data, &inferred)) {
        ESP_LOGW(TAG, "numbered legacy inference: invalid score or meter");
        return;
    }
    if (inferred.inferred_dots != 0)
        s_numbered_inferred_categories |= MUSIC_NUMBERED_INFERRED_DOTS;
    if (inferred.inferred_ties != 0)
        s_numbered_inferred_categories |= MUSIC_NUMBERED_INFERRED_TIES;
    ESP_LOGI(TAG,
             "numbered inferred: ties=%u slurs=%u gliss=%u dotted=%u",
             (unsigned)inferred.inferred_ties,
             (unsigned)inferred.inferred_slurs,
             (unsigned)inferred.inferred_glissandi,
             (unsigned)inferred.inferred_dots);
}

/* ── 刷新简谱显示（使用逐音符着色渲染） ── */
static void update_notation_display(void)
{
    if (!s_midi_parsed || s_midi_data.note_count <= 0) {
        ESP_LOGW(TAG, "no MIDI data to display");
        return;
    }

    lv_obj_t *gesture_target = guider_ui.music_screen
                                   ? guider_ui.music_screen
                                   : guider_ui.music_screen_cont_1;
    lv_obj_t *content = guider_ui.music_screen_cont_1;
    if (gesture_target &&
        (!s_navigation_event_target ||
         !lv_obj_is_valid(s_navigation_event_target) ||
         s_navigation_event_target != gesture_target)) {
        lv_obj_add_event_cb(gesture_target, navigation_gesture_event_cb,
                            LV_EVENT_GESTURE, NULL);
        s_navigation_event_target = gesture_target;
    }
    if (content) {
        lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
        bool continuous_scroll = s_creator_active &&
                                 s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF;
        lv_obj_stop_scroll_anim(content);
        lv_obj_set_scroll_dir(content, continuous_scroll ?
                              LV_DIR_VER : LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(content, continuous_scroll ?
                                  LV_SCROLLBAR_MODE_AUTO :
                                  LV_SCROLLBAR_MODE_OFF);
    }

    if (s_notation_type == MUSIC_DISPLAY_NOTATION_NUMBERED) {
        ensure_line_groups();
        reset_page_span_index();
        bool grand_staff = score_has_lower_staff();
        configure_numbered_rows(grand_staff);
        uint64_t measure_ticks = 0;
        if (!score_measure_ticks_locked(&measure_ticks)) {
            ESP_LOGE(TAG, "invalid meter for numbered notation paging");
            return;
        }
        for (int line = 0; line < MUSIC_LINES_PER_PAGE; ++line) {
            lv_obj_t *group = line_group_for_index(line);
            if (!group) continue;
            /* Span coordinates are needed immediately for octave overlays;
             * LVGL skips layout work for hidden objects. */
            lv_obj_clear_flag(group, LV_OBJ_FLAG_HIDDEN);
            int start_measure = grand_staff ?
                s_current_page * MUSIC_MEASURES_PER_SYSTEM :
                (s_current_page * MUSIC_LINES_PER_PAGE + line) *
                    MUSIC_MEASURES_PER_SYSTEM;
            uint8_t staff = grand_staff ? (uint8_t)(line + 1) : 1;
            size_t note_ref_count = 0;
            bool notation_truncated = false;
            bool has_line = midi_generate_measure_range_mapped(
                &s_midi_data,
                s_numbered_text_buffer, sizeof(s_numbered_text_buffer),
                start_measure, MUSIC_MEASURES_PER_SYSTEM, staff,
                s_numbered_note_refs, MAX_NOTES, &note_ref_count,
                &notation_truncated);
            if (notation_truncated) {
                ESP_LOGW(TAG,
                         "numbered notation truncated: page=%d line=%d refs=%u",
                         s_current_page, line, (unsigned)note_ref_count);
            }
            uint64_t start_tick = (uint64_t)start_measure * measure_ticks;
            uint64_t end_tick = start_tick +
                (uint64_t)MUSIC_MEASURES_PER_SYSTEM * measure_ticks;
            s_numbered_row_start_tick[line] = start_tick > UINT32_MAX ?
                                              UINT32_MAX :
                                              (uint32_t)start_tick;
            if (end_tick > UINT32_MAX) end_tick = UINT32_MAX;
            s_numbered_row_end_tick[line] = (uint32_t)end_tick;
            s_numbered_row_staff[line] = staff;
            if ((uint32_t)end_tick > s_numbered_page_end_tick)
                s_numbered_page_end_tick = (uint32_t)end_tick;
            render_colored_line(
                group, has_line ? s_numbered_text_buffer : " ",
                has_line ? s_numbered_note_refs : NULL,
                has_line ? note_ref_count : 0);
            center_numbered_row(group, line, grand_staff);
            refresh_numbered_marker_positions(group);
        }
        build_numbered_connection_index();
        for (int line = 0; line < MUSIC_LINES_PER_PAGE; ++line) {
            if (s_line_groups[line]) lv_obj_invalidate(s_line_groups[line]);
        }
    } else if (s_staff_view) {
        leland_score_view_show_page(s_staff_view, s_current_page, false);
    }

    /* 更新页码 */
    if (guider_ui.music_screen_page_index) {
        char page_str[16];
        snprintf(page_str, sizeof(page_str), "%d/%d", s_current_page + 1, s_total_pages);
        lv_label_set_text(guider_ui.music_screen_page_index, page_str);
    }
}

/* ── 应用 MIDI 数据到 UI ── */
static void staff_note_click_event_cb(lv_event_t *event)
{
    if (!s_creator_active || !s_staff_view ||
        s_notation_type != MUSIC_DISPLAY_NOTATION_STAFF ||
        !s_note_selection_callback)
        return;

    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int note_index;
    int measure_number;
    if (!leland_score_view_hit_test_note(s_staff_view,
                                         point.x, point.y,
                                         &note_index, &measure_number))
        return;
    s_note_selection_callback(note_index, measure_number,
                              s_note_selection_user_data);
}

static void staff_scroll_event_cb(lv_event_t *event)
{
    if (!s_creator_active) return;
    lv_obj_t *content = lv_event_get_target(event);
    if (!content || s_total_pages < 1) return;
    int height = lv_obj_get_content_height(content);
    if (height <= 0) height = 479;
    int page = (lv_obj_get_scroll_y(content) + height / 2) / height;
    if (page < 0) page = 0;
    if (page >= s_total_pages) page = s_total_pages - 1;
    if (page == s_current_page) return;
    s_current_page = page;
    if (guider_ui.music_screen_page_index) {
        char page_str[16];
        snprintf(page_str, sizeof(page_str), "%d/%d",
                 s_current_page + 1, s_total_pages);
        lv_label_set_text(guider_ui.music_screen_page_index, page_str);
    }
}

static bool score_has_lower_staff(void)
{
    for (int i = 0; i < s_midi_data.note_count; ++i) {
        if (s_midi_data.notes[i].staff == 2) return true;
    }
    return false;
}

static int score_total_measures(void)
{
    uint64_t measure_ticks = 0;
    if (!score_measure_ticks_locked(&measure_ticks) ||
        s_midi_data.note_count <= 0)
        return 1;
    uint64_t end_tick = 0;
    for (int i = 0; i < s_midi_data.note_count; ++i) {
        uint64_t end = (uint64_t)s_midi_data.notes[i].start_tick +
                       (uint64_t)s_midi_data.notes[i].duration;
        if (end > end_tick) end_tick = end;
    }
    uint64_t measure_count = end_tick / measure_ticks +
        (end_tick % measure_ticks != 0 ? 1U : 0U);
    int measures = measure_count > INT_MAX ? INT_MAX : (int)measure_count;
    return measures > 0 ? measures : 1;
}

static int numbered_page_count(void)
{
    int measures_per_page = score_has_lower_staff() ?
                            MUSIC_MEASURES_PER_SYSTEM :
                            MUSIC_MEASURES_PER_SYSTEM *
                            MUSIC_LINES_PER_PAGE;
    int pages = (score_total_measures() + measures_per_page - 1) /
                measures_per_page;
    return pages > 0 ? pages : 1;
}

static void configure_numbered_rows(bool grand_staff)
{
    for (int i = 0; i < MUSIC_LINES_PER_PAGE; ++i) {
        if (!s_line_groups[i]) continue;
        lv_obj_set_pos(s_line_groups[i], grand_staff ? 90 : 42,
                       25 + i * 220);
        lv_obj_set_size(s_line_groups[i], grand_staff ? 875 : 929,
                        MUSIC_NUMBERED_ROW_HEIGHT_PX);
        if (s_line_prefixes[i]) {
            if (grand_staff)
                lv_obj_clear_flag(s_line_prefixes[i], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_line_prefixes[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void apply_midi_data_to_ui(void)
{
    if (!guider_ui.music_screen) {
        ESP_LOGE(TAG, "music_screen NULL");
        return;
    }

    /* 切换到 music_screen */
    lv_screen_load(guider_ui.music_screen);
    update_read_only_back_button();

    /* 创建动态乐谱行 spangroup */
    ensure_line_groups();
    for (int i = 0; i < MUSIC_LINES_PER_PAGE; ++i) {
        if (s_line_groups[i])
            lv_obj_add_flag(s_line_groups[i], LV_OBJ_FLAG_HIDDEN);
        if (s_line_prefixes[i])
            lv_obj_add_flag(s_line_prefixes[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (s_notation_type == MUSIC_DISPLAY_NOTATION_NUMBERED) {
        prepare_numbered_legacy_inference();
        if (s_staff_view) leland_score_view_set_hidden(s_staff_view, true);
        lv_obj_scroll_to_y(guider_ui.music_screen_cont_1, 0, LV_ANIM_OFF);
        s_total_pages = numbered_page_count();
        ESP_LOGI(TAG, "rendering numbered notation (%d pages)",
                 s_total_pages);
    } else {
        clear_numbered_legacy_inference();
        if (!s_staff_view && guider_ui.music_screen_cont_1) {
            s_staff_view = leland_score_view_create(
                guider_ui.music_screen_cont_1);
            lv_obj_add_event_cb(guider_ui.music_screen_cont_1,
                                staff_scroll_event_cb, LV_EVENT_SCROLL, NULL);
            lv_obj_add_event_cb(guider_ui.music_screen_cont_1,
                                staff_note_click_event_cb,
                                LV_EVENT_SHORT_CLICKED, NULL);
        }
        if (!s_staff_view) {
            ESP_LOGE(TAG, "unable to create Leland score view");
            return;
        }

        /* A repeated practice can reuse byte-identical MIDI and make the
         * Leland view skip rebuilding. Clear feedback stored in the existing
         * scene before that fast path so a new attempt always starts clean. */
        leland_score_view_clear_note_colors(s_staff_view);
        leland_score_view_set_paginated(s_staff_view, !s_creator_active);
        leland_score_view_set_hidden(s_staff_view, false);
        char render_error[128] = {0};
        if (!leland_score_view_set_midi(s_staff_view, &s_midi_data,
                                        render_error,
                                        sizeof(render_error))) {
            ESP_LOGE(TAG, "Leland score layout failed: %s", render_error);
            /* A normal preview must never expose the previously rendered
             * score when its own layout fails.  The retained scene is useful
             * for Creator live-update recovery, so leave that path unchanged
             * and only hide stale content outside Creator Mode. */
            if (!s_creator_active) {
                leland_score_view_set_hidden(s_staff_view, true);
                if (guider_ui.music_screen_status_label) {
                    lv_label_set_text(guider_ui.music_screen_status_label,
                                      "乐谱排版失败");
                }
            }
            return;
        }
        leland_score_view_set_note_guide(s_staff_view,
                                         s_expected_first_slot,
                                         s_expected_last_slot);
        s_total_pages = leland_score_view_page_count(s_staff_view);
        ESP_LOGI(TAG, "rendering staff notation (%d pages)", s_total_pages);
    }

    /* 显示各元素 */
    if (guider_ui.music_screen_page_index)
        lv_obj_clear_flag(guider_ui.music_screen_page_index, LV_OBJ_FLAG_HIDDEN);
    if (guider_ui.music_screen_title)
        lv_obj_clear_flag(guider_ui.music_screen_title, LV_OBJ_FLAG_HIDDEN);
    if (guider_ui.music_screen_tonality_text)
        lv_obj_clear_flag(guider_ui.music_screen_tonality_text, LV_OBJ_FLAG_HIDDEN);
    if (guider_ui.music_screen_warning_infos)
        lv_obj_add_flag(guider_ui.music_screen_warning_infos, LV_OBJ_FLAG_HIDDEN);
    /* 标题 */
    if (guider_ui.music_screen_title) {
        lv_label_set_text(guider_ui.music_screen_title,
                          s_midi_data.title[0] ? s_midi_data.title : " ");
        const lv_font_t *font = app_font_chinese_22();
        if (font)
            lv_obj_set_style_text_font(guider_ui.music_screen_title, font, 0);
    }

    /* 调性 + 拍号 + BPM */
    if (guider_ui.music_screen_tonality_text) {
        char tonality_str[64];
        if (s_midi_data.tonality_forced) {
            midi_tonality_to_string(s_midi_data.tonality_sf,
                                    s_midi_data.tonality_minor,
                                    tonality_str, sizeof(tonality_str));
        } else {
            snprintf(tonality_str, sizeof(tonality_str), "调号=未定");
        }
        int len = strlen(tonality_str);
        int denom = s_midi_data.time_sig_den >= 0 &&
                    s_midi_data.time_sig_den <= 6 ?
                    1 << s_midi_data.time_sig_den : 4;
        snprintf(tonality_str + len, sizeof(tonality_str) - len, " %d/%d",
                 s_midi_data.time_sig_num, denom);
        len = strlen(tonality_str);
        char bpm_str[16];
        midi_bpm_to_string(s_midi_data.bpm, bpm_str, sizeof(bpm_str));
        snprintf(tonality_str + len, sizeof(tonality_str) - len, "  %s", bpm_str);
        lv_label_set_text(guider_ui.music_screen_tonality_text, tonality_str);
        const lv_font_t *font = app_font_chinese_22();
        if (font)
            lv_obj_set_style_text_font(guider_ui.music_screen_tonality_text,
                                       font, 0);
    }

    if (!s_creator_active && guider_ui.music_screen_status_label) {
        lv_label_set_text(guider_ui.music_screen_status_label,
                          s_read_only_active ? "只读浏览" : "演奏中..");
        const lv_font_t *font = app_font_chinese_22();
        if (font)
            lv_obj_set_style_text_font(guider_ui.music_screen_status_label,
                                       font, 0);
    }

    if (s_total_pages < 1) s_total_pages = 1;
    update_notation_display();
}

/* ── 手势翻页 ── */
static void handle_gesture(music_msg_type_t gesture)
{
    if (s_creator_active || !s_midi_parsed) return;

    bool page_changed = false;
    switch (gesture) {
    case MUSIC_MSG_SWITCH_SCREEN:
    case MUSIC_MSG_GESTURE_SWIPE_LEFT:
        if (s_current_page < s_total_pages - 1) {
            s_current_page++;
            page_changed = true;
        }
        break;
    case MUSIC_MSG_GESTURE_SWIPE_RIGHT:
        if (s_current_page > 0) {
            s_current_page--;
            page_changed = true;
        }
        break;
    default:
        break;
    }
    if (!page_changed) return;
    bsp_display_lock(portMAX_DELAY);
    update_notation_display();
    bsp_display_unlock();
}

/* ── MIDI 数据处理 ── */
static void handle_midi_data(const uint8_t *data, size_t len)
{
    midi_data_t *result = heap_caps_calloc(1, sizeof(*result),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) result = calloc(1, sizeof(*result));
    if (!result) {
        ESP_LOGE(TAG, "MIDI parse workspace allocation failed");
        return;
    }
    if (!midi_parse(data, len, result)) {
        ESP_LOGE(TAG, "MIDI parse failed");
        free(result);
        return;
    }
    ESP_LOGI(TAG, "MIDI parsed: BPM=%d, Notes=%d", result->bpm,
             result->note_count);
    s_midi_data = *result;
    free(result);
    s_midi_parsed = true;
    reset_numbered_legacy_inference_state();
    s_current_page = 0;
    reset_note_status();
    s_midi_data.title[0] = '\0';

    bsp_display_lock(portMAX_DELAY);
    apply_midi_data_to_ui();
    bsp_display_unlock();
}

static void handle_parsed_midi(midi_data_t *md)
{
    if (!md) return;
    s_midi_data = *md;
    s_midi_parsed = true;
    reset_numbered_legacy_inference_state();
    s_current_page = 0;
    reset_note_status();
    free(md);

    bsp_display_lock(portMAX_DELAY);
    apply_midi_data_to_ui();
    bsp_display_unlock();
}

static void apply_empty_snapshot_to_ui(void)
{
    if (!guider_ui.music_screen) setup_scr_music_screen(&guider_ui);
    if (!guider_ui.music_screen) return;

    lv_screen_load(guider_ui.music_screen);
    ensure_line_groups();
    for (int i = 0; i < MUSIC_LINES_PER_PAGE; ++i) {
        if (s_line_groups[i])
            lv_obj_add_flag(s_line_groups[i], LV_OBJ_FLAG_HIDDEN);
        if (s_line_prefixes[i])
            lv_obj_add_flag(s_line_prefixes[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_staff_view) leland_score_view_set_hidden(s_staff_view, true);
    if (guider_ui.music_screen_warning_infos)
        lv_obj_add_flag(guider_ui.music_screen_warning_infos,
                        LV_OBJ_FLAG_HIDDEN);
    if (guider_ui.music_screen_title)
        lv_label_set_text(guider_ui.music_screen_title,
                          s_midi_data.title[0] ? s_midi_data.title :
                          "Creator Mode");
    if (guider_ui.music_screen_tonality_text) {
        char info[64];
        if (s_midi_data.tonality_forced) {
            midi_tonality_to_string(s_midi_data.tonality_sf,
                                    s_midi_data.tonality_minor,
                                    info, sizeof(info));
        } else {
            snprintf(info, sizeof(info), "调号=未定");
        }
        const int denominator = s_midi_data.time_sig_den >= 0 &&
                                s_midi_data.time_sig_den <= 6 ?
                                1 << s_midi_data.time_sig_den : 4;
        const size_t length = strlen(info);
        snprintf(info + length, sizeof(info) - length, " %d/%d  BPM=%d",
                 s_midi_data.time_sig_num, denominator, s_midi_data.bpm);
        lv_label_set_text(guider_ui.music_screen_tonality_text, info);
    }
    if (guider_ui.music_screen_page_index)
        lv_label_set_text(guider_ui.music_screen_page_index, "1/1");
    s_total_pages = 1;
}

static void handle_midi_snapshot(midi_data_t *md, int notation_type,
                                 bool allow_empty)
{
    if (!md) return;
    if (!s_creator_active) {
        free(md);
        return;
    }
    s_notation_type = notation_type;
    s_midi_data = *md;
    reset_numbered_legacy_inference_state();
    s_current_page = 0;
    reset_note_status();
    bool empty = s_midi_data.note_count <= 0;
    s_midi_parsed = !empty;
    free(md);

    bsp_display_lock(portMAX_DELAY);
    if (empty) {
        if (allow_empty) apply_empty_snapshot_to_ui();
    } else {
        apply_midi_data_to_ui();
        if (s_total_pages > 1) {
            s_current_page = s_total_pages - 1;
            update_notation_display();
        }
    }
    bsp_display_unlock();
}

static bool take_latest_snapshot(midi_data_t **snapshot, int *notation_type,
                                 bool *allow_empty)
{
    if (!snapshot || !notation_type || !allow_empty || !s_snapshot_mutex)
        return false;
    *snapshot = NULL;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    if (s_snapshot_mailbox.latest) {
        *snapshot = creator_snapshot_mailbox_take(&s_snapshot_mailbox);
        *notation_type = s_latest_snapshot_notation;
        *allow_empty = s_latest_snapshot_allow_empty;
    }
    xSemaphoreGive(s_snapshot_mutex);
    return *snapshot != NULL;
}
/* ── 实时音符状态更新 ── */
void music_display_apply_note_result(int target_index, int expected_midi,
                                      int played_midi, float confidence,
                                      bool pitch_ok, bool rhythm_ok)
{
    if (target_index < 1 || target_index > MUSIC_MAX_RESULT_SLOTS) return;
    if (s_creator_active || s_read_only_active) return;

    bsp_display_lock(portMAX_DELAY);

    /* 错误时显示警告 */
    int idx0 = target_index - 1;
    uint8_t status;
    if (played_midi < 0) {
        status = 4; /* missing */
    } else if (expected_midi >= 0 &&
               abs(played_midi - expected_midi) <=
                   MUSIC_DISPLAY_GREEN_PITCH_TOLERANCE) {
        status = 1; /* pitch-tolerant correct */
    } else if (expected_midi >= 0 &&
               abs(played_midi - expected_midi) <
                   MUSIC_DISPLAY_RED_PITCH_THRESHOLD) {
        status = 2; /* noticeable, but acceptable in beginner mode */
    } else if (!pitch_ok) {
        status = 3; /* wrong */
    } else if (!rhythm_ok) {
        status = 2; /* partial */
    } else {
        status = 1; /* correct */
    }

    s_note_status[idx0] = status;

    /* 自动翻页 */
    if (s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF && s_staff_view)
        leland_score_view_set_note_color(s_staff_view, idx0,
                                         status == 1 ? 0x16a34a :
                                         status == 2 ? 0xca8a04 :
                                         status == 4 ? 0x9ca3af :
                                                       0xdc2626);
    else if (s_notation_type == MUSIC_DISPLAY_NOTATION_NUMBERED &&
             current_page_may_contain_slot(idx0))
        update_note_span_color(idx0, status);

    bsp_display_unlock();
}

void music_display_set_expected_note_group(int first_target_index,
                                           int last_target_index)
{
    int first = first_target_index - 1;
    int last = last_target_index - 1;
    if (first_target_index <= 0 || last_target_index < first_target_index) {
        first = -1;
        last = -1;
    }

    bsp_display_lock(portMAX_DELAY);
    if (s_expected_first_slot == first &&
        s_expected_last_slot == last) {
        bsp_display_unlock();
        return;
    }
    s_expected_first_slot = first;
    s_expected_last_slot = last;
    if (s_staff_view) {
        leland_score_view_set_note_guide(s_staff_view, first, last);
    }
    /* Numbered notation stores its guide in span styles. Staff notation now
     * moves independent cached overlay rectangles, so touching every span
     * and line group here would reintroduce unnecessary LVGL work. */
    if (s_notation_type == MUSIC_DISPLAY_NOTATION_NUMBERED) {
        for (int i = 0; i < s_page_note_span_count; ++i) {
            music_note_span_ref_t *ref = &s_page_note_spans[i];
            if (!ref->span || ref->slot_index < 0 ||
                ref->slot_index >= MUSIC_MAX_RESULT_SLOTS) {
                continue;
            }
            apply_note_span_style(ref, s_note_status[ref->slot_index]);
        }
        for (int line = 0; line < MUSIC_LINES_PER_PAGE; ++line) {
            if (s_line_groups[line] && lv_obj_is_valid(s_line_groups[line])) {
                lv_spangroup_refr_mode(s_line_groups[line]);
                lv_obj_invalidate(s_line_groups[line]);
            }
        }
    }
    bsp_display_unlock();
}

void music_display_reset_note_feedback(void)
{
    bsp_display_lock(portMAX_DELAY);
    reset_note_status();
    s_expected_first_slot = -1;
    s_expected_last_slot = -1;
    s_current_page = 0;
    if (s_staff_view) {
        leland_score_view_clear_note_colors(s_staff_view);
        leland_score_view_set_note_guide(s_staff_view, -1, -1);
    }
    for (int i = 0; i < s_page_note_span_count; ++i) {
        apply_note_span_style(&s_page_note_spans[i], MUSIC_STATUS_UNKNOWN);
    }
    if (s_midi_parsed) update_notation_display();
    bsp_display_unlock();
}

void music_display_reset_read_only_playback_view(void)
{
    bsp_display_lock(portMAX_DELAY);
    s_expected_first_slot = -1;
    s_expected_last_slot = -1;
    s_current_page = 0;
    if (s_staff_view) {
        leland_score_view_set_note_guide(s_staff_view, -1, -1);
    }
    for (int i = 0; i < s_page_note_span_count; ++i) {
        const int slot = s_page_note_spans[i].slot_index;
        const uint8_t status = slot >= 0 && slot < MUSIC_MAX_RESULT_SLOTS
                                   ? s_note_status[slot]
                                   : MUSIC_STATUS_UNKNOWN;
        apply_note_span_style(&s_page_note_spans[i], status);
    }
    if (s_midi_parsed) update_notation_display();
    bsp_display_unlock();
}

static bool current_page_note_range_locked(int *first_note_idx,
                                           int *last_note_idx)
{
    int first = -1;
    int last = -1;

    if (s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF) {
        if (s_staff_view) {
            last = leland_score_view_get_last_note_on_page(
                s_staff_view, s_current_page);
            if (s_current_page == 0) {
                first = 0;
            } else {
                int previous_last = leland_score_view_get_last_note_on_page(
                    s_staff_view, s_current_page - 1);
                first = previous_last >= 0 ? previous_last + 1 : 0;
            }
        }
    } else {
        first = s_page_first_slot;
        last = s_page_last_slot;
    }

    if (first < 0 || last < first || last >= s_midi_data.note_count) {
        return false;
    }
    if (first_note_idx) *first_note_idx = first;
    if (last_note_idx) *last_note_idx = last;
    return true;
}

static bool current_page_end_time_ms_locked(int64_t *page_end_ms)
{
    if (!page_end_ms || !s_midi_parsed || s_midi_data.note_count <= 0 ||
        s_midi_data.bpm <= 0 || s_midi_data.ticks_per_quarter <= 0) {
        return false;
    }

    uint64_t max_end_tick = 0;
    if (s_notation_type == MUSIC_DISPLAY_NOTATION_NUMBERED &&
        s_numbered_page_end_tick > 0) {
        /* Keep trailing rests/extensions on screen for their full measures. */
        max_end_tick = s_numbered_page_end_tick;
    } else {
        int first_note_idx;
        int last_note_idx;
        if (!current_page_note_range_locked(&first_note_idx,
                                            &last_note_idx)) {
            return false;
        }
        for (int i = first_note_idx; i <= last_note_idx; ++i) {
            const midi_note_t *note = &s_midi_data.notes[i];
            uint64_t note_end_tick =
                (uint64_t)note->start_tick + (uint64_t)note->duration;
            if (note_end_tick > max_end_tick) max_end_tick = note_end_tick;
        }
    }

    uint64_t ticks_per_minute =
        (uint64_t)s_midi_data.bpm *
        (uint64_t)s_midi_data.ticks_per_quarter;
    if (ticks_per_minute == 0) return false;
    *page_end_ms =
        (int64_t)(max_end_tick * 60000ULL / ticks_per_minute);
    return true;
}

static bool score_measure_ticks_locked(uint64_t *measure_ticks)
{
    if (!measure_ticks || s_midi_data.ticks_per_quarter <= 0 ||
        s_midi_data.time_sig_num <= 0 || s_midi_data.time_sig_den < 0 ||
        s_midi_data.time_sig_den > 6)
        return false;
    const uint64_t denominator = UINT64_C(1) << s_midi_data.time_sig_den;
    const uint64_t beats = (uint64_t)s_midi_data.time_sig_num;
    if (beats > UINT64_MAX / 4U) return false;
    const uint64_t quarter_units = beats * 4U;
    if ((uint64_t)s_midi_data.ticks_per_quarter >
        UINT64_MAX / quarter_units)
        return false;
    const uint64_t numerator =
        (uint64_t)s_midi_data.ticks_per_quarter * quarter_units;
    if (denominator == 0 || numerator == 0 ||
        numerator % denominator != 0)
        return false;
    *measure_ticks = numerator / denominator;
    return *measure_ticks > 0;
}

static bool score_tick_time_ms_locked(uint64_t tick, int64_t *time_ms)
{
    if (!time_ms || s_midi_data.bpm <= 0 ||
        s_midi_data.ticks_per_quarter <= 0)
        return false;
    const uint64_t ticks_per_minute =
        (uint64_t)s_midi_data.bpm *
        (uint64_t)s_midi_data.ticks_per_quarter;
    if (ticks_per_minute == 0 || tick > UINT64_MAX / 60000U)
        return false;
    const uint64_t milliseconds = tick * 60000U / ticks_per_minute;
    if (milliseconds > INT64_MAX) return false;
    *time_ms = (int64_t)milliseconds;
    return true;
}

/* Use the next logical page's first measure rather than the last audible
 * note on the current page.  Measure time remains available across long
 * rests and empty systems, and a sustained note crossing a page boundary no
 * longer holds the old page until its eventual Note Off. */
static bool next_page_start_time_ms_locked(int64_t *page_start_ms)
{
    if (!page_start_ms || s_current_page < 0 ||
        s_current_page >= s_total_pages - 1)
        return false;

    uint64_t start_tick = 0;
    if (s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF) {
        if (!s_staff_view ||
            !leland_score_view_get_page_start_tick(
                s_staff_view, s_current_page + 1, &start_tick))
            return false;
    } else {
        uint64_t measure_ticks;
        if (!score_measure_ticks_locked(&measure_ticks)) return false;
        const uint64_t measures_per_page = score_has_lower_staff() ?
            MUSIC_MEASURES_PER_SYSTEM :
            MUSIC_MEASURES_PER_SYSTEM * MUSIC_LINES_PER_PAGE;
        const uint64_t next_measure =
            (uint64_t)(s_current_page + 1) * measures_per_page;
        if (next_measure > UINT64_MAX / measure_ticks) return false;
        start_tick = next_measure * measure_ticks;
    }
    return score_tick_time_ms_locked(start_tick, page_start_ms);
}

void music_display_check_time_page_turn(int64_t score_time_ms)
{
    if (score_time_ms < 0) return;

    bsp_display_lock(portMAX_DELAY);
    int advanced_pages = 0;
    const bool timed_navigation =
        (s_practice_active && !s_practice_paused &&
         !s_creator_active && !s_read_only_active) ||
        (s_read_only_active &&
         s_read_only_playback_state == MUSIC_DISPLAY_PLAYBACK_PLAYING);
    while (timed_navigation &&
           s_current_page < s_total_pages - 1) {
        int64_t page_start_ms;
        if (next_page_start_time_ms_locked(&page_start_ms)) {
            int64_t turn_time_ms = page_start_ms -
                MUSIC_PAGE_TURN_LOOKAHEAD_MS;
            if (turn_time_ms < 0) turn_time_ms = 0;
            if (score_time_ms < turn_time_ms) break;
        } else {
            /* Malformed legacy data may not expose a page measure boundary.
             * Retain a note-end fallback, but do not reintroduce the old
             * fixed 500 ms delay. */
            int64_t page_end_ms;
            if (!current_page_end_time_ms_locked(&page_end_ms) ||
                score_time_ms < page_end_ms)
                break;
        }
        s_current_page++;
        update_notation_display();
        advanced_pages++;
    }
    bsp_display_unlock();

    if (advanced_pages > 0) {
        ESP_LOGI(TAG, "time paging advanced %d page(s), score_time=%lld ms",
                 advanced_pages, (long long)score_time_ms);
    }
}

void music_display_show_score(const char *result_json)
{
    if (!result_json) return;
    if (s_creator_active || s_read_only_active) return;

    bsp_display_lock(portMAX_DELAY);
    if (!guider_ui.end_screen) {
        setup_scr_end_screen(&guider_ui);
    }
    if (!guider_ui.end_screen) {
        bsp_display_unlock();
        return;
    }

    /* 解析 JSON 获取分数 */
    float total_score = 0, pitch_score = 0, rhythm_score = 0, complete_score = 0;
    /* 简单字符串解析（不用 cJSON 避免依赖） */
    const char *p = result_json;
    #define SCAN_KEY(key, var) do { \
        const char *k = strstr(p, "\"" key "\":"); \
        if (k) { k += strlen("\"" key "\":"); var = (float)atof(k); } \
    } while(0)
    SCAN_KEY("total_score", total_score);
    SCAN_KEY("pitch_score", pitch_score);
    SCAN_KEY("rhythm_score", rhythm_score);
    SCAN_KEY("complete_score", complete_score);
    #undef SCAN_KEY

    /* 设置综合评分 */
    if (guider_ui.end_screen_comprehensiveScore) {
        char txt[64];
        snprintf(txt, sizeof(txt), "综合评分:%.1f", total_score);
        lv_label_set_text(guider_ui.end_screen_comprehensiveScore, txt);
    }

    /* 设置各项评分条 */
    set_score_bar_label(guider_ui.end_screen_yinzhunScore, pitch_score);
    set_score_bar_label(guider_ui.end_screen_xuanlvScore, rhythm_score);
    set_score_bar_label(guider_ui.end_screen_completionScore, complete_score);

    lv_screen_load(guider_ui.end_screen);
    bsp_display_unlock();
}

/* ── 任务入口 ── */
static void music_display_task(void *arg)
{
    (void)arg;
    music_msg_t msg;

    ESP_LOGI(TAG, "music_display task started");
    vTaskDelay(pdMS_TO_TICKS(1000));

    bsp_display_lock(portMAX_DELAY);
    if (!guider_ui.music_screen) setup_scr_music_screen(&guider_ui);
    app_font_apply_missing_cjk(guider_ui.music_screen);
    update_notation_display();
    bsp_display_unlock();
    ESP_LOGI(TAG, "music_screen initialized");

    while (1) {
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(16)) == pdTRUE) {
            switch (msg.type) {
            case MUSIC_MSG_MIDI_DATA:
                if (msg.param.midi.len == 0 && msg.param.midi.data) {
                    /* 已解析的 MIDI 数据 */
                    handle_parsed_midi((midi_data_t *)msg.param.midi.data);
                } else {
                    handle_midi_data(msg.param.midi.data, msg.param.midi.len);
                    if (msg.param.midi.data) { free(msg.param.midi.data); msg.param.midi.data = NULL; }
                }
                break;
            case MUSIC_MSG_MIDI_SNAPSHOT:
                handle_midi_snapshot(msg.param.snapshot.parsed,
                                     msg.param.snapshot.notation_type,
                                     msg.param.snapshot.allow_empty);
                break;
            case MUSIC_MSG_SWITCH_SCREEN:
            case MUSIC_MSG_GESTURE_SWIPE_LEFT:
            case MUSIC_MSG_GESTURE_SWIPE_RIGHT:
                handle_gesture(msg.type);
                break;
            default:
                break;
            }
        }

        midi_data_t *latest = NULL;
        int latest_notation = MUSIC_DISPLAY_NOTATION_STAFF;
        bool latest_allow_empty = false;
        if (take_latest_snapshot(&latest, &latest_notation,
                                 &latest_allow_empty)) {
            handle_midi_snapshot(latest, latest_notation,
                                 latest_allow_empty);
        }

        /* 警告容器 2s 自动隐藏 */
    }
}

static char key_ascii_upper(char value)
{
    return value >= 'a' && value <= 'z' ?
           (char)(value - ('a' - 'A')) : value;
}

static bool key_name_equal(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        if (key_ascii_upper(*left) != key_ascii_upper(*right)) return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static bool key_word_starts_with(const char *text, const char *word)
{
    if (!text || !word) return false;
    while (*word) {
        if (!*text || key_ascii_upper(*text) != key_ascii_upper(*word))
            return false;
        ++text;
        ++word;
    }
    return true;
}

static bool parse_legacy_key_signature(const char *text, int *fifths,
                                       bool *minor)
{
    static const char *const major_tonics[15] = {
        "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C",
        "G", "D", "A", "E", "B", "F#", "C#",
    };
    static const char *const minor_tonics[15] = {
        "Ab", "Eb", "Bb", "F", "C", "G", "D", "A",
        "E", "B", "F#", "C#", "G#", "D#", "A#",
    };

    if (!text || !fifths || !minor) return false;
    const char *cursor = strchr(text, '=');
    cursor = cursor ? cursor + 1 : text;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;

    const char letter = key_ascii_upper(*cursor);
    if (letter < 'A' || letter > 'G') return false;
    ++cursor;

    char tonic[3] = {letter, '\0', '\0'};
    if (*cursor == '#') {
        tonic[1] = '#';
        ++cursor;
    } else if (*cursor == 'b' || *cursor == 'B') {
        tonic[1] = 'b';
        ++cursor;
    } else if (strncmp(cursor, "\xE2\x99\xAF", 3) == 0) {
        tonic[1] = '#';
        cursor += 3;
    } else if (strncmp(cursor, "\xE2\x99\xAD", 3) == 0) {
        tonic[1] = 'b';
        cursor += 3;
    }

    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    bool is_minor = key_word_starts_with(cursor, "minor") ||
                    key_word_starts_with(cursor, "min") ||
                    (cursor[0] == 'm' &&
                     (cursor[1] == '\0' || cursor[1] == ' ' ||
                      cursor[1] == '\t'));
    if (strstr(cursor, "\xE5\xB0\x8F\xE8\xB0\x83") != NULL)
        is_minor = true;

    const char *const *tonics = is_minor ? minor_tonics : major_tonics;
    for (int sf = -7; sf <= 7; ++sf) {
        if (!key_name_equal(tonic, tonics[sf + 7])) continue;
        *fifths = sf;
        *minor = is_minor;
        return true;
    }
    return false;
}

static void read_json_key_signature(const cJSON *root, int *fifths,
                                    bool *minor, bool *explicit_key)
{
    *fifths = 0;
    *minor = false;
    *explicit_key = false;

    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    cJSON *explicit_value =
        cJSON_GetObjectItemCaseSensitive(root, "key_explicit");
    const bool has_explicit_value = cJSON_IsBool(explicit_value);
    /* An explicit open-key marker wins over compatibility fields that an
     * older writer may have left behind in the same document. */
    if (has_explicit_value && cJSON_IsFalse(explicit_value)) return;
    if (cJSON_IsString(key) &&
        parse_legacy_key_signature(key->valuestring, fifths, minor)) {
        *explicit_key = true;
    }

    cJSON *numeric_fifths =
        cJSON_GetObjectItemCaseSensitive(root, "key_fifths");
    if (!cJSON_IsNumber(numeric_fifths) ||
        numeric_fifths->valuedouble != numeric_fifths->valueint ||
        numeric_fifths->valueint < -7 || numeric_fifths->valueint > 7) {
        if (has_explicit_value)
            *explicit_key = cJSON_IsTrue(explicit_value);
        return;
    }

    *fifths = numeric_fifths->valueint;
    cJSON *numeric_minor =
        cJSON_GetObjectItemCaseSensitive(root, "key_minor");
    if (cJSON_IsBool(numeric_minor)) *minor = cJSON_IsTrue(numeric_minor);
    *explicit_key = has_explicit_value ? cJSON_IsTrue(explicit_value) : true;
}

/* ── 从标准乐谱 JSON 生成显示 ── */
bool music_display_apply_score_json_with_options(
    const char *json, const music_display_score_options_t *options)
{
    if (!json || !s_queue) return false;

    s_notation_type = options ? options->notation_type :
                      MUSIC_DISPLAY_NOTATION_STAFF;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "apply_score_json: invalid JSON");
        return false;
    }

    cJSON *notes_arr = cJSON_GetObjectItemCaseSensitive(root, "notes");
    if (!cJSON_IsArray(notes_arr) || cJSON_GetArraySize(notes_arr) <= 0) {
        ESP_LOGE(TAG, "apply_score_json: missing or empty notes");
        cJSON_Delete(root);
        return false;
    }
    const int source_note_count = cJSON_GetArraySize(notes_arr);
    if (source_note_count > MAX_NOTES) {
        ESP_LOGE(TAG,
                 "apply_score_json: score has %d notes, preview limit is %d",
                 source_note_count, MAX_NOTES);
        cJSON_Delete(root);
        return false;
    }

    /* Read metadata */
    cJSON *bpm_j   = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *ts_j    = cJSON_GetObjectItemCaseSensitive(root, "time_signature");
    cJSON *title_j = cJSON_GetObjectItemCaseSensitive(root, "title");
    cJSON *tpq_j   = cJSON_GetObjectItemCaseSensitive(root,
                                                      "ticks_per_quarter");
    cJSON *source_j = cJSON_GetObjectItemCaseSensitive(root, "source");
    const bool creator_source = cJSON_IsString(source_j) &&
                                source_j->valuestring != NULL &&
                                strcmp(source_j->valuestring, "creator") == 0;

    int source_bpm = cJSON_IsNumber(bpm_j) ? bpm_j->valueint : 120;
    if (source_bpm < 1) source_bpm = 120;
    int bpm_val = options && options->tempo_bpm > 0 ?
                  options->tempo_bpm : source_bpm;

    int ts_num = 4, ts_den_pow = 2;
    if (cJSON_IsString(ts_j)) {
        const char *ts = ts_j->valuestring;
        const char *slash = strchr(ts, '/');
        if (slash) { ts_num = atoi(ts); ts_den_pow = atoi(slash + 1); }
    }
    if (ts_num < 1) ts_num = 4;
    if      (ts_den_pow == 4) ts_den_pow = 2;
    else if (ts_den_pow == 8) ts_den_pow = 3;
    else                      ts_den_pow = 2;
    if (options && options->time_sig_num > 0 &&
        (options->time_sig_den == 4 || options->time_sig_den == 8)) {
        ts_num = options->time_sig_num;
        ts_den_pow = options->time_sig_den == 8 ? 3 : 2;
    }

    int sf;
    bool minor;
    bool explicit_key;
    read_json_key_signature(root, &sf, &minor, &explicit_key);

    midi_data_t *md = heap_caps_calloc(1, sizeof(midi_data_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!md) md = calloc(1, sizeof(midi_data_t));
    if (!md) { cJSON_Delete(root); return false; }

    md->bpm               = bpm_val;
    md->ticks_per_quarter = cJSON_IsNumber(tpq_j) && tpq_j->valueint > 0 ?
                            tpq_j->valueint : 480;
    md->time_sig_num      = ts_num;
    md->time_sig_den      = ts_den_pow;
    md->tonality_sf       = sf;
    md->tonality_minor    = minor;
    md->tonality_forced   = explicit_key;
    md->numbered_legacy_inference_allowed = !creator_source;
    md->title[0] = '\0';
    if (cJSON_IsString(title_j)) {
        const char *src = title_j->valuestring;
        size_t slen = strlen(src);
        size_t maxlen = sizeof(md->title) - 1;
        if (slen > maxlen) slen = maxlen;
        memcpy(md->title, src, slen);
        md->title[slen] = '\0';
    }

    /* JSON seconds describe the source score. Practice tempo changes playback,
     * not the written positions of notes inside measures. */
    float ticks_per_sec = (source_bpm / 60.0f) *
                          (float)md->ticks_per_quarter;
    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, notes_arr) {
        if (count >= MAX_NOTES) break;
        cJSON *midi_j  = cJSON_GetObjectItemCaseSensitive(item, "midi");
        cJSON *start_j = cJSON_GetObjectItemCaseSensitive(item, "start");
        cJSON *dur_j   = cJSON_GetObjectItemCaseSensitive(item, "duration");
        cJSON *staff_j = cJSON_GetObjectItemCaseSensitive(item, "staff");
        cJSON *voice_j = cJSON_GetObjectItemCaseSensitive(item, "voice");
        cJSON *start_tick_j = cJSON_GetObjectItemCaseSensitive(item,
                                                               "start_tick");
        cJSON *duration_ticks_j = cJSON_GetObjectItemCaseSensitive(
            item, "duration_ticks");
        cJSON *dots_j = cJSON_GetObjectItemCaseSensitive(item, "dots");
        cJSON *tie_j = cJSON_GetObjectItemCaseSensitive(item, "tie_flags");
        cJSON *slur_start_j = cJSON_GetObjectItemCaseSensitive(item,
                                                               "slur_start");
        cJSON *slur_stop_j = cJSON_GetObjectItemCaseSensitive(item,
                                                              "slur_stop");
        cJSON *gliss_start_j = cJSON_GetObjectItemCaseSensitive(item,
                                                                "gliss_start");
        cJSON *gliss_stop_j = cJSON_GetObjectItemCaseSensitive(item,
                                                               "gliss_stop");
        if (!cJSON_IsNumber(midi_j)) continue;
        int m = midi_j->valueint;
        if (m < 0 || m > 127) continue;
        float start_sec = cJSON_IsNumber(start_j) ? (float)start_j->valuedouble : 0.0f;
        float dur_sec   = cJSON_IsNumber(dur_j)   ? (float)dur_j->valuedouble   : 0.5f;
        if (dur_sec <= 0.01f) dur_sec = 0.1f;
        md->notes[count].note       = (uint8_t)m;
        md->notes[count].velocity   = 80;
        md->notes[count].staff      = cJSON_IsNumber(staff_j) &&
                                      staff_j->valueint == 2 ? 2 : 1;
        md->notes[count].voice      = cJSON_IsNumber(voice_j) &&
                                      voice_j->valueint > 0 ?
                                      (uint8_t)voice_j->valueint : 1;
        md->notes[count].start_tick = cJSON_IsNumber(start_tick_j) &&
                                      start_tick_j->valuedouble >= 0 ?
            (uint32_t)start_tick_j->valuedouble :
            (uint32_t)(start_sec * ticks_per_sec + 0.5f);
        md->notes[count].duration = cJSON_IsNumber(duration_ticks_j) &&
                                    duration_ticks_j->valuedouble > 0 ?
            (uint32_t)duration_ticks_j->valuedouble :
            (uint32_t)(dur_sec * ticks_per_sec + 0.5f);
        md->notes[count].dots = cJSON_IsNumber(dots_j) ?
                                (uint8_t)dots_j->valueint : 0;
        md->notes[count].tie_flags = cJSON_IsNumber(tie_j) ?
                                     (uint8_t)tie_j->valueint : 0;
        md->notes[count].slur_start = cJSON_IsNumber(slur_start_j) ?
                                      (uint8_t)slur_start_j->valueint : 0;
        md->notes[count].slur_stop = cJSON_IsNumber(slur_stop_j) ?
                                     (uint8_t)slur_stop_j->valueint : 0;
        md->notes[count].gliss_start = cJSON_IsNumber(gliss_start_j) ?
                                       (uint8_t)gliss_start_j->valueint : 0;
        md->notes[count].gliss_stop = cJSON_IsNumber(gliss_stop_j) ?
                                      (uint8_t)gliss_stop_j->valueint : 0;
        count++;
    }
    md->note_count = count;
    cJSON *raw_events = cJSON_GetObjectItemCaseSensitive(
        root, "performance_events");
    cJSON *raw = NULL;
    cJSON_ArrayForEach(raw, raw_events) {
        if (md->raw_event_count >= MAX_MIDI_RAW_EVENTS) break;
        cJSON *type_j = cJSON_GetObjectItemCaseSensitive(raw, "type");
        cJSON *tick_j = cJSON_GetObjectItemCaseSensitive(raw, "tick");
        cJSON *channel_j = cJSON_GetObjectItemCaseSensitive(raw, "channel");
        cJSON *data1_j = cJSON_GetObjectItemCaseSensitive(raw, "data1");
        cJSON *value_j = cJSON_GetObjectItemCaseSensitive(raw, "value");
        midi_raw_event_t *target =
            &md->raw_events[md->raw_event_count++];
        target->type = cJSON_IsString(type_j) &&
                       strcmp(type_j->valuestring, "pitch_bend") == 0 ?
                       MIDI_RAW_PITCH_BEND : MIDI_RAW_CONTROL_CHANGE;
        target->tick = cJSON_IsNumber(tick_j) ?
                       (uint32_t)tick_j->valuedouble : 0;
        target->channel = cJSON_IsNumber(channel_j) ?
                          (uint8_t)channel_j->valueint : 0;
        target->data1 = cJSON_IsNumber(data1_j) ?
                        (uint8_t)data1_j->valueint : 0;
        target->value = cJSON_IsNumber(value_j) ?
                        (int16_t)value_j->valueint : 0;
    }
    cJSON_Delete(root);
    if (count == 0) { free(md); return false; }

    qsort(md->notes, (size_t)count, sizeof(midi_note_t), compare_midi_note_start);
    unsigned tie_count = 0;
    unsigned slur_count = 0;
    unsigned gliss_count = 0;
    unsigned dot_count = 0;
    for (int index = 0; index < count; ++index) {
        const midi_note_t *note = &md->notes[index];
        if ((note->tie_flags & MIDI_NOTE_TIE_START) != 0) ++tie_count;
        if (note->slur_start != 0) ++slur_count;
        if (note->gliss_start != 0) ++gliss_count;
        if (note->dots != 0) ++dot_count;
    }
    ESP_LOGI(TAG, "apply_score_json: %d notes, BPM=%d, TS=%d/%d",
             count, bpm_val, ts_num, 1 << ts_den_pow);
    ESP_LOGI(TAG,
             "numbered metadata: ties=%u slurs=%u gliss=%u dotted=%u",
             tie_count, slur_count, gliss_count, dot_count);
    if (creator_source) {
        ESP_LOGI(TAG, "numbered legacy inference: skipped source=creator");
    }

    music_msg_t msg = {
        .type = MUSIC_MSG_MIDI_DATA,
        .param.midi = { .data = (uint8_t *)md, .len = 0 },
    };
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(500)) != pdPASS) {
        free(md);
        ESP_LOGW(TAG, "display queue full");
        return false;
    }
    return true;
}

bool music_display_apply_score_json(const char *json)
{
    return music_display_apply_score_json_with_options(json, NULL);
}

bool music_display_submit_midi_snapshot(
    midi_data_t *snapshot, const music_display_score_options_t *options,
    bool allow_empty)
{
    if (!snapshot) return false;
    if (!s_queue) {
        free(snapshot);
        return false;
    }
    if (!s_snapshot_mutex) {
        free(snapshot);
        return false;
    }
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    midi_data_t *superseded = creator_snapshot_mailbox_replace(
        &s_snapshot_mailbox, snapshot);
    s_latest_snapshot_notation = options ? options->notation_type :
                                 MUSIC_DISPLAY_NOTATION_STAFF;
    s_latest_snapshot_allow_empty = allow_empty;
    xSemaphoreGive(s_snapshot_mutex);
    /* A live burst retains only the newest immutable frame. The consumer
     * polls this slot every 16 ms, so queue pressure cannot discard the final
     * Note On/Off state or accumulate stale full-score rebuilds. */
    free(superseded);
    return true;
}
void music_display_set_note_selection_callback(
    music_display_note_selection_cb_t callback, void *user_data)
{
    s_note_selection_callback = callback;
    s_note_selection_user_data = user_data;
}

void music_display_set_creator_active(bool active)
{
    s_creator_active = active;
    if (active) {
        s_read_only_active = false;
        s_practice_active = false;
        s_practice_paused = false;
    }
}

void music_display_set_read_only(bool active)
{
    s_read_only_active = active;
    if (active) {
        s_creator_active = false;
        s_practice_active = false;
        s_practice_paused = false;
    } else {
        s_read_only_playback_state = MUSIC_DISPLAY_PLAYBACK_STOPPED;
    }
}

void music_display_set_playback_button_callback(
    music_display_playback_button_cb_t callback, void *user_data)
{
    s_playback_button_callback = callback;
    s_playback_button_user_data = user_data;
}

void music_display_set_read_only_playback_state(
    music_display_playback_state_t state, const char *message)
{
    bsp_display_lock(portMAX_DELAY);
    s_read_only_playback_state = state;
    update_read_only_playback_label();
    if (s_read_only_active && guider_ui.music_screen_status_label &&
        lv_obj_is_valid(guider_ui.music_screen_status_label)) {
        if (message && message[0]) {
            lv_label_set_text(guider_ui.music_screen_status_label, message);
        } else if (state == MUSIC_DISPLAY_PLAYBACK_PLAYING) {
            lv_label_set_text(guider_ui.music_screen_status_label,
                              "正在播放");
        } else if (state == MUSIC_DISPLAY_PLAYBACK_PAUSED) {
            lv_label_set_text(guider_ui.music_screen_status_label,
                              "播放已暂停");
        } else if (state == MUSIC_DISPLAY_PLAYBACK_PREPARING) {
            lv_label_set_text(guider_ui.music_screen_status_label,
                              "正在准备播放");
        } else {
            lv_label_set_text(guider_ui.music_screen_status_label,
                              "只读浏览");
        }
    }
    bsp_display_unlock();
}

void music_display_set_practice_navigation_state(bool active, bool paused)
{
    s_practice_active = active;
    s_practice_paused = active && paused;
}

bool music_display_request_page_turn(
    music_display_page_direction_t direction,
    music_display_page_source_t source)
{
    if (s_creator_active || !s_queue || !s_midi_parsed) return false;
    if (source == MUSIC_DISPLAY_PAGE_SOURCE_TOUCH &&
        ((s_practice_active && !s_practice_paused) ||
         (s_read_only_active &&
          s_read_only_playback_state == MUSIC_DISPLAY_PLAYBACK_PLAYING))) {
        return false;
    }
    if (source != MUSIC_DISPLAY_PAGE_SOURCE_TOUCH &&
        source != MUSIC_DISPLAY_PAGE_SOURCE_HAND_GESTURE &&
        source != MUSIC_DISPLAY_PAGE_SOURCE_AUTO &&
        source != MUSIC_DISPLAY_PAGE_SOURCE_VOICE) {
        return false;
    }

    music_msg_t message = {0};
    if (direction == MUSIC_DISPLAY_PAGE_NEXT) {
        if (s_current_page >= s_total_pages - 1) return false;
        message.type = MUSIC_MSG_GESTURE_SWIPE_LEFT;
    } else if (direction == MUSIC_DISPLAY_PAGE_PREVIOUS) {
        if (s_current_page <= 0) return false;
        message.type = MUSIC_MSG_GESTURE_SWIPE_RIGHT;
    } else {
        return false;
    }
    return xQueueSend(s_queue, &message, 0) == pdPASS;
}

void music_display_start(void)
{
    reset_note_status();
    s_queue = xQueueCreate(8, sizeof(music_msg_t));
    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (!s_queue || !s_snapshot_mutex) {
        ESP_LOGE(TAG, "display queue/state create failed");
        if (s_queue) vQueueDelete(s_queue);
        if (s_snapshot_mutex) vSemaphoreDelete(s_snapshot_mutex);
        s_queue = NULL;
        s_snapshot_mutex = NULL;
        return;
    }

    BaseType_t ok = xTaskCreate(music_display_task, "music_display", 16384, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_snapshot_mutex);
        s_queue = NULL;
        s_snapshot_mutex = NULL;
        return;
    }
    ESP_LOGI(TAG, "music_display task started");
}
