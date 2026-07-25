#include "score_ui_flow.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "app_font.h"
#include "score_storage.h"

LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_18);
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_22);
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_26);
LV_FONT_DECLARE(lv_font_montserratMedium_16);
LV_FONT_DECLARE(lv_font_montserratMedium_34);

#define SCORE_ROW_WIDTH  936
#define SCORE_ROW_HEIGHT 108

static const char *TAG = "score_ui_flow";
static score_info_t s_scores[SCORE_LIST_MAX];
static int s_score_count;
static int s_selected_index = -1;
static score_info_t s_selected_score;
static bool s_selected_score_valid;
static score_practice_options_t s_options;
static score_ui_start_cb_t s_start_callback;
static void *s_start_user_data;
static score_ui_prepare_changed_cb_t s_prepare_changed_callback;
static void *s_prepare_changed_user_data;
static score_ui_choose_back_override_cb_t s_choose_back_override;
static void *s_choose_back_user_data;
static bool s_choose_population_enabled = true;
static lv_obj_t *s_prepare_status;
static lv_obj_t *s_initialized_prepare_screen;
static bool s_prepare_status_overridden;
static lv_obj_t *s_preview_button;

static const uint32_t COLOR_INK = 0x3A1D14;
static const uint32_t COLOR_MUTED = 0x8B7355;
static const uint32_t COLOR_ACCENT = 0x8A4F14;
static const uint32_t COLOR_SELECTED = 0x3F2204;
static const uint32_t COLOR_UNSELECTED = 0xEEDEC6;

static const lv_font_t *chinese_ui_font(void)
{
    const lv_font_t *font = app_font_chinese_22();
    return font ? font : &lv_font_SourceHanSansSCBold_22;
}

void score_ui_flow_set_start_callback(score_ui_start_cb_t callback,
                                      void *user_data)
{
    s_start_callback = callback;
    s_start_user_data = user_data;
}

void score_ui_flow_set_prepare_changed_callback(
    score_ui_prepare_changed_cb_t callback,
    void *user_data)
{
    s_prepare_changed_callback = callback;
    s_prepare_changed_user_data = user_data;
}

void score_ui_flow_set_prepare_status(const char *text)
{
    s_prepare_status_overridden = true;
    if (s_prepare_status && lv_obj_is_valid(s_prepare_status))
        lv_label_set_text(s_prepare_status, text ? text : "");
}

void score_ui_flow_set_choose_back_override(
    score_ui_choose_back_override_cb_t callback,
    void *user_data)
{
    s_choose_back_override = callback;
    s_choose_back_user_data = user_data;
}

void score_ui_flow_set_choose_population_enabled(bool enabled)
{
    s_choose_population_enabled = enabled;
}

static void load_main_screen(void)
{
    ui_load_scr_animation(&guider_ui, &guider_ui.screen,
                          &guider_ui.screen_del, &guider_ui.choose_screen_del,
                          setup_scr_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                          180, 0, false, false);
}

static void choose_back_cb(lv_event_t *event)
{
    (void)event;
    if (s_choose_back_override &&
        s_choose_back_override(s_choose_back_user_data))
        return;
    load_main_screen();
}

