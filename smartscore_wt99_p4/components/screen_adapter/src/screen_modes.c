#include "screen_modes.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_font.h"
#include "audio_s3_adapter.h"
#include "creator_mode.h"
#include "creator_mode_integration.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "score_ui_flow.h"
#include "speaker_service.h"

#define COLOR_BG          0xFFF7EA
#define COLOR_CARD        0xF3E2C7
#define COLOR_CARD_LIGHT  0xFFF9EF
#define COLOR_INK         0x351B10
#define COLOR_MUTED       0x8B7355
#define COLOR_ACCENT      0x6B3515
#define COLOR_SELECTED    0x3F2204
#define COLOR_BORDER      0xC8A778

#define TONE_PRESET_COUNT 6
#define WAV_PAGE_SIZE 8
#define WAV_ROW_WIDTH 936
#define WAV_ROW_HEIGHT 86
#define NOTE_MONITOR_CAPACITY 8
#define NOTE_MONITOR_CHORD_WINDOW_US 300000LL
#define NOTE_MONITOR_HOLD_US 2200000LL

LV_FONT_DECLARE(lv_font_gudianChinese_42);
LV_FONT_DECLARE(lv_font_montserratMedium_34);

typedef struct {
    const char *name;
    float frequency_hz;
} tone_preset_t;

typedef struct {
    bool valid;
    uint8_t midi[NOTE_MONITOR_CAPACITY];
    size_t note_count;
    uint32_t sid;
    uint32_t seq;
    uint32_t sender_ts_ms;
    int64_t last_note_on_us;
    float latest_frequency_hz;
    float latest_confidence;
    bool has_frequency;
    bool has_confidence;
    bool native_poly;
    s3_music_poly_kind_t poly_kind;
    char poly_name[S3_MUSIC_POLY_NAME_MAX];
} note_monitor_snapshot_t;

typedef struct {
    bool valid;
    uint8_t raw_count;
    s3_music_diagnostic_candidate_t
        raw[S3_MUSIC_DIAGNOSTIC_MAX_RAW];
    s3_music_result_kind_t candidate_kind;
    uint8_t candidate_count;
    uint8_t candidate_notes[S3_MUSIC_DIAGNOSTIC_MAX_NOTES];
    s3_music_result_kind_t final_kind;
    uint8_t final_count;
    uint8_t final_notes[S3_MUSIC_DIAGNOSTIC_MAX_NOTES];
    int8_t octave_shift;
    float snr_db[3];
    char reject[S3_MUSIC_DIAGNOSTIC_REASON_MAX];
} note_monitor_diagnostic_t;

typedef struct {
    lv_ui *ui;
    lv_obj_t *hub;
    lv_obj_t *metronome;
    lv_obj_t *calibration;
    lv_obj_t *note_monitor;
    lv_obj_t *metronome_bpm_label;
    lv_obj_t *metronome_status_label;
    lv_obj_t *meter_buttons[4];
    lv_obj_t *calibration_frequency_label;
    lv_obj_t *calibration_status_label;
    lv_obj_t *tone_buttons[TONE_PRESET_COUNT];
    lv_obj_t *playback_status_label;
    lv_obj_t *note_monitor_value_label;
    lv_obj_t *note_monitor_kind_label;
    lv_obj_t *note_monitor_detail_label;
    lv_obj_t *note_monitor_diagnostic_label;
    lv_obj_t *note_monitor_link_label;
    lv_timer_t *status_timer;
    uint16_t metronome_bpm;
    uint8_t metronome_num;
    uint8_t metronome_den;
    float calibration_frequency_hz;
    size_t wav_page;
    size_t wav_total;
    esp_err_t note_monitor_error;
    char wav_names[WAV_PAGE_SIZE][SPEAKER_FILE_NAME_MAX];
    bool metronome_visible;
    bool calibration_visible;
    bool playback_visible;
    bool note_monitor_visible;
} screen_modes_context_t;

static const char *TAG = "screen_modes";
static lv_font_t s_choose_title_font;
static lv_font_t s_note_monitor_font;
static screen_modes_context_t s_modes;
static portMUX_TYPE s_note_monitor_lock = portMUX_INITIALIZER_UNLOCKED;
static note_monitor_snapshot_t s_note_monitor_snapshot;
static note_monitor_diagnostic_t s_note_monitor_diagnostic;
static bool s_note_monitor_capture_enabled;
static void hide_mode_status(void);

static const tone_preset_t s_tone_presets[] = {
    {"C3  130.81 Hz", 130.81f},
    {"C4  261.63 Hz", 261.63f},
    {"E4  329.63 Hz", 329.63f},
    {"A4  440.00 Hz", 440.00f},
    {"C5  523.25 Hz", 523.25f},
    {"A5  880.00 Hz", 880.00f},
};

static const lv_font_t *ui_font(void)
{
    const lv_font_t *font = app_font_chinese_22();
    return font ? font : LV_FONT_DEFAULT;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            int x, int y, int width, int height,
                            lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int width, int height,
                             lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    if (text != NULL && text[0] != '\0') {
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, ui_font(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INK), 0);
        lv_obj_center(label);
    }
    if (callback != NULL) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

static void set_button_label_font(lv_obj_t *button, const lv_font_t *font)
{
    if (button == NULL || font == NULL ||
        lv_obj_get_child_count(button) == 0) {
        return;
    }
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0), font,
                               LV_PART_MAIN);
}

static void load_screen(lv_obj_t *screen, lv_screen_load_anim_t animation)
{
    if (screen != NULL && lv_obj_is_valid(screen)) {
        lv_screen_load_anim(screen, animation, 180, 0, false);
    }
}

