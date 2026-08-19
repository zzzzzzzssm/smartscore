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
static music_display_note_selection_cb_t s_note_selection_callback;
static void *s_note_selection_user_data;

#define MUSIC_MAX_RESULT_SLOTS 1024
#define MUSIC_LINES_PER_PAGE 2
#define MUSIC_MEASURES_PER_SYSTEM 2
#define MUSIC_MAX_NOTE_SPANS_PER_PAGE 384
#define MUSIC_STATUS_UNKNOWN 0xff
#define MUSIC_PAGE_TURN_GRACE_MS 500
#define MUSIC_DISPLAY_GREEN_PITCH_TOLERANCE 2
#define MUSIC_DISPLAY_RED_PITCH_THRESHOLD 6

static uint8_t s_note_status[MUSIC_MAX_RESULT_SLOTS];

/* ── 逐音符 span 追踪 ── */
typedef struct {
    lv_obj_t *group;
    lv_span_t *span;
    int slot_index;
} music_note_span_ref_t;

static music_note_span_ref_t s_page_note_spans[MUSIC_MAX_NOTE_SPANS_PER_PAGE];
static int s_page_note_span_count;
static int s_page_first_slot = -1;
static int s_page_last_slot = -1;
static int s_expected_first_slot = -1;
static int s_expected_last_slot = -1;

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
        lv_obj_set_size(s_line_groups[i], 929, 190);
        lv_spangroup_set_align(s_line_groups[i], LV_TEXT_ALIGN_LEFT);
        lv_spangroup_set_overflow(s_line_groups[i], LV_SPAN_OVERFLOW_CLIP);
        lv_spangroup_set_mode(s_line_groups[i], LV_SPAN_MODE_BREAK);
        lv_obj_set_style_bg_opa(s_line_groups[i], 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_line_groups[i], 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_line_groups[i], 0, LV_PART_MAIN|LV_STATE_DEFAULT);
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
    s_read_only_active = false;
    if (s_read_only_back_button &&
        lv_obj_is_valid(s_read_only_back_button))
        lv_obj_add_flag(s_read_only_back_button, LV_OBJ_FLAG_HIDDEN);
    if (s_read_only_notation_button &&
        lv_obj_is_valid(s_read_only_notation_button))
        lv_obj_add_flag(s_read_only_notation_button, LV_OBJ_FLAG_HIDDEN);
    if (!score_ui_flow_return_to_preparation(&guider_ui)) {
        score_ui_flow_open_choose(&guider_ui);
    }
}

