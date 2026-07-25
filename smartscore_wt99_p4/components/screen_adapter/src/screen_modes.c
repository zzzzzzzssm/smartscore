#include "screen_modes.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_font.h"
#include "creator_mode.h"
#include "esp_log.h"
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

LV_FONT_DECLARE(lv_font_gudianChinese_42);
LV_FONT_DECLARE(lv_font_montserratMedium_34);

typedef struct {
    const char *name;
    float frequency_hz;
} tone_preset_t;

typedef struct {
    lv_ui *ui;
    lv_obj_t *hub;
    lv_obj_t *metronome;
    lv_obj_t *calibration;
    lv_obj_t *metronome_bpm_label;
    lv_obj_t *metronome_status_label;
    lv_obj_t *meter_buttons[4];
    lv_obj_t *calibration_frequency_label;
    lv_obj_t *calibration_status_label;
    lv_obj_t *tone_buttons[TONE_PRESET_COUNT];
    lv_obj_t *playback_status_label;
    lv_timer_t *status_timer;
    uint16_t metronome_bpm;
    uint8_t metronome_num;
    uint8_t metronome_den;
    float calibration_frequency_hz;
    size_t wav_page;
    size_t wav_total;
    char wav_names[WAV_PAGE_SIZE][SPEAKER_FILE_NAME_MAX];
    bool metronome_visible;
    bool calibration_visible;
    bool playback_visible;
} screen_modes_context_t;

static const char *TAG = "screen_modes";
static lv_font_t s_choose_title_font;
static screen_modes_context_t s_modes;

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

static void hide_mode_status(void)
{
    s_modes.metronome_visible = false;
    s_modes.calibration_visible = false;
    s_modes.playback_visible = false;
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
