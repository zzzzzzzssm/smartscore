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



void setup_scr_screen(lv_ui *ui)
{
    //Write codes screen
    ui->screen = lv_obj_create(NULL);
    lv_obj_set_size(ui->screen, 1024, 600);
    lv_obj_set_scrollbar_mode(ui->screen, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_backgroung
    ui->screen_backgroung = lv_image_create(ui->screen);
    lv_obj_set_pos(ui->screen_backgroung, 0, 0);
    lv_obj_set_size(ui->screen_backgroung, 1024, 600);
    lv_obj_add_flag(ui->screen_backgroung, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_backgroung, &_setupScreenBackground_resized_resized_RGB565A8_1024x600);
    lv_image_set_pivot(ui->screen_backgroung, 50,50);
    lv_image_set_rotation(ui->screen_backgroung, 0);

    //Write style for screen_backgroung, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_backgroung, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_backgroung, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_Title
    ui->screen_Title = lv_label_create(ui->screen);
    lv_obj_set_pos(ui->screen_Title, 304, 140);
    lv_obj_set_size(ui->screen_Title, 415, 88);
    lv_label_set_text(ui->screen_Title, "智能乐谱");
    lv_label_set_long_mode(ui->screen_Title, LV_LABEL_LONG_WRAP);

    //Write style for screen_Title, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_Title, lv_color_hex(0xffbf00), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_Title, &lv_font_gudianChinese_84, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_Title, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_Title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_Title, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_start_button
    ui->screen_start_button = lv_button_create(ui->screen);
    lv_obj_set_pos(ui->screen_start_button, 422, 271);
    lv_obj_set_size(ui->screen_start_button, 176, 59);
    ui->screen_start_button_label = lv_label_create(ui->screen_start_button);
    lv_label_set_text(ui->screen_start_button_label, "演奏模式");
    lv_label_set_long_mode(ui->screen_start_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->screen_start_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->screen_start_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->screen_start_button_label, LV_PCT(100));

    //Write style for screen_start_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_start_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_start_button, lv_color_hex(0x311111), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_start_button, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->screen_start_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_start_button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_start_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_start_button, lv_color_hex(0xF5E6D3), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_start_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_start_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_start_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_setting_button
    ui->screen_setting_button = lv_button_create(ui->screen);
    lv_obj_set_pos(ui->screen_setting_button, 422, 435);
    lv_obj_set_size(ui->screen_setting_button, 176, 69);
    ui->screen_setting_button_label = lv_label_create(ui->screen_setting_button);
    lv_label_set_text(ui->screen_setting_button_label, "设置");
    lv_label_set_long_mode(ui->screen_setting_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->screen_setting_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->screen_setting_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->screen_setting_button_label, LV_PCT(100));

    //Write style for screen_setting_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_setting_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_setting_button, lv_color_hex(0x311111), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_setting_button, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->screen_setting_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_setting_button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_setting_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_setting_button, lv_color_hex(0xF5E6D3), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_setting_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_setting_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_setting_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_choose_button
    ui->screen_choose_button = lv_button_create(ui->screen);
    lv_obj_set_pos(ui->screen_choose_button, 422, 359);
    lv_obj_set_size(ui->screen_choose_button, 176, 59);
    ui->screen_choose_button_label = lv_label_create(ui->screen_choose_button);
    lv_label_set_text(ui->screen_choose_button_label, "其他模式");
    lv_label_set_long_mode(ui->screen_choose_button_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->screen_choose_button_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->screen_choose_button, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(ui->screen_choose_button_label, LV_PCT(100));

    //Write style for screen_choose_button, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_choose_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_choose_button, lv_color_hex(0x311111), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_choose_button, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui->screen_choose_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_choose_button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_choose_button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_choose_button, lv_color_hex(0xF5E6D3), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_choose_button, &lv_font_gudianChinese_34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_choose_button, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_choose_button, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of screen.


    //Update current screen layout.
    lv_obj_update_layout(ui->screen);

    //Init events for screen.
    events_init_screen(ui);
}
