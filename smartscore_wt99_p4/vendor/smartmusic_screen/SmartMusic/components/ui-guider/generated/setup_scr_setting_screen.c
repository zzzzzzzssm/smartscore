/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#include "lvgl.h"
#include <stdio.h>
#include "gui_guider.h"
#include "events_init.h"
#include "widgets_init.h"
#include "custom.h"



void setup_scr_setting_screen(lv_ui *ui)
{
    //Write codes setting_screen
    ui->setting_screen = lv_obj_create(NULL);
    lv_obj_set_size(ui->setting_screen, 1024, 600);
    lv_obj_set_scrollbar_mode(ui->setting_screen, LV_SCROLLBAR_MODE_OFF);

    //Write style for setting_screen, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen, lv_color_hex(0xF7EDDC), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_Title
    ui->setting_screen_Title = lv_label_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_Title, 310, 17);
    lv_obj_set_size(ui->setting_screen_Title, 404, 73);
    lv_label_set_text(ui->setting_screen_Title, "设置页面");
    lv_label_set_long_mode(ui->setting_screen_Title, LV_LABEL_LONG_WRAP);

    //Write style for setting_screen_Title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_Title, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_Title, &lv_font_gudianChinese_42, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_Title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_Title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_reset_button
    ui->setting_screen_reset_button = lv_button_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_reset_button, 746, 521);
    lv_obj_set_size(ui->setting_screen_reset_button, 178, 63);
    ui->setting_screen_reset_button_label = lv_label_create(ui->setting_screen_reset_button);
    lv_label_set_text(ui->setting_screen_reset_button_label, "重置");
    lv_label_set_long_mode(ui->setting_screen_reset_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->setting_screen_reset_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->setting_screen_reset_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->setting_screen_reset_button_label, LV_PCT(100));

    //Write style for setting_screen_reset_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_reset_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->setting_screen_reset_button, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui->setting_screen_reset_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui->setting_screen_reset_button, lv_color_hex(0x3D1F1F), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ui->setting_screen_reset_button, LV_BORDER_SIDE_FULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_reset_button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_reset_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_reset_button, lv_color_hex(0x3D1F1F), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_reset_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_reset_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_reset_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_back_button
    ui->setting_screen_back_button = lv_button_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_back_button, 428, 522);
    lv_obj_set_size(ui->setting_screen_back_button, 178, 63);
    ui->setting_screen_back_button_label = lv_label_create(ui->setting_screen_back_button);
    lv_label_set_text(ui->setting_screen_back_button_label, "返回");
    lv_label_set_long_mode(ui->setting_screen_back_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->setting_screen_back_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->setting_screen_back_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->setting_screen_back_button_label, LV_PCT(100));

    //Write style for setting_screen_back_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_back_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->setting_screen_back_button, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui->setting_screen_back_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui->setting_screen_back_button, lv_color_hex(0x3D1F1F), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ui->setting_screen_back_button, LV_BORDER_SIDE_FULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_back_button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_back_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_back_button, lv_color_hex(0x3D1F1F), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_back_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_back_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_back_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_save_button
    ui->setting_screen_save_button = lv_button_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_save_button, 108, 524);
    lv_obj_set_size(ui->setting_screen_save_button, 178, 63);
    ui->setting_screen_save_button_label = lv_label_create(ui->setting_screen_save_button);
    lv_label_set_text(ui->setting_screen_save_button_label, "保存设置");
    lv_label_set_long_mode(ui->setting_screen_save_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->setting_screen_save_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->setting_screen_save_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->setting_screen_save_button_label, LV_PCT(100));

    //Write style for setting_screen_save_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_save_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_save_button, lv_color_hex(0x3D1F1F), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_save_button, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->setting_screen_save_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_save_button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_save_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_save_button, lv_color_hex(0xF5E6D3), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_save_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_save_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_save_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for setting_screen_save_button, Part: LV_PART_MAIN, State: LV_STATE_PRESSED.
    lv_obj_set_style_bg_opa(ui->setting_screen_save_button, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(ui->setting_screen_save_button, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_save_button, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ui->setting_screen_save_button, 0, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui->setting_screen_save_button, 5, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui->setting_screen_save_button, 0, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui->setting_screen_save_button, lv_color_hex(0xF5E6D3), LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_font(ui->setting_screen_save_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(ui->setting_screen_save_button, 255, LV_PART_MAIN|LV_STATE_PRESSED);

    //Write codes setting_screen_content
    ui->setting_screen_content = lv_obj_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_content, 33, 77);
    lv_obj_set_size(ui->setting_screen_content, 958, 431);
    lv_obj_set_scrollbar_mode(ui->setting_screen_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ui->setting_screen_content, LV_OBJ_FLAG_SCROLLABLE);

    //Write style for setting_screen_content, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->setting_screen_content, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_content, lv_color_hex(0xFAF3E7), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_content, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_content, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_voice_slide
    ui->setting_screen_voice_slide = lv_slider_create(ui->setting_screen_content);
    lv_obj_set_pos(ui->setting_screen_voice_slide, 320, 35);
    lv_obj_set_size(ui->setting_screen_voice_slide, 592, 31);
    lv_slider_set_range(ui->setting_screen_voice_slide, 0, 100);
    lv_slider_set_mode(ui->setting_screen_voice_slide, LV_SLIDER_MODE_NORMAL);
    lv_slider_set_value(ui->setting_screen_voice_slide, 70, LV_ANIM_OFF);

    //Write style for setting_screen_voice_slide, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_voice_slide, 200, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_voice_slide, lv_color_hex(0xB8A88A), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_voice_slide, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_voice_slide, 50, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui->setting_screen_voice_slide, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(ui->setting_screen_voice_slide, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(ui->setting_screen_voice_slide, lv_color_hex(0xD4C5A9), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_voice_slide, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for setting_screen_voice_slide, Part: LV_PART_INDICATOR, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_voice_slide, 255, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_voice_slide, lv_color_hex(0x5C3A1E), LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_voice_slide, LV_GRAD_DIR_NONE, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_voice_slide, 50, LV_PART_INDICATOR|LV_STATE_DEFAULT);

    //Write style for setting_screen_voice_slide, Part: LV_PART_KNOB, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_voice_slide, 255, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_voice_slide, lv_color_hex(0x2C1810), LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_voice_slide, LV_GRAD_DIR_NONE, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_voice_slide, 50, LV_PART_KNOB|LV_STATE_DEFAULT);

    //Write style for setting_screen_voice_slide, Part: LV_PART_KNOB, State: LV_STATE_FOCUSED.
    lv_obj_set_style_bg_opa(ui->setting_screen_voice_slide, 255, LV_PART_KNOB|LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(ui->setting_screen_voice_slide, lv_color_hex(0x1A3A52), LV_PART_KNOB|LV_STATE_FOCUSED);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_voice_slide, LV_GRAD_DIR_NONE, LV_PART_KNOB|LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ui->setting_screen_voice_slide, 50, LV_PART_KNOB|LV_STATE_FOCUSED);

    //Write codes setting_screen_voice_text
    ui->setting_screen_voice_text = lv_label_create(ui->setting_screen_content);
    lv_obj_set_pos(ui->setting_screen_voice_text, 109, 30);
    lv_obj_set_size(ui->setting_screen_voice_text, 128, 48);
    lv_label_set_text(ui->setting_screen_voice_text, "音量：");
    lv_label_set_long_mode(ui->setting_screen_voice_text, LV_LABEL_LONG_WRAP);

    //Write style for setting_screen_voice_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_voice_text, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_voice_text, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_voice_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_voice_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_voice_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_voice_test_icon
    ui->setting_screen_voice_test_icon = lv_button_create(ui->setting_screen_content);
    lv_obj_set_pos(ui->setting_screen_voice_test_icon, 48, 21);
    lv_obj_set_size(ui->setting_screen_voice_test_icon, 54, 56);
    ui->setting_screen_voice_test_icon_label = lv_label_create(ui->setting_screen_voice_test_icon);
    lv_label_set_text(ui->setting_screen_voice_test_icon_label, "" LV_SYMBOL_VOLUME_MAX "");
    lv_label_set_long_mode(ui->setting_screen_voice_test_icon_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->setting_screen_voice_test_icon_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->setting_screen_voice_test_icon, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->setting_screen_voice_test_icon_label, LV_PCT(100));

    //Write style for setting_screen_voice_test_icon, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_voice_test_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_voice_test_icon, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_voice_test_icon, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->setting_screen_voice_test_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_voice_test_icon, 54, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_voice_test_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_voice_test_icon, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_voice_test_icon, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_voice_test_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_voice_test_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_brightness_text
    ui->setting_screen_brightness_text = lv_label_create(ui->setting_screen_content);
    lv_obj_set_pos(ui->setting_screen_brightness_text, 110, 101);
    lv_obj_set_size(ui->setting_screen_brightness_text, 128, 48);
    lv_label_set_text(ui->setting_screen_brightness_text, "亮度：");
    lv_label_set_long_mode(ui->setting_screen_brightness_text, LV_LABEL_LONG_WRAP);

    //Write style for setting_screen_brightness_text, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_brightness_text, lv_color_hex(0x2C1810), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_brightness_text, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_brightness_text, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_brightness_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_brightness_text, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_brightness_slide
    ui->setting_screen_brightness_slide = lv_slider_create(ui->setting_screen_content);
    lv_obj_set_pos(ui->setting_screen_brightness_slide, 320, 108);
    lv_obj_set_size(ui->setting_screen_brightness_slide, 593, 31);
    lv_slider_set_range(ui->setting_screen_brightness_slide, 0, 100);
    lv_slider_set_mode(ui->setting_screen_brightness_slide, LV_SLIDER_MODE_NORMAL);
    lv_slider_set_value(ui->setting_screen_brightness_slide, 40, LV_ANIM_OFF);

    //Write style for setting_screen_brightness_slide, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_brightness_slide, 200, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_brightness_slide, lv_color_hex(0xB8A88A), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_brightness_slide, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_brightness_slide, 50, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui->setting_screen_brightness_slide, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(ui->setting_screen_brightness_slide, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(ui->setting_screen_brightness_slide, lv_color_hex(0xD4C5A9), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_brightness_slide, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for setting_screen_brightness_slide, Part: LV_PART_INDICATOR, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_brightness_slide, 255, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_brightness_slide, lv_color_hex(0x5C3A1E), LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_brightness_slide, LV_GRAD_DIR_NONE, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_brightness_slide, 50, LV_PART_INDICATOR|LV_STATE_DEFAULT);

    //Write style for setting_screen_brightness_slide, Part: LV_PART_KNOB, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_brightness_slide, 255, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_brightness_slide, lv_color_hex(0x2C1810), LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_brightness_slide, LV_GRAD_DIR_NONE, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_brightness_slide, 50, LV_PART_KNOB|LV_STATE_DEFAULT);

    //Write style for setting_screen_brightness_slide, Part: LV_PART_KNOB, State: LV_STATE_FOCUSED.
    lv_obj_set_style_bg_opa(ui->setting_screen_brightness_slide, 255, LV_PART_KNOB|LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(ui->setting_screen_brightness_slide, lv_color_hex(0x1A3A52), LV_PART_KNOB|LV_STATE_FOCUSED);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_brightness_slide, LV_GRAD_DIR_NONE, LV_PART_KNOB|LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ui->setting_screen_brightness_slide, 50, LV_PART_KNOB|LV_STATE_FOCUSED);

    //Write codes setting_screen_brightness_icon
    ui->setting_screen_brightness_icon = lv_obj_create(ui->setting_screen_content);
    lv_obj_set_pos(ui->setting_screen_brightness_icon, 49, 96);
    lv_obj_set_size(ui->setting_screen_brightness_icon, 54, 54);
    lv_obj_set_scrollbar_mode(ui->setting_screen_brightness_icon, LV_SCROLLBAR_MODE_OFF);

    //Write style for setting_screen_brightness_icon, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->setting_screen_brightness_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_brightness_icon, 54, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->setting_screen_brightness_icon, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_brightness_icon, lv_color_hex(0xEEDEC6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_brightness_icon, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->setting_screen_brightness_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->setting_screen_brightness_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->setting_screen_brightness_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->setting_screen_brightness_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_brightness_icon, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_btn_1
    ui->setting_screen_btn_1 = lv_button_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_btn_1, 16, 15);
    lv_obj_set_size(ui->setting_screen_btn_1, 23, 23);
    ui->setting_screen_btn_1_label = lv_label_create(ui->setting_screen_btn_1);
    lv_label_set_text(ui->setting_screen_btn_1_label, "");
    lv_label_set_long_mode(ui->setting_screen_btn_1_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->setting_screen_btn_1_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->setting_screen_btn_1, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->setting_screen_btn_1_label, LV_PCT(100));

    //Write style for setting_screen_btn_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->setting_screen_btn_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->setting_screen_btn_1, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->setting_screen_btn_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->setting_screen_btn_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->setting_screen_btn_1, 54, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->setting_screen_btn_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->setting_screen_btn_1, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->setting_screen_btn_1, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->setting_screen_btn_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->setting_screen_btn_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_2
    ui->setting_screen_line_2 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_2, 27, 6);
    lv_obj_set_size(ui->setting_screen_line_2, 1, 4);
    static lv_point_precise_t setting_screen_line_2[] = {{0, 0},{0, 60}};
    lv_line_set_points(ui->setting_screen_line_2, setting_screen_line_2, 2);

    //Write style for setting_screen_line_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_2, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_2, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_2, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_1
    ui->setting_screen_line_1 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_1, 27, 42);
    lv_obj_set_size(ui->setting_screen_line_1, 1, 5);
    static lv_point_precise_t setting_screen_line_1[] = {{0, 0},{0, 60}};
    lv_line_set_points(ui->setting_screen_line_1, setting_screen_line_1, 2);

    //Write style for setting_screen_line_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_1, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_1, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_1, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_3
    ui->setting_screen_line_3 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_3, 40, 37);
    lv_obj_set_size(ui->setting_screen_line_3, 3, 6);
    static lv_point_precise_t setting_screen_line_3[] = {{0, 0},{60, 60}};
    lv_line_set_points(ui->setting_screen_line_3, setting_screen_line_3, 2);

    //Write style for setting_screen_line_3, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_3, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_3, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_3, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_4
    ui->setting_screen_line_4 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_4, 10, 11);
    lv_obj_set_size(ui->setting_screen_line_4, 5, 7);
    static lv_point_precise_t setting_screen_line_4[] = {{0, 0},{60, 60}};
    lv_line_set_points(ui->setting_screen_line_4, setting_screen_line_4, 2);

    //Write style for setting_screen_line_4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_4, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_4, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_4, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_5
    ui->setting_screen_line_5 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_5, 5, 27);
    lv_obj_set_size(ui->setting_screen_line_5, 5, 9);
    static lv_point_precise_t setting_screen_line_5[] = {{60, 0},{0, 0}};
    lv_line_set_points(ui->setting_screen_line_5, setting_screen_line_5, 2);

    //Write style for setting_screen_line_5, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_5, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_5, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_5, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_5, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_6
    ui->setting_screen_line_6 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_6, 43, 26);
    lv_obj_set_size(ui->setting_screen_line_6, 5, 2);
    static lv_point_precise_t setting_screen_line_6[] = {{60, 0},{0, 0}};
    lv_line_set_points(ui->setting_screen_line_6, setting_screen_line_6, 2);

    //Write style for setting_screen_line_6, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_6, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_6, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_6, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_6, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_7
    ui->setting_screen_line_7 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_7, 10, 33);
    lv_obj_set_size(ui->setting_screen_line_7, 4, 11);
    static lv_point_precise_t setting_screen_line_7[] = {{10, 0},{0, 10}};
    lv_line_set_points(ui->setting_screen_line_7, setting_screen_line_7, 2);

    //Write style for setting_screen_line_7, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_7, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_7, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_7, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_7, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_line_8
    ui->setting_screen_line_8 = lv_line_create(ui->setting_screen_brightness_icon);
    lv_obj_set_pos(ui->setting_screen_line_8, 39, 8);
    lv_obj_set_size(ui->setting_screen_line_8, 5, 13);
    static lv_point_precise_t setting_screen_line_8[] = {{10, 0},{0, 10}};
    lv_line_set_points(ui->setting_screen_line_8, setting_screen_line_8, 2);

    //Write style for setting_screen_line_8, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(ui->setting_screen_line_8, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui->setting_screen_line_8, lv_color_hex(0x5C3A1E), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(ui->setting_screen_line_8, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui->setting_screen_line_8, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_img_1
    ui->setting_screen_img_1 = lv_image_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_img_1, 304, 19);
    lv_obj_set_size(ui->setting_screen_img_1, 122, 55);
    lv_obj_add_flag(ui->setting_screen_img_1, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->setting_screen_img_1, &_huawen_RGB565A8_122x55);
    lv_image_set_pivot(ui->setting_screen_img_1, 50,50);
    lv_image_set_rotation(ui->setting_screen_img_1, 0);

    //Write style for setting_screen_img_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->setting_screen_img_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->setting_screen_img_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes setting_screen_img_2
    ui->setting_screen_img_2 = lv_image_create(ui->setting_screen);
    lv_obj_set_pos(ui->setting_screen_img_2, 591, 20);
    lv_obj_set_size(ui->setting_screen_img_2, 113, 52);
    lv_obj_add_flag(ui->setting_screen_img_2, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->setting_screen_img_2, &_huawen2_RGB565A8_113x52);
    lv_image_set_pivot(ui->setting_screen_img_2, 50,50);
    lv_image_set_rotation(ui->setting_screen_img_2, 0);

    //Write style for setting_screen_img_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->setting_screen_img_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->setting_screen_img_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of setting_screen.


    //Update current screen layout.
    lv_obj_update_layout(ui->setting_screen);

    //Init events for setting_screen.
    events_init_setting_screen(ui);

}