static void set_option_button(lv_obj_t *button, bool selected)
{
    if (!button) return;
    lv_obj_set_style_bg_color(button,
        lv_color_hex(selected ? COLOR_SELECTED : COLOR_UNSELECTED),
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(button,
        lv_color_hex(selected ? 0xFFFFFF : COLOR_INK),
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, selected ? 0 : 1,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, lv_color_hex(0xC9AA83),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void refresh_prepare_options(void)
{
    lv_ui *ui = &guider_ui;
    char text[32];
    snprintf(text, sizeof(text), "%d", s_options.score_bpm);
    if (ui->music_preparation_speed_text)
        lv_label_set_text(ui->music_preparation_speed_text, text);
    snprintf(text, sizeof(text), "%d/%d", s_options.score_time_sig_num,
             s_options.score_time_sig_den);
    if (ui->music_preparation_paishu_text)
        lv_label_set_text(ui->music_preparation_paishu_text, text);
    set_option_button(ui->music_preparation_metronment_paihao_input_button_1,
                      s_options.input_source == SCORE_INPUT_MICROPHONE);
    set_option_button(ui->music_preparation_metronment_paihao_input_button_2,
                      s_options.input_source == SCORE_INPUT_USB_MIDI);
    set_option_button(ui->music_preparation_music_type_button_1,
                      s_options.notation_type == SCORE_NOTATION_NUMBERED);
    set_option_button(ui->music_preparation_music_type_button_2,
                      s_options.notation_type == SCORE_NOTATION_STAFF);
}

static bool notify_prepare_changed(void)
{
    if (!s_selected_score_valid) return false;
    if (!s_prepare_changed_callback) return true;
    return s_prepare_changed_callback(&s_selected_score, &s_options,
                                      s_prepare_changed_user_data);
}

static void delete_prepare_object(lv_obj_t **object)
{
    if (!object || !*object) return;
    if (lv_obj_is_valid(*object)) lv_obj_delete(*object);
    *object = NULL;
}

static void remove_metronome_controls(lv_ui *ui)
{
    delete_prepare_object(&ui->music_preparation_metronme_title);
    delete_prepare_object(&ui->music_preparation_metronome_switch);
    delete_prepare_object(&ui->music_preparation_metronme_speed_title);
    delete_prepare_object(&ui->music_preparation_metroneme_speed_button_1);
    delete_prepare_object(&ui->music_preparation_metroneme_speed_button_2);
    delete_prepare_object(&ui->music_preparation_metroneme_speed_button_3);
    delete_prepare_object(&ui->music_preparation_metroneme_speed_button_4);
    delete_prepare_object(&ui->music_preparation_metronment_paihao_title);
    delete_prepare_object(&ui->music_preparation_metronment_paihao_button_1);
    delete_prepare_object(&ui->music_preparation_metronment_paihao_button_2);
    delete_prepare_object(&ui->music_preparation_metronment_paihao_button_3);
    delete_prepare_object(&ui->music_preparation_metronment_paihao_button_4);

    ui->music_preparation_metroneme_speed_button_1_label = NULL;
    ui->music_preparation_metroneme_speed_button_2_label = NULL;
    ui->music_preparation_metroneme_speed_button_3_label = NULL;
    ui->music_preparation_metroneme_speed_button_4_label = NULL;
    ui->music_preparation_metronment_paihao_button_1_label = NULL;
    ui->music_preparation_metronment_paihao_button_2_label = NULL;
    ui->music_preparation_metronment_paihao_button_3_label = NULL;
    ui->music_preparation_metronment_paihao_button_4_label = NULL;
}

static void layout_remaining_prepare_controls(lv_ui *ui)
{
    lv_obj_set_height(ui->music_preparation_setting_content, 174);

    lv_obj_set_pos(ui->music_preparation_yinpin_title, 7, 25);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_input_button_1,
                   175, 14);
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_input_button_2,
                   401, 14);
    /* The old hidden "input source" label remains hidden outside the compact
     * settings container, matching the requested label-free input row. */
    lv_obj_set_pos(ui->music_preparation_metronment_paihao_input_title,
                   62, 389);

    lv_obj_set_pos(ui->music_preparation_music_type, 6, 105);
    lv_obj_set_pos(ui->music_preparation_music_type_button_1, 175, 92);
    lv_obj_set_pos(ui->music_preparation_music_type_button_2, 401, 92);
    lv_obj_set_pos(ui->music_preparation_start_button, 531, 530);
    lv_obj_set_size(ui->music_preparation_start_button, 250, 54);
}

static void input_source_cb(lv_event_t *event)
{
    s_options.input_source = (score_input_source_t)(intptr_t)
                             lv_event_get_user_data(event);
    notify_prepare_changed();
    refresh_prepare_options();
}

static void notation_type_cb(lv_event_t *event)
{
    s_options.notation_type = (score_notation_type_t)(intptr_t)
                              lv_event_get_user_data(event);
    notify_prepare_changed();
    refresh_prepare_options();
}

static void prepare_back_cb(lv_event_t *event)
{
    (void)event;
    score_ui_flow_open_choose(&guider_ui);
}

static bool start_prepared_with_mode(bool read_only)
{
    if (!s_selected_score_valid || !s_start_callback) return false;

    score_practice_options_t options = s_options;
    options.read_only = read_only;
    if (s_prepare_status)
        lv_label_set_text(s_prepare_status, "正在载入乐谱…");
    s_prepare_status_overridden = false;
    bool accepted = s_start_callback(s_selected_score.filename,
                                     &options, s_start_user_data);
    if (!accepted && !s_prepare_status_overridden && s_prepare_status)
        lv_label_set_text(s_prepare_status, "乐谱载入失败，请返回重试");
    return accepted;
}

static void prepare_start_cb(lv_event_t *event)
{
    (void)event;
    if (!s_selected_score_valid || !s_start_callback) {
        if (s_prepare_status)
            lv_label_set_text(s_prepare_status, "乐谱尚未准备好");
        return;
    }
    start_prepared_with_mode(false);
}

static void prepare_preview_cb(lv_event_t *event)
{
    (void)event;
    start_prepared_with_mode(true);
}

static lv_obj_t *create_back_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, 22, 18);
    lv_obj_set_size(button, 118, 48);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xF7EDDC), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xC9AA83), 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, LV_SYMBOL_LEFT " 返回");
    lv_obj_set_style_text_font(label, chinese_ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INK), 0);
    lv_obj_center(label);
    return button;
}