static void read_only_notation_event_cb(lv_event_t *event)
{
    (void)event;
    if (!s_read_only_active || s_creator_active || !s_midi_parsed) return;

    s_notation_type =
        s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF
            ? MUSIC_DISPLAY_NOTATION_NUMBERED
            : MUSIC_DISPLAY_NOTATION_STAFF;
    s_current_page = 0;
    apply_midi_data_to_ui();
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

    if (s_read_only_active && !s_creator_active) {
        lv_obj_clear_flag(s_read_only_back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_read_only_notation_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(
            s_read_only_notation_label,
            s_notation_type == MUSIC_DISPLAY_NOTATION_STAFF
                ? "\xE5\x88\x87\xE6\x8D\xA2\xE7\xAE\x80\xE8\xB0\xB1"
                : "\xE5\x88\x87\xE6\x8D\xA2\xE4\xBA\x94\xE7\xBA\xBF\xE8\xB0\xB1");
    } else {
        lv_obj_add_flag(s_read_only_back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_read_only_notation_button, LV_OBJ_FLAG_HIDDEN);
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
                                uint32_t start_tick, uint32_t end_tick,
                                uint8_t staff);
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

static bool token_is_note_glyph(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static lv_obj_t *line_group_for_index(int line_idx)
{
    ensure_line_groups();
    if (line_idx < 0 || line_idx >= MUSIC_LINES_PER_PAGE) return NULL;
    return s_line_groups[line_idx];
}

static int next_note_index_for_range(int cursor, uint32_t start_tick,
                                     uint32_t end_tick, uint8_t staff)
{
    for (int i = cursor; i < s_midi_data.note_count; ++i) {
        const midi_note_t *note = &s_midi_data.notes[i];
        uint8_t note_staff = note->staff == 2 ? 2 : 1;
        if (note->start_tick >= end_tick) break;
        if (note->start_tick >= start_tick &&
            (!staff || note_staff == staff)) return i;
    }
    return -1;
}

static void reset_page_span_index(void)
{
    s_page_note_span_count = 0;
    s_page_first_slot = -1;
    s_page_last_slot = -1;
}

static void remember_page_note_span(lv_obj_t *group, lv_span_t *span, int slot_index)
{
    if (!group || !span || slot_index < 0) return;
    if (s_page_note_span_count >= MUSIC_MAX_NOTE_SPANS_PER_PAGE) return;
    s_page_note_spans[s_page_note_span_count++] = (music_note_span_ref_t){
        .group = group, .span = span, .slot_index = slot_index };
    if (s_page_first_slot < 0 || slot_index < s_page_first_slot) s_page_first_slot = slot_index;
    if (slot_index > s_page_last_slot) s_page_last_slot = slot_index;
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
                                uint32_t start_tick, uint32_t end_tick,
                                uint8_t staff)
{
    clear_spangroup(group);
    if (!group) return;
    if (!line || line[0] == '\0') {
        add_text_span(group, " ", lv_color_hex(0x000000));
        lv_spangroup_refr_mode(group);
        return;
    }

    int note_cursor = 0;
    const char *p = line;
    char token[64];
    bool first = true;

    while (*p) {
        /* 跳过空格 */
        while (*p == ' ') { p++; }
        if (!*p) break;

        /* 提取一个 token（直到空格或结尾） */
        int ti = 0;
        while (*p && *p != ' ' && ti < (int)sizeof(token) - 1) {
            token[ti++] = *p++;
        }
        token[ti] = '\0';
        if (ti == 0) continue;

        /* 在 token 之间加空格 span（保留原始排版） */
        if (!first) {
            add_text_span(group, " ", lv_color_hex(0x000000));
        }
        first = false;

        /* 判断这是否是一个音符 glyph 开头 */
        if (ti > 0 && token_is_note_glyph(token[0])) {
            int note_idx = next_note_index_for_range(note_cursor, start_tick,
                                                     end_tick, staff);
            if (note_idx >= 0) note_cursor = note_idx + 1;
            uint8_t status = (note_idx >= 0 &&
                              note_idx < MUSIC_MAX_RESULT_SLOTS) ?
                             s_note_status[note_idx] :
                             MUSIC_STATUS_UNKNOWN;
            lv_span_t *span = add_text_span(group, token, color_for_status(status));
            if (span && note_idx >= 0) {
                if (status == MUSIC_STATUS_UNKNOWN &&
                    note_idx >= s_expected_first_slot &&
                    note_idx <= s_expected_last_slot) {
                    lv_style_t *style = lv_span_get_style(span);
                    lv_style_set_text_color(style, lv_color_hex(0x0284C7));
                    lv_style_set_text_decor(style, LV_TEXT_DECOR_UNDERLINE);
                }
                remember_page_note_span(group, span, note_idx);
            }
        } else {
            /* 小节线 | 或其他分隔符 */
            add_text_span(group, token, lv_color_hex(0x000000));
        }
    }

    lv_spangroup_refr_mode(group);
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
        int measure_ticks = s_midi_data.ticks_per_quarter *
                            s_midi_data.time_sig_num;
        for (int line = 0; line < MUSIC_LINES_PER_PAGE; ++line) {
            lv_obj_t *group = line_group_for_index(line);
            if (!group) continue;
            int start_measure = grand_staff ?
                s_current_page * MUSIC_MEASURES_PER_SYSTEM :
                (s_current_page * MUSIC_LINES_PER_PAGE + line) *
                    MUSIC_MEASURES_PER_SYSTEM;
            uint8_t staff = grand_staff ? (uint8_t)(line + 1) : 1;
            char notation[512];
            bool has_line = midi_generate_measure_range(
                &s_midi_data, notation, sizeof(notation), start_measure,
                MUSIC_MEASURES_PER_SYSTEM, staff);
            uint32_t start_tick = (uint32_t)start_measure * measure_ticks;
            uint32_t end_tick = start_tick +
                (uint32_t)MUSIC_MEASURES_PER_SYSTEM * measure_ticks;
            render_colored_line(group, has_line ? notation : " ",
                                start_tick, end_tick, staff);
            lv_obj_clear_flag(group, LV_OBJ_FLAG_HIDDEN);
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
    int measure_ticks = s_midi_data.ticks_per_quarter *
                        s_midi_data.time_sig_num;
    if (measure_ticks <= 0 || s_midi_data.note_count <= 0) return 1;
    uint32_t end_tick = 0;
    for (int i = 0; i < s_midi_data.note_count; ++i) {
        uint32_t end = s_midi_data.notes[i].start_tick +
                       s_midi_data.notes[i].duration;
        if (end > end_tick) end_tick = end;
    }
    int measures = (int)((end_tick + (uint32_t)measure_ticks - 1) /
                         (uint32_t)measure_ticks);
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
        lv_obj_set_size(s_line_groups[i], grand_staff ? 875 : 929, 190);
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
        if (s_staff_view) leland_score_view_set_hidden(s_staff_view, true);
        lv_obj_scroll_to_y(guider_ui.music_screen_cont_1, 0, LV_ANIM_OFF);
        s_total_pages = numbered_page_count();
        ESP_LOGI(TAG, "rendering numbered notation (%d pages)",
                 s_total_pages);
    } else {
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

        leland_score_view_set_hidden(s_staff_view, false);
        char render_error[128] = {0};
        if (!leland_score_view_set_midi(s_staff_view, &s_midi_data,
                                        render_error,
                                        sizeof(render_error))) {
            ESP_LOGE(TAG, "Leland score layout failed: %s", render_error);
            return;
        }
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
        midi_tonality_to_string(s_midi_data.tonality_sf, s_midi_data.tonality_minor,
                                tonality_str, sizeof(tonality_str));
        int len = strlen(tonality_str);
        int denom = 1 << s_midi_data.time_sig_den;
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
        int denominator = 1 << s_midi_data.time_sig_den;
        snprintf(info, sizeof(info), "C major  %d/%d  BPM=%d",
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

    int first_note_idx;
    int last_note_idx;
    if (!current_page_note_range_locked(&first_note_idx, &last_note_idx)) {
        return false;
    }

    uint64_t max_end_tick = 0;
    for (int i = first_note_idx; i <= last_note_idx; ++i) {
        const midi_note_t *note = &s_midi_data.notes[i];
        uint64_t note_end_tick =
            (uint64_t)note->start_tick + (uint64_t)note->duration;
        if (note_end_tick > max_end_tick) max_end_tick = note_end_tick;
    }

    uint64_t ticks_per_minute =
        (uint64_t)s_midi_data.bpm *
        (uint64_t)s_midi_data.ticks_per_quarter;
    if (ticks_per_minute == 0) return false;
    *page_end_ms =
        (int64_t)(max_end_tick * 60000ULL / ticks_per_minute);
    return true;
}

void music_display_check_time_page_turn(int64_t score_time_ms)
{
    if (score_time_ms < 0) return;

    bsp_display_lock(portMAX_DELAY);
    int advanced_pages = 0;
    while (s_practice_active && !s_practice_paused &&
           !s_creator_active && !s_read_only_active &&
           s_current_page < s_total_pages - 1) {
        int64_t page_end_ms;
        if (!current_page_end_time_ms_locked(&page_end_ms) ||
            score_time_ms <= page_end_ms + MUSIC_PAGE_TURN_GRACE_MS) {
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

    /* Read metadata */
    cJSON *bpm_j   = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *ts_j    = cJSON_GetObjectItemCaseSensitive(root, "time_signature");
    cJSON *key_j   = cJSON_GetObjectItemCaseSensitive(root, "key");
    cJSON *title_j = cJSON_GetObjectItemCaseSensitive(root, "title");
    cJSON *tpq_j   = cJSON_GetObjectItemCaseSensitive(root,
                                                      "ticks_per_quarter");

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

    int sf = 0;
    bool minor = false;
    if (cJSON_IsString(key_j)) {
        const char *ks = key_j->valuestring;
        const char *eq = strchr(ks, '=');
        const char *kn = eq ? eq + 1 : ks;
        /* "major" also starts with 'm'; only an explicit minor spelling or
         * compact notation such as "Am" denotes a minor key. */
        minor = strstr(kn, "minor") != NULL || strstr(kn, "Minor") != NULL;
        size_t key_len = strlen(kn);
        if (!minor && key_len >= 2 && kn[key_len - 1] == 'm') minor = true;
        while (*kn == ' ' || *kn == '#' || *kn == 'b' || *kn == 'B') kn++;
        if      (kn[0] == 'C') sf = 0;
        else if (kn[0] == 'G') sf = kn[1] == '#' ? 0 : 1;
        else if (kn[0] == 'D') sf = 2;
        else if (kn[0] == 'A') sf = 3;
        else if (kn[0] == 'E') sf = 4;
        else if (kn[0] == 'B') sf = kn[1] == '#' ? 0 : 5;
        else if (kn[0] == 'F') sf = kn[1] == '#' ? 6 : -1;
    }

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
    ESP_LOGI(TAG, "apply_score_json: %d notes, BPM=%d, TS=%d/%d",
             count, bpm_val, ts_num, 1 << ts_den_pow);

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
    }
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
        s_practice_active && !s_practice_paused) {
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