bool screen_modes_voice_go_home(void)
{
    if (!s_modes.ui || !s_modes.ui->screen ||
        !lv_obj_is_valid(s_modes.ui->screen)) {
        return false;
    }
    if (creator_mode_voice_go_home(s_modes.ui->screen)) {
        hide_mode_status();
        return true;
    }

    lv_obj_t *active_screen = lv_screen_active();
    bool on_hub = s_modes.hub && lv_obj_is_valid(s_modes.hub) &&
                  active_screen == s_modes.hub;
    bool on_metronome =
        s_modes.metronome && lv_obj_is_valid(s_modes.metronome) &&
        active_screen == s_modes.metronome;
    bool on_calibration =
        s_modes.calibration && lv_obj_is_valid(s_modes.calibration) &&
        active_screen == s_modes.calibration;
    bool on_playback =
        s_modes.playback_visible && s_modes.ui->choose_screen &&
        lv_obj_is_valid(s_modes.ui->choose_screen) &&
        active_screen == s_modes.ui->choose_screen;
    bool on_note_monitor =
        s_modes.note_monitor && lv_obj_is_valid(s_modes.note_monitor) &&
        active_screen == s_modes.note_monitor;
    if (!on_hub && !on_metronome && !on_calibration &&
        !on_playback && !on_note_monitor) {
        return false;
    }

    if (on_metronome)
        (void)speaker_service_metronome_stop();
    if (on_calibration)
        (void)speaker_service_stop();
    if (on_playback) {
        (void)speaker_service_stop_file();
        score_ui_flow_set_choose_back_override(NULL, NULL);
    }
    hide_mode_status();
    load_screen(s_modes.ui->screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

static void reset_note_monitor_capture(bool enabled)
{
    portENTER_CRITICAL(&s_note_monitor_lock);
    memset(&s_note_monitor_snapshot, 0, sizeof(s_note_monitor_snapshot));
    memset(&s_note_monitor_diagnostic, 0,
           sizeof(s_note_monitor_diagnostic));
    s_note_monitor_capture_enabled = enabled;
    portEXIT_CRITICAL(&s_note_monitor_lock);
}

static void stop_note_monitor(void)
{
    bool was_enabled;
    portENTER_CRITICAL(&s_note_monitor_lock);
    was_enabled = s_note_monitor_capture_enabled;
    s_note_monitor_capture_enabled = false;
    memset(&s_note_monitor_snapshot, 0, sizeof(s_note_monitor_snapshot));
    memset(&s_note_monitor_diagnostic, 0,
           sizeof(s_note_monitor_diagnostic));
    portEXIT_CRITICAL(&s_note_monitor_lock);
    if (was_enabled) {
        esp_err_t err = audio_s3_adapter_stop_session();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "unable to stop Audio S3 note monitor: %s",
                     esp_err_to_name(err));
        }
    }
    s_modes.note_monitor_error = ESP_OK;
}

static void hide_mode_status(void)
{
    if (s_modes.note_monitor_visible) stop_note_monitor();
    s_modes.metronome_visible = false;
    s_modes.calibration_visible = false;
    s_modes.playback_visible = false;
    s_modes.note_monitor_visible = false;
}

static void hub_back_cb(lv_event_t *event)
{
    (void)event;
    hide_mode_status();
    if (s_modes.ui != NULL) {
        load_screen(s_modes.ui->screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
    }
}

static void create_hub_screen(void);

static void load_hub(void)
{
    if (s_modes.hub == NULL || !lv_obj_is_valid(s_modes.hub)) {
        create_hub_screen();
    }
    hide_mode_status();
    load_screen(s_modes.hub, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void creator_cb(lv_event_t *event)
{
    (void)event;
    creator_mode_open(s_modes.hub);
}

static void metronome_back_cb(lv_event_t *event)
{
    (void)event;
    speaker_service_metronome_stop();
    load_hub();
}

static void calibration_back_cb(lv_event_t *event)
{
    (void)event;
    speaker_service_stop();
    load_hub();
}

static void set_result_text(lv_obj_t *label, const char *success,
                            esp_err_t error)
{
    if (label == NULL || !lv_obj_is_valid(label)) {
        return;
    }
    if (error == ESP_OK) {
        lv_label_set_text(label, success);
        return;
    }
    char text[96];
    snprintf(text, sizeof(text), "操作失败：%s", esp_err_to_name(error));
    lv_label_set_text(label, text);
}

static void refresh_bpm_label(void)
{
    if (s_modes.metronome_bpm_label == NULL ||
        !lv_obj_is_valid(s_modes.metronome_bpm_label)) {
        return;
    }
    char text[32];
    snprintf(text, sizeof(text), "%u BPM",
             (unsigned)s_modes.metronome_bpm);
    lv_label_set_text(s_modes.metronome_bpm_label, text);
}

static void set_meter_selected(void)
{
    const uint8_t nums[] = {2, 3, 4, 6};
    const uint8_t dens[] = {4, 4, 4, 8};
    for (size_t index = 0; index < 4; ++index) {
        lv_obj_t *button = s_modes.meter_buttons[index];
        if (button == NULL || !lv_obj_is_valid(button)) continue;
        bool selected = s_modes.metronome_num == nums[index] &&
                        s_modes.metronome_den == dens[index];
        lv_obj_set_style_bg_color(
            button,
            lv_color_hex(selected ? COLOR_SELECTED : COLOR_CARD), 0);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label != NULL) {
            lv_obj_set_style_text_color(
                label, lv_color_hex(selected ? 0xFFFFFF : COLOR_INK), 0);
        }
    }
}

static void bpm_adjust_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int next = (int)s_modes.metronome_bpm + delta;
    if (next < 30) next = 30;
    if (next > 240) next = 240;
    s_modes.metronome_bpm = (uint16_t)next;
    refresh_bpm_label();
}

static void meter_cb(lv_event_t *event)
{
    int packed = (int)(intptr_t)lv_event_get_user_data(event);
    s_modes.metronome_num = (uint8_t)(packed / 10);
    s_modes.metronome_den = (uint8_t)(packed % 10);
    set_meter_selected();
}

static void metronome_start_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_metronome_start(
        s_modes.metronome_bpm, s_modes.metronome_num,
        s_modes.metronome_den);
    set_result_text(s_modes.metronome_status_label,
                    "节拍器正在运行", err);
}

static void metronome_pause_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_metronome_pause();
    set_result_text(s_modes.metronome_status_label,
                    "节拍器已暂停", err);
}

static void metronome_stop_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_metronome_stop();
    set_result_text(s_modes.metronome_status_label,
                    "节拍器已停止", err);
}