#if 0
static lv_obj_t *create_prepare_option_button(lv_obj_t *parent,
                                              const char *text,
                                              int x)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, 170);
    lv_obj_set_size(button, 192, 55);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, chinese_ui_font(), 0);
    lv_obj_center(label);
    return button;
}

static void ensure_practice_mode_controls(lv_ui *ui)
{
    if (s_mode_title && lv_obj_is_valid(s_mode_title) &&
        s_mode_read_button && lv_obj_is_valid(s_mode_read_button) &&
        s_mode_follow_button && lv_obj_is_valid(s_mode_follow_button))
        return;

    s_mode_title = lv_label_create(ui->music_preparation_setting_content);
    lv_obj_set_pos(s_mode_title, 6, 182);
    lv_obj_set_size(s_mode_title, 134, 32);
    lv_label_set_text(s_mode_title, "练习模式");
    lv_obj_set_style_text_font(s_mode_title, chinese_ui_font(), 0);
    lv_obj_set_style_text_color(s_mode_title, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_align(s_mode_title, LV_TEXT_ALIGN_CENTER, 0);

    s_mode_read_button = create_prepare_option_button(
        ui->music_preparation_setting_content, "只读看谱", 175);
    s_mode_follow_button = create_prepare_option_button(
        ui->music_preparation_setting_content, "演奏跟谱", 401);
    lv_obj_add_event_cb(s_mode_read_button, practice_mode_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)true);
    lv_obj_add_event_cb(s_mode_follow_button, practice_mode_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)false);
}
#endif

