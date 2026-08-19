#include "creator_mode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_font.h"
#include "bsp/esp-bsp.h"
#include "creator_recorder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "music_display.h"
#include "score_storage.h"
#include "freertos/FreeRTOS.h"

#define COLOR_BG         0xFFF7EA
#define COLOR_CARD       0xF3E2C7
#define COLOR_INK        0x351B10
#define TXT_CREATOR_PREP  "\xE5\x88\x9B\xE4\xBD\x9C\xE8\x80\x85\xE6\xA8\xA1\xE5\xBC\x8F\xE8\xAE\xBE\xE7\xBD\xAE"
#define TXT_BACK          "\xE8\xBF\x94\xE5\x9B\x9E"
#define TXT_TIME_SIG      "\xE6\x8B\x8D\xE5\x8F\xB7"
#define TXT_VOICE_MODE    "\xE5\xA3\xB0\xE9\x83\xA8\xE6\xA8\xA1\xE5\xBC\x8F"
#define TXT_SINGLE_VOICE  "\xE5\x8D\x95\xE5\xA3\xB0\xE9\x83\xA8"
#define TXT_DOUBLE_VOICE  "\xE5\x8F\x8C\xE5\xA3\xB0\xE9\x83\xA8"
#define TXT_START         "\xE5\xBC\x80\xE5\xA7\x8B\xE5\x88\x9B\xE4\xBD\x9C"
#define TXT_PAUSE         "\xE6\x9A\x82\xE5\x81\x9C"
#define TXT_RESUME        "\xE7\xBB\xA7\xE7\xBB\xAD"
#define TXT_EXIT          "\xE9\x80\x80\xE5\x87\xBA"
#define TXT_RECORDING     "\xE5\xBD\x95\xE5\x88\xB6\xE4\xB8\xAD"
#define TXT_PAUSED        "\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C"
#define TXT_FINISH        "\xE7\xBB\x93\xE6\x9D\x9F"
#define TXT_NO_NOTES      "\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8F\xAF\xE4\xBF\x9D\xE5\xAD\x98\xE7\x9A\x84\xE9\x9F\xB3\xE7\xAC\xA6"
#define TXT_SAVE_FAILED   "\xE4\xBF\x9D\xE5\xAD\x98\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE8\xAF\xB7\xE6\xA3\x80\xE6\x9F\xA5SD\xE5\x8D\xA1"
#define TXT_MEASURE_FMT   "\xE7\xAC\xAC%d\xE5\xB0\x8F\xE8\x8A\x82"
#define TXT_OVERWRITE     "\xE9\x87\x8D\xE5\xBC\xB9\xE8\xA6\x86\xE7\x9B\x96"

#define COLOR_ACCENT     0x6B3515
#define COLOR_SELECTED   0x3F2204
#define COLOR_UNSELECTED 0xEEDFC9
#define TXT_TAP_OVERWRITE "\xE7\x82\xB9\xE5\x87\xBB\xE9\x9F\xB3\xE7\xAC\xA6\xEF\xBC\x8C\xE4\xBB\x8E\xE8\xAF\xA5\xE5\xB0\x8F\xE8\x8A\x82\xE9\x87\x8D\xE5\xBC\xB9"