static void create_metronome_screen(void)
{
    s_modes.metronome = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_modes.metronome, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_modes.metronome, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_modes.metronome, LV_SCROLLBAR_MODE_OFF);

    make_button(s_modes.metronome, "返回", 24, 20, 130, 50,
                metronome_back_cb, NULL);
    make_label(s_modes.metronome, "节拍器模式", 342, 24, 340, 48,
               LV_TEXT_ALIGN_CENTER);
    make_label(s_modes.metronome, "速度", 180, 145, 130, 40,
               LV_TEXT_ALIGN_CENTER);
    lv_obj_t *minus = make_button(
        s_modes.metronome, "-", 335, 132, 80, 58,
        bpm_adjust_cb, (void *)(intptr_t)-5);
    set_button_label_font(minus, &lv_font_montserratMedium_34);
    s_modes.metronome_bpm_label = make_label(
        s_modes.metronome, "", 430, 139, 165, 48, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *plus = make_button(
        s_modes.metronome, "+", 610, 132, 80, 58,
        bpm_adjust_cb, (void *)(intptr_t)5);
    set_button_label_font(plus, &lv_font_montserratMedium_34);
    refresh_bpm_label();

    make_label(s_modes.metronome, "拍号", 180, 255, 130, 40,
               LV_TEXT_ALIGN_CENTER);
    const char *meter_text[] = {"2/4", "3/4", "4/4", "6/8"};
    const int meter_value[] = {24, 34, 44, 68};
    for (size_t index = 0; index < 4; ++index) {
        s_modes.meter_buttons[index] = make_button(
            s_modes.metronome, meter_text[index],
            325 + (int)index * 125, 244, 105, 54, meter_cb,
            (void *)(intptr_t)meter_value[index]);
    }
    set_meter_selected();

    make_button(s_modes.metronome, "开始 / 重新开始", 245, 380, 220, 62,
                metronome_start_cb, NULL);
    make_button(s_modes.metronome, "暂停", 482, 380, 130, 62,
                metronome_pause_cb, NULL);
    make_button(s_modes.metronome, "停止", 629, 380, 130, 62,
                metronome_stop_cb, NULL);
    s_modes.metronome_status_label = make_label(
        s_modes.metronome, "节拍器已停止", 260, 480, 504, 40,
        LV_TEXT_ALIGN_CENTER);
}

static void open_metronome_cb(lv_event_t *event)
{
    (void)event;
    if (s_modes.metronome == NULL ||
        !lv_obj_is_valid(s_modes.metronome)) {
        create_metronome_screen();
    }
    hide_mode_status();
    s_modes.metronome_visible = true;
    load_screen(s_modes.metronome, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void refresh_tone_selection(void)
{
    if (s_modes.calibration_frequency_label != NULL &&
        lv_obj_is_valid(s_modes.calibration_frequency_label)) {
        char text[48];
        snprintf(text, sizeof(text), "%.2f Hz",
                 (double)s_modes.calibration_frequency_hz);
        lv_label_set_text(s_modes.calibration_frequency_label, text);
    }
    for (size_t index = 0; index < TONE_PRESET_COUNT; ++index) {
        lv_obj_t *button = s_modes.tone_buttons[index];
        if (button == NULL || !lv_obj_is_valid(button)) continue;
        bool selected =
            fabsf(s_modes.calibration_frequency_hz -
                  s_tone_presets[index].frequency_hz) < 0.01f;
        lv_obj_set_style_bg_color(
            button,
            lv_color_hex(selected ? COLOR_SELECTED : COLOR_CARD), 0);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label != NULL) {
            lv_obj_set_style_text_color(
                label, lv_color_hex(selected ? 0xFFFFFF : COLOR_INK), 0);
        }
    }
}

static void tone_preset_cb(lv_event_t *event)
{
    const tone_preset_t *preset =
        (const tone_preset_t *)lv_event_get_user_data(event);
    if (preset == NULL) return;
    s_modes.calibration_frequency_hz = preset->frequency_hz;
    refresh_tone_selection();
}

static void tone_start_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_play_tone(
        s_modes.calibration_frequency_hz, 600000, 0.12f);
    set_result_text(s_modes.calibration_status_label,
                    "校准音正在播放，离开页面时会自动停止", err);
}

static void tone_stop_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_stop();
    set_result_text(s_modes.calibration_status_label,
                    "校准音已停止", err);
}

static void create_calibration_screen(void)
{
    s_modes.calibration = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_modes.calibration,
                              lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_modes.calibration, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_modes.calibration, LV_SCROLLBAR_MODE_OFF);

    make_button(s_modes.calibration, "返回", 24, 20, 130, 50,
                calibration_back_cb, NULL);
    make_label(s_modes.calibration, "校准音模式", 342, 24, 340, 48,
               LV_TEXT_ALIGN_CENTER);
    make_label(s_modes.calibration,
               "选择标准音，播放后可用于乐器调音或输入校准",
               212, 105, 600, 42, LV_TEXT_ALIGN_CENTER);
    for (size_t index = 0; index < TONE_PRESET_COUNT; ++index) {
        int column = (int)(index % 3);
        int row = (int)(index / 3);
        s_modes.tone_buttons[index] = make_button(
            s_modes.calibration, s_tone_presets[index].name,
            165 + column * 245, 165 + row * 82, 205, 58,
            tone_preset_cb, (void *)&s_tone_presets[index]);
    }
    s_modes.calibration_frequency_label = make_label(
        s_modes.calibration, "", 382, 326, 260, 44,
        LV_TEXT_ALIGN_CENTER);
    refresh_tone_selection();
    make_button(s_modes.calibration, "播放校准音", 300, 392, 205, 62,
                tone_start_cb, NULL);
    make_button(s_modes.calibration, "停止", 530, 392, 160, 62,
                tone_stop_cb, NULL);
    s_modes.calibration_status_label = make_label(
        s_modes.calibration, "校准音已停止", 210, 492, 604, 40,
        LV_TEXT_ALIGN_CENTER);
}