static void ensure_preview_button(lv_ui *ui)
{
    if (s_preview_button && lv_obj_is_valid(s_preview_button)) return;

    s_preview_button = lv_button_create(ui->music_preparation);
    lv_obj_set_pos(s_preview_button, 243, 530);
    lv_obj_set_size(s_preview_button, 250, 54);
    lv_obj_set_style_bg_color(s_preview_button, lv_color_hex(0xF7EDDC), 0);
    lv_obj_set_style_border_width(s_preview_button, 1, 0);
    lv_obj_set_style_border_color(s_preview_button,
                                  lv_color_hex(0xC9AA83), 0);
    lv_obj_set_style_radius(s_preview_button, 5, 0);
    lv_obj_set_style_shadow_width(s_preview_button, 0, 0);
    lv_obj_t *label = lv_label_create(s_preview_button);
    lv_label_set_text(label, "预览");
    lv_obj_set_style_text_font(label, chinese_ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INK), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(s_preview_button, prepare_preview_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void init_preparation_screen(bool read_only)
{
    lv_ui *ui = &guider_ui;
    const score_info_t *score = &s_selected_score;
    s_options = (score_practice_options_t){
        .score_bpm = score->bpm > 0 ? score->bpm : 120,
        .score_time_sig_num = 4,
        .score_time_sig_den = 4,
        .metronome_bpm = score->bpm > 0 ? score->bpm : 120,
        .metronome_time_sig_num = 4,
        .metronome_time_sig_den = 4,
        .metronome_enabled = false,
        .input_source = SCORE_INPUT_USB_MIDI,
        .notation_type = SCORE_NOTATION_STAFF,
        .read_only = read_only,
    };
    sscanf(score->time_signature, "%d/%d", &s_options.score_time_sig_num,
           &s_options.score_time_sig_den);
    if (s_options.score_time_sig_num <= 0 ||
        s_options.score_time_sig_den <= 0) {
        s_options.score_time_sig_num = 4;
        s_options.score_time_sig_den = 4;
    }
    s_options.metronome_time_sig_num = s_options.score_time_sig_num;
    s_options.metronome_time_sig_den = s_options.score_time_sig_den;

    lv_label_set_text(ui->music_preparation_measure_text, "钢琴");
    lv_label_set_text(ui->music_preparation_diaoxing_text,
                      score->key[0] ? score->key : "C major");
    lv_label_set_text(ui->music_preparation_Title, score->title);
    lv_obj_set_style_text_font(ui->music_preparation_Title,
                               chinese_ui_font(), 0);
    lv_obj_set_style_text_font(ui->music_preparation_measure_text,
                               chinese_ui_font(), 0);
    lv_obj_set_style_text_font(ui->music_preparation_diaoxing_text,
                               chinese_ui_font(), 0);
    lv_obj_set_style_text_font(ui->music_preparation_music_type,
                               chinese_ui_font(), 0);
    lv_obj_set_style_text_font(ui->music_preparation_music_type_button_1_label,
                               chinese_ui_font(), 0);
    lv_obj_set_style_text_font(ui->music_preparation_music_type_button_2_label,
                               chinese_ui_font(), 0);

    remove_metronome_controls(ui);
    layout_remaining_prepare_controls(ui);

    /* The generated screens are retained between visits. Reuse the existing
     * controls instead of stacking another set of LVGL callbacks each time. */
    if (s_initialized_prepare_screen == ui->music_preparation &&
        s_prepare_status && lv_obj_is_valid(s_prepare_status)) {
        ensure_preview_button(ui);
        lv_label_set_text(s_prepare_status, "");
        refresh_prepare_options();
        notify_prepare_changed();
        return;
    }

    if (s_initialized_prepare_screen &&
        s_initialized_prepare_screen != ui->music_preparation &&
        lv_obj_is_valid(s_initialized_prepare_screen)) {
        lv_obj_delete(s_initialized_prepare_screen);
    }
    s_initialized_prepare_screen = ui->music_preparation;
    s_prepare_status = NULL;
    s_preview_button = NULL;
    ensure_preview_button(ui);

    lv_obj_add_event_cb(ui->music_preparation_metronment_paihao_input_button_1,
                        input_source_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCORE_INPUT_MICROPHONE);
    lv_obj_add_event_cb(ui->music_preparation_metronment_paihao_input_button_2,
                        input_source_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCORE_INPUT_USB_MIDI);
    lv_obj_add_event_cb(ui->music_preparation_music_type_button_1,
                        notation_type_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCORE_NOTATION_NUMBERED);
    lv_obj_add_event_cb(ui->music_preparation_music_type_button_2,
                        notation_type_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCORE_NOTATION_STAFF);
    lv_obj_add_event_cb(ui->music_preparation_start_button, prepare_start_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = create_back_button(ui->music_preparation);
    lv_obj_add_event_cb(back, prepare_back_cb, LV_EVENT_CLICKED, NULL);
    s_prepare_status = lv_label_create(ui->music_preparation);
    lv_obj_set_pos(s_prepare_status, 182, 495);
    lv_obj_set_size(s_prepare_status, 660, 28);
    lv_obj_set_style_text_font(s_prepare_status, chinese_ui_font(), 0);
    lv_obj_set_style_text_color(s_prepare_status, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_align(s_prepare_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_prepare_status, "");
    refresh_prepare_options();
    notify_prepare_changed();
}

static void choose_read_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_score_count) return;
    s_selected_index = index;
    score_ui_flow_open_preparation(&guider_ui, &s_scores[index], true);
}

static void choose_score_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_score_count) return;
    s_selected_index = index;
    ESP_LOGI(TAG, "selected %s", s_scores[index].filename);
    score_ui_flow_open_preparation(&guider_ui, &s_scores[index], false);
}