#define TXT_WAIT_FIRST "\xE7\xAD\x89\xE5\xBE\x85\xE9\xA6\x96\xE9\x9F\xB3"
#define TXT_SELECT_RANGE "\xE8\xAF\xB7\xE4\xBE\x9D\xE6\xAC\xA1\xE7\x82\xB9\xE5\x87\xBB\xE9\x87\x8D\xE5\xBC\xB9\xE5\x8C\xBA\xE9\x97\xB4\xE7\x9A\x84\xE8\xB5\xB7\xE7\x82\xB9\xE5\x92\x8C\xE7\xBB\x88\xE7\x82\xB9\xE9\x9F\xB3\xE7\xAC\xA6"
#define TXT_RANGE_START_FMT "\xE5\xB7\xB2\xE9\x80\x89\xE7\xAC\xAC%d\xE5\xB0\x8F\xE8\x8A\x82\xEF\xBC\x8C\xE8\xAF\xB7\xE7\x82\xB9\xE5\x87\xBB\xE7\xBB\x88\xE7\x82\xB9\xE9\x9F\xB3\xE7\xAC\xA6"
#define CREATOR_LIVE_REFRESH_MS UINT32_C(40)
typedef struct {
    lv_ui *ui;
    lv_obj_t *preparation;
    lv_obj_t *return_screen;
    lv_obj_t *bpm_buttons[4];
    lv_obj_t *time_buttons[4];
    lv_obj_t *staff_buttons[2];
    lv_obj_t *pause_button;
    lv_obj_t *pause_label;
    lv_obj_t *exit_button;
    lv_obj_t *finish_button;
    lv_obj_t *overwrite_panel;
    lv_obj_t *overwrite_hint_label;
    int range_start_measure;
    lv_timer_t *auto_pause_timer;
    uint32_t last_live_publish_ms;
    bool live_refresh_pending;
    bool live_publish_in_progress;
    creator_recorder_config_t config;
    volatile bool active;
    volatile bool initialized;
    volatile bool saving;
    esp_err_t last_error;
    char saved_title[64];
    char saved_filename[64];
} creator_mode_context_t;

static const char *TAG = "creator_mode";
static creator_mode_context_t s_creator;
static portMUX_TYPE s_live_refresh_lock = portMUX_INITIALIZER_UNLOCKED;

static bool valid_config(const creator_recorder_config_t *config)
{
    if (!config) return false;
    bool valid_bpm = config->bpm == 60 || config->bpm == 80 ||
                     config->bpm == 100 || config->bpm == 120;
    bool valid_time =
        (config->time_sig_num == 2 && config->time_sig_den == 4) ||
        (config->time_sig_num == 3 && config->time_sig_den == 4) ||
        (config->time_sig_num == 4 && config->time_sig_den == 4) ||
        (config->time_sig_num == 6 && config->time_sig_den == 8);
    bool valid_staff = config->staff_mode == CREATOR_STAFF_SINGLE ||
                       config->staff_mode == CREATOR_STAFF_GRAND;
    return valid_bpm && valid_time && valid_staff;
}

static creator_recorder_config_t normalized_remote_config(
    const creator_recorder_config_t *config)
{
    creator_recorder_config_t result = *config;
    result.duration_tolerance_percent = 30;
    result.grand_staff_split_note = 60;
    result.chord_window_ms = 45;
    result.max_notes_per_onset = 10;
    return result;
}

static const lv_font_t *ui_font(void)
{
    const lv_font_t *font = app_font_chinese_22();
    return font ? font : LV_FONT_DEFAULT;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            int width, int height)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y,
                             int width, int height, lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xC8A778), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_UNSELECTED), 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INK), 0);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return button;
}

static void set_selected(lv_obj_t *button, bool selected)
{
    if (!button) return;
    lv_obj_set_style_bg_color(button,
        lv_color_hex(selected ? COLOR_SELECTED : COLOR_UNSELECTED), 0);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label)
        lv_obj_set_style_text_color(label,
            lv_color_hex(selected ? 0xFFFFFF : COLOR_INK), 0);
}

static void refresh_preparation(void)
{
    const int bpms[] = {60, 80, 100, 120};
    const int time_num[] = {2, 3, 4, 6};
    const int time_den[] = {4, 4, 4, 8};
    for (int i = 0; i < 4; ++i) {
        set_selected(s_creator.bpm_buttons[i],
                     s_creator.config.bpm == bpms[i]);
        set_selected(s_creator.time_buttons[i],
                     s_creator.config.time_sig_num == time_num[i] &&
                     s_creator.config.time_sig_den == time_den[i]);
    }
    set_selected(s_creator.staff_buttons[0],
                 s_creator.config.staff_mode == CREATOR_STAFF_SINGLE);
    set_selected(s_creator.staff_buttons[1],
                 s_creator.config.staff_mode == CREATOR_STAFF_GRAND);
}

