/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#ifndef GUI_GUIDER_H
#define GUI_GUIDER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"


typedef struct
{
  
	lv_obj_t *screen;
	bool screen_del;
	lv_obj_t *screen_backgroung;
	lv_obj_t *screen_Title;
	lv_obj_t *screen_start_button;
	lv_obj_t *screen_start_button_label;
	lv_obj_t *screen_btn_1;
	lv_obj_t *screen_btn_1_label;
	lv_obj_t *screen_setting_button;
	lv_obj_t *screen_setting_button_label;
	lv_obj_t *screen_choose_button;
	lv_obj_t *screen_choose_button_label;
	lv_obj_t *music_preparation;
	bool music_preparation_del;
	lv_obj_t *music_preparation_Title;
	lv_obj_t *music_preparation_content;
	lv_obj_t *music_preparation_line_1;
	lv_obj_t *music_preparation_line_2;
	lv_obj_t *music_preparation_line_3;
	lv_obj_t *music_preparation_line_4;
	lv_obj_t *music_preparation_speed_icon;
	lv_obj_t *music_preparation_diaoxing_icon;
	lv_obj_t *music_preparation_measurement_icon;
	lv_obj_t *music_preparation_paishu_icon;
	lv_obj_t *music_preparation_measurement_title;
	lv_obj_t *music_preparation_measure_text;
	lv_obj_t *music_preparation_diaoxing_title;
	lv_obj_t *music_preparation_paishu_title;
	lv_obj_t *music_preparation_speed_title;
	lv_obj_t *music_preparation_diaoxing_text;
	lv_obj_t *music_preparation_paishu_text;
	lv_obj_t *music_preparation_setting_content;
	lv_obj_t *music_preparation_metronme_title;
	lv_obj_t *music_preparation_speed_text;
	lv_obj_t *music_preparation_metronome_switch;
	lv_obj_t *music_preparation_metronme_speed_title;
	lv_obj_t *music_preparation_metroneme_speed_button_1;
	lv_obj_t *music_preparation_metroneme_speed_button_1_label;
	lv_obj_t *music_preparation_metroneme_speed_button_2;
	lv_obj_t *music_preparation_metroneme_speed_button_2_label;
	lv_obj_t *music_preparation_metroneme_speed_button_3;
	lv_obj_t *music_preparation_metroneme_speed_button_3_label;
	lv_obj_t *music_preparation_metroneme_speed_button_4;
	lv_obj_t *music_preparation_metroneme_speed_button_4_label;
	lv_obj_t *music_preparation_metronment_paihao_title;
	lv_obj_t *music_preparation_metronment_paihao_button_1;
	lv_obj_t *music_preparation_metronment_paihao_button_1_label;
	lv_obj_t *music_preparation_metronment_paihao_button_2;
	lv_obj_t *music_preparation_metronment_paihao_button_2_label;
	lv_obj_t *music_preparation_metronment_paihao_button_3;
	lv_obj_t *music_preparation_metronment_paihao_button_3_label;
	lv_obj_t *music_preparation_metronment_paihao_button_4;
	lv_obj_t *music_preparation_metronment_paihao_button_4_label;
	lv_obj_t *music_preparation_yinpin_title;
	lv_obj_t *music_preparation_metronment_paihao_input_title;
	lv_obj_t *music_preparation_metronment_paihao_input_button_1;
	lv_obj_t *music_preparation_metronment_paihao_input_button_1_label;
	lv_obj_t *music_preparation_metronment_paihao_input_button_2;
	lv_obj_t *music_preparation_metronment_paihao_input_button_2_label;
	lv_obj_t *music_preparation_music_type;
	lv_obj_t *music_preparation_music_type_button_1;
	lv_obj_t *music_preparation_music_type_button_1_label;
	lv_obj_t *music_preparation_music_type_button_2;
	lv_obj_t *music_preparation_music_type_button_2_label;
	lv_obj_t *music_preparation_start_button;
	lv_obj_t *music_preparation_start_button_label;
	lv_obj_t *music_preparation_img_1;
	lv_obj_t *music_preparation_img_2;
	lv_obj_t *music_screen;
	bool music_screen_del;
	lv_obj_t *music_screen_title;
	lv_obj_t *music_screen_tonality_text;
	lv_obj_t *music_screen_page_index;
	lv_obj_t *music_screen_warning_infos;
	lv_obj_t *music_screen_label_1;
	lv_obj_t *music_screen_label_2;
	lv_obj_t *music_screen_cont_1;
	lv_obj_t *music_screen_status_label;
	lv_obj_t *end_screen;
	bool end_screen_del;
	lv_obj_t *end_screen_background;
	lv_obj_t *end_screen_completionScore;
	lv_obj_t *end_screen_wanzhengdu_text;
	lv_obj_t *end_screen_xuanlvScore;
	lv_obj_t *end_screen_yinzhunScore;
	lv_obj_t *end_screen_jiezou_text;
	lv_obj_t *end_screen_yinzhun_text;
	lv_obj_t *end_screen_comprehensiveScore;
	lv_obj_t *setting_screen;
	bool setting_screen_del;
	lv_obj_t *setting_screen_Title;
	lv_obj_t *setting_screen_reset_button;
	lv_obj_t *setting_screen_reset_button_label;
	lv_obj_t *setting_screen_back_button;
	lv_obj_t *setting_screen_back_button_label;
	lv_obj_t *setting_screen_save_button;
	lv_obj_t *setting_screen_save_button_label;
	lv_obj_t *setting_screen_content;
	lv_obj_t *setting_screen_voice_slide;
	lv_obj_t *setting_screen_voice_text;
	lv_obj_t *setting_screen_voice_test_icon;
	lv_obj_t *setting_screen_voice_test_icon_label;
	lv_obj_t *setting_screen_brightness_text;
	lv_obj_t *setting_screen_brightness_slide;
	lv_obj_t *setting_screen_brightness_icon;
	lv_obj_t *setting_screen_btn_1;
	lv_obj_t *setting_screen_btn_1_label;
	lv_obj_t *setting_screen_line_2;
	lv_obj_t *setting_screen_line_1;
	lv_obj_t *setting_screen_line_3;
	lv_obj_t *setting_screen_line_4;
	lv_obj_t *setting_screen_line_5;
	lv_obj_t *setting_screen_line_6;
	lv_obj_t *setting_screen_line_7;
	lv_obj_t *setting_screen_line_8;
	lv_obj_t *setting_screen_img_1;
	lv_obj_t *setting_screen_img_2;
	lv_obj_t *choose_screen;
	bool choose_screen_del;
	lv_obj_t *choose_screen_label_1;
	lv_obj_t *choose_screen_content;
	lv_obj_t *choose_screen_back_button;
	lv_obj_t *choose_screen_back_button_label;
	lv_obj_t *choose_screen_img_1;
	lv_obj_t *choose_screen_img_2;
}lv_ui;