bool score_ui_flow_open_preparation(lv_ui *ui,
                                    const score_info_t *score,
                                    bool read_only)
{
    if (!ui || !score || score->filename[0] == '\0') return false;
    s_selected_score = *score;
    s_selected_score_valid = true;
    bool old_screen_deleted = false;
    ui_load_scr_animation(ui, &ui->music_preparation,
                          &ui->music_preparation_del,
                          &old_screen_deleted,
                          setup_scr_music_preparation,
                          LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false, false);
    init_preparation_screen(read_only);
    ESP_LOGI(TAG, "preparation opened: %s mode=%s",
             score->filename, read_only ? "read_only" : "follow");
    return true;
}

bool score_ui_flow_return_to_preparation(lv_ui *ui)
{
    if (!ui || !s_selected_score_valid) return false;

    /* Preview is a temporary presentation mode. Keep the user's input and
     * notation choices when returning instead of rebuilding defaults. */
    score_practice_options_t saved_options = s_options;
    if (!score_ui_flow_open_preparation(ui, &s_selected_score, false)) {
        return false;
    }
    s_options = saved_options;
    s_options.read_only = false;
    refresh_prepare_options();
    notify_prepare_changed();
    return true;
}

bool score_ui_flow_apply_preparation_options(
    score_input_source_t input_source,
    score_notation_type_t notation_type,
    bool read_only)
{
    if (!s_selected_score_valid ||
        (input_source != SCORE_INPUT_MICROPHONE &&
         input_source != SCORE_INPUT_USB_MIDI) ||
        (notation_type != SCORE_NOTATION_NUMBERED &&
         notation_type != SCORE_NOTATION_STAFF))
        return false;
    s_options.input_source = input_source;
    s_options.notation_type = notation_type;
    s_options.read_only = read_only;
    refresh_prepare_options();
    return notify_prepare_changed();
}

bool score_ui_flow_copy_preparation(score_info_t *score,
                                    score_practice_options_t *options)
{
    if (!s_selected_score_valid || !score || !options) return false;
    *score = s_selected_score;
    *options = s_options;
    return true;
}

bool score_ui_flow_start_prepared(void)
{
    if (!s_selected_score_valid || !s_start_callback) return false;
    if (s_prepare_status) lv_label_set_text(s_prepare_status,
                                            "正在载入乐谱…");
    s_prepare_status_overridden = false;
    bool accepted = s_start_callback(s_selected_score.filename,
                                     &s_options, s_start_user_data);
    if (!accepted && !s_prepare_status_overridden && s_prepare_status)
        lv_label_set_text(s_prepare_status, "乐谱载入失败，请返回重试");
    return accepted;
}