static void open_calibration_cb(lv_event_t *event)
{
    (void)event;
    if (s_modes.calibration == NULL ||
        !lv_obj_is_valid(s_modes.calibration)) {
        create_calibration_screen();
    }
    hide_mode_status();
    s_modes.calibration_visible = true;
    load_screen(s_modes.calibration, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void update_playback_status(void)
{
    if (s_modes.playback_status_label == NULL ||
        !lv_obj_is_valid(s_modes.playback_status_label)) {
        return;
    }
    speaker_status_t status = {0};
    speaker_service_get_status(&status);
    char text[360];
    switch (status.state) {
    case SPEAKER_STATE_FILE:
        snprintf(text, sizeof(text), "正在播放：%s",
                 status.file_name[0] ? status.file_name : "WAV");
        break;
    case SPEAKER_STATE_FILE_PAUSED:
        snprintf(text, sizeof(text), "已暂停：%s",
                 status.file_name[0] ? status.file_name : "WAV");
        break;
    case SPEAKER_STATE_ERROR:
        snprintf(text, sizeof(text), "播放失败：%s",
                 esp_err_to_name(status.last_error));
        break;
    default:
        snprintf(text, sizeof(text), "请选择 /sdcard/wav 中的歌曲");
        break;
    }
    lv_label_set_text(s_modes.playback_status_label, text);
}

static void wav_play_cb(lv_event_t *event)
{
    const char *name = (const char *)lv_event_get_user_data(event);
    esp_err_t err = speaker_service_play_file(name);
    if (err != ESP_OK) {
        set_result_text(s_modes.playback_status_label, "", err);
    } else if (s_modes.playback_status_label != NULL) {
        char text[340];
        snprintf(text, sizeof(text), "正在打开：%s", name);
        lv_label_set_text(s_modes.playback_status_label, text);
    }
}

static void wav_pause_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_pause_file();
    if (err != ESP_OK) {
        set_result_text(s_modes.playback_status_label, "", err);
    } else {
        update_playback_status();
    }
}

static void wav_stop_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = speaker_service_stop_file();
    set_result_text(s_modes.playback_status_label,
                    "播放已停止", err);
}

static void populate_wav_list(void);

static void wav_refresh_cb(lv_event_t *event)
{
    (void)event;
    populate_wav_list();
}

static void wav_page_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    size_t pages = (s_modes.wav_total + WAV_PAGE_SIZE - 1) / WAV_PAGE_SIZE;
    if (pages == 0) pages = 1;
    if (delta < 0 && s_modes.wav_page > 1) {
        --s_modes.wav_page;
    } else if (delta > 0 && s_modes.wav_page < pages) {
        ++s_modes.wav_page;
    }
    populate_wav_list();
}

static lv_obj_t *make_content_row(lv_obj_t *content, int height)
{
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_set_size(row, WAV_ROW_WIDTH, height);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CARD_LIGHT), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0xDCC5A6), 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    return row;
}