typedef void (*ui_setup_scr_t)(lv_ui * ui);

void ui_init_style(lv_style_t * style);

void ui_load_scr_animation(lv_ui *ui, lv_obj_t ** new_scr, bool * new_scr_del, bool * old_scr_del, ui_setup_scr_t setup_scr,
                           lv_screen_load_anim_t anim_type, uint32_t time, uint32_t delay, bool is_clean, bool auto_del);

void ui_animation(void * var, uint32_t duration, int32_t delay, int32_t start_value, int32_t end_value, lv_anim_path_cb_t path_cb,
                  uint32_t repeat_cnt, uint32_t repeat_delay, uint32_t playback_time, uint32_t playback_delay,
                  lv_anim_exec_xcb_t exec_cb, lv_anim_start_cb_t start_cb, lv_anim_completed_cb_t ready_cb, lv_anim_deleted_cb_t deleted_cb);


void init_scr_del_flag(lv_ui *ui);

void setup_bottom_layer(void);

void setup_ui(lv_ui *ui);

void video_play(lv_ui *ui);

void init_keyboard(lv_ui *ui);

extern lv_ui guider_ui;


void setup_scr_screen(lv_ui *ui);
void setup_scr_music_preparation(lv_ui *ui);
void setup_scr_music_screen(lv_ui *ui);
void setup_scr_end_screen(lv_ui *ui);
void setup_scr_setting_screen(lv_ui *ui);
void setup_scr_choose_screen(lv_ui *ui);
LV_IMAGE_DECLARE(_setupScreenBackground_resized_resized_RGB565A8_1024x600);

LV_IMAGE_DECLARE(_1music_RGB565A8_53x53);

LV_IMAGE_DECLARE(_music_RGB565A8_53x53);

LV_IMAGE_DECLARE(_piano_RGB565A8_53x53);
LV_IMAGE_DECLARE(_huawen_RGB565A8_122x55);
LV_IMAGE_DECLARE(_huawen2_RGB565A8_113x52);
LV_IMAGE_DECLARE(_endScreenBackground_resized_resized_RGB565A8_1024x600);

LV_FONT_DECLARE(lv_font_gudianChinese_84)
LV_FONT_DECLARE(lv_font_gudianChinese_34)
LV_FONT_DECLARE(lv_font_montserratMedium_16)
LV_FONT_DECLARE(lv_font_gudianChinese_42)
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_18)
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_26)
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_28)
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_22)
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_42)
LV_FONT_DECLARE(lv_font_gudianChinese_18)
LV_FONT_DECLARE(lv_font_montserratMedium_34)
LV_FONT_DECLARE(lv_font_SourceHanSansSCBold_32)
LV_FONT_DECLARE(lv_font_unscii_32)
LV_FONT_DECLARE(lv_font_gudianChinese_48)


#ifdef __cplusplus
}
#endif
#endif