static void populate_choose_list(lv_ui *ui)
{
    lv_obj_t *content = ui->choose_screen_content;
    if (!content) return;
    if (ui->choose_screen_label_1)
        lv_label_set_text(ui->choose_screen_label_1, "选择乐谱");
    score_ui_flow_set_choose_back_override(NULL, NULL);
    lv_obj_clean(content);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_style_pad_top(content, 6, 0);
    lv_obj_set_style_pad_bottom(content, 6, 0);
    s_score_count = score_storage_scan(s_scores, SCORE_LIST_MAX);
    if (s_score_count < 0) s_score_count = 0;

    if (s_score_count == 0) {
        lv_obj_t *empty = lv_label_create(content);
        lv_obj_set_width(empty, SCORE_ROW_WIDTH);
        lv_label_set_text(empty,
            "设备中没有乐谱文件\n请将 score.json 文件写入 storage 分区");
        lv_obj_set_style_text_font(empty, chinese_ui_font(), 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_MUTED), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < s_score_count; ++i) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_size(row, SCORE_ROW_WIDTH, SCORE_ROW_HEIGHT);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFF9EF), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xDCC5A6), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_set_pos(icon, 18, 22);
        lv_obj_set_size(icon, 64, 64);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(0xF1E0C5), 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_t *icon_text = lv_label_create(icon);
        lv_label_set_text(icon_text, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(icon_text, &lv_font_montserratMedium_34, 0);
        lv_obj_set_style_text_color(icon_text, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_center(icon_text);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_pos(title, 100, 17);
        lv_obj_set_size(title, 520, 32);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_label_set_text(title, s_scores[i].title);
        lv_obj_set_style_text_font(title, chinese_ui_font(), 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_INK), 0);

        char detail[160];
        snprintf(detail, sizeof(detail),
                 "钢琴  ·  %s  ·  %s  ·  BPM %d  ·  %d 小节 / %d 音符",
                 s_scores[i].key[0] ? s_scores[i].key : "C major",
                 s_scores[i].time_signature[0] ?
                    s_scores[i].time_signature : "4/4",
                 s_scores[i].bpm, s_scores[i].measure_count,
                 s_scores[i].note_count);
        lv_obj_t *meta = lv_label_create(row);
        lv_obj_set_pos(meta, 100, 63);
        lv_obj_set_size(meta, 580, 24);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
        lv_label_set_text(meta, detail);
        lv_obj_set_style_text_font(meta, chinese_ui_font(), 0);
        lv_obj_set_style_text_color(meta, lv_color_hex(COLOR_MUTED), 0);

#if 0
        lv_obj_t *read = lv_button_create(row);
        lv_obj_set_pos(read, 690, 27);
        lv_obj_set_size(read, 100, 54);
        lv_obj_set_style_bg_color(read, lv_color_hex(0xF7EDDC), 0);
        lv_obj_set_style_border_width(read, 1, 0);
        lv_obj_set_style_border_color(read, lv_color_hex(0xC9AA83), 0);
        lv_obj_set_style_radius(read, 8, 0);
        lv_obj_t *read_label = lv_label_create(read);
        lv_label_set_text(read_label, "只读");
        lv_obj_set_style_text_font(read_label, chinese_ui_font(), 0);
        lv_obj_set_style_text_color(read_label, lv_color_hex(COLOR_INK), 0);
        lv_obj_center(read_label);
        lv_obj_add_event_cb(read, choose_read_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
#endif

        lv_obj_t *perform = lv_button_create(row);
        lv_obj_set_pos(perform, 730, 27);
        lv_obj_set_size(perform, 180, 54);
        lv_obj_set_style_bg_color(perform, lv_color_hex(COLOR_SELECTED), 0);
        lv_obj_set_style_radius(perform, 8, 0);
        lv_obj_t *perform_label = lv_label_create(perform);
        lv_label_set_text(perform_label, "演奏");
        lv_obj_set_style_text_font(perform_label, chinese_ui_font(), 0);
        lv_obj_set_style_text_color(perform_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(perform_label);
        lv_obj_add_event_cb(perform, choose_score_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

void score_ui_flow_init_choose(lv_ui *ui)
{
    if (!ui || !ui->choose_screen) return;
    if (ui->choose_screen_back_button)
        lv_obj_add_event_cb(ui->choose_screen_back_button, choose_back_cb,
                            LV_EVENT_CLICKED, NULL);
    if (s_choose_population_enabled)
        populate_choose_list(ui);
}

void score_ui_flow_open_choose(lv_ui *ui)
{
    if (!ui) return;
    bool creating_screen = ui->choose_screen_del;
    ui_load_scr_animation(ui, &ui->choose_screen, &ui->choose_screen_del,
                          ui->music_preparation &&
                          lv_screen_active() == ui->music_preparation ?
                              &ui->music_preparation_del : &ui->screen_del,
                          setup_scr_choose_screen,
                          LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false, false);
    if (!creating_screen) populate_choose_list(ui);
}