static bool publish_snapshot(bool allow_empty, bool include_active)
{
    midi_data_t *snapshot = heap_caps_calloc(
        1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return false;
    bool ready = include_active ?
        creator_recorder_snapshot_live(snapshot,
                                       (uint64_t)esp_timer_get_time()) :
        creator_recorder_snapshot(snapshot);
    if (!ready) {
        free(snapshot);
        return false;
    }
    music_display_score_options_t options = {
        .tempo_bpm = s_creator.config.bpm,
        .time_sig_num = s_creator.config.time_sig_num,
        .time_sig_den = s_creator.config.time_sig_den,
        .notation_type = MUSIC_DISPLAY_NOTATION_STAFF,
    };
    return music_display_submit_midi_snapshot(snapshot, &options,
                                              allow_empty);
}

static void flush_live_refresh(uint32_t now_ms)
{
    bool claimed = false;
    taskENTER_CRITICAL(&s_live_refresh_lock);
    if (s_creator.live_refresh_pending &&
        !s_creator.live_publish_in_progress &&
        (!s_creator.last_live_publish_ms ||
         now_ms - s_creator.last_live_publish_ms >= CREATOR_LIVE_REFRESH_MS)) {
        s_creator.live_refresh_pending = false;
        s_creator.live_publish_in_progress = true;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_live_refresh_lock);
    if (!claimed) return;

    bool submitted = publish_snapshot(false, true);
    taskENTER_CRITICAL(&s_live_refresh_lock);
    s_creator.live_publish_in_progress = false;
    if (submitted)
        s_creator.last_live_publish_ms = now_ms;
    else
        s_creator.live_refresh_pending = true;
    taskEXIT_CRITICAL(&s_live_refresh_lock);
}

static void request_live_refresh(uint64_t timestamp_us)
{
    taskENTER_CRITICAL(&s_live_refresh_lock);
    s_creator.live_refresh_pending = true;
    taskEXIT_CRITICAL(&s_live_refresh_lock);
    flush_live_refresh((uint32_t)(timestamp_us / 1000U));
}

static void reset_range_selection_ui(void)
{
    s_creator.range_start_measure = 0;
    if (s_creator.overwrite_hint_label &&
        lv_obj_is_valid(s_creator.overwrite_hint_label))
        lv_label_set_text(s_creator.overwrite_hint_label, TXT_SELECT_RANGE);
}

static void set_recording_ui(bool recording)
{
    if (s_creator.ui && s_creator.ui->music_screen_status_label) {
        lv_label_set_text(s_creator.ui->music_screen_status_label,
                          recording ? TXT_RECORDING : TXT_PAUSED);
        lv_obj_set_style_text_font(s_creator.ui->music_screen_status_label,
                                   ui_font(), 0);
    }
    if (s_creator.pause_label)
        lv_label_set_text(s_creator.pause_label,
                          recording ? TXT_PAUSE : TXT_RESUME);
    if (s_creator.overwrite_panel) {
        if (recording)
            lv_obj_add_flag(s_creator.overwrite_panel, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_creator.overwrite_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_waiting_ui(void)
{
    set_recording_ui(true);
    if (s_creator.ui && s_creator.ui->music_screen_status_label)
        lv_label_set_text(s_creator.ui->music_screen_status_label,
                          TXT_WAIT_FIRST);
}

static void sync_auto_pause_ui(bool was_recording)
{
    if (!was_recording ||
        creator_recorder_state() != CREATOR_RECORDER_PAUSED)
        return;
    publish_snapshot(true, false);
    bsp_display_lock(portMAX_DELAY);
    reset_range_selection_ui();
    set_recording_ui(false);
    bsp_display_unlock();
    ESP_LOGI(TAG, "overwrite range completed; Creator paused");
}

static void auto_pause_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_creator.active) return;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    flush_live_refresh((uint32_t)(now_us / 1000U));
    if (!creator_recorder_poll(now_us)) return;
    publish_snapshot(true, false);
    reset_range_selection_ui();
    set_recording_ui(false);
    ESP_LOGI(TAG, "overwrite range timer reached the end; Creator paused");
}

static esp_err_t pause_locked(void)
{
    if (!s_creator.active ||
        creator_recorder_state() != CREATOR_RECORDER_RECORDING)
        return ESP_ERR_INVALID_STATE;
    creator_recorder_pause((uint64_t)esp_timer_get_time());
    publish_snapshot(true, false);
    reset_range_selection_ui();
    set_recording_ui(false);
    s_creator.last_error = ESP_OK;
    return ESP_OK;
}

static esp_err_t resume_locked(void)
{
    if (!s_creator.active ||
        creator_recorder_state() != CREATOR_RECORDER_PAUSED)
        return ESP_ERR_INVALID_STATE;
    reset_range_selection_ui();
    creator_recorder_resume((uint64_t)esp_timer_get_time());
    set_waiting_ui();
    s_creator.last_error = ESP_OK;
    return ESP_OK;
}

static lv_obj_t *creator_return_screen(void)
{
    if (s_creator.return_screen &&
        lv_obj_is_valid(s_creator.return_screen)) {
        return s_creator.return_screen;
    }
    return s_creator.ui ? s_creator.ui->screen : NULL;
}

static void exit_locked(void)
{
    creator_recorder_stop();
    s_creator.active = false;
    s_creator.saving = false;
    taskENTER_CRITICAL(&s_live_refresh_lock);
    s_creator.live_refresh_pending = false;
    taskEXIT_CRITICAL(&s_live_refresh_lock);
    music_display_set_creator_active(false);
    reset_range_selection_ui();
    if (s_creator.pause_button)
        lv_obj_add_flag(s_creator.pause_button, LV_OBJ_FLAG_HIDDEN);
    if (s_creator.exit_button)
        lv_obj_add_flag(s_creator.exit_button, LV_OBJ_FLAG_HIDDEN);
    if (s_creator.overwrite_panel)
        lv_obj_add_flag(s_creator.overwrite_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *target = creator_return_screen();
    if (target)
        lv_screen_load_anim(target,
                            LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

static esp_err_t finish_locked(void)
{
    if (!s_creator.active ||
        creator_recorder_state() != CREATOR_RECORDER_PAUSED)
        return ESP_ERR_INVALID_STATE;

    midi_data_t *snapshot = heap_caps_calloc(
        1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot || !creator_recorder_snapshot(snapshot) ||
        snapshot->note_count <= 0) {
        free(snapshot);
        if (s_creator.ui && s_creator.ui->music_screen_status_label)
            lv_label_set_text(s_creator.ui->music_screen_status_label,
                              TXT_NO_NOTES);
        s_creator.last_error = ESP_ERR_INVALID_SIZE;
        return s_creator.last_error;
    }

    s_creator.saving = true;
    esp_err_t err = score_storage_save_midi_auto(
        snapshot, s_creator.saved_title, sizeof(s_creator.saved_title),
        s_creator.saved_filename, sizeof(s_creator.saved_filename));
    free(snapshot);
    s_creator.saving = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save creator score failed: %s", esp_err_to_name(err));
        if (s_creator.ui && s_creator.ui->music_screen_status_label)
            lv_label_set_text(s_creator.ui->music_screen_status_label,
                              TXT_SAVE_FAILED);
        s_creator.last_error = err;
        return err;
    }

    ESP_LOGI(TAG, "creator score saved: %s (%s)",
             s_creator.saved_title, s_creator.saved_filename);
    s_creator.last_error = ESP_OK;
    exit_locked();
    return ESP_OK;
}

static void pause_cb(lv_event_t *event)
{
    (void)event;
    if (creator_recorder_state() == CREATOR_RECORDER_RECORDING)
        pause_locked();
    else if (creator_recorder_state() == CREATOR_RECORDER_PAUSED)
        resume_locked();
}

static void exit_cb(lv_event_t *event)
{
    (void)event;
    exit_locked();
}

static void finish_cb(lv_event_t *event)
{
    (void)event;
    finish_locked();
}

static void score_note_selected_cb(int note_index, int measure_number,
                                   void *user_data)
{
    (void)note_index;
    (void)user_data;
    if (!s_creator.active || measure_number < 1 ||
        creator_recorder_state() != CREATOR_RECORDER_PAUSED)
        return;

    if (s_creator.range_start_measure == 0) {
        s_creator.range_start_measure = measure_number;
        if (s_creator.overwrite_hint_label &&
            lv_obj_is_valid(s_creator.overwrite_hint_label)) {
            char text[64];
            snprintf(text, sizeof(text), TXT_RANGE_START_FMT, measure_number);
            lv_label_set_text(s_creator.overwrite_hint_label, text);
        }
        ESP_LOGI(TAG, "overwrite range start selected: measure %d",
                 measure_number);
        return;
    }

    int first = s_creator.range_start_measure;
    int last = measure_number;
    if (first > last) {
        int swap = first;
        first = last;
        last = swap;
    }
    if (creator_recorder_resume_measure_range(
            first, last,
            (uint64_t)esp_timer_get_time())) {
        reset_range_selection_ui();
        publish_snapshot(true, false);
        set_waiting_ui();
        ESP_LOGI(TAG, "overwrite range armed: measures %d-%d", first, last);
    } else {
        reset_range_selection_ui();
    }
}

static void ensure_music_controls(void)
{
    lv_ui *ui = s_creator.ui;
    if (!ui) return;
    if (!ui->music_screen) {
        setup_scr_music_screen(ui);
        ui->music_screen_del = false;
        app_font_apply_missing_cjk(ui->music_screen);
    }
    if (!ui->music_screen) return;

    if (!s_creator.pause_button ||
        !lv_obj_is_valid(s_creator.pause_button)) {
        s_creator.pause_button = make_button(
            ui->music_screen, TXT_PAUSE, 12, 10, 104, 48,
            &s_creator.pause_label);
        lv_obj_add_event_cb(s_creator.pause_button, pause_cb,
                            LV_EVENT_CLICKED, NULL);
        s_creator.exit_button = make_button(
            ui->music_screen, TXT_EXIT, 126, 10, 88, 48, NULL);
        lv_obj_add_event_cb(s_creator.exit_button, exit_cb,
                            LV_EVENT_CLICKED, NULL);

        s_creator.overwrite_panel = lv_obj_create(ui->music_screen);
        lv_obj_set_pos(s_creator.overwrite_panel, 225, 7);
        lv_obj_set_size(s_creator.overwrite_panel, 500, 56);
        lv_obj_set_style_pad_all(s_creator.overwrite_panel, 3, 0);
        lv_obj_set_style_radius(s_creator.overwrite_panel, 8, 0);
        lv_obj_set_style_bg_color(s_creator.overwrite_panel,
                                  lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_border_width(s_creator.overwrite_panel, 1, 0);
        lv_obj_set_style_border_color(s_creator.overwrite_panel,
                                      lv_color_hex(0xC8A778), 0);
        lv_obj_clear_flag(s_creator.overwrite_panel, LV_OBJ_FLAG_SCROLLABLE);
        s_creator.overwrite_hint_label = make_label(
            s_creator.overwrite_panel, TXT_SELECT_RANGE, 4, 8, 374, 32);
        s_creator.finish_button = make_button(
            s_creator.overwrite_panel, TXT_FINISH, 386, 2, 100, 46, NULL);
        lv_obj_add_event_cb(s_creator.finish_button, finish_cb,
                            LV_EVENT_CLICKED, NULL);
    }
    lv_obj_clear_flag(s_creator.pause_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_creator.exit_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_creator.finish_button, LV_OBJ_FLAG_HIDDEN);
}

static esp_err_t start_locked(const creator_recorder_config_t *config)
{
    if (!s_creator.initialized || !valid_config(config))
        return ESP_ERR_INVALID_ARG;
    if (s_creator.active) return ESP_ERR_INVALID_STATE;

    s_creator.config = normalized_remote_config(config);
    s_creator.last_error = ESP_OK;
    s_creator.saved_title[0] = '\0';
    s_creator.saved_filename[0] = '\0';
    taskENTER_CRITICAL(&s_live_refresh_lock);
    s_creator.last_live_publish_ms = 0;
    s_creator.live_refresh_pending = false;
    s_creator.live_publish_in_progress = false;
    taskEXIT_CRITICAL(&s_live_refresh_lock);
    creator_recorder_begin(&s_creator.config,
                           (uint64_t)esp_timer_get_time());
    s_creator.active = true;
    music_display_set_creator_active(true);
    ensure_music_controls();
    reset_range_selection_ui();
    set_waiting_ui();
    publish_snapshot(true, false);
    if (s_creator.ui && s_creator.ui->music_screen)
        lv_screen_load_anim(s_creator.ui->music_screen,
                            LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
    return ESP_OK;
}

static void start_cb(lv_event_t *event)
{
    (void)event;
    start_locked(&s_creator.config);
}

static void prep_back_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_t *target = creator_return_screen();
    if (target)
        lv_screen_load_anim(target,
                            LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

static void bpm_cb(lv_event_t *event)
{
    s_creator.config.bpm =
        (int)(intptr_t)lv_event_get_user_data(event);
    refresh_preparation();
}

static void time_cb(lv_event_t *event)
{
    int packed = (int)(intptr_t)lv_event_get_user_data(event);
    s_creator.config.time_sig_num = packed / 10;
    s_creator.config.time_sig_den = packed % 10;
    refresh_preparation();
}

static void staff_cb(lv_event_t *event)
{
    s_creator.config.staff_mode = (creator_staff_mode_t)(intptr_t)
                                  lv_event_get_user_data(event);
    refresh_preparation();
}

static void create_preparation_screen(void)
{
    s_creator.preparation = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_creator.preparation, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_creator.preparation, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_creator.preparation, LV_SCROLLBAR_MODE_OFF);

    make_label(s_creator.preparation, TXT_CREATOR_PREP,
               250, 30, 524, 48);
    lv_obj_t *back = make_button(s_creator.preparation, TXT_BACK,
                                 24, 24, 110, 48, NULL);
    lv_obj_add_event_cb(back, prep_back_cb, LV_EVENT_CLICKED, NULL);

    make_label(s_creator.preparation, "BPM", 85, 135, 160, 44);
    const int bpms[] = {60, 80, 100, 120};
    for (int i = 0; i < 4; ++i) {
        char text[12];
        snprintf(text, sizeof(text), "%d", bpms[i]);
        s_creator.bpm_buttons[i] = make_button(
            s_creator.preparation, text, 285 + i * 145, 130, 120, 52, NULL);
        lv_obj_add_event_cb(s_creator.bpm_buttons[i], bpm_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)bpms[i]);
    }

    make_label(s_creator.preparation, TXT_TIME_SIG,
               85, 235, 160, 44);
    const char *time_text[] = {"2/4", "3/4", "4/4", "6/8"};
    const int time_value[] = {24, 34, 44, 68};
    for (int i = 0; i < 4; ++i) {
        s_creator.time_buttons[i] = make_button(
            s_creator.preparation, time_text[i],
            285 + i * 145, 230, 120, 52, NULL);
        lv_obj_add_event_cb(s_creator.time_buttons[i], time_cb,
                            LV_EVENT_CLICKED,
                            (void *)(intptr_t)time_value[i]);
    }

    make_label(s_creator.preparation, TXT_VOICE_MODE,
               65, 335, 200, 44);
    s_creator.staff_buttons[0] = make_button(
        s_creator.preparation, TXT_SINGLE_VOICE, 285, 330, 240, 52, NULL);
    s_creator.staff_buttons[1] = make_button(
        s_creator.preparation, TXT_DOUBLE_VOICE, 550, 330, 240, 52, NULL);
    lv_obj_add_event_cb(s_creator.staff_buttons[0], staff_cb,
                        LV_EVENT_CLICKED,
                        (void *)(intptr_t)CREATOR_STAFF_SINGLE);
    lv_obj_add_event_cb(s_creator.staff_buttons[1], staff_cb,
                        LV_EVENT_CLICKED,
                        (void *)(intptr_t)CREATOR_STAFF_GRAND);

    lv_obj_t *start = make_button(s_creator.preparation, TXT_START,
                                  355, 470, 314, 58, NULL);
    lv_obj_set_style_bg_color(start, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_t *start_label = lv_obj_get_child(start, 0);
    if (start_label)
        lv_obj_set_style_text_color(start_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(start, start_cb, LV_EVENT_CLICKED, NULL);
    refresh_preparation();
}

static void entry_cb(lv_event_t *event)
{
    (void)event;
    creator_mode_open(s_creator.ui ? s_creator.ui->screen : NULL);
}

void creator_mode_open(lv_obj_t *return_screen)
{
    if (!s_creator.ui) return;
    s_creator.return_screen =
        return_screen && lv_obj_is_valid(return_screen)
            ? return_screen
            : s_creator.ui->screen;
    if (!s_creator.preparation ||
        !lv_obj_is_valid(s_creator.preparation))
        create_preparation_screen();
    refresh_preparation();
    lv_screen_load_anim(s_creator.preparation,
                        LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static esp_err_t creator_mode_initialize(lv_ui *ui, bool bind_entry)
{
    if (!ui || (bind_entry && !ui->screen_choose_button))
        return ESP_ERR_INVALID_ARG;
    s_creator.ui = ui;
    s_creator.return_screen = ui->screen;
    s_creator.config = (creator_recorder_config_t){
        .bpm = 100,
        .time_sig_num = 4,
        .time_sig_den = 4,
        .staff_mode = CREATOR_STAFF_GRAND,
        .duration_tolerance_percent = 30,
        .grand_staff_split_note = 60,
        .chord_window_ms = 45,
        .max_notes_per_onset = 10,
    };
    esp_err_t err = creator_recorder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "creator recorder init failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (!s_creator.auto_pause_timer)
        s_creator.auto_pause_timer = lv_timer_create(auto_pause_timer_cb,
                                                      20, NULL);
    if (!s_creator.auto_pause_timer) {
        ESP_LOGE(TAG, "unable to create Creator range timer");
        return ESP_ERR_NO_MEM;
    }
    music_display_set_note_selection_callback(score_note_selected_cb, NULL);
    if (bind_entry) {
        lv_obj_add_event_cb(ui->screen_choose_button, entry_cb,
                            LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "Creator Mode entry connected to Other Modes");
    } else {
        ESP_LOGI(TAG, "Creator Mode initialized with detached entry");
    }
    s_creator.initialized = true;
    s_creator.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t creator_mode_init(lv_ui *ui)
{
    return creator_mode_initialize(ui, true);
}

esp_err_t creator_mode_init_detached(lv_ui *ui)
{
    return creator_mode_initialize(ui, false);
}

bool creator_mode_is_active(void)
{
    return s_creator.active;
}

esp_err_t creator_mode_start(const creator_recorder_config_t *config)
{
    if (!s_creator.initialized) return ESP_ERR_INVALID_STATE;
    bsp_display_lock(portMAX_DELAY);
    esp_err_t err = start_locked(config);
    bsp_display_unlock();
    return err;
}

esp_err_t creator_mode_pause(void)
{
    if (!s_creator.initialized) return ESP_ERR_INVALID_STATE;
    bsp_display_lock(portMAX_DELAY);
    esp_err_t err = pause_locked();
    bsp_display_unlock();
    return err;
}

esp_err_t creator_mode_resume(void)
{
    if (!s_creator.initialized) return ESP_ERR_INVALID_STATE;
    bsp_display_lock(portMAX_DELAY);
    esp_err_t err = resume_locked();
    bsp_display_unlock();
    return err;
}

esp_err_t creator_mode_finish(void)
{
    if (!s_creator.initialized) return ESP_ERR_INVALID_STATE;
    bsp_display_lock(portMAX_DELAY);
    esp_err_t err = finish_locked();
    bsp_display_unlock();
    return err;
}

esp_err_t creator_mode_cancel(void)
{
    if (!s_creator.initialized || !s_creator.active)
        return ESP_ERR_INVALID_STATE;
    bsp_display_lock(portMAX_DELAY);
    s_creator.last_error = ESP_OK;
    exit_locked();
    bsp_display_unlock();
    return ESP_OK;
}

void creator_mode_get_status(creator_mode_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->available = s_creator.initialized;
    if (!s_creator.initialized) {
        status->state = CREATOR_MODE_STATE_IDLE;
        snprintf(status->message, sizeof(status->message),
                 "Creator Mode is not ready");
        return;
    }

    bsp_display_lock(portMAX_DELAY);
    status->active = s_creator.active;
    status->config = s_creator.config;
    status->last_error = s_creator.last_error;
    status->note_count = creator_recorder_note_count();
    status->measure_count = creator_recorder_measure_count();
    status->waiting_first_note =
        s_creator.active && creator_recorder_waiting_for_first_note();
    snprintf(status->saved_title, sizeof(status->saved_title), "%s",
             s_creator.saved_title);
    snprintf(status->saved_filename, sizeof(status->saved_filename), "%s",
             s_creator.saved_filename);

    creator_recorder_state_t recorder_state = creator_recorder_state();
    if (s_creator.saving) {
        status->state = CREATOR_MODE_STATE_SAVING;
        snprintf(status->message, sizeof(status->message), "Saving to SD card");
    } else if (s_creator.active && s_creator.last_error != ESP_OK) {
        status->state = CREATOR_MODE_STATE_ERROR;
        snprintf(status->message, sizeof(status->message), "%s",
                 esp_err_to_name(s_creator.last_error));
    } else if (!s_creator.active) {
        status->state = CREATOR_MODE_STATE_IDLE;
        snprintf(status->message, sizeof(status->message),
                 status->saved_filename[0] ? "Saved to SD card" : "Ready");
    } else if (recorder_state == CREATOR_RECORDER_PAUSED) {
        status->state = CREATOR_MODE_STATE_PAUSED;
        snprintf(status->message, sizeof(status->message), "Paused");
    } else {
        status->state = CREATOR_MODE_STATE_RECORDING;
        snprintf(status->message, sizeof(status->message),
                 status->waiting_first_note ? "Waiting for the first note"
                                            : "Recording");
    }
    bsp_display_unlock();
}

bool creator_mode_add_connection(creator_connection_kind_t kind,
                                 int start_note_index,
                                 int end_note_index,
                                 uint8_t number)
{
    if (!s_creator.active || !creator_recorder_add_connection(
            kind, start_note_index, end_note_index, number))
        return false;
    request_live_refresh((uint64_t)esp_timer_get_time());
    return true;
}

bool creator_mode_handle_midi_event(const usb_midi_input_event_t *event)
{
    if (!event || !s_creator.active) return false;

    switch (event->type) {
    case USB_MIDI_INPUT_DEVICE_CONNECTED:
        ESP_LOGI(TAG, "Creator MIDI connected: %s",
                 event->product[0] ? event->product : "unknown");
        break;
    case USB_MIDI_INPUT_DEVICE_DISCONNECTED:
        creator_recorder_pause(event->timestamp_us);
        publish_snapshot(true, false);
        bsp_display_lock(portMAX_DELAY);
        reset_range_selection_ui();
        set_recording_ui(false);
        bsp_display_unlock();
        ESP_LOGW(TAG, "Creator MIDI disconnected; recording paused");
        break;
    case USB_MIDI_INPUT_NOTE_ON: {
        bool was_recording = creator_recorder_state() ==
                             CREATOR_RECORDER_RECORDING;
        bool was_waiting = creator_recorder_waiting_for_first_note();
        if (creator_recorder_note_on(event->channel, event->note,
                                     event->velocity,
                                     event->timestamp_us)) {
            if (was_waiting) {
                bsp_display_lock(portMAX_DELAY);
                set_recording_ui(true);
                bsp_display_unlock();
            }
            request_live_refresh(event->timestamp_us);
        }
        sync_auto_pause_ui(was_recording);
        break;
    }
    case USB_MIDI_INPUT_NOTE_OFF: {
        bool was_recording = creator_recorder_state() ==
                             CREATOR_RECORDER_RECORDING;
        if (creator_recorder_note_off(event->channel, event->note,
                                      event->timestamp_us))
            request_live_refresh(event->timestamp_us);
        sync_auto_pause_ui(was_recording);
        break;
    }
    case USB_MIDI_INPUT_CONTROL_CHANGE:
        if (creator_recorder_control_change(event->channel,
                                            event->controller,
                                            event->value,
                                            event->timestamp_us) &&
            event->controller == 64)
            request_live_refresh(event->timestamp_us);
        break;
    case USB_MIDI_INPUT_PITCH_BEND:
        creator_recorder_pitch_bend(event->channel, event->pitch_bend,
                                    event->timestamp_us);
        /* Pitch bend is retained for explicit/future gliss semantics. It does
         * not alter notation or trigger a costly redraw by itself. */
        break;
    default:
        break;
    }
    return true;
}