static void populate_wav_list(void)
{
    if (s_modes.ui == NULL || s_modes.ui->choose_screen_content == NULL) {
        return;
    }
    lv_obj_t *content = s_modes.ui->choose_screen_content;
    lv_obj_clean(content);
    s_modes.playback_status_label = NULL;
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_style_pad_top(content, 6, 0);
    lv_obj_set_style_pad_bottom(content, 6, 0);

    lv_obj_t *toolbar = make_content_row(content, 72);
    s_modes.playback_status_label = make_label(
        toolbar, "正在读取 /sdcard/wav", 20, 20, 520, 34,
        LV_TEXT_ALIGN_LEFT);
    make_button(toolbar, "暂停 / 继续", 548, 10, 150, 50,
                wav_pause_cb, NULL);
    make_button(toolbar, "停止", 710, 10, 92, 50,
                wav_stop_cb, NULL);
    make_button(toolbar, "刷新", 814, 10, 102, 50,
                wav_refresh_cb, NULL);

    size_t count = 0;
    size_t actual_page = 1;
    esp_err_t err = speaker_service_list_files_page(
        NULL, s_modes.wav_page, WAV_PAGE_SIZE, s_modes.wav_names,
        WAV_PAGE_SIZE, &count, &s_modes.wav_total, &actual_page);
    s_modes.wav_page = actual_page;
    if (err != ESP_OK) {
        char message[160];
        snprintf(message, sizeof(message),
                 "无法读取 /sdcard/wav：%s\n请检查 SD 卡和 wav 文件夹",
                 esp_err_to_name(err));
        make_label(content, message, 0, 0, WAV_ROW_WIDTH, 90,
                   LV_TEXT_ALIGN_CENTER);
        set_result_text(s_modes.playback_status_label,
                        "歌曲目录不可用", err);
        return;
    }
    if (count == 0) {
        make_label(content,
                   "/sdcard/wav 中没有可播放的 .wav 文件",
                   0, 0, WAV_ROW_WIDTH, 80, LV_TEXT_ALIGN_CENTER);
    }

    for (size_t index = 0; index < count; ++index) {
        lv_obj_t *row = make_content_row(content, WAV_ROW_HEIGHT);
        lv_obj_t *icon = make_label(row, LV_SYMBOL_AUDIO,
                                    22, 22, 50, 40,
                                    LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_t *name = make_label(row, s_modes.wav_names[index],
                                    90, 17, 650, 48,
                                    LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        make_button(row, "播放", 790, 16, 120, 54,
                    wav_play_cb, s_modes.wav_names[index]);
    }

    size_t pages = (s_modes.wav_total + WAV_PAGE_SIZE - 1) / WAV_PAGE_SIZE;
    if (pages == 0) pages = 1;
    lv_obj_t *pager = make_content_row(content, 68);
    make_button(pager, "上一页", 235, 9, 125, 50,
                wav_page_cb, (void *)(intptr_t)-1);
    char page_text[80];
    snprintf(page_text, sizeof(page_text), "第 %u / %u 页，共 %u 首",
             (unsigned)s_modes.wav_page, (unsigned)pages,
             (unsigned)s_modes.wav_total);
    make_label(pager, page_text, 370, 18, 205, 36,
               LV_TEXT_ALIGN_CENTER);
    make_button(pager, "下一页", 585, 9, 125, 50,
                wav_page_cb, (void *)(intptr_t)1);
    update_playback_status();
}

static bool playback_back_override(void *user_data)
{
    (void)user_data;
    speaker_service_stop_file();
    score_ui_flow_set_choose_back_override(NULL, NULL);
    load_hub();
    return true;
}

static void open_playback_cb(lv_event_t *event)
{
    (void)event;
    lv_ui *ui = s_modes.ui;
    if (ui == NULL) return;
    /* Playback reuses only the choose-page layout. Suppress score scanning
     * while that page is created for WAV files. */
    score_ui_flow_set_choose_population_enabled(false);
    if (ui->choose_screen == NULL || ui->choose_screen_del) {
        setup_scr_choose_screen(ui);
        ui->choose_screen_del = false;
    }
    score_ui_flow_set_choose_population_enabled(true);
    s_choose_title_font = lv_font_gudianChinese_42;
    const lv_font_t *fallback = app_font_chinese_22();
    if (fallback != NULL) s_choose_title_font.fallback = fallback;
    lv_obj_set_style_text_font(ui->choose_screen_label_1,
                               &s_choose_title_font, 0);
    lv_label_set_text(ui->choose_screen_label_1, "选择歌曲");
    score_ui_flow_set_choose_back_override(playback_back_override, NULL);
    s_modes.wav_page = 1;
    populate_wav_list();
    hide_mode_status();
    s_modes.playback_visible = true;
    load_screen(ui->choose_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void sync_metronome_controls(const speaker_status_t *status)
{
    if (status->metronome.state == SPEAKER_METRONOME_STOPPED) {
        return;
    }
    if (status->metronome.bpm >= 30 &&
        status->metronome.bpm <= 240) {
        s_modes.metronome_bpm = status->metronome.bpm;
    }
    if (status->metronome.beats_per_measure != 0 &&
        status->metronome.beat_unit != 0) {
        s_modes.metronome_num = status->metronome.beats_per_measure;
        s_modes.metronome_den = status->metronome.beat_unit;
    }
    refresh_bpm_label();
    set_meter_selected();
}

static void sync_tone_controls(const speaker_status_t *status)
{
    if (status->state != SPEAKER_STATE_TONE ||
        status->frequency_hz < 20.0f) {
        return;
    }
    s_modes.calibration_frequency_hz = status->frequency_hz;
    refresh_tone_selection();
}

static void midi_note_name(uint8_t midi, char *buffer, size_t buffer_size)
{
    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B",
    };
    snprintf(buffer, buffer_size, "%s%d", names[midi % 12],
             (int)midi / 12 - 1);
}

static void sort_midi_notes(uint8_t *notes, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        uint8_t value = notes[i];
        size_t j = i;
        while (j > 0 && notes[j - 1] > value) {
            notes[j] = notes[j - 1];
            --j;
        }
        notes[j] = value;
    }
}

static const char *diagnostic_source_name(uint8_t source)
{
    switch (source) {
        case 0: return "YIN";
        case 1: return "低音YIN";
        case 2: return "低音模板";
        case 3: return "频谱";
        default: return "未知";
    }
}

static const char *diagnostic_kind_name(s3_music_result_kind_t kind)
{
    switch (kind) {
        case S3_MUSIC_RESULT_SINGLE: return "单音";
        case S3_MUSIC_RESULT_INTERVAL: return "双音";
        case S3_MUSIC_RESULT_CHORD: return "和弦";
        case S3_MUSIC_RESULT_SILENCE: return "静音";
        case S3_MUSIC_RESULT_UNKNOWN:
        default: return "未知";
    }
}

static void format_diagnostic_notes(const uint8_t *notes, uint8_t count,
                                    char *output, size_t capacity)
{
    size_t used = 0;
    for (uint8_t index = 0; index < count; ++index) {
        char name[12];
        midi_note_name(notes[index], name, sizeof(name));
        const int written = snprintf(output + used, capacity - used,
                                     index == 0 ? "%s" : "·%s", name);
        if (written < 0 || (size_t)written >= capacity - used) return;
        used += (size_t)written;
    }
    if (used == 0 && capacity > 1) snprintf(output, capacity, "—");
}

static void update_note_monitor_diagnostic(
    const note_monitor_diagnostic_t *diagnostic)
{
    if (s_modes.note_monitor_diagnostic_label == NULL ||
        !lv_obj_is_valid(s_modes.note_monitor_diagnostic_label)) {
        return;
    }
    if (!diagnostic->valid) {
        lv_label_set_text(s_modes.note_monitor_diagnostic_label,
                          "原始候选：等待诊断数据\n最终结果：—\n拒绝原因：—");
        return;
    }
    char raw[300] = {0};
    size_t used = 0;
    for (uint8_t index = 0; index < diagnostic->raw_count; ++index) {
        const s3_music_diagnostic_candidate_t *candidate =
            &diagnostic->raw[index];
        char name[12];
        midi_note_name(candidate->midi, name, sizeof(name));
        const int written = snprintf(
            raw + used, sizeof(raw) - used,
            index == 0 ? "%s %s %.0f%%" : " | %s %s %.0f%%",
            diagnostic_source_name(candidate->source), name,
            (double)(candidate->confidence * 100.0f));
        if (written < 0 || (size_t)written >= sizeof(raw) - used) break;
        used += (size_t)written;
    }
    if (used == 0) snprintf(raw, sizeof(raw), "—");
    char final_notes[64] = {0};
    format_diagnostic_notes(diagnostic->final_notes,
                            diagnostic->final_count,
                            final_notes, sizeof(final_notes));
    char text[512];
    snprintf(text, sizeof(text),
             "原始候选：%s\n最终结果：%s %s  SNR低/中/高 %.1f/%.1f/%.1f dB\n"
             "拒绝原因：%s%s",
             raw, diagnostic_kind_name(diagnostic->final_kind), final_notes,
             (double)diagnostic->snr_db[0],
             (double)diagnostic->snr_db[1],
             (double)diagnostic->snr_db[2],
             diagnostic->reject[0] ? diagnostic->reject : "unknown",
             diagnostic->octave_shift == 0
                 ? "" : diagnostic->octave_shift < 0
                     ? "  八度修正↓" : "  八度修正↑");
    lv_label_set_text(s_modes.note_monitor_diagnostic_label, text);
}

static void update_note_monitor(void)
{
    if (!s_modes.note_monitor_visible ||
        s_modes.note_monitor_value_label == NULL ||
        !lv_obj_is_valid(s_modes.note_monitor_value_label)) {
        return;
    }

    note_monitor_snapshot_t snapshot;
    note_monitor_diagnostic_t diagnostic;
    portENTER_CRITICAL(&s_note_monitor_lock);
    snapshot = s_note_monitor_snapshot;
    diagnostic = s_note_monitor_diagnostic;
    portEXIT_CRITICAL(&s_note_monitor_lock);
    update_note_monitor_diagnostic(&diagnostic);

    audio_s3_adapter_status_t adapter = {0};
    audio_s3_adapter_get_status(&adapter);
    if (s_modes.note_monitor_link_label != NULL) {
        char link[128];
        if (s_modes.note_monitor_error != ESP_OK) {
            snprintf(link, sizeof(link), "Audio S3 启动失败：%s",
                     esp_err_to_name(s_modes.note_monitor_error));
        } else if (adapter.connected) {
            snprintf(link, sizeof(link),
                     "Audio S3 已连接 · SID %lu · 序号 %lu",
                     (unsigned long)snapshot.sid,
                     (unsigned long)snapshot.seq);
        } else {
            snprintf(link, sizeof(link), "正在等待 Audio S3 连接…");
        }
        lv_label_set_text(s_modes.note_monitor_link_label, link);
    }

    const int64_t now_us = esp_timer_get_time();
    if (!snapshot.valid || snapshot.note_count == 0 ||
        now_us - snapshot.last_note_on_us > NOTE_MONITOR_HOLD_US) {
        lv_label_set_text(s_modes.note_monitor_value_label, "—");
        lv_label_set_text(s_modes.note_monitor_kind_label, "等待演奏");
        lv_label_set_text(s_modes.note_monitor_detail_label,
                          "请在 Audio S3 麦克风前弹奏单音或和弦");
        return;
    }

    sort_midi_notes(snapshot.midi, snapshot.note_count);
    char value[192] = {0};
    size_t used = 0;
    for (size_t i = 0; i < snapshot.note_count; ++i) {
        char name[12];
        midi_note_name(snapshot.midi[i], name, sizeof(name));
        int written = snprintf(value + used, sizeof(value) - used,
                               "%s%s", i == 0 ? "" : " · ", name);
        if (written < 0 || (size_t)written >= sizeof(value) - used) break;
        used += (size_t)written;
    }
    lv_label_set_text(s_modes.note_monitor_value_label, value);

    char kind[96];
    if (snapshot.native_poly &&
        snapshot.poly_kind == S3_MUSIC_POLY_CHORD &&
        snapshot.poly_name[0] != '\0') {
        snprintf(kind, sizeof(kind), "和弦 · %s", snapshot.poly_name);
    } else if (snapshot.native_poly &&
               snapshot.poly_kind == S3_MUSIC_POLY_INTERVAL) {
        snprintf(kind, sizeof(kind), "双音");
    } else if (snapshot.note_count > 1) {
        snprintf(kind, sizeof(kind), "%u 音和弦",
                 (unsigned)snapshot.note_count);
    } else {
        snprintf(kind, sizeof(kind), "单音");
    }
    lv_label_set_text(s_modes.note_monitor_kind_label, kind);

    char midi_list[96] = {0};
    used = 0;
    for (size_t i = 0; i < snapshot.note_count; ++i) {
        int written = snprintf(midi_list + used, sizeof(midi_list) - used,
                               "%s%u", i == 0 ? "" : ", ",
                               (unsigned)snapshot.midi[i]);
        if (written < 0 || (size_t)written >= sizeof(midi_list) - used) break;
        used += (size_t)written;
    }
    char detail[192];
    if (snapshot.has_frequency && snapshot.has_confidence) {
        snprintf(detail, sizeof(detail),
                 "MIDI: %s   最新频率: %.1f Hz   置信度: %.0f%%",
                 midi_list, (double)snapshot.latest_frequency_hz,
                 (double)(snapshot.latest_confidence * 100.0f));
    } else if (snapshot.has_frequency) {
        snprintf(detail, sizeof(detail),
                 "MIDI: %s   最新频率: %.1f Hz",
                 midi_list, (double)snapshot.latest_frequency_hz);
    } else if (snapshot.has_confidence) {
        snprintf(detail, sizeof(detail),
                 "MIDI: %s   置信度: %.0f%%",
                 midi_list,
                 (double)(snapshot.latest_confidence * 100.0f));
    } else {
        snprintf(detail, sizeof(detail), "MIDI: %s", midi_list);
    }
    lv_label_set_text(s_modes.note_monitor_detail_label, detail);
}

static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    speaker_status_t status = {0};
    speaker_service_get_status(&status);

    sync_metronome_controls(&status);
    sync_tone_controls(&status);

    if (s_modes.metronome_visible &&
        s_modes.metronome_status_label != NULL &&
        lv_obj_is_valid(s_modes.metronome_status_label)) {
        char text[96];
        unsigned beat = status.metronome.beat_index;
        if (beat == 0) beat = 1;
        if (status.metronome.state == SPEAKER_METRONOME_RUNNING) {
            snprintf(text, sizeof(text), "节拍器运行中 · 第 %u 拍", beat);
        } else if (status.metronome.state == SPEAKER_METRONOME_PAUSED) {
            snprintf(text, sizeof(text), "节拍器已暂停 · 第 %u 拍", beat);
        } else {
            snprintf(text, sizeof(text), "节拍器已停止");
        }
        lv_label_set_text(s_modes.metronome_status_label, text);
    }
    if (s_modes.calibration_visible &&
        s_modes.calibration_status_label != NULL &&
        lv_obj_is_valid(s_modes.calibration_status_label)) {
        if (status.state == SPEAKER_STATE_ERROR) {
            char text[96];
            snprintf(text, sizeof(text), "校准音错误：%s",
                     esp_err_to_name(status.last_error));
            lv_label_set_text(s_modes.calibration_status_label, text);
        } else if (status.state == SPEAKER_STATE_TONE) {
            lv_label_set_text(s_modes.calibration_status_label,
                              "校准音正在播放");
        } else {
            lv_label_set_text(s_modes.calibration_status_label,
                              "校准音已停止");
        }
    }
    if (s_modes.playback_visible) {
        update_playback_status();
    }
    if (s_modes.note_monitor_visible) {
        update_note_monitor();
    }
}

static void note_monitor_back_cb(lv_event_t *event)
{
    (void)event;
    stop_note_monitor();
    s_modes.note_monitor_visible = false;
    load_hub();
}

static void create_note_monitor_screen(void)
{
    s_modes.note_monitor = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_modes.note_monitor, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_modes.note_monitor, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_modes.note_monitor, LV_SCROLLBAR_MODE_OFF);

    make_button(s_modes.note_monitor, "返回", 24, 20, 130, 50,
                note_monitor_back_cb, NULL);
    make_label(s_modes.note_monitor, "音符识别测试", 332, 24, 360, 48,
               LV_TEXT_ALIGN_CENTER);
    make_label(s_modes.note_monitor,
               "实时显示 Audio S3 串口识别到的演奏音符",
               232, 82, 560, 36, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *panel = lv_obj_create(s_modes.note_monitor);
    lv_obj_set_pos(panel, 90, 125);
    lv_obj_set_size(panel, 844, 380);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFDF8), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x7DD3FC), 0);
    lv_obj_set_style_radius(panel, 18, 0);

    s_modes.note_monitor_value_label = make_label(
        panel, "—", 35, 24, 774, 72, LV_TEXT_ALIGN_CENTER);
    s_note_monitor_font = lv_font_montserratMedium_34;
    s_note_monitor_font.fallback = ui_font();
    lv_obj_set_style_text_font(s_modes.note_monitor_value_label,
                               &s_note_monitor_font, 0);
    lv_obj_set_style_text_color(s_modes.note_monitor_value_label,
                                lv_color_hex(0x0369A1), 0);
    lv_label_set_long_mode(s_modes.note_monitor_value_label,
                           LV_LABEL_LONG_DOT);

    s_modes.note_monitor_kind_label = make_label(
        panel, "等待演奏", 235, 104, 374, 38, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_color(s_modes.note_monitor_kind_label,
                                lv_color_hex(COLOR_ACCENT), 0);
    s_modes.note_monitor_detail_label = make_label(
        panel, "请在 Audio S3 麦克风前弹奏单音或和弦",
        55, 150, 734, 48, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_modes.note_monitor_detail_label,
                           LV_LABEL_LONG_WRAP);
    s_modes.note_monitor_diagnostic_label = make_label(
        panel,
        "原始候选：等待诊断数据\n最终结果：—\n拒绝原因：—",
        28, 214, 788, 138, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(s_modes.note_monitor_diagnostic_label,
                           LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_modes.note_monitor_diagnostic_label,
                                lv_color_hex(COLOR_MUTED), 0);

    s_modes.note_monitor_link_label = make_label(
        s_modes.note_monitor, "正在等待 Audio S3 连接…",
        160, 525, 704, 40, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_color(s_modes.note_monitor_link_label,
                                lv_color_hex(COLOR_MUTED), 0);
}

static void open_note_monitor_cb(lv_event_t *event)
{
    (void)event;
    if (s_modes.note_monitor == NULL ||
        !lv_obj_is_valid(s_modes.note_monitor)) {
        create_note_monitor_screen();
    }
    hide_mode_status();
    reset_note_monitor_capture(true);
    esp_err_t err = audio_s3_adapter_start_demo_session();
    s_modes.note_monitor_error = err;
    if (err != ESP_OK) {
        reset_note_monitor_capture(false);
        char text[128];
        snprintf(text, sizeof(text), "Audio S3 启动失败：%s",
                 esp_err_to_name(err));
        lv_label_set_text(s_modes.note_monitor_link_label, text);
        ESP_LOGW(TAG, "%s", text);
    }
    s_modes.note_monitor_visible = true;
    load_screen(s_modes.note_monitor, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static lv_obj_t *make_mode_card(lv_obj_t *parent, const char *title,
                                const char *description,
                                lv_event_cb_t callback)
{
    lv_obj_t *button = make_button(parent, "", 0, 0, 215, 250,
                                   callback, NULL);
    lv_obj_set_style_pad_all(button, 16, 0);
    lv_obj_set_style_pad_row(button, 24, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = make_label(
        button, title, 0, 0, 191, 40, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(title_label, LV_SIZE_CONTENT);
    lv_obj_t *description_label = make_label(
        button, description, 0, 0, 191, 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(description_label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(description_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(description_label,
                                lv_color_hex(COLOR_MUTED), 0);
    return button;
}

static void create_hub_screen(void)
{
    s_modes.hub = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_modes.hub, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_modes.hub, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_modes.hub, LV_SCROLLBAR_MODE_OFF);

    make_button(s_modes.hub, "返回", 24, 20, 130, 50,
                hub_back_cb, NULL);
    make_label(s_modes.hub, "其他模式", 342, 24, 340, 48,
               LV_TEXT_ALIGN_CENTER);
    make_label(s_modes.hub, "请选择要进入的功能", 332, 92, 360, 40,
               LV_TEXT_ALIGN_CENTER);

    lv_obj_t *cards = lv_obj_create(s_modes.hub);
    lv_obj_set_pos(cards, 0, 150);
    lv_obj_set_size(cards, 1024, 310);
    lv_obj_clear_flag(cards, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_set_style_pad_column(cards, 28, 0);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_mode_card(cards, "创作者模式",
                   "USB MIDI 实时录制\n自动生成乐谱",
                   creator_cb);
    make_mode_card(cards, "节拍器模式",
                   "设置 BPM 与拍号\n同步控制节拍状态",
                   open_metronome_cb);
    make_mode_card(cards, "校准模式",
                   "选择标准校准音\n用于调音与校准",
                   open_calibration_cb);
    make_mode_card(cards, "播放模式",
                   "读取 SD 卡 WAV\n控制本地歌曲",
                   open_playback_cb);

    lv_obj_t *monitor_button = make_button(
        s_modes.hub, "音符识别测试", 780, 505, 220, 60,
        open_note_monitor_cb, NULL);
    lv_obj_set_style_bg_color(monitor_button, lv_color_hex(0xE0F2FE), 0);
    lv_obj_set_style_border_color(monitor_button, lv_color_hex(0x38BDF8), 0);
}

static void other_modes_cb(lv_event_t *event)
{
    (void)event;
    if (s_modes.hub == NULL || !lv_obj_is_valid(s_modes.hub)) {
        create_hub_screen();
    }
    hide_mode_status();
    load_screen(s_modes.hub, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

esp_err_t screen_modes_init(lv_ui *ui)
{
    if (ui == NULL || ui->screen_choose_button == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_modes, 0, sizeof(s_modes));
    s_modes.ui = ui;
    s_modes.metronome_bpm = 100;
    s_modes.metronome_num = 4;
    s_modes.metronome_den = 4;
    s_modes.calibration_frequency_hz = 440.0f;
    s_modes.wav_page = 1;
    lv_obj_add_event_cb(ui->screen_choose_button, other_modes_cb,
                        LV_EVENT_CLICKED, NULL);
    s_modes.status_timer = lv_timer_create(status_timer_cb, 250, NULL);
    if (s_modes.status_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Other Modes hub connected");
    return ESP_OK;
}

void screen_modes_handle_audio_s3_event(const s3_music_event_t *event)
{
    if (event == NULL) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_note_monitor_lock);
    if (!s_note_monitor_capture_enabled) {
        portEXIT_CRITICAL(&s_note_monitor_lock);
        return;
    }

    note_monitor_snapshot_t *snapshot = &s_note_monitor_snapshot;
    if (event->type == S3_MUSIC_EVENT_DIAGNOSTIC) {
        note_monitor_diagnostic_t *diagnostic =
            &s_note_monitor_diagnostic;
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->valid = true;
        diagnostic->raw_count = event->diagnostic_raw_count;
        memcpy(diagnostic->raw, event->diagnostic_raw,
               sizeof(diagnostic->raw));
        diagnostic->candidate_kind = event->diagnostic_candidate_kind;
        diagnostic->candidate_count = event->diagnostic_candidate_count;
        memcpy(diagnostic->candidate_notes,
               event->diagnostic_candidate_notes,
               sizeof(diagnostic->candidate_notes));
        diagnostic->final_kind = event->diagnostic_final_kind;
        diagnostic->final_count = event->diagnostic_final_count;
        memcpy(diagnostic->final_notes, event->diagnostic_final_notes,
               sizeof(diagnostic->final_notes));
        diagnostic->octave_shift = event->diagnostic_octave_shift;
        memcpy(diagnostic->snr_db, event->diagnostic_snr_db,
               sizeof(diagnostic->snr_db));
        memcpy(diagnostic->reject, event->diagnostic_reject,
               sizeof(diagnostic->reject));
        snapshot->sid = event->sid;
        snapshot->seq = event->seq;
        portEXIT_CRITICAL(&s_note_monitor_lock);
        return;
    }
    if (event->type == S3_MUSIC_EVENT_POLY) {
        if (event->note_count < 2 ||
            event->note_count > S3_MUSIC_POLY_MAX_NOTES) {
            portEXIT_CRITICAL(&s_note_monitor_lock);
            return;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->valid = true;
        snapshot->native_poly = true;
        snapshot->poly_kind = event->poly_kind;
        snapshot->note_count = event->note_count;
        memcpy(snapshot->midi, event->notes, event->note_count);
        memcpy(snapshot->poly_name, event->poly_name,
               sizeof(snapshot->poly_name));
        snapshot->sid = event->sid;
        snapshot->seq = event->seq;
        snapshot->sender_ts_ms = event->sender_ts_ms;
        snapshot->last_note_on_us = now_us;
        snapshot->latest_confidence = event->confidence;
        snapshot->has_confidence = event->has_confidence;
        portEXIT_CRITICAL(&s_note_monitor_lock);
        return;
    }
    if (event->type != S3_MUSIC_EVENT_NOTE_ON || event->velocity == 0) {
        portEXIT_CRITICAL(&s_note_monitor_lock);
        return;
    }
    if (snapshot->native_poly && snapshot->valid &&
        snapshot->sid == event->sid) {
        uint32_t sender_delta =
            event->sender_ts_ms - snapshot->sender_ts_ms;
        if (sender_delta > UINT32_MAX / 2U) {
            sender_delta = snapshot->sender_ts_ms - event->sender_ts_ms;
        }
        if (sender_delta <= 80) {
            /* Audio S3 emits a selected melody NOTE_ON beside the same poly
             * result. Do not let that compatibility event replace or append
             * to the native chord snapshot. */
            portEXIT_CRITICAL(&s_note_monitor_lock);
            return;
        }
        /* This is a genuinely later melody result, not the compatibility
         * NOTE_ON paired with the poly frame. Start a fresh fallback group. */
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->valid = true;
    }
    if (!snapshot->valid || snapshot->last_note_on_us <= 0 ||
        now_us - snapshot->last_note_on_us >
            NOTE_MONITOR_CHORD_WINDOW_US) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->valid = true;
    }

    bool duplicate = false;
    for (size_t i = 0; i < snapshot->note_count; ++i) {
        if (snapshot->midi[i] == event->midi) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate && snapshot->note_count < NOTE_MONITOR_CAPACITY) {
        snapshot->midi[snapshot->note_count++] = event->midi;
    }
    snapshot->sid = event->sid;
    snapshot->seq = event->seq;
    snapshot->sender_ts_ms = event->sender_ts_ms;
    snapshot->last_note_on_us = now_us;
    snapshot->latest_frequency_hz = event->frequency_hz;
    snapshot->latest_confidence = event->confidence;
    snapshot->has_frequency = event->has_frequency;
    snapshot->has_confidence = event->has_confidence;
    portEXIT_CRITICAL(&s_note_monitor_lock);
}
